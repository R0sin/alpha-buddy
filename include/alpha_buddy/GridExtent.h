#pragma once

#include <cmath>
#include <cstdint>

namespace alpha_buddy {

struct GridExtent {
    int32_t first;
    int32_t last;
};

// pushRotateZoom samples pixel centers. For a centered, unrotated image,
// covered coordinates are [center - size * zoom / 2, center + size * zoom / 2).
inline GridExtent gridExtent(float center, int32_t size, float zoom) {
    const float half = size * zoom * 0.5f;
    return {static_cast<int32_t>(std::ceil(center - half)),
            static_cast<int32_t>(std::ceil(center + half)) - 1};
}

}  // namespace alpha_buddy
