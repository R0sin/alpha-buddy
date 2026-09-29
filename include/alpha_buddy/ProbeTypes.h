#pragma once

#include <cstddef>
#include <cstdint>

namespace alpha_buddy {

enum class ProbeStage : uint8_t {
    Configuration,
    WifiAssociation,
    SocketConnect,
    InitCommand,
    InitEvent,
    OpenSession,
    SonyHandshake,
    LiveViewRequest,
    Autofocus,
    Capture,
    JpegValidation,
    Complete,
};

enum class ProbeStatus : uint8_t {
    Started,
    Passed,
    Warning,
    Failed,
};

enum class TransportError : uint8_t {
    None,
    SocketConnect,
    SocketClosed,
    Timeout,
    InvalidPacket,
    PacketTooLarge,
    UnexpectedPacket,
    WriteFailed,
    SinkRejected,
};

struct ProbeEvent {
    ProbeStage stage = ProbeStage::Configuration;
    ProbeStatus status = ProbeStatus::Started;
    TransportError transportError = TransportError::None;
    uint8_t attempt = 0;
    uint8_t substep = 0;
    uint16_t operationCode = 0;
    uint16_t responseCode = 0;
    uint32_t elapsedMs = 0;
    size_t bytes = 0;
};

struct PortOpenResult {
    bool ok = false;
    ProbeStage failedStage = ProbeStage::SocketConnect;
    TransportError transportError = TransportError::None;
    uint16_t responseCode = 0;
    uint32_t elapsedMs = 0;
};

struct OperationResult {
    bool transportOk = false;
    TransportError transportError = TransportError::None;
    uint16_t responseCode = 0;
    uint32_t elapsedMs = 0;
    size_t dataBytes = 0;
};

struct ProbeReport {
    bool success = false;
    ProbeStage failedStage = ProbeStage::Configuration;
    TransportError transportError = TransportError::None;
    uint8_t attempts = 0;
    uint8_t warnings = 0;
    uint16_t operationCode = 0;
    uint16_t responseCode = 0;
    uint32_t elapsedMs = 0;
    size_t jpegBytes = 0;
    uint16_t jpegWidth = 0;
    uint16_t jpegHeight = 0;
};

}  // namespace alpha_buddy
