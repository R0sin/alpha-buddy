#include "alpha_buddy/SonyLiveViewStream.h"

#include <array>

#ifdef ALPHA_BUDDY_NATIVE
#include <chrono>
#else
#include <Arduino.h>
#endif

#include "alpha_buddy/SonyLiveViewProbe.h"

#ifdef ALPHA_BUDDY_PROPERTY_PROBE
#include "PropertySnapshotProbe.h"
#endif

namespace alpha_buddy {

namespace {

constexpr uint16_t kResponseOk = 0x2001;
constexpr uint16_t kResponseOperationNotSupported = 0x2005;
constexpr uint16_t kResponseInvalidStorageId = 0x2008;
constexpr uint16_t kResponseStoreNotAvailable = 0x2013;
constexpr uint16_t kResponseCaptureAlreadyTerminated = 0x2018;
constexpr uint16_t kResponseInvalidObjectHandle = 0x2009;
constexpr uint16_t kResponseAccessDenied = 0x200F;
constexpr uint16_t kOperationSetControlDeviceB = 0x9207;
constexpr uint32_t kPropertyAutoFocus = 0xD2C1;
constexpr uint32_t kPropertyCapture = 0xD2C2;
constexpr uint8_t kControlReleased = 1;
constexpr uint8_t kControlPressed = 2;
constexpr uint8_t kObjectReadyAttempts = 20;
constexpr uint32_t kObjectReadyDelayMs = 50;
constexpr uint8_t kFrameAccessAttempts = 50;
constexpr uint32_t kFrameAccessDelayMs = 20;
constexpr uint32_t kShutterPressMs = 100;
constexpr uint32_t kCaptureFeedbackMs = 700;

struct HandshakeStep {
    uint16_t operationCode;
    std::array<uint32_t, 3> parameters;
    size_t parameterCount;
};

constexpr std::array<HandshakeStep, 7> kHandshake = {{
    {SonyLiveViewProbe::kOperationGetDeviceInfo, {0, 0, 0}, 0},
    {SonyLiveViewProbe::kOperationGetStorageIds, {0, 0, 0}, 0},
    {SonyLiveViewProbe::kOperationSdioConnect, {1, 0, 0}, 3},
    {SonyLiveViewProbe::kOperationSdioConnect, {2, 0, 0}, 3},
    {SonyLiveViewProbe::kOperationSdioGetExtendedDeviceInfo, {0x12C, 0, 0}, 3},
    {SonyLiveViewProbe::kOperationSdioConnect, {3, 0, 0}, 3},
    {SonyLiveViewProbe::kOperationSdioGetExtendedDeviceInfo, {0x12C, 0, 0}, 3},
}};

uint32_t nowMs() {
#ifdef ALPHA_BUDDY_NATIVE
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
#else
    return millis();
#endif
}

bool acceptableHandshakeResponse(uint16_t responseCode) {
    return responseCode == kResponseOk ||
           responseCode == kResponseOperationNotSupported ||
           responseCode == kResponseInvalidStorageId ||
           responseCode == kResponseStoreNotAvailable ||
           responseCode == kResponseCaptureAlreadyTerminated;
}

bool controlAccepted(const OperationResult& result) {
    return result.transportOk && result.responseCode == kResponseOk;
}

OperationResult setSonyControl(IPtpIpCommandPort& port,
                               uint32_t propertyCode,
                               bool pressed) {
    const uint8_t payload[] = {
        pressed ? kControlPressed : kControlReleased,
        0,
    };
    return port.executeWithData(kOperationSetControlDeviceB,
                                &propertyCode,
                                1,
                                payload,
                                sizeof(payload));
}

void shutterDelay() {
#ifndef ALPHA_BUDDY_NATIVE
    delay(kShutterPressMs);
#endif
}

void record(IProbeTrace& trace,
            ProbeStage stage,
            ProbeStatus status,
            uint8_t substep,
            uint16_t operationCode,
            const OperationResult& result,
            size_t bytes = 0) {
    ProbeEvent event;
    event.stage = stage;
    event.status = status;
    event.substep = substep;
    event.operationCode = operationCode;
    event.responseCode = result.responseCode;
    event.transportError = result.transportError;
    event.elapsedMs = result.elapsedMs;
    event.bytes = bytes == 0 ? result.dataBytes : bytes;
    trace.record(event);
}

}  // namespace

SonyLiveViewStream::SonyLiveViewStream(IPtpIpCommandPort& port, IProbeTrace& trace)
    : _port(port), _trace(trace) {}

StreamReport SonyLiveViewStream::run(const PtpSessionConfig& session,
                                     JpegFrameSink& frame,
                                     IFramePresenter& presenter,
                                     const StreamOptions& options,
                                     IShutterInput* shutterInput) {
    StreamReport report;
    const uint32_t startedAt = nowMs();
    presenter.setCameraModes({});
    presenter.setConnection(CameraConnection::Connecting);
    const PortOpenResult opened = _port.open(session);
    if (!opened.ok) {
        report.failedStage = opened.failedStage;
        report.transportError = opened.transportError;
        report.responseCode = opened.responseCode;
        report.elapsedMs = nowMs() - startedAt;
        _port.close();
        presenter.setConnection(CameraConnection::Disconnected);
        return report;
    }

    for (size_t index = 0; index < kHandshake.size(); ++index) {
        const HandshakeStep& step = kHandshake[index];
        const OperationResult result = _port.execute(step.operationCode,
                                                     step.parameters.data(),
                                                     step.parameterCount,
                                                     nullptr);
        report.operationCode = step.operationCode;
        report.responseCode = result.responseCode;
        if (!result.transportOk || !acceptableHandshakeResponse(result.responseCode)) {
            record(_trace,
                   ProbeStage::SonyHandshake,
                   ProbeStatus::Failed,
                   static_cast<uint8_t>(index),
                   step.operationCode,
                   result);
            report.failedStage = ProbeStage::SonyHandshake;
            report.transportError = result.transportError;
            report.elapsedMs = nowMs() - startedAt;
            _port.close();
            presenter.setConnection(CameraConnection::Disconnected);
            return report;
        }
        if (result.responseCode != kResponseOk) {
            ++report.warnings;
        }
    }

    if (options.preferLowResolution) {
        // ILCE-7CM2 2.01 advertises D26A as UINT8, enum {1, 2};
        // these wire values differ from the SDK's public Low/High enum.
        constexpr uint16_t operation = 0x9205;
        const uint32_t property = 0xD26A;
        const uint8_t lowQuality = 1;
        const OperationResult result = _port.executeWithData(operation, &property, 1,
                                                             &lowQuality, 1);
        record(_trace, ProbeStage::SonyHandshake,
               controlAccepted(result) ? ProbeStatus::Passed : ProbeStatus::Warning,
               static_cast<uint8_t>(kHandshake.size()), operation, result, 1);
#ifndef ALPHA_BUDDY_NATIVE
        Serial.printf("{\"event\":\"live_view_quality\",\"requested\":\"low\","
                      "\"accepted\":%s,\"response\":\"0x%04X\"}\n",
                      controlAccepted(result) ? "true" : "false", result.responseCode);
#endif
        if (!result.transportOk) {
            report.failedStage = ProbeStage::SonyHandshake;
            report.transportError = result.transportError;
            report.operationCode = operation;
            report.responseCode = result.responseCode;
            report.elapsedMs = nowMs() - startedAt;
            _port.close();
            presenter.setConnection(CameraConnection::Disconnected);
            return report;
        }
        if (!controlAccepted(result)) {
            ++report.warnings;
        }
    }
    presenter.setConnection(CameraConnection::Connected);
    const uint32_t streamStartedAt = nowMs();
    CameraModeSink modeSink;
    uint32_t lastModePoll = streamStartedAt - 1000;
    uint32_t lastModes = UINT32_MAX;
    FocusMode currentFocus = FocusMode::Unknown;
#ifndef ALPHA_BUDDY_NATIVE
    uint32_t windowStartedAt = streamStartedAt;
    uint32_t windowFrames = 0;
    uint32_t windowBytes = 0;
    uint32_t previousInfoMs = 0;
    uint32_t previousGetMs = 0;
    uint32_t previousPresentMs = 0;
#endif
    uint8_t consecutiveFrameFailures = 0;
    bool shutterGestureActive = false;
    bool autofocusStarted = false;
    bool controlSessionHealthy = true;
    uint32_t feedbackExpiresAtMs = 0;

    auto recordControl = [&](ProbeStage stage,
                             uint8_t substep,
                             const OperationResult& result) {
        record(_trace,
               stage,
               controlAccepted(result) ? ProbeStatus::Passed : ProbeStatus::Failed,
               substep,
               kOperationSetControlDeviceB,
               result,
               2);
    };

    auto processShutterInput = [&]() {
        if (shutterInput == nullptr) {
            return;
        }
        const ShutterInputEvents events = shutterInput->poll();
        if (events.pressed) {
            shutterGestureActive = true;
            feedbackExpiresAtMs = 0;
            presenter.setCaptureFeedback(CaptureFeedback::Autofocus);
            autofocusStarted = currentFocus != FocusMode::MF || nowMs() - lastModePoll > 3500;
            if (autofocusStarted) {
                presenter.setPreviewPaused(true);
                const OperationResult autofocusOn = setSonyControl(_port,
                                                                   kPropertyAutoFocus,
                                                                   true);
                recordControl(ProbeStage::Autofocus, 1, autofocusOn);
                presenter.setPreviewPaused(false);
                if (!controlAccepted(autofocusOn)) {
                    presenter.setCaptureFeedback(CaptureFeedback::Failure);
                    if (!autofocusOn.transportOk) {
                        report.failedStage = ProbeStage::Autofocus;
                        report.transportError = autofocusOn.transportError;
                        report.operationCode = kOperationSetControlDeviceB;
                        report.responseCode = autofocusOn.responseCode;
                        controlSessionHealthy = false;
                    }
                }
            }
        }

        if (!controlSessionHealthy) return;
        if (events.released && shutterGestureActive) {
            if (events.cancelled) {
                bool released = true;
                if (autofocusStarted) {
                    presenter.setPreviewPaused(true);
                    const OperationResult result = setSonyControl(_port, kPropertyAutoFocus, false);
                    recordControl(ProbeStage::Autofocus, 2, result);
                    presenter.setPreviewPaused(false);
                    released = controlAccepted(result);
                    if (!released) {
                        report.failedStage = ProbeStage::Autofocus;
                        report.transportError = result.transportError;
                        report.operationCode = kOperationSetControlDeviceB;
                        report.responseCode = result.responseCode;
                        controlSessionHealthy = false;
                    }
                }
                shutterGestureActive = false;
                autofocusStarted = false;
                feedbackExpiresAtMs = 0;
                presenter.setCaptureFeedback(released ? CaptureFeedback::Live : CaptureFeedback::Failure);
#ifndef ALPHA_BUDDY_NATIVE
                Serial.printf("{\"event\":\"shutter_cancelled\",\"af_released\":%s}\n",
                              released ? "true" : "false");
#endif
                return;
            }
            presenter.setPreviewPaused(true);
            presenter.setCaptureFeedback(CaptureFeedback::Capturing);
            ++report.shutterAttempts;

            const OperationResult captureOn = setSonyControl(_port,
                                                             kPropertyCapture,
                                                             true);
            recordControl(ProbeStage::Capture, 1, captureOn);
            report.lastShutterResponse = captureOn.responseCode;
            shutterDelay();

            const OperationResult captureOff = setSonyControl(_port,
                                                              kPropertyCapture,
                                                              false);
            recordControl(ProbeStage::Capture, 2, captureOff);
            OperationResult autofocusOff;
            autofocusOff.transportOk = true;
            autofocusOff.responseCode = kResponseOk;
            if (autofocusStarted) {
                autofocusOff = setSonyControl(_port, kPropertyAutoFocus, false);
                recordControl(ProbeStage::Autofocus, 2, autofocusOff);
            }
            autofocusStarted = false;

            presenter.setPreviewPaused(false);
            const bool captureSucceeded = controlAccepted(captureOn) &&
                                          controlAccepted(captureOff) &&
                                          controlAccepted(autofocusOff);
            if (captureSucceeded) {
                ++report.shutterSuccesses;
                presenter.setCaptureFeedback(CaptureFeedback::Success);
            } else {
                ++report.shutterFailures;
                presenter.setCaptureFeedback(CaptureFeedback::Failure);
            }
            if (!controlAccepted(captureOff) || !controlAccepted(autofocusOff)) {
                const OperationResult& cleanupFailure = !controlAccepted(captureOff)
                    ? captureOff
                    : autofocusOff;
                report.failedStage = ProbeStage::Capture;
                report.transportError = cleanupFailure.transportError;
                report.operationCode = kOperationSetControlDeviceB;
                report.responseCode = cleanupFailure.responseCode;
                controlSessionHealthy = false;
            }
            feedbackExpiresAtMs = nowMs() + kCaptureFeedbackMs;
            shutterGestureActive = false;
        }

        if (!shutterGestureActive && feedbackExpiresAtMs != 0 &&
            static_cast<int32_t>(nowMs() - feedbackExpiresAtMs) >= 0) {
            feedbackExpiresAtMs = 0;
            presenter.setCaptureFeedback(CaptureFeedback::Live);
        }
    };

    while (options.durationMs == 0 || nowMs() - streamStartedAt < options.durationMs) {
#ifdef ALPHA_BUDDY_PROPERTY_PROBE
        if (Serial.available() && Serial.read() == 'p') {
            presenter.setPreviewPaused(true);
            const OperationResult snapshot = capturePropertySnapshot(_port);
            presenter.setPreviewPaused(false);
            if (!snapshot.transportOk) {
                report.failedStage = ProbeStage::SonyHandshake;
                report.transportError = snapshot.transportError;
                report.operationCode = 0x9209;
                report.responseCode = snapshot.responseCode;
                break;
            }
        }
#endif
        if (options.readCameraModes && !shutterGestureActive && nowMs() - lastModePoll >= 1000) {
            modeSink.reset();
            const OperationResult result = _port.execute(0x9209, nullptr, 0, &modeSink);
            CameraModes modes;
            const bool valid = result.transportOk && result.responseCode == kResponseOk && modeSink.decode(modes);
            currentFocus = valid ? modes.focus : FocusMode::Unknown;
            presenter.setCameraModes(modes);
            lastModePoll = nowMs();
            if (lastModes != modes.packed() || !valid) {
#ifndef ALPHA_BUDDY_NATIVE
                Serial.printf("{\"event\":\"camera_modes\",\"valid\":%s,\"exposure\":%u,"
                              "\"focus\":%u,\"silent\":%u,\"read_ms\":%lu}\n",
                              valid ? "true" : "false", unsigned(modes.exposure),
                              unsigned(modes.focus), unsigned(modes.silent),
                              static_cast<unsigned long>(result.elapsedMs));
#endif
                lastModes = modes.packed();
            }
            if (!result.transportOk) {
                report.failedStage = ProbeStage::LiveViewRequest;
                report.transportError = result.transportError;
                report.operationCode = 0x9209;
                report.responseCode = result.responseCode;
                break;
            }
        }
        processShutterInput();
        if (!controlSessionHealthy) {
            break;
        }
        frame.reset();
        const uint32_t frameStartedAt = nowMs();
        const uint32_t objectHandle = SonyLiveViewProbe::kLegacyLiveViewObjectHandle;
        OperationResult objectInfoResult;
        bool objectReady = false;
        const uint32_t objectInfoStartedAt = nowMs();
        for (uint8_t readyAttempt = 0;
             readyAttempt < kObjectReadyAttempts;
             ++readyAttempt) {
            objectInfoResult = _port.execute(SonyLiveViewProbe::kOperationGetObjectInfo,
                                             &objectHandle,
                                             1,
                                             nullptr);
            if (objectInfoResult.transportOk &&
                objectInfoResult.responseCode == kResponseOk) {
                objectReady = true;
                break;
            }
            if (!objectInfoResult.transportOk ||
                objectInfoResult.responseCode != kResponseInvalidObjectHandle) {
                break;
            }
#ifndef ALPHA_BUDDY_NATIVE
            delay(kObjectReadyDelayMs);
#endif
        }
        report.objectInfoMs += nowMs() - objectInfoStartedAt;
        if (!objectReady) {
            ++report.frameFailures;
            ++consecutiveFrameFailures;
            report.failedStage = ProbeStage::LiveViewRequest;
            report.transportError = objectInfoResult.transportError;
            report.operationCode = SonyLiveViewProbe::kOperationGetObjectInfo;
            report.responseCode = objectInfoResult.responseCode;
            if (!objectInfoResult.transportOk || consecutiveFrameFailures >= 10) {
                break;
            }
            continue;
        }

        OperationResult result;
        const uint32_t getObjectStartedAt = nowMs();
        for (uint8_t frameAttempt = 0;
             frameAttempt < kFrameAccessAttempts;
             ++frameAttempt) {
            frame.reset();
            result = _port.execute(SonyLiveViewProbe::kOperationGetObject,
                                   &objectHandle,
                                   1,
                                   &frame);
            if (!result.transportOk || result.responseCode != kResponseAccessDenied) {
                break;
            }
#ifndef ALPHA_BUDDY_NATIVE
            delay(kFrameAccessDelayMs);
#endif
        }
        report.getObjectMs += nowMs() - getObjectStartedAt;
        const uint32_t frameMs = nowMs() - frameStartedAt;
        if (frameMs > report.maximumFrameMs) {
            report.maximumFrameMs = frameMs;
        }
        report.operationCode = SonyLiveViewProbe::kOperationGetObject;
        report.responseCode = result.responseCode;
        report.lastJpegBytes = frame.size();

        if (!result.transportOk || result.responseCode != kResponseOk || !frame.validJpeg()) {
            ++report.frameFailures;
            ++consecutiveFrameFailures;
            report.failedStage = frame.validJpeg()
                ? ProbeStage::LiveViewRequest
                : ProbeStage::JpegValidation;
            report.transportError = result.transportOk
                ? TransportError::SinkRejected
                : result.transportError;
            if (!result.transportOk || consecutiveFrameFailures >= 10) {
                break;
            }
            continue;
        }
        const uint32_t presentStartedAt = nowMs();
        const bool presented = presenter.present(frame.data(),
                                                 frame.size(),
                                                 frame.width(),
                                                 frame.height());
        report.presentMs += nowMs() - presentStartedAt;
        if (!presented) {
            ++report.frameFailures;
            report.failedStage = ProbeStage::JpegValidation;
            report.transportError = TransportError::SinkRejected;
            break;
        }
        consecutiveFrameFailures = 0;
        ++report.framesPresented;
#ifndef ALPHA_BUDDY_NATIVE
        ++windowFrames;
        windowBytes += frame.size();
        const uint32_t windowMs = nowMs() - windowStartedAt;
        if (windowMs >= 5000) {
            Serial.printf("{\"event\":\"live_view_perf\",\"frames\":%lu,"
                          "\"window_ms\":%lu,\"fps\":%.2f,\"jpeg_avg_bytes\":%lu,"
                          "\"jpeg_width\":%u,\"jpeg_height\":%u,"
                          "\"object_info_avg_ms\":%.2f,\"get_object_avg_ms\":%.2f,"
                          "\"enqueue_avg_ms\":%.2f,\"frame_failures\":%lu}\n",
                          static_cast<unsigned long>(windowFrames),
                          static_cast<unsigned long>(windowMs),
                          1000.0 * windowFrames / windowMs,
                          static_cast<unsigned long>(windowBytes / windowFrames),
                          static_cast<unsigned>(frame.width()),
                          static_cast<unsigned>(frame.height()),
                          double(report.objectInfoMs - previousInfoMs) / windowFrames,
                          double(report.getObjectMs - previousGetMs) / windowFrames,
                          double(report.presentMs - previousPresentMs) / windowFrames,
                          static_cast<unsigned long>(report.frameFailures));
            windowStartedAt = nowMs();
            windowFrames = 0;
            windowBytes = 0;
            previousInfoMs = report.objectInfoMs;
            previousGetMs = report.getObjectMs;
            previousPresentMs = report.presentMs;
        }
#endif
        processShutterInput();
        if (!controlSessionHealthy) {
            break;
        }
    }

    if (shutterGestureActive && autofocusStarted) {
        const OperationResult autofocusOff = setSonyControl(_port,
                                                            kPropertyAutoFocus,
                                                            false);
        recordControl(ProbeStage::Autofocus, 2, autofocusOff);
        presenter.setCaptureFeedback(CaptureFeedback::Live);
    }

    report.elapsedMs = nowMs() - startedAt;
    report.success = options.durationMs != 0 &&
                     report.framesPresented > 0 &&
                     nowMs() - streamStartedAt >= options.durationMs;
    if (report.success) {
        report.failedStage = ProbeStage::Complete;
        report.transportError = TransportError::None;
    }
    _port.close();
    presenter.setConnection(CameraConnection::Disconnected);
    return report;
}

}  // namespace alpha_buddy
