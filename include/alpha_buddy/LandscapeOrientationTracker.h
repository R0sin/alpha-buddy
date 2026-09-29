#pragma once

#include <cstdint>

namespace alpha_buddy {

enum class LandscapeOrientation : uint8_t {
    Rotation1,
    Rotation3,
};

struct LandscapeGravity {
    float verticalG;
    float horizontalG;
};

struct HudRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct HudPoint {
    int32_t x;
    int32_t y;
};

// Coordinates are local to the rotated display. setRotation(1/3) already
// rotates the HUD with the preview; a local reflection would mirror the icons.
inline HudRect orientHudRect(LandscapeOrientation,
                             int32_t,
                             int32_t,
                             HudRect rect) {
    return rect;
}

inline HudPoint orientHudPoint(LandscapeOrientation,
                               int32_t,
                               int32_t,
                               HudPoint point) {
    return point;
}

inline LandscapeGravity stickS3LandscapeGravity(float imuX, float imuY) {
    LandscapeGravity gravity;
    gravity.verticalG = imuY;
    gravity.horizontalG = -imuX;
    return gravity;
}

class LandscapeOrientationTracker {
public:
    LandscapeOrientation update(float verticalG,
                                float horizontalG,
                                uint32_t nowMs,
                                bool locked) {
        if (!_filterReady) {
            _filteredVerticalG = verticalG;
            _filteredHorizontalG = horizontalG;
            _filterReady = true;
        } else {
            _filteredVerticalG += kFilterAlpha * (verticalG - _filteredVerticalG);
            _filteredHorizontalG += kFilterAlpha * (horizontalG - _filteredHorizontalG);
        }

        if (locked) {
            _candidateReady = false;
            return _orientation;
        }

        LandscapeOrientation observed;
        if (!classify(observed)) {
            _candidateReady = false;
            return _orientation;
        }
        if (!_resolved) {
            _orientation = observed;
            _resolved = true;
            _lastCommitMs = nowMs;
            return _orientation;
        }
        if (observed == _orientation) {
            _candidateReady = false;
            return _orientation;
        }
        if (!_candidateReady || observed != _candidate) {
            _candidate = observed;
            _candidateSinceMs = nowMs;
            _candidateReady = true;
            return _orientation;
        }
        if (nowMs - _candidateSinceMs >= kStableMs &&
            nowMs - _lastCommitMs >= kMinimumHoldMs) {
            _orientation = observed;
            _lastCommitMs = nowMs;
            _candidateReady = false;
        }
        return _orientation;
    }

    LandscapeOrientation orientation() const { return _orientation; }
    bool resolved() const { return _resolved; }
    bool observed(LandscapeOrientation& result) const {
        return _filterReady && classify(result);
    }

private:
    bool classify(LandscapeOrientation& result) const {
        const float verticalMagnitude =
            _filteredVerticalG < 0.0f ? -_filteredVerticalG : _filteredVerticalG;
        const float horizontalMagnitude =
            _filteredHorizontalG < 0.0f ? -_filteredHorizontalG : _filteredHorizontalG;
        if (verticalMagnitude < kMinimumGravityG ||
            verticalMagnitude < horizontalMagnitude + kAxisAdvantageG) {
            return false;
        }
        result = _filteredVerticalG >= 0.0f
            ? LandscapeOrientation::Rotation1
            : LandscapeOrientation::Rotation3;
        return true;
    }

    static constexpr float kFilterAlpha = 0.20f;
    static constexpr float kMinimumGravityG = 0.55f;
    static constexpr float kAxisAdvantageG = 0.18f;
    static constexpr uint32_t kStableMs = 200;
    static constexpr uint32_t kMinimumHoldMs = 200;

    float _filteredVerticalG = 0.0f;
    float _filteredHorizontalG = 0.0f;
    LandscapeOrientation _orientation = LandscapeOrientation::Rotation1;
    LandscapeOrientation _candidate = LandscapeOrientation::Rotation1;
    uint32_t _candidateSinceMs = 0;
    uint32_t _lastCommitMs = 0;
    bool _filterReady = false;
    bool _resolved = false;
    bool _candidateReady = false;
};

}  // namespace alpha_buddy
