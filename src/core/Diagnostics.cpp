#include "alpha_buddy/Diagnostics.h"

#include <cstdio>

namespace alpha_buddy {

const char* probeStageName(ProbeStage stage) {
    switch (stage) {
        case ProbeStage::Configuration: return "configuration";
        case ProbeStage::WifiAssociation: return "wifi_association";
        case ProbeStage::SocketConnect: return "socket_connect";
        case ProbeStage::InitCommand: return "init_command";
        case ProbeStage::InitEvent: return "init_event";
        case ProbeStage::OpenSession: return "open_session";
        case ProbeStage::SonyHandshake: return "sony_handshake";
        case ProbeStage::LiveViewRequest: return "live_view_request";
        case ProbeStage::Autofocus: return "autofocus";
        case ProbeStage::Capture: return "capture";
        case ProbeStage::JpegValidation: return "jpeg_validation";
        case ProbeStage::Complete: return "complete";
    }
    return "unknown";
}

const char* probeStatusName(ProbeStatus status) {
    switch (status) {
        case ProbeStatus::Started: return "started";
        case ProbeStatus::Passed: return "passed";
        case ProbeStatus::Warning: return "warning";
        case ProbeStatus::Failed: return "failed";
    }
    return "unknown";
}

const char* transportErrorName(TransportError error) {
    switch (error) {
        case TransportError::None: return "none";
        case TransportError::SocketConnect: return "socket_connect";
        case TransportError::SocketClosed: return "socket_closed";
        case TransportError::Timeout: return "timeout";
        case TransportError::InvalidPacket: return "invalid_packet";
        case TransportError::PacketTooLarge: return "packet_too_large";
        case TransportError::UnexpectedPacket: return "unexpected_packet";
        case TransportError::WriteFailed: return "write_failed";
        case TransportError::SinkRejected: return "sink_rejected";
    }
    return "unknown";
}

bool formatProbeEvent(const ProbeEvent& event, char* output, size_t capacity) {
    if (output == nullptr || capacity == 0) {
        return false;
    }
    const int written = std::snprintf(
        output,
        capacity,
        "{\"event\":\"gate_a\",\"stage\":\"%s\",\"status\":\"%s\","
        "\"attempt\":%u,\"substep\":%u,\"transport_error\":\"%s\","
        "\"opcode\":\"0x%04X\",\"response\":\"0x%04X\",\"bytes\":%lu,"
        "\"elapsed_ms\":%lu}",
        probeStageName(event.stage),
        probeStatusName(event.status),
        static_cast<unsigned>(event.attempt),
        static_cast<unsigned>(event.substep),
        transportErrorName(event.transportError),
        static_cast<unsigned>(event.operationCode),
        static_cast<unsigned>(event.responseCode),
        static_cast<unsigned long>(event.bytes),
        static_cast<unsigned long>(event.elapsedMs));
    return written >= 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace alpha_buddy
