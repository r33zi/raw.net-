#pragma once

#include <cstddef>
#include <cstdint>

// Supplied class getter at RVA 0x569DE0. Build compatibility still requires
// live verification. A decoded ID alone does not identify a player class.
namespace class_manager {
constexpr uint32_t CLASS_DESC_OFFSET = 0x8;
constexpr uint32_t RAW_FIELD_OFFSET = 0x1C;
constexpr uint32_t IMM_SUB = 0x826F3CF6;
constexpr uint32_t IMM_XOR = 0xCB2FD80B;

inline bool ValidRange(uint64_t address, size_t size) {
    constexpr uint64_t limit = 0x7FFFFFFFFFFFULL;
    return address > 0x10000 && address < limit && size <= limit - address;
}

// Read returns true only for a complete target-process read. Clear the output
// on failure; zero is a possible decoded ID, not a read-failure sentinel.
template <typename Read>
bool TryGetClassId(uint64_t entity, uint32_t& id, Read read) {
    id = 0;
    uint64_t vtable = 0, firstEntry = 0, descriptor = 0;
    uint32_t raw = 0;
    if (!ValidRange(entity, sizeof(vtable)) ||
        !read(entity, &vtable, sizeof(vtable)) ||
        !ValidRange(vtable, sizeof(firstEntry)) ||
        !read(vtable, &firstEntry, sizeof(firstEntry)) ||
        !ValidRange(firstEntry, CLASS_DESC_OFFSET + sizeof(descriptor)) ||
        !read(firstEntry + CLASS_DESC_OFFSET, &descriptor, sizeof(descriptor)) ||
        !ValidRange(descriptor, RAW_FIELD_OFFSET + sizeof(raw)) ||
        !read(descriptor + RAW_FIELD_OFFSET, &raw, sizeof(raw)))
        return false;
    id = static_cast<uint32_t>(raw - IMM_SUB) ^ IMM_XOR;
    return true;
}
} // namespace class_manager
