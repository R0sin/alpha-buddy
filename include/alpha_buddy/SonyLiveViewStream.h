#pragma once

#include <cstdint>

#include "alpha_buddy/IFramePresenter.h"
#include "alpha_buddy/IProbeTrace.h"
#include "alpha_buddy/IPtpIpCommandPort.h"
#include "alpha_buddy/IShutterInput.h"
#include "alpha_buddy/JpegFrameSink.h"
#include "alpha_buddy/ProbeTypes.h"

namespace alpha_buddy {

struct StreamOptions {
    uint32_t durationMs = 60000;
    // Request the camera's smaller JPEG once per session; rejection is optional.
    bool preferLowResolution = false;
    bool readCameraModes = false;
};

struct StreamReport {
    bool success = false;
    ProbeStage failedStage = ProbeStage::SocketConnect;
    TransportError transportError = TransportError::None;
    uint8_t warnings = 0;
    uint16_t operationCode = 0;
    uint16_t responseCode = 0;
    uint32_t elapsedMs = 0;
    uint32_t framesPresented = 0;
    uint32_t frameFailures = 0;
    uint32_t maximumFrameMs = 0;
    uint32_t objectInfoMs = 0;
    uint32_t getObjectMs = 0;
    uint32_t presentMs = 0;
    uint32_t maximumDisplayMs = 0;
    uint32_t shutterAttempts = 0;
    uint32_t shutterSuccesses = 0;
    uint32_t shutterFailures = 0;
    uint16_t lastShutterResponse = 0;
    size_t lastJpegBytes = 0;
};

class SonyLiveViewStream {
public:
    SonyLiveViewStream(IPtpIpCommandPort& port, IProbeTrace& trace);

    StreamReport run(const PtpSessionConfig& session,
                     JpegFrameSink& frame,
                     IFramePresenter& presenter,
                     const StreamOptions& options = {},
                     IShutterInput* shutterInput = nullptr);

private:
    IPtpIpCommandPort& _port;
    IProbeTrace& _trace;
};

}  // namespace alpha_buddy
