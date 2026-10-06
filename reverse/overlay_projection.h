#pragma once

#include <cmath>

namespace overlay_projection {

// Same row-vector convention as the existing view-projection reader.
inline bool ValidMatrix(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    // Reject empty/degenerate output even when the memory read succeeded.
    const auto nonzero = [m](int column) {
        return std::fabs(m[column]) + std::fabs(m[column + 4]) +
            std::fabs(m[column + 8]) + std::fabs(m[column + 12]) > 0.000001f;
    };
    return nonzero(0) && nonzero(1) && nonzero(3);
}

// Projection and viewport inclusion are separate: a visible line/box may have
// endpoints outside the viewport. ImGui clips those primitives when drawing.
template<class Vec>
bool Project(const float* m, const Vec& world, Vec& screen, int width, int height) {
    screen = {};
    if (width <= 0 || height <= 0 || !std::isfinite(world.x) ||
        !std::isfinite(world.y) || !std::isfinite(world.z)) return false;
    const float w = m[3]*world.x + m[7]*world.y + m[11]*world.z + m[15];
    if (!std::isfinite(w) || w < 0.001f) return false;
    const float x = width * 0.5f *
        (world.x*m[0] + world.y*m[4] + world.z*m[8] + m[12]) / w + width*0.5f;
    const float y = -height * 0.5f *
        (world.x*m[1] + world.y*m[5] + world.z*m[9] + m[13]) / w + height*0.5f;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    screen = {x, y, w};
    return true;
}

template<class Vec>
bool Inside(const Vec& screen, int width, int height) {
    return screen.x >= 0 && screen.y >= 0 && screen.x <= width && screen.y <= height;
}

}
