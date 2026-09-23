#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace framepos {

struct Vec3 { float x, y, z; };
struct Match { uint32_t offset; Vec3 position; };

inline bool Valid(Vec3 pos) {
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z) &&
        std::fabs(pos.x) < 20000.f && std::fabs(pos.y) < 20000.f &&
        std::fabs(pos.z) < 20000.f &&
        std::fabs(pos.x) + std::fabs(pos.y) + std::fabs(pos.z) > 0.001f;
}

inline std::vector<Match> FindPairs(const uint8_t* block, size_t size) {
    std::vector<Match> matches;
    constexpr size_t stride = 0x850;
    if (!block || size < stride + 20) return matches;
    for (size_t off = 8; off + stride + sizeof(Vec3) <= size; off += 4) {
        float leadOne, leadZero;
        std::memcpy(&leadOne, block + off - 8, sizeof(float));
        std::memcpy(&leadZero, block + off - 4, sizeof(float));
        if (!std::isfinite(leadOne) || !std::isfinite(leadZero) ||
            std::fabs(leadOne - 1.f) > 0.001f || std::fabs(leadZero) > 0.001f)
            continue;
        Vec3 position, copy;
        std::memcpy(&position, block + off, sizeof(position));
        std::memcpy(&copy, block + off + stride, sizeof(copy));
        if (!Valid(position) || !Valid(copy) ||
            std::fabs(position.x - copy.x) > 2.f ||
            std::fabs(position.y - copy.y) > 2.f ||
            std::fabs(position.z - copy.z) > 2.f) continue;
        matches.push_back({static_cast<uint32_t>(off), position});
    }
    return matches;
}

class Tracker {
    struct Sample {
        Vec3 position{};
        uint64_t unchangedSince = 0;
        bool initialized = false;
    };
    struct Component {
        std::unordered_map<uint32_t, Sample> samples;
        std::unordered_set<uint32_t> rejected;
        uint32_t preferred = 0;
    };
    std::unordered_map<uint64_t, Component> components;

public:
    bool Choose(uint64_t component, const std::vector<Match>& matches,
                uint64_t nowMs, Vec3& result) {
        auto& state = components[component];
        bool anotherMoved = false;
        for (const auto& match : matches) {
            auto& sample = state.samples[match.offset];
            const float delta = std::fabs(match.position.x - sample.position.x) +
                std::fabs(match.position.y - sample.position.y) +
                std::fabs(match.position.z - sample.position.z);
            if (!sample.initialized || delta > 0.01f) {
                anotherMoved |= sample.initialized && delta > 0.01f &&
                    !state.rejected.count(match.offset);
                sample.position = match.position;
                sample.unchangedSince = nowMs;
                sample.initialized = true;
            }
        }
        if (anotherMoved) {
            for (const auto& match : matches) {
                auto& sample = state.samples[match.offset];
                if (nowMs > sample.unchangedSince + 3000)
                    state.rejected.insert(match.offset);
            }
        }
        const Match* selected = nullptr;
        for (const auto& match : matches) {
            if (state.rejected.count(match.offset)) continue;
            if (!selected || match.offset == state.preferred ||
                (state.samples[match.offset].unchangedSince >
                 state.samples[selected->offset].unchangedSince))
                selected = &match;
            if (match.offset == state.preferred &&
                nowMs - state.samples[match.offset].unchangedSince < 3000) break;
        }
        if (!selected) return false;
        state.preferred = selected->offset;
        result = selected->position;
        return true;
    }

    void Clear() { components.clear(); }
};

}
