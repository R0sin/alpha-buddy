#pragma once

#include <cstdint>

namespace alpha_buddy {

enum class CameraConnection : uint8_t { Disconnected, Connecting, Connected };
enum class ViewfinderHint : uint8_t { None, Waiting, Stalled };

inline bool canDisplayPreview(CameraConnection connection,
                              uint32_t frameSession,
                              uint32_t currentSession) {
    return connection == CameraConnection::Connected && frameSession == currentSession;
}

inline ViewfinderHint viewfinderHint(CameraConnection connection,
                                     bool previewPaused,
                                     uint32_t nowMs,
                                     uint32_t lastFrameMs,
                                     uint32_t resumedAtMs) {
    if (connection != CameraConnection::Connected) return ViewfinderHint::Waiting;
    // Unsigned subtraction also handles the millis() rollover.
    if (!previewPaused && nowMs - lastFrameMs > 1000 && nowMs - resumedAtMs > 1000) {
        return ViewfinderHint::Stalled;
    }
    return ViewfinderHint::None;
}

}  // namespace alpha_buddy
