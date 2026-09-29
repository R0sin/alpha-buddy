#include "alpha_buddy/SonyLiveViewProbe.h"

#include <array>

#ifndef ALPHA_BUDDY_NATIVE
#include <Arduino.h>
#endif

namespace alpha_buddy {

namespace {

// Adapted from Alpha-Fairy commit fd1f2989f952a77d01cdb23b788058c39f091c89
// (MIT, Frank Zhao). Alpha-Fairy disabled its unsuccessful preview path; the
// legacy handle was subsequently validated on ILCE-7CM2 firmware 2.01 by
// alpha-buddy Gate A on 2026-08-13.

constexpr uint16_t kResponseOk = 0x2001;
constexpr uint16_t kResponseOperationNotSupported = 0x2005;
constexpr uint16_t kResponseInvalidStorageId = 0x2008;
constexpr uint16_t kResponseStoreNotAvailable = 0x2013;
constexpr uint16_t kResponseCaptureAlreadyTerminated = 0x2018;
constexpr uint16_t kResponseInvalidObjectHandle = 0x2009;
constexpr uint16_t kResponseAccessDenied = 0x200F;
constexpr uint8_t kObjectReadyAttempts = 20;
constexpr uint32_t kObjectReadyDelayMs = 50;
constexpr uint8_t kFrameAccessAttempts = 50;
constexpr uint32_t kFrameAccessDelayMs = 20;

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

}  // namespace

SonyLiveViewProbe::SonyLiveViewProbe(IPtpIpCommandPort& port, IProbeTrace& trace)
    : _port(port), _trace(trace) {}

ProbeReport SonyLiveViewProbe::run(const PtpSessionConfig& session,
                                   JpegFrameSink& frame,
                                   const ProbeOptions& options) {
    ProbeReport report;
    const uint8_t maximumAttempts = options.maximumAttempts == 0 ? 1 : options.maximumAttempts;

    for (uint8_t attempt = 1; attempt <= maximumAttempts; ++attempt) {
        report.attempts = attempt;
        frame.reset();

        ProbeEvent opening;
        opening.stage = ProbeStage::SocketConnect;
        opening.status = ProbeStatus::Started;
        opening.attempt = attempt;
        _trace.record(opening);

        const PortOpenResult openResult = _port.open(session);
        report.elapsedMs += openResult.elapsedMs;
        ProbeEvent opened;
        opened.stage = openResult.ok ? ProbeStage::OpenSession : openResult.failedStage;
        opened.status = openResult.ok ? ProbeStatus::Passed : ProbeStatus::Failed;
        opened.attempt = attempt;
        opened.transportError = openResult.transportError;
        opened.responseCode = openResult.responseCode;
        opened.elapsedMs = openResult.elapsedMs;
        _trace.record(opened);

        if (!openResult.ok) {
            report.failedStage = openResult.failedStage;
            report.transportError = openResult.transportError;
            report.responseCode = openResult.responseCode;
            _port.close();
            continue;
        }

        bool transportFailed = false;
        for (size_t index = 0; index < kHandshake.size(); ++index) {
            const auto& step = kHandshake[index];
            const OperationResult result = _port.execute(step.operationCode,
                                                         step.parameters.data(),
                                                         step.parameterCount,
                                                         nullptr);
            report.elapsedMs += result.elapsedMs;
            report.operationCode = step.operationCode;
            report.responseCode = result.responseCode;
            if (!result.transportOk) {
                record(ProbeStage::SonyHandshake,
                       ProbeStatus::Failed,
                       attempt,
                       static_cast<uint8_t>(index),
                       step.operationCode,
                       result);
                report.failedStage = ProbeStage::SonyHandshake;
                report.transportError = result.transportError;
                transportFailed = true;
                break;
            }

            const bool accepted = isAcceptableHandshakeResponse(result.responseCode);
            if (!accepted || result.responseCode != kResponseOk) {
                ++report.warnings;
            }
            record(ProbeStage::SonyHandshake,
                   accepted ? (result.responseCode == kResponseOk
                                   ? ProbeStatus::Passed
                                   : ProbeStatus::Warning)
                            : ProbeStatus::Warning,
                   attempt,
                   static_cast<uint8_t>(index),
                   step.operationCode,
                   result);
        }

        if (transportFailed) {
            _port.close();
            continue;
        }

        const uint32_t objectHandle = kLegacyLiveViewObjectHandle;
        OperationResult objectInfoResult;
        bool objectReady = false;
        for (uint8_t readyAttempt = 0;
             readyAttempt < kObjectReadyAttempts;
             ++readyAttempt) {
            objectInfoResult = _port.execute(kOperationGetObjectInfo,
                                             &objectHandle,
                                             1,
                                             nullptr);
            report.elapsedMs += objectInfoResult.elapsedMs;
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
        if (!objectReady) {
            record(ProbeStage::LiveViewRequest,
                   ProbeStatus::Failed,
                   attempt,
                   0,
                   kOperationGetObjectInfo,
                   objectInfoResult);
            report.failedStage = ProbeStage::LiveViewRequest;
            report.transportError = objectInfoResult.transportError;
            report.operationCode = kOperationGetObjectInfo;
            report.responseCode = objectInfoResult.responseCode;
            _port.close();
            continue;
        }

        OperationResult frameResult;
        for (uint8_t frameAttempt = 0;
             frameAttempt < kFrameAccessAttempts;
             ++frameAttempt) {
            frame.reset();
            frameResult = _port.execute(kOperationGetObject,
                                        &objectHandle,
                                        1,
                                        &frame);
            report.elapsedMs += frameResult.elapsedMs;
            if (!frameResult.transportOk ||
                frameResult.responseCode != kResponseAccessDenied) {
                break;
            }
#ifndef ALPHA_BUDDY_NATIVE
            delay(kFrameAccessDelayMs);
#endif
        }
        report.operationCode = kOperationGetObject;
        report.responseCode = frameResult.responseCode;
        report.jpegBytes = frame.size();
        report.jpegWidth = frame.width();
        report.jpegHeight = frame.height();

        if (!frameResult.transportOk || frameResult.responseCode != kResponseOk) {
            record(ProbeStage::LiveViewRequest,
                   ProbeStatus::Failed,
                   attempt,
                   0,
                   kOperationGetObject,
                   frameResult,
                   frame.size());
            report.failedStage = ProbeStage::LiveViewRequest;
            report.transportError = frameResult.transportError;
            _port.close();
            continue;
        }

        record(ProbeStage::LiveViewRequest,
               ProbeStatus::Passed,
               attempt,
               0,
               kOperationGetObject,
               frameResult,
               frame.size());

        if (!frame.validJpeg()) {
            record(ProbeStage::JpegValidation,
                   ProbeStatus::Failed,
                   attempt,
                   0,
                   kOperationGetObject,
                   frameResult,
                   frame.size());
            report.failedStage = ProbeStage::JpegValidation;
            report.transportError = TransportError::SinkRejected;
            _port.close();
            continue;
        }

        record(ProbeStage::JpegValidation,
               ProbeStatus::Passed,
               attempt,
               0,
               kOperationGetObject,
               frameResult,
               frame.size());
        ProbeEvent completed;
        completed.stage = ProbeStage::Complete;
        completed.status = ProbeStatus::Passed;
        completed.attempt = attempt;
        completed.bytes = frame.size();
        completed.elapsedMs = report.elapsedMs;
        _trace.record(completed);

        report.success = true;
        report.failedStage = ProbeStage::Complete;
        report.transportError = TransportError::None;
        report.jpegWidth = frame.width();
        report.jpegHeight = frame.height();
        _port.close();
        return report;
    }

    return report;
}

void SonyLiveViewProbe::record(ProbeStage stage,
                               ProbeStatus status,
                               uint8_t attempt,
                               uint8_t substep,
                               uint16_t operationCode,
                               const OperationResult& result,
                               size_t bytes) {
    ProbeEvent event;
    event.stage = stage;
    event.status = status;
    event.transportError = result.transportError;
    event.attempt = attempt;
    event.substep = substep;
    event.operationCode = operationCode;
    event.responseCode = result.responseCode;
    event.elapsedMs = result.elapsedMs;
    event.bytes = bytes == 0 ? result.dataBytes : bytes;
    _trace.record(event);
}

bool SonyLiveViewProbe::isAcceptableHandshakeResponse(uint16_t responseCode) {
    return responseCode == kResponseOk ||
           responseCode == kResponseOperationNotSupported ||
           responseCode == kResponseInvalidStorageId ||
           responseCode == kResponseStoreNotAvailable ||
           responseCode == kResponseCaptureAlreadyTerminated;
}

}  // namespace alpha_buddy
