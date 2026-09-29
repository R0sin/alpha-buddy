#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/StickS3ShutterInput.h"

#include "alpha_buddy/SideButtonDoubleClick.h"
#include "alpha_buddy/FlipCancelGesture.h"

#include <atomic>
#include <new>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace alpha_buddy {

namespace {

constexpr gpio_num_t kButtonAPin = GPIO_NUM_11;
constexpr gpio_num_t kButtonBPin = GPIO_NUM_12;
constexpr uint32_t kPollIntervalMs = 5;
constexpr uint32_t kDebounceMs = 15;
constexpr uint32_t kTaskStackBytes = 2048;
constexpr UBaseType_t kTaskPriority = 2;
constexpr BaseType_t kTaskCore = 1;

uint8_t pressedButtons() {
    return (digitalRead(kButtonAPin) == LOW ? 1U : 0U) |
           (digitalRead(kButtonBPin) == LOW ? 2U : 0U);
}

}  // namespace

class StickS3ShutterInput::Impl {
public:
    ~Impl() {
        stop();
        if (_stopped != nullptr) {
            vSemaphoreDelete(_stopped);
        }
    }

    bool start() {
        if (_running.load()) {
            return true;
        }
        _stopped = xSemaphoreCreateBinary();
        if (_stopped == nullptr) {
            return false;
        }
        _stableButtons = pressedButtons();
        _lastRawButtons = _stableButtons;
        _lastRawChangeMs = millis();
        if (_stableButtons & 1U) {
            _events.store(1);
            _held.store(true);
        }
        if (_stableButtons != 0) {
            _activityPending.store(true);
        }
        _running.store(true);
        if (xTaskCreatePinnedToCore(taskEntry,
                                    "alpha-button",
                                    kTaskStackBytes,
                                    this,
                                    kTaskPriority,
                                    &_task,
                                    kTaskCore) != pdPASS) {
            _running.store(false);
            _task = nullptr;
            return false;
        }
        return true;
    }

    void stop() {
        if (!_running.exchange(false)) {
            return;
        }
        if (_task != nullptr) {
            xSemaphoreTake(_stopped, portMAX_DELAY);
            _task = nullptr;
        }
    }

    ShutterInputEvents takeEvents() {
        ShutterInputEvents events;
        const unsigned bits = _events.exchange(0);
        events.pressed = (bits & 1U) != 0;
        events.released = (bits & 2U) != 0;
        events.cancelled = (bits & 4U) != 0;
        return events;
    }

    bool held() const { return _held.load(); }
    bool cancelPending() const { return _cancelPending.load(); }
    bool cancelCounting() {
        portENTER_CRITICAL(&_gestureLock);
        const bool counting = _held.load() && _gesture.counting(millis());
        portEXIT_CRITICAL(&_gestureLock);
        return counting;
    }
    void observeOrientation(bool resolved, LandscapeOrientation screen,
                            bool valid, LandscapeOrientation observed, uint32_t now) {
        portENTER_CRITICAL(&_gestureLock);
        _orientationResolved = resolved;
        _screenOrientation = screen;
        if (_held.load()) {
            _gesture.observe(valid, observed, now);
            _cancelPending.store(_gesture.cancelled());
        }
        portEXIT_CRITICAL(&_gestureLock);
    }

    bool takeActivity() { return _activityPending.exchange(false); }
    bool takeGridToggle() { return (_gridClicks.exchange(0) & 1U) != 0; }

private:
    static void taskEntry(void* context) {
        static_cast<Impl*>(context)->taskLoop();
    }

    void taskLoop() {
        while (_running.load()) {
            const uint32_t now = millis();
            const uint8_t rawButtons = pressedButtons();
            if (rawButtons != _lastRawButtons) {
                _lastRawButtons = rawButtons;
                _lastRawChangeMs = now;
            } else if (rawButtons != _stableButtons &&
                       now - _lastRawChangeMs >= kDebounceMs) {
                const uint8_t pressed = rawButtons & ~_stableButtons;
                const uint8_t released = _stableButtons & ~rawButtons;
                _stableButtons = rawButtons;
                if (pressed & 1U) {
                    portENTER_CRITICAL(&_gestureLock);
                    _gesture.begin(_orientationResolved, _screenOrientation);
                    _cancelPending.store(false);
                    _held.store(true);
                    _events.fetch_or(1U);
                    portEXIT_CRITICAL(&_gestureLock);
                }
                if (released & 1U) {
                    portENTER_CRITICAL(&_gestureLock);
                    _events.fetch_or(2U | (_gesture.cancelled() ? 4U : 0U));
                    _held.store(false);
                    _cancelPending.store(false);
                    portEXIT_CRITICAL(&_gestureLock);
                }
                if (pressed != 0) {
                    _activityPending.store(true);
                }
            }
            if (_sideClick.update((_stableButtons & 2U) != 0, now)) {
                _gridClicks.fetch_add(1);
            }
            vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
        }
        xSemaphoreGive(_stopped);
        vTaskDelete(nullptr);
    }

    std::atomic<bool> _running{false};
    std::atomic<unsigned> _events{0};
    std::atomic<bool> _held{false}, _cancelPending{false};
    portMUX_TYPE _gestureLock = portMUX_INITIALIZER_UNLOCKED;
    FlipCancelGesture _gesture;
    bool _orientationResolved = false;
    LandscapeOrientation _screenOrientation = LandscapeOrientation::Rotation1;
    // Separate from shutter events: the power timer must not consume AF/capture input.
    std::atomic<bool> _activityPending{false};
    SideButtonDoubleClick _sideClick;
    std::atomic<unsigned> _gridClicks{0};
    uint8_t _stableButtons = 0;
    uint8_t _lastRawButtons = 0;
    uint32_t _lastRawChangeMs = 0;
    SemaphoreHandle_t _stopped = nullptr;
    TaskHandle_t _task = nullptr;
};

StickS3ShutterInput::StickS3ShutterInput() = default;

StickS3ShutterInput::~StickS3ShutterInput() {
    end();
}

bool StickS3ShutterInput::begin() {
    if (_impl != nullptr) {
        return true;
    }
    _impl = new (std::nothrow) Impl();
    if (_impl == nullptr || !_impl->start()) {
        delete _impl;
        _impl = nullptr;
        return false;
    }
    return true;
}

void StickS3ShutterInput::end() {
    delete _impl;
    _impl = nullptr;
}

ShutterInputEvents StickS3ShutterInput::poll() {
    return _impl == nullptr ? ShutterInputEvents{} : _impl->takeEvents();
}

bool StickS3ShutterInput::takeActivity() {
    return _impl != nullptr && _impl->takeActivity();
}

bool StickS3ShutterInput::takeGridToggle() {
    return _impl != nullptr && _impl->takeGridToggle();
}

bool StickS3ShutterInput::held() const {
    return _impl != nullptr && _impl->held();
}

bool StickS3ShutterInput::cancelPending() const {
    return _impl != nullptr && _impl->cancelPending();
}

bool StickS3ShutterInput::cancelCounting() const {
    return _impl != nullptr && _impl->cancelCounting();
}

void StickS3ShutterInput::observeOrientation(bool resolved, LandscapeOrientation screen,
                                           bool valid, LandscapeOrientation observed, uint32_t now) {
    if (_impl) _impl->observeOrientation(resolved, screen, valid, observed, now);
}

}  // namespace alpha_buddy

#endif
