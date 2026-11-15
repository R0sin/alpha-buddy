#pragma once

#include "alpha_buddy/CameraModes.h"

namespace alpha_buddy {

// Reference policy: alpha-fairy fd1f2989, RemoteShutter.ino.
// D213 interpretation still needs target-camera validation. It only controls
// release timing; it never confirms exposure. Feed only post-submit snapshots.
class SonyCaptureTiming {
public:
    void start(uint32_t now, bool manualFocus) {
        _phase = manualFocus ? Phase::Holding : Phase::Focusing;
        _phaseStarted = now;
    }
    bool active() const { return _phase != Phase::Idle; }
    bool focusing() const { return _phase == Phase::Focusing; }
    void observe(uint32_t now, const CameraModes& fresh) {
        if (focusing() && (fresh.focus == FocusMode::MF || fresh.focusFound == 6 ||
            (fresh.focus == FocusMode::S && fresh.focusFound == 2))) {
            _phase = Phase::Holding;
            _phaseStarted = now;
        }
    }
    bool readyToRelease(uint32_t now) {
        if (focusing() && now - _phaseStarted >= 2000) {
            _phase = Phase::Holding;
            _phaseStarted += 2000;
        }
        return _phase == Phase::Holding && now - _phaseStarted >= 200;
    }
    void reset() { _phase = Phase::Idle; }
private:
    enum class Phase : uint8_t { Idle, Focusing, Holding };
    Phase _phase = Phase::Idle;
    uint32_t _phaseStarted = 0;
};

} // namespace alpha_buddy
