#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <utility>

namespace game_signatures {
// Operand offsets are measured from the start of the matching instruction.
// RIP operands resolve to referenced addresses. MOV globals use pointer slots;
// LEA globals use the address directly. Neither encoding dereferences memory.
enum class Encoding { Rip32, RipAddress32, Field32, Field8 };
enum class Field { GameManager, ProfileManager, EntityList, EntityCount, EntityArray, Health, MaxHealth, TeamId, Name, OperatorId, Stance, IsAlive, SkeletonComponent, BoneArray, TransformStride, TransformArray, ReplicationComponent, CurrentWeapon, MovementComponent, VisibilityState, World, Names, Objects, Count };
struct Signature { Field field; const char* name; const char* pattern; size_t operand; Encoding encoding; };
// Preferred verified variants first. EntityList.Array(v4), marked invalid by
// the supplier, is deliberately excluded: LEA does not load an array pointer.
inline constexpr Signature Signatures[] = {
    {Field::GameManager, "GameManager", "48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B D8 48 85 C0", 3, Encoding::Rip32},
    {Field::ProfileManager, "ProfileManager", "48 89 05 ? ? ? ? 48 8D 4F 70", 3, Encoding::Rip32},
    {Field::EntityList, "EntityList", "48 8B 8B ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8", 3, Encoding::Field32},
    {Field::EntityList, "EntityList", "48 8B 89 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? 48", 3, Encoding::Field32},
    {Field::EntityList, "EntityList", "48 8B 8F ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? 84", 3, Encoding::Field32},
    {Field::EntityCount, "EntityCount", "8B 81 ? ? ? ? 85 C0 7E", 2, Encoding::Field32},
    {Field::EntityArray, "EntityArray", "48 8B 81 ? ? ? ? 48 8D 0C C8", 3, Encoding::Field32},
    {Field::EntityArray, "EntityArray", "4C 8B 81 ? ? ? ? 4D 85 C0", 3, Encoding::Field32},
    {Field::EntityArray, "EntityArray", "48 8B 89 ? ? ? ? 48 8D 04 C1", 3, Encoding::Field32},
    {Field::Health, "Health", "F3 0F 10 80 ? ? ? ? F3 0F 5C C1", 4, Encoding::Field32},
    {Field::MaxHealth, "MaxHealth", "8B 80 ? ? ? ? 89 44 24 ? E8", 2, Encoding::Field32},
    {Field::TeamId, "TeamId", "83 B8 ? ? ? ? 00 75 ? 48 8B", 2, Encoding::Field32},
    {Field::TeamId, "TeamId", "8B 90 ? ? ? ? 3B 91", 2, Encoding::Field32},
    {Field::TeamId, "TeamId", "44 8B 80 ? ? ? ? 44 3B 81", 3, Encoding::Field32},
    {Field::Name, "Name", "48 8D 90 ? ? ? ? 48 8B CE E8 ? ? ? ? 84 C0 74", 3, Encoding::Field32},
    {Field::OperatorId, "OperatorId", "44 8B 80 ? ? ? ? 41 83 F8 FF 74", 3, Encoding::Field32},
    {Field::Stance, "Stance", "8B 88 ? ? ? ? 83 F9 03 77", 2, Encoding::Field32},
    {Field::IsAlive, "IsAlive", "80 B8 ? ? ? ? 00 75 ? 32 C0 C3", 2, Encoding::Field32},
    {Field::SkeletonComponent, "SkeletonComponent", "48 8B 88 ? ? ? ? E8 ? ? ? ? 48 85 C0 74 ? 48 8B 48", 3, Encoding::Field32},
    {Field::BoneArray, "BoneArray", "48 8B 81 ? ? ? ? 48 03 C2", 3, Encoding::Field32},
    {Field::TransformStride, "TransformStride", "69 C0 ? ? ? ? 48 03 C1 C3", 2, Encoding::Field32},
    {Field::TransformArray, "TransformArray", "48 8B 41 ? 4C 8D 04 C0", 3, Encoding::Field8},
    {Field::ReplicationComponent, "ReplicationComponent", "48 8B 89 ? ? ? ? 48 85 C9 74 ? 48 8B 01 FF 50", 3, Encoding::Field32},
    {Field::CurrentWeapon, "CurrentWeapon", "48 8B 8F ? ? ? ? 48 85 C9 74 ? 48 8B 01 FF 50", 3, Encoding::Field32},
    {Field::MovementComponent, "MovementComponent", "48 8B 8F ? ? ? ? 48 85 C9 74 ? F3 0F 10", 3, Encoding::Field32},
    {Field::MovementComponent, "MovementComponent", "48 8B 8B ? ? ? ? 48 85 C9 74 ? F3 0F 10", 3, Encoding::Field32},
    // Additional supplied globals: diagnostic only until their game/build and
    // object layouts are confirmed. Do not alias World to GameManager.
    {Field::World, "g_world", "48 8B 15 ?? ?? ?? ?? 48 8D 4F ?? 4C 8B C8", 3, Encoding::Rip32},
    {Field::Names, "g_names", "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? ?? 48 8D 54 24", 3, Encoding::RipAddress32},
    {Field::Objects, "g_objects", "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01", 3, Encoding::RipAddress32},
    {Field::VisibilityState, "VisibilityState", "80 B8 ? ? ? ? 01 74", 2, Encoding::Field32},
};
enum class Status { Missing, Resolved, Ambiguous, Invalid };
struct Result { Status status = Status::Missing; uint64_t value = 0; };
using Layout = std::array<Result, static_cast<size_t>(Field::Count)>;
inline const Result& Get(const Layout& layout, Field field) { return layout[static_cast<size_t>(field)]; }
inline bool Has(const Layout& layout, Field field) { return Get(layout, field).status == Status::Resolved; }

inline std::vector<int> Parse(const char* text) {
    std::vector<int> bytes;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    if (!text) return bytes;
    while (*text) {
        if (*text == ' ') { ++text; continue; }
        if (*text == '?') { ++text; if (*text == '?') ++text; bytes.push_back(-1); }
        else {
            const int high = hex(*text++);
            if (high < 0 || !*text) return {};
            const int low = hex(*text++);
            if (low < 0) return {};
            bytes.push_back(high * 16 + low);
        }
        if (*text && *text != ' ') return {};
    }
    return bytes;
}

inline Result Resolve(const uint8_t* data, size_t size, uint64_t base, const Signature& sig) {
    const auto pattern = Parse(sig.pattern);
    const size_t width = sig.encoding == Encoding::Field8 ? 1 : 4;
    if (pattern.empty() || sig.operand > pattern.size() || width > pattern.size() - sig.operand)
        return {Status::Invalid, 0};
    if (!data || size < pattern.size()) return {};
    Result result;
    for (size_t i = 0; i <= size - pattern.size(); ++i) {
        bool matches = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] >= 0 && data[i+j] != pattern[j]) { matches = false; break; }
        }
        if (!matches) continue;
        int32_t operand = 0;
        if (width == 1) {
            int8_t disp = 0;
            std::memcpy(&disp, data + i + sig.operand, 1);
            operand = disp;
        } else std::memcpy(&operand, data + i + sig.operand, 4);
        uint64_t value = 0;
        if (sig.encoding == Encoding::Rip32 || sig.encoding == Encoding::RipAddress32) {
            const size_t end = i + sig.operand + width;
            if (base > UINT64_MAX - end) return {Status::Invalid, 0};
            value = base + end;
            if (operand < 0) {
                const uint64_t distance = static_cast<uint64_t>(-static_cast<int64_t>(operand));
                if (value < distance) return {Status::Invalid, 0};
                value -= distance;
            } else {
                if (value > UINT64_MAX - static_cast<uint32_t>(operand)) return {Status::Invalid, 0};
                value += operand;
            }
        } else {
            // These layouts use offsets from the object start, not negative fields.
            if (operand < 0) return {Status::Invalid, 0};
            value = static_cast<uint32_t>(operand);
        }
        if (result.status == Status::Resolved && result.value != value)
            return {Status::Ambiguous, 0};
        result = {Status::Resolved, value};
    }
    return result;
}

inline Layout Scan(const uint8_t* data, size_t size, uint64_t base) {
    Layout layout{};
    for (const auto& sig : Signatures) {
        auto& result = layout[static_cast<size_t>(sig.field)];
        // Fall back only when absent; a conflicting preferred match fails closed.
        if (result.status == Status::Missing) result = Resolve(data, size, base, sig);
    }
    return layout;
}

inline bool CanReadEntities(const Layout& layout) {
    return Has(layout, Field::GameManager) && Has(layout, Field::EntityList) &&
        Has(layout, Field::EntityCount) && Has(layout, Field::EntityArray);
}

// Read the supplied manager -> list -> pointer array layout. A successful empty
// list is distinct from a failed read. Never publish a partial or changing list.
template<class Read>
bool ReadEntities(const Layout& layout, Read read, std::vector<uint64_t>& out) {
    out.clear();
    if (!CanReadEntities(layout)) return false;
    const auto pointer = [](uint64_t p) { return p > 0x10000 && p < 0x7FFFFFFFFFFFULL; };
    const auto fieldRead = [&](uint64_t object, Field field, void* data, size_t bytes) {
        const uint64_t offset = Get(layout, field).value;
        return pointer(object) && offset <= 0x100000 && read(object + offset, data, bytes);
    };
    const uint64_t slot = Get(layout, Field::GameManager).value;
    uint64_t manager = 0, list = 0, array = 0;
    int32_t count = 0;
    if (!pointer(slot) || !read(slot, &manager, sizeof(manager)) ||
        !fieldRead(manager, Field::EntityList, &list, sizeof(list)) ||
        !fieldRead(list, Field::EntityCount, &count, sizeof(count)) || count < 0 || count > 512 ||
        !fieldRead(list, Field::EntityArray, &array, sizeof(array))) return false;
    std::vector<uint64_t> entries(static_cast<size_t>(count));
    if (count && (!pointer(array) || !read(array, entries.data(), entries.size() * sizeof(uint64_t)))) return false;
    uint64_t managerAfter = 0, listAfter = 0, arrayAfter = 0;
    int32_t countAfter = 0;
    if (!read(slot, &managerAfter, sizeof(managerAfter)) || managerAfter != manager ||
        !fieldRead(manager, Field::EntityList, &listAfter, sizeof(listAfter)) || listAfter != list ||
        !fieldRead(list, Field::EntityCount, &countAfter, sizeof(countAfter)) || countAfter != count ||
        !fieldRead(list, Field::EntityArray, &arrayAfter, sizeof(arrayAfter)) || arrayAfter != array) return false;
    for (auto entry : entries) if (!pointer(entry)) return false;
    out = std::move(entries);
    return true;
}
} // namespace game_signatures
