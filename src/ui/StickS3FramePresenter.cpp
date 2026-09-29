#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/StickS3FramePresenter.h"
#include "alpha_buddy/AccessPoint.h"
#include "alpha_buddy/StickS3ShutterInput.h"
#include "alpha_buddy/DisconnectedShutdown.h"

#include "alpha_buddy/LandscapeOrientationTracker.h"
#include "alpha_buddy/ModeIcon.h"
#include "alpha_buddy/GridExtent.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

#include <M5Unified.h>
#include <Preferences.h>
#include <cmath>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace alpha_buddy {

namespace {

constexpr uint8_t kFrameSlotCount = 2;
constexpr uint8_t kStopSlot = 0xFF;
constexpr uint32_t kDisplayTaskStackBytes = 8192;
constexpr UBaseType_t kDisplayTaskPriority = 1;
// Wi-Fi runs on core 0; keep JPEG decoding off its receive path.
constexpr BaseType_t kDisplayTaskCore = 1;
constexpr uint32_t kUiPollIntervalMs = 40;
constexpr uint32_t kBatteryPollIntervalMs = 5000;
constexpr uint32_t kPowerPollIntervalMs = 250;
constexpr int16_t kUsbPresentMillivolts = 4000;
constexpr int32_t kStatusWidth = 36;
constexpr int32_t kStatusSlotHeight = 27;
constexpr int32_t kStatusSlotCount = 5;
constexpr int32_t kStatusIconWidth = 20;
constexpr int32_t kStatusIconHeight = kStatusIconWidth * 3 / 4;
constexpr int32_t kStatusIconInsetY = (kStatusSlotHeight - kStatusIconHeight) / 2;
constexpr int32_t kBatteryTop = kStatusIconInsetY;

struct BatteryState {
    int32_t level = -1;
    bool charging = false;
    bool valid = false;
};

struct FrameSlot {
    uint8_t* jpeg = nullptr;
    size_t capacity = 0;
    size_t jpegBytes = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t readyAtMs = 0;
    uint32_t session = 0;
};

bool reserveSlot(FrameSlot& slot, size_t requiredBytes) {
    if (requiredBytes <= slot.capacity) {
        return true;
    }
    const size_t roundedBytes = (requiredBytes + 4095U) & ~size_t{4095U};
    auto* replacement = static_cast<uint8_t*>(
        heap_caps_malloc(roundedBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (replacement == nullptr) {
        return false;
    }
    heap_caps_free(slot.jpeg);
    slot.jpeg = replacement;
    slot.capacity = roundedBytes;
    return true;
}

void releaseSlot(FrameSlot& slot) {
    heap_caps_free(slot.jpeg);
    slot = FrameSlot{};
}

}  // namespace

class StickS3FramePresenter::Impl {
public:
    explicit Impl(StickS3ShutterInput& shutterInput) : _shutterInput(shutterInput) {}

    ~Impl() {
        stop();
        for (FrameSlot& slot : _slots) {
            releaseSlot(slot);
        }
        if (_freeSlots != nullptr) {
            vQueueDelete(_freeSlots);
        }
        if (_readySlots != nullptr) {
            vQueueDelete(_readySlots);
        }
        if (_stopped != nullptr) {
            vSemaphoreDelete(_stopped);
        }
    }

    bool start() {
        _settingsReady = _settings.begin("alpha-buddy-ui", false);
        if (_settingsReady) _gridVisible = _settings.getBool("grid", false);
        else Serial.println("Grid settings unavailable");
        M5.Display.setRotation(1);
        M5.Display.fillScreen(TFT_BLACK);

        _freeSlots = xQueueCreate(kFrameSlotCount, sizeof(uint8_t));
        _readySlots = xQueueCreate(1, sizeof(uint8_t));
        _stopped = xSemaphoreCreateBinary();
        if (_freeSlots == nullptr || _readySlots == nullptr || _stopped == nullptr) {
            return false;
        }
        for (uint8_t index = 0; index < kFrameSlotCount; ++index) {
            if (xQueueSend(_freeSlots, &index, 0) != pdTRUE) {
                return false;
            }
        }
        if (xTaskCreatePinnedToCore(taskEntry,
                                    "alpha-display",
                                    kDisplayTaskStackBytes,
                                    this,
                                    kDisplayTaskPriority,
                                    &_task,
                                    kDisplayTaskCore) != pdPASS) {
            _task = nullptr;
            return false;
        }
        _running = true;
        return true;
    }

    void setFeedback(CaptureFeedback feedback) {
        _feedback.store(feedback);
    }

    void setWaitingForWifi(bool waiting) {
        _waitingForWifi.store(waiting);
    }

    void setConnection(CameraConnection connection) {
        if (connection == CameraConnection::Connected) {
            // Waiting-time button presses extend power-on time, not a future shot.
            _shutterInput.poll();
            _resumedAtMs.store(millis());
        } else {
            // Invalidate queued frames as well as the frame already on screen.
            ++_session;
            _cameraModes.store(0);
        }
        const CameraConnection previous = _connection.exchange(connection);
        if (connection == CameraConnection::Connected ||
            previous == CameraConnection::Connected) {
            // Preserve even a brief successful session between power polls.
            _powerTimerReset.store(true);
        }
    }

    void setPreviewPaused(bool paused) {
        if (!paused) _resumedAtMs.store(millis());
        _previewPaused.store(paused);
    }

    void setCameraModes(CameraModes modes) {
        _cameraModes.store(modes.packed());
        _modesUpdatedAt.store(millis());
    }

    bool enqueue(const uint8_t* jpeg,
                 size_t jpegBytes,
                 uint16_t width,
                 uint16_t height) {
        const uint32_t readyAtMs = millis();
        const uint32_t session = _session.load();
        if (!_running || !_healthy || jpeg == nullptr || jpegBytes == 0 ||
            width == 0 || height == 0) {
            return false;
        }

        uint8_t slotIndex = 0;
        if (xQueueReceive(_freeSlots, &slotIndex, 0) != pdTRUE) {
            // Taking a queued slot transfers ownership; never overwrite the
            // slot held by draw(). present() and stop() share one caller.
            if (xQueueReceive(_readySlots, &slotIndex, 0) == pdTRUE) {
                ++_replacedFrames;
            } else if (xQueueReceive(_freeSlots, &slotIndex, portMAX_DELAY) != pdTRUE) {
                return false;
            }
        }
        if (!_healthy || !reserveSlot(_slots[slotIndex], jpegBytes)) {
            xQueueSend(_freeSlots, &slotIndex, portMAX_DELAY);
            return false;
        }

        FrameSlot& slot = _slots[slotIndex];
        std::memcpy(slot.jpeg, jpeg, jpegBytes);
        slot.jpegBytes = jpegBytes;
        slot.width = width;
        slot.height = height;
        slot.readyAtMs = readyAtMs;
        slot.session = session;
        uint8_t staleIndex = 0;
        if (xQueueReceive(_readySlots, &staleIndex, 0) == pdTRUE) {
            xQueueSend(_freeSlots, &staleIndex, portMAX_DELAY);
            ++_replacedFrames;
        }
        if (xQueueSend(_readySlots, &slotIndex, portMAX_DELAY) != pdTRUE) {
            xQueueSend(_freeSlots, &slotIndex, portMAX_DELAY);
            return false;
        }
        return true;
    }

    FramePresentationStats stop() {
        if (!_running) {
            return _stats;
        }
        if (xQueueSend(_readySlots, &kStopSlot, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(_stopped, portMAX_DELAY);
        }
        _running = false;
        _task = nullptr;
        return _stats;
    }

private:
    static int drawJpegBlock(JPEGDRAW* block) {
        auto* canvas = static_cast<M5Canvas*>(block->pUser);
        // iWidth is the source stride; iWidthUsed clips the final MCU column.
        for (int row = 0; row < block->iHeight; ++row) {
            canvas->pushImage(block->x, block->y + row, block->iWidthUsed, 1,
                             reinterpret_cast<const lgfx::rgb565_t*>(
                                 block->pPixels + row * block->iWidth));
        }
        return 1;
    }

    static void taskEntry(void* context) {
        static_cast<Impl*>(context)->taskLoop();
    }

    void taskLoop() {
        for (;;) {
            updateSensors();
            updatePower();
            updateGrid();
            uint8_t slotIndex = kStopSlot;
            if (xQueueReceive(_readySlots,
                              &slotIndex,
                              pdMS_TO_TICKS(kUiPollIntervalMs)) != pdTRUE) {
                refreshHud();
                continue;
            }
            if (slotIndex == kStopSlot) {
                break;
            }

            FrameSlot& slot = _slots[slotIndex];
            if (!canDisplayPreview(_connection.load(), slot.session, _session.load())) {
                xQueueSend(_freeSlots, &slotIndex, portMAX_DELAY);
                refreshHud();
                continue;
            }
            const uint32_t startedAt = millis();
            if (_windowStartedAt == 0) {
                _windowStartedAt = startedAt;
            }
            const uint32_t waitMs = startedAt - slot.readyAtMs;
            uint32_t decodeMs = 0;
            uint32_t blitMs = 0;
            const bool displayed = draw(slot, decodeMs, blitMs);
            const uint32_t elapsedMs = millis() - startedAt;
            _stats.totalDisplayMs += elapsedMs;
            if (elapsedMs > _stats.maximumDisplayMs) {
                _stats.maximumDisplayMs = elapsedMs;
            }
            if (displayed) {
                ++_stats.framesDisplayed;
                ++_windowFrames;
                _windowWaitMs += waitMs;
                _windowDecodeMs += decodeMs;
                _windowBlitMs += blitMs;
                _windowAgeMs += waitMs + elapsedMs;
                _windowMaxAgeMs = std::max(_windowMaxAgeMs, waitMs + elapsedMs);
            } else {
                ++_stats.failures;
                _healthy = false;
            }
            refreshHud();
            xQueueSend(_freeSlots, &slotIndex, portMAX_DELAY);
            emitPerformance();
            // With a continuously full queue this task would otherwise remain
            // runnable forever and starve the idle task feeding the watchdog.
            vTaskDelay(1);
        }
        xSemaphoreGive(_stopped);
        vTaskDelete(nullptr);
    }

    void emitPerformance() {
        const uint32_t now = millis();
        const uint32_t windowMs = now - _windowStartedAt;
        if (windowMs < 5000 || _windowFrames == 0) {
            return;
        }
        Serial.printf("{\"event\":\"display_perf\",\"frames\":%lu,\"window_ms\":%lu,"
                      "\"fps\":%.2f,\"ready_wait_avg_ms\":%.2f,\"decode_avg_ms\":%.2f,"
                      "\"blit_avg_ms\":%.2f,\"ready_to_display_avg_ms\":%.2f,"
                      "\"ready_to_display_max_ms\":%lu,\"replaced_frames\":%lu,"
                      "\"display_failures\":%lu}\n",
                      static_cast<unsigned long>(_windowFrames),
                      static_cast<unsigned long>(windowMs),
                      1000.0 * _windowFrames / windowMs,
                      double(_windowWaitMs) / _windowFrames,
                      double(_windowDecodeMs) / _windowFrames,
                      double(_windowBlitMs) / _windowFrames,
                      double(_windowAgeMs) / _windowFrames,
                      static_cast<unsigned long>(_windowMaxAgeMs),
                      static_cast<unsigned long>(_replacedFrames.exchange(0)),
                      static_cast<unsigned long>(_stats.failures));
        _windowStartedAt = now;
        _windowFrames = 0;
        _windowWaitMs = 0;
        _windowDecodeMs = 0;
        _windowBlitMs = 0;
        _windowAgeMs = 0;
        _windowMaxAgeMs = 0;
    }

    void updatePower() {
        const uint32_t now = millis();
        if (_powerReadAttempted && now - _lastPowerReadMs < kPowerPollIntervalMs) {
            return;
        }
        _powerReadAttempted = true;
        _lastPowerReadMs = now;
        const int16_t vbusMv = M5.Power.getVBUSVoltage();
        const bool externalPower = vbusMv < 0 || vbusMv >= kUsbPresentMillivolts;
        const bool buttonActivity = _shutterInput.takeActivity();
        const bool sessionChanged = _powerTimerReset.exchange(false);
        if (_shutdown.update(now,
                             _connection.load() == CameraConnection::Connected,
                             externalPower,
                             buttonActivity || sessionChanged)) {
            Serial.println("{\"event\":\"auto_power_off\",\"reason\":\"disconnected_timeout\"}");
            // Runs on the display owner task, including during blocking handshakes.
            M5.Power.powerOff();
        }
    }

    void updateSensors() {
        const uint32_t now = millis();
        if (M5.Imu.isEnabled() &&
            (!_imuReadAttempted || now - _lastImuReadMs >= kUiPollIntervalMs)) {
            _imuReadAttempted = true;
            _lastImuReadMs = now;
            if (M5.Imu.update()) {
                const m5::imu_data_t data = M5.Imu.getImuData();
                const LandscapeOrientation before = _orientation.orientation();
                const LandscapeGravity gravity =
                    stickS3LandscapeGravity(data.accel.x, data.accel.y);
                _orientation.update(gravity.verticalG,
                                    gravity.horizontalG,
                                    now,
                                    _shutterInput.held() || _feedback.load() != CaptureFeedback::Live);
                LandscapeOrientation observed = before;
                const bool valid = _orientation.observed(observed);
                _shutterInput.observeOrientation(_orientation.resolved(), _orientation.orientation(),
                                                 valid, observed, now);
                if (_orientation.orientation() != before) {
                    _layoutDirty = true;
                }
            }
        }

        if (!_batteryReadAttempted ||
            now - _lastBatteryReadMs >= kBatteryPollIntervalMs) {
            _batteryReadAttempted = true;
            _lastBatteryReadMs = now;
            const int32_t level = M5.Power.getBatteryLevel();
            if (level >= 0 && level <= 100) {
                const bool charging =
                    M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
                if (!_battery.valid || _battery.level != level ||
                    _battery.charging != charging) {
                    _battery.level = level;
                    _battery.charging = charging;
                    _battery.valid = true;
                    _hudDirty = true;
                }
            }
        }
    }

    bool draw(const FrameSlot& slot, uint32_t& decodeMs, uint32_t& blitMs) {
        refreshHud();

        constexpr float decodeScale = 0.25f;
        const int32_t decodedWidth = static_cast<int32_t>(slot.width * decodeScale);
        const int32_t decodedHeight = static_cast<int32_t>(slot.height * decodeScale);
        if (decodedWidth <= 0 || decodedHeight <= 0) {
            return false;
        }
        if (_canvas.width() != decodedWidth || _canvas.height() != decodedHeight) {
            _canvas.deleteSprite();
            _canvas.setColorDepth(16);
            if (_canvas.createSprite(decodedWidth, decodedHeight) == nullptr) {
                return false;
            }
            _canvas.setPivot((decodedWidth - 1) * 0.5f,
                             (decodedHeight - 1) * 0.5f);
        }
        const uint32_t decodeStartedAt = millis();
        if (!_jpeg.openRAM(slot.jpeg, static_cast<int>(slot.jpegBytes), drawJpegBlock)) {
            return false;
        }
        _jpeg.setUserPointer(&_canvas);
        _jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        const bool decoded = _jpeg.getWidth() == slot.width &&
                             _jpeg.getHeight() == slot.height &&
                             _jpeg.decode(0, 0, JPEG_SCALE_QUARTER);
        _jpeg.close();
        if (!decoded) {
            return false;
        }
        decodeMs = millis() - decodeStartedAt;
        const uint32_t blitStartedAt = millis();
        blitCanvas();
        _previewVisible = true;
        _visibleSession = slot.session;
        _lastFrameMs = millis();
        refreshHud();
        blitMs = millis() - blitStartedAt;
        return true;
    }

    void updateGrid() {
        if (!_shutterInput.takeGridToggle()) return;
        _gridVisible = !_gridVisible;
        // The retained canvas never contains grid ink, so hiding needs no new frame.
        if (_healthy && _previewVisible &&
            canDisplayPreview(_connection.load(), _visibleSession, _session.load())) {
            blitCanvas();
        }
        if (!_settingsReady || _settings.putBool("grid", _gridVisible) != 1) {
            Serial.println("Grid setting could not be saved");
        }
    }

    void blitCanvas() {
        const int32_t decodedWidth = _canvas.width();
        const int32_t decodedHeight = _canvas.height();
        const int32_t screenHeight = M5.Display.height();
        const int32_t previewWidth = M5.Display.width() - kStatusWidth;
        M5.Display.startWrite();
        const float displayZoom = std::min(
            static_cast<float>(previewWidth) / decodedWidth,
            static_cast<float>(screenHeight) / decodedHeight);
        const bool statusOnRight =
            _appliedOrientation == LandscapeOrientation::Rotation1;
        const float previewCenterX = statusOnRight
            ? previewWidth * 0.5f
            : kStatusWidth + previewWidth * 0.5f;
        _canvas.pushRotateZoom(previewCenterX,
                               screenHeight * 0.5f,
                               0.0f,
                               displayZoom,
                               displayZoom);
        {
            // Pixel centers follow pushRotateZoom's pivot; stay inside the image.
            const int32_t left = static_cast<int32_t>(std::ceil(
                previewCenterX - (decodedWidth - 1) * displayZoom * 0.5f));
            const int32_t top = static_cast<int32_t>(std::ceil(
                screenHeight * 0.5f - (decodedHeight - 1) * displayZoom * 0.5f));
            const int32_t right = static_cast<int32_t>(std::floor(
                previewCenterX + (decodedWidth - 1) * displayZoom * 0.5f));
            const int32_t bottom = static_cast<int32_t>(std::floor(
                screenHeight * 0.5f + (decodedHeight - 1) * displayZoom * 0.5f));
            // Grid endpoints cover the full image; feedback retains its inset bounds.
            const GridExtent gridX = gridExtent(previewCenterX, decodedWidth, displayZoom);
            const GridExtent gridY = gridExtent(screenHeight * 0.5f, decodedHeight, displayZoom);
            for (int i = 1; _gridVisible && i <= 2; ++i) {
                M5.Display.drawFastVLine(left + (right - left + 1) * i / 3,
                                        gridY.first, gridY.last - gridY.first + 1, TFT_BLACK);
                M5.Display.drawFastHLine(gridX.first, top + (bottom - top + 1) * i / 3,
                                        gridX.last - gridX.first + 1, TFT_BLACK);
            }
            _drawnBorderColor = frameBorderColor();
            if (_drawnBorderColor != TFT_BLACK) {
                for (int inset = 0; inset < 2; ++inset)
                    M5.Display.drawRect(left + inset, top + inset,
                                        right - left + 1 - inset * 2,
                                        bottom - top + 1 - inset * 2, _drawnBorderColor);
            }
        }
        M5.Display.endWrite();
        M5.Display.waitDMA();
    }

    void refreshHud() {
        const CameraConnection connection = _connection.load();
        const bool credentialsVisible = _waitingForWifi.load() &&
            connection != CameraConnection::Connected;
        const bool credentialsChanged = credentialsVisible != _credentialsVisible;
        const bool paused = _previewPaused.load();
        const uint32_t resumedAt = _resumedAtMs.load();
        const uint32_t now = millis();
        const ViewfinderHint hint = viewfinderHint(connection, paused,
                                                   now, _lastFrameMs, resumedAt);
        const bool blink = hint == ViewfinderHint::Waiting && (now / 500) % 2 == 0;
        const bool clearPreview = _previewVisible &&
            !canDisplayPreview(connection, _visibleSession, _session.load());
        // Apply orientation on the UI poll path too: waiting for a camera or
        // a new JPEG must not prevent rotation. Reuse only a current-session frame.
        const LandscapeOrientation desiredOrientation = _orientation.orientation();
        const bool rotated = _layoutDirty || desiredOrientation != _appliedOrientation;
        if (rotated) {
            M5.Display.setRotation(
                desiredOrientation == LandscapeOrientation::Rotation1 ? 1 : 3);
            M5.Display.fillScreen(TFT_BLACK);
            _appliedOrientation = desiredOrientation;
            _layoutDirty = false;
            _hudDirty = true;
        }
        // Keep feedback responsive even when no new JPEG arrives. The canvas
        // contains only the image, so a redraw also cleanly removes the border.
        if (_previewVisible && !clearPreview &&
            (rotated || frameBorderColor() != _drawnBorderColor))
            blitCanvas();
        const bool modesVisible = connection == CameraConnection::Connected;
        const bool cancelPending = modesVisible && _shutterInput.cancelPending();
        const uint32_t modes = visibleCameraModes(connection, _cameraModes.load(), now, _modesUpdatedAt.load());
        if (!_hudDirty && hint == _hint && blink == _blink && !clearPreview && !credentialsChanged &&
            modes == _drawnModes && modesVisible == _modesVisible &&
            cancelPending == _cancelPending) return;
        _cancelPending = cancelPending;
        _drawnModes = modes;
        _modesVisible = modesVisible;
        _hint = hint;
        _blink = blink;
        const bool onRight = _appliedOrientation == LandscapeOrientation::Rotation1;
        M5.Display.startWrite();
        if (clearPreview || credentialsChanged) {
            M5.Display.fillRect(onRight ? 0 : kStatusWidth, 0,
                                M5.Display.width() - kStatusWidth,
                                M5.Display.height(), TFT_BLACK);
            _previewVisible = false;
        }
        if (credentialsVisible && (credentialsChanged || rotated || clearPreview)) {
            const int32_t left = (onRight ? 0 : kStatusWidth) + 6;
            const int32_t top = M5.Display.height() / 2 - 24;
            M5.Display.setFont(&fonts::Font0);
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
            M5.Display.setCursor(left, top);
            M5.Display.printf("SSID %s", kApSsid);
            M5.Display.setCursor(left, top + 32);
            M5.Display.printf("PASS %s", apPassword);
        }
        _credentialsVisible = credentialsVisible;
        drawBatteryHud(_appliedOrientation, onRight, M5.Display.width(), M5.Display.height());
        if (modesVisible) drawCameraModes(onRight, modes);
        drawConnectionHint(onRight);
        M5.Display.endWrite();
        M5.Display.waitDMA();
        _hudDirty = false;
    }

    void drawConnectionHint(bool onRight) {
        if (!_cancelPending && _hint == ViewfinderHint::None) return;
        constexpr int32_t width = kStatusIconWidth;
        constexpr int32_t height = kStatusIconHeight;
        const int32_t left = (onRight ? M5.Display.width() - kStatusWidth : 0)
            + (kStatusWidth - width) / 2;
        // Center the camera hint in the fifth 36x27 slot.
        constexpr int32_t top = (kStatusSlotCount - 1) * kStatusSlotHeight
            + kStatusIconInsetY;
        const uint16_t color = _cancelPending ? TFT_YELLOW :
            (_hint == ViewfinderHint::Stalled ? TFT_ORANGE : TFT_WHITE);
        const auto rect = [&](int x, int y, int w, int h, uint16_t ink) {
            const HudRect r = orientHudRect(_appliedOrientation, width, height, {x, y, w, h});
            M5.Display.fillRect(left + r.x, top + r.y, r.width, r.height, ink);
        };
        const auto circle = [&](int x, int y, int radius, uint16_t ink) {
            const HudPoint p = orientHudPoint(_appliedOrientation, width, height, {x, y});
            M5.Display.fillCircle(left + p.x, top + p.y, radius, ink);
        };
        // Two-pixel shell, clipped corners and a round lens match the battery's
        // compact, filled pixel treatment without hairline details.
        rect(0, 3, 16, height - 3, color);
        rect(2, 5, 12, height - 7, TFT_BLACK);
        rect(0, 3, 1, 1, TFT_BLACK);
        rect(15, 3, 1, 1, TFT_BLACK);
        rect(0, height - 1, 1, 1, TFT_BLACK);
        rect(15, height - 1, 1, 1, TFT_BLACK);
        rect(5, 0, 6, 4, color);
        circle(8, 8, 3, color);
        circle(8, 8, 1, TFT_BLACK);
        if (_cancelPending) {
            // A diagonal cut through the camera reads as "do not take this shot".
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int distance = std::abs(14 * x + 18 * y - 252);
                    if (distance <= 44) rect(x, y, 1, 1, TFT_BLACK);
                    if (distance <= 16) rect(x, y, 1, 1, TFT_YELLOW);
                }
            }
        } else if (_hint == ViewfinderHint::Waiting) {
            if (_blink) rect(17, height - 4, 3, 3, color);
        } else {
            rect(17, 2, 3, height - 7, color);
            rect(17, height - 3, 3, 3, color);
        }
    }

    void drawCameraModes(bool onRight, uint32_t modes) {
        const int left = (onRight ? M5.Display.width() - kStatusWidth : 0) + 8;
        for (unsigned slot = 0; slot < 3; ++slot) {
            const ModeIcon icon = modeIcon(slot, uint8_t(modes >> (slot * 8)));
            const int top = int(slot + 1) * kStatusSlotHeight + kStatusIconInsetY;
            for (int y = 0; y < 15; ++y) {
                for (int x = 0; x < 20; ++x) {
                    if (!(icon.rows[y] & (uint32_t{1} << x))) continue;
                    const HudPoint p = orientHudPoint(_appliedOrientation, 20, 15, {x, y});
                    M5.Display.drawPixel(left + p.x, top + p.y, icon.unknown ? TFT_DARKGREY : TFT_WHITE);
                }
            }
        }
    }

    void drawBatteryHud(LandscapeOrientation orientation,
                        bool statusOnRight,
                        int32_t screenWidth,
                        int32_t screenHeight) {
        const int32_t statusX = statusOnRight ? screenWidth - kStatusWidth : 0;
        M5.Display.fillRect(statusX, 0, kStatusWidth, screenHeight, TFT_BLACK);

        const int32_t iconX = statusX + (kStatusWidth - kStatusIconWidth) / 2;
        const auto drawIconRect = [&](int32_t x,
                                      int32_t y,
                                      int32_t width,
                                      int32_t height,
                                      uint16_t color) {
            const HudRect rect = orientHudRect(
                orientation,
                kStatusIconWidth,
                kStatusIconHeight,
                HudRect{x, y, width, height});
            M5.Display.drawRect(iconX + rect.x,
                                kBatteryTop + rect.y,
                                rect.width,
                                rect.height,
                                color);
        };
        const auto fillIconRect = [&](int32_t x,
                                      int32_t y,
                                      int32_t width,
                                      int32_t height,
                                      uint16_t color) {
            const HudRect rect = orientHudRect(
                orientation,
                kStatusIconWidth,
                kStatusIconHeight,
                HudRect{x, y, width, height});
            M5.Display.fillRect(iconX + rect.x,
                                kBatteryTop + rect.y,
                                rect.width,
                                rect.height,
                                color);
        };
        const auto drawIconLine = [&](int32_t x0,
                                      int32_t y0,
                                      int32_t x1,
                                      int32_t y1,
                                      uint16_t color) {
            const HudPoint start = orientHudPoint(
                orientation,
                kStatusIconWidth,
                kStatusIconHeight,
                HudPoint{x0, y0});
            const HudPoint end = orientHudPoint(
                orientation,
                kStatusIconWidth,
                kStatusIconHeight,
                HudPoint{x1, y1});
            M5.Display.drawLine(iconX + start.x,
                                kBatteryTop + start.y,
                                iconX + end.x,
                                kBatteryTop + end.y,
                                color);
        };
        const uint16_t color = !_battery.valid
            ? TFT_DARKGREY
            : (_battery.level < 10
                ? TFT_RED
                : (_battery.level < 20 ? TFT_ORANGE : TFT_WHITE));
        drawIconRect(0, 0, kStatusIconWidth - 3, kStatusIconHeight, color);
        fillIconRect(kStatusIconWidth - 3,
                     3,
                     3,
                     kStatusIconHeight - 6,
                     color);
        if (_battery.valid) {
            const int32_t segments = _battery.level == 0
                ? 0
                : std::min<int32_t>(4, (_battery.level + 24) / 25);
            for (int32_t index = 0; index < segments; ++index) {
                fillIconRect(3 + index * 3,
                             3,
                             2,
                             kStatusIconHeight - 6,
                             color);
            }
        }
        if (_battery.valid && _battery.charging) {
            const int32_t centerX = iconX + (kStatusIconWidth - 3) / 2;
            fillIconRect(centerX - iconX - 3,
                         1,
                         7,
                         kStatusIconHeight - 2,
                         TFT_BLACK);
            drawIconLine(centerX - iconX + 2,
                         2,
                         centerX - iconX - 1,
                         kStatusIconHeight / 2,
                         TFT_YELLOW);
            drawIconLine(centerX - iconX - 1,
                         kStatusIconHeight / 2,
                         centerX - iconX + 1,
                         kStatusIconHeight / 2,
                         TFT_YELLOW);
            drawIconLine(centerX - iconX + 1,
                         kStatusIconHeight / 2,
                         centerX - iconX - 2,
                         kStatusIconHeight - 3,
                         TFT_YELLOW);
        }
    }

    uint16_t feedbackColor(CaptureFeedback feedback) const {
        uint16_t color = TFT_BLACK;
        switch (feedback) {
            case CaptureFeedback::Live: color = TFT_BLACK; break;
            case CaptureFeedback::Autofocus: color = TFT_YELLOW; break;
            case CaptureFeedback::Capturing: color = TFT_ORANGE; break;
            case CaptureFeedback::Success: color = TFT_GREEN; break;
            case CaptureFeedback::Failure: color = TFT_RED; break;
        }
        return color;
    }

    uint16_t frameBorderColor() const {
        if (_connection.load() == CameraConnection::Connected && _shutterInput.cancelCounting())
            return (millis() / 200) % 2 == 0 ? TFT_YELLOW : TFT_BLACK;
        return feedbackColor(_feedback.load());
    }

    FrameSlot _slots[kFrameSlotCount];
    StickS3ShutterInput& _shutterInput;
    bool _cancelPending = false;
    uint16_t _drawnBorderColor = TFT_BLACK;
    DisconnectedShutdown _shutdown;
    std::atomic<bool> _powerTimerReset{false};
    uint32_t _lastPowerReadMs = 0;
    bool _powerReadAttempted = false;
    QueueHandle_t _freeSlots = nullptr;
    QueueHandle_t _readySlots = nullptr;
    SemaphoreHandle_t _stopped = nullptr;
    TaskHandle_t _task = nullptr;
    M5Canvas _canvas{&M5.Display};
    JPEGDEC _jpeg;
    Preferences _settings;
    bool _settingsReady = false;
    bool _gridVisible = false;
    std::atomic<bool> _running{false};
    std::atomic<bool> _healthy{true};
    std::atomic<CaptureFeedback> _feedback{CaptureFeedback::Live};
    std::atomic<CameraConnection> _connection{CameraConnection::Disconnected};
    std::atomic<bool> _waitingForWifi{false};
    bool _credentialsVisible = false;
    std::atomic<uint32_t> _cameraModes{0};
    std::atomic<uint32_t> _modesUpdatedAt{0};
    uint32_t _drawnModes = 0;
    bool _modesVisible = false;
    std::atomic<uint32_t> _session{0};
    uint32_t _visibleSession = 0;
    bool _previewVisible = false;
    std::atomic<bool> _previewPaused{false};
    std::atomic<uint32_t> _resumedAtMs{0};
    uint32_t _lastFrameMs = 0;
    ViewfinderHint _hint = ViewfinderHint::None;
    bool _blink = false;
    LandscapeOrientationTracker _orientation;
    LandscapeOrientation _appliedOrientation = LandscapeOrientation::Rotation1;
    BatteryState _battery;
    uint32_t _lastImuReadMs = 0;
    uint32_t _lastBatteryReadMs = 0;
    bool _imuReadAttempted = false;
    bool _batteryReadAttempted = false;
    bool _layoutDirty = false;
    bool _hudDirty = true;
    FramePresentationStats _stats;
    std::atomic<uint32_t> _replacedFrames{0};
    uint32_t _windowStartedAt = 0;
    uint32_t _windowFrames = 0;
    uint32_t _windowWaitMs = 0;
    uint32_t _windowDecodeMs = 0;
    uint32_t _windowBlitMs = 0;
    uint32_t _windowAgeMs = 0;
    uint32_t _windowMaxAgeMs = 0;
};

StickS3FramePresenter::StickS3FramePresenter() = default;

StickS3FramePresenter::~StickS3FramePresenter() {
    end();
}

bool StickS3FramePresenter::begin(StickS3ShutterInput& shutterInput) {
    if (_impl != nullptr) {
        return true;
    }
    _impl = new (std::nothrow) Impl(shutterInput);
    if (_impl == nullptr || !_impl->start()) {
        delete _impl;
        _impl = nullptr;
        return false;
    }
    return true;
}

bool StickS3FramePresenter::present(const uint8_t* jpeg,
                                    size_t jpegBytes,
                                    uint16_t width,
                                    uint16_t height) {
    return _impl != nullptr && _impl->enqueue(jpeg, jpegBytes, width, height);
}

void StickS3FramePresenter::setCaptureFeedback(CaptureFeedback feedback) {
    if (_impl != nullptr) {
        _impl->setFeedback(feedback);
    }
}

void StickS3FramePresenter::setConnection(CameraConnection connection) {
    if (_impl != nullptr) _impl->setConnection(connection);
}

void StickS3FramePresenter::setWaitingForWifi(bool waiting) {
    if (_impl != nullptr) _impl->setWaitingForWifi(waiting);
}

void StickS3FramePresenter::setPreviewPaused(bool paused) {
    if (_impl != nullptr) _impl->setPreviewPaused(paused);
}

void StickS3FramePresenter::setCameraModes(CameraModes modes) {
    if (_impl != nullptr) _impl->setCameraModes(modes);
}

FramePresentationStats StickS3FramePresenter::end() {
    if (_impl == nullptr) {
        return _lastStats;
    }
    _lastStats = _impl->stop();
    delete _impl;
    _impl = nullptr;
    return _lastStats;
}

}  // namespace alpha_buddy

#endif
