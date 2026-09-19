#pragma once
#include <windows.h>
#include <cstdint>
#include <cmath>
#include "driver.h"

struct AimTarget {
    float x, y, z;
    bool valid;
};

extern bool IsValidAddr(uint64_t p);

static AimTarget GetAimPosition(uint64_t entity, float rootX, float rootY, float rootZ, int hitboxSel) {
    AimTarget t = {};
    t.x = rootX;
    t.y = rootY;
    t.valid = true;
    switch (hitboxSel) {
    case 0: t.z = rootZ + 1.6f; break;
    case 1: t.z = rootZ + 1.45f; break;
    case 2: t.z = rootZ + 1.2f; break;
    case 3: t.z = rootZ + 0.85f; break;
    case 4: t.z = rootZ + 0.2f; break;
    default: t.z = rootZ + 1.6f; break;
    }
    return t;
}
