#include "overlay_projection.h"
#include <cassert>
#include <limits>

struct Vec3 { float x, y, z; };

int main() {
    // Camera looks along +Y with Z up; depth comes from Y, not world Z.
    const float matrix[16] = {1,0,0,0, 0,0,0,1, 0,1,0,0, 0,0,1,0};
    assert(overlay_projection::ValidMatrix(matrix));
    Vec3 screen{};
    assert(overlay_projection::Project(matrix, Vec3{0,10,0}, screen, 1920,1080));
    assert(screen.x == 960 && screen.y == 540 && screen.z == 10);
    assert(overlay_projection::Inside(screen,1920,1080));

    // A player can have feet below the viewport while the head is visible.
    Vec3 feet{}, head{};
    assert(overlay_projection::Project(matrix, Vec3{0,1,-1.2f}, feet,1920,1080));
    assert(overlay_projection::Project(matrix, Vec3{0,1,0.52f}, head,1920,1080));
    assert(!overlay_projection::Inside(feet,1920,1080));
    assert(overlay_projection::Inside(head,1920,1080));
    assert(feet.y > 1080 && head.y < 1080 && feet.y > head.y);

    // Both bone endpoints may be outside, while the segment crosses the view.
    Vec3 left{}, right{};
    assert(overlay_projection::Project(matrix, Vec3{-2,1,0}, left,1920,1080));
    assert(overlay_projection::Project(matrix, Vec3{2,1,0}, right,1920,1080));
    assert(left.x < 0 && right.x > 1920 && left.y == right.y);

    assert(!overlay_projection::Project(matrix, Vec3{0,-1,0}, screen,1920,1080));
    assert(!overlay_projection::Project(matrix, Vec3{0,0,0}, screen,1920,1080));
    assert(!overlay_projection::Project(matrix, Vec3{0,1,0}, screen,0,1080));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    assert(!overlay_projection::Project(matrix, Vec3{nan,1,0}, screen,1920,1080));
    float invalid[16]{};
    assert(!overlay_projection::ValidMatrix(invalid));
    invalid[0] = nan;
    assert(!overlay_projection::ValidMatrix(invalid));
    invalid[0] = std::numeric_limits<float>::infinity();
    assert(!overlay_projection::ValidMatrix(invalid));
    assert(!overlay_projection::Project(invalid, Vec3{1,1,1}, screen,1920,1080));

    // A translated camera catches transposed-matrix regressions.
    float translated[16];
    for (int i=0; i<16; ++i) translated[i] = matrix[i];
    translated[12] = -4;
    translated[13] = -2;
    translated[15] = -5;
    assert(overlay_projection::Project(translated, Vec3{4,15,2}, screen,800,600));
    assert(screen.x == 400 && screen.y == 300 && screen.z == 10);
}
