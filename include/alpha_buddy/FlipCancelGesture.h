#pragma once

#include "alpha_buddy/LandscapeOrientationTracker.h"

namespace alpha_buddy {

// Called under the input lock: only fresh, continuously opposite samples count.
class FlipCancelGesture {
public:
    void begin(bool resolved, LandscapeOrientation orientation) {
        _baselineValid = resolved;
        _baseline = orientation;
        _timing = false;
        _cancelled = false;
    }

    void observe(bool valid, LandscapeOrientation orientation, uint32_t now) {
        if (_cancelled) return;
        if (!_baselineValid || !valid || orientation == _baseline) {
            _timing = false;
            return;
        }
        if (!_timing || now - _lastSample > 150) {
            _since = now;
            _timing = true;
        }
        _lastSample = now;
        if (now - _since > 1000) _cancelled = true;
    }

    bool cancelled() const { return _cancelled; }
    bool counting(uint32_t now) const {
        return _timing && !_cancelled && now - _lastSample <= 150;
    }

private:
    LandscapeOrientation _baseline = LandscapeOrientation::Rotation1;
    bool _baselineValid = false, _timing = false, _cancelled = false;
    uint32_t _since = 0, _lastSample = 0;
};

} // namespace alpha_buddy
