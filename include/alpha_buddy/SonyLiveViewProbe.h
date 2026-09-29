#pragma once

#include <cstdint>

#include "alpha_buddy/IProbeTrace.h"
#include "alpha_buddy/IPtpIpCommandPort.h"
#include "alpha_buddy/JpegFrameSink.h"

namespace alpha_buddy {

struct ProbeOptions {
    uint8_t maximumAttempts = 3;
};

class SonyLiveViewProbe {
public:
    static constexpr uint16_t kOperationGetDeviceInfo = 0x1001;
    static constexpr uint16_t kOperationGetStorageIds = 0x1004;
    static constexpr uint16_t kOperationGetObjectInfo = 0x1008;
    static constexpr uint16_t kOperationGetObject = 0x1009;
    static constexpr uint16_t kOperationSdioConnect = 0x9201;
    static constexpr uint16_t kOperationSdioGetExtendedDeviceInfo = 0x9202;
    static constexpr uint32_t kLegacyLiveViewObjectHandle = 0xFFFFC002;

    SonyLiveViewProbe(IPtpIpCommandPort& port, IProbeTrace& trace);
    ProbeReport run(const PtpSessionConfig& session,
                    JpegFrameSink& frame,
                    const ProbeOptions& options = {});

private:
    void record(ProbeStage stage,
                ProbeStatus status,
                uint8_t attempt,
                uint8_t substep,
                uint16_t operationCode,
                const OperationResult& result,
                size_t bytes = 0);
    static bool isAcceptableHandshakeResponse(uint16_t responseCode);

    IPtpIpCommandPort& _port;
    IProbeTrace& _trace;
};

}  // namespace alpha_buddy
