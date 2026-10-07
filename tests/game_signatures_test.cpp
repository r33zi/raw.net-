#include "game_signatures.h"
#include <cassert>
#include <map>
#include <limits>
using namespace game_signatures;

struct Memory {
    std::map<uint64_t, uint8_t> bytes;
    bool changeCount = false;
    template<class T> void Put(uint64_t address, T value) {
        const auto* data = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i) bytes[address+i] = data[i];
    }
    bool Read(uint64_t address, void* out, size_t size) {
        for (size_t i = 0; i < size; ++i) if (!bytes.count(address+i)) return false;
        for (size_t i = 0; i < size; ++i) static_cast<uint8_t*>(out)[i] = bytes.at(address+i);
        if (changeCount && address == 0x400000) Put(0x300020, int32_t{1});
        return true;
    }
};

int main() {
    const uint8_t manager[] = {0x48,0x8B,0x0D,0xF9,0xFF,0xFF,0xFF,
        0xE8,0,0,0,0,0x48,0x8B,0xD8,0x48,0x85,0xC0};
    auto layout = Scan(manager, sizeof(manager), 0x100000);
    assert(Get(layout, Field::GameManager).value == 0x100000); // signed -7
    assert(Has(layout, Field::GameManager));
    assert(!CanReadEntities(layout));
    assert(Resolve(manager, sizeof(manager), 3, Signatures[0]).value == 3);
    assert(Resolve(manager, sizeof(manager), UINT64_MAX, Signatures[0]).status == Status::Invalid);
    assert(Resolve(manager, sizeof(manager)-1, 0x100000, Signatures[0]).status == Status::Missing);
    auto positive = std::vector<uint8_t>(manager, manager + sizeof(manager));
    positive[3] = 0x20; positive[4] = positive[5] = positive[6] = 0;
    assert(Get(Scan(positive.data(), positive.size(), 0x100000), Field::GameManager).value == 0x100027);
    auto duplicate = positive;
    duplicate.insert(duplicate.end(), positive.begin(), positive.end());
    assert(Get(Scan(duplicate.data(), duplicate.size(), 0x100000), Field::GameManager).status == Status::Ambiguous);
    positive[3] = 0xF8; positive[4] = positive[5] = positive[6] = 0xFF;
    assert(Resolve(positive.data(), positive.size(), 0, Signatures[0]).status == Status::Invalid);

    // Global addresses come from the first instruction end, not the CALLs
    // later in the pattern. LEA globals must not be dereferenced by the scanner.
    const uint8_t world[] = {0x48,0x8B,0x15,0x20,0,0,0,0x48,0x8D,0x4F,0x70,0x4C,0x8B,0xC8};
    const uint8_t names[] = {0x48,0x8D,0x0D,0xF9,0xFF,0xFF,0xFF,
        0xE8,0x10,0,0,0,0xC6,0x05,0x20,0,0,0,1,0x48,0x8D,0x54,0x24};
    const uint8_t objects[] = {0x48,0x8D,0x0D,0x40,0,0,0,
        0xE8,0x10,0,0,0,0xE8,0x20,0,0,0,0xE8,0x30,0,0,0,0xC6,0x05,0,0,0,0,1};
    const auto worldLayout = Scan(world, sizeof(world), 0x200000);
    const auto namesLayout = Scan(names, sizeof(names), 0x200000);
    const auto objectsLayout = Scan(objects, sizeof(objects), 0x200000);
    assert(Has(worldLayout, Field::World) && Get(worldLayout, Field::World).value == 0x200027);
    assert(Has(namesLayout, Field::Names) && Get(namesLayout, Field::Names).value == 0x200000);
    assert(Has(objectsLayout, Field::Objects) && Get(objectsLayout, Field::Objects).value == 0x200047);
    assert(!Has(worldLayout, Field::GameManager) && !CanReadEntities(worldLayout));
    assert(!Has(Scan(objects, sizeof(objects)-1, 0x200000), Field::Objects));

    // disp8 must not include the following 4C 8D 04 instruction bytes.
    const uint8_t transform[] = {0x48,0x8B,0x41,0x78,0x4C,0x8D,0x04,0xC0};
    assert(Get(Scan(transform, sizeof(transform), 0), Field::TransformArray).value == 0x78);
    const uint8_t team[] = {0x83,0xB8,0x44,0,0,0,0,0x75,2,0x48,0x8B};
    assert(Get(Scan(team, sizeof(team), 0), Field::TeamId).value == 0x44);
    const uint8_t invalidArray[] = {0x4C,0x8D,0x89,0x10,0,0,0};
    assert(!Has(Scan(invalidArray, sizeof(invalidArray), 0), Field::EntityArray));
    const uint8_t fallbackList[] = {0x48,0x8B,0x8F,0x18,0,0,0,0x48,0x85,0xC9,
        0x0F,0x84,0,0,0,0,0xE8,0,0,0,0,0x84};
    const uint8_t preferredList[] = {0x48,0x8B,0x8B,0x28,0,0,0,0x48,0x85,0xC9,
        0x0F,0x84,0,0,0,0,0xE8};
    assert(Get(Scan(fallbackList, sizeof(fallbackList), 0), Field::EntityList).value == 0x18);
    std::vector<uint8_t> variants(fallbackList, fallbackList + sizeof(fallbackList));
    variants.insert(variants.end(), preferredList, preferredList + sizeof(preferredList));
    assert(Get(Scan(variants.data(), variants.size(), 0), Field::EntityList).value == 0x28);
    variants.insert(variants.end(), preferredList, preferredList + sizeof(preferredList));
    assert(Get(Scan(variants.data(), variants.size(), 0), Field::EntityList).status == Status::Resolved);
    variants[variants.size()-sizeof(preferredList)+3] = 0x38;
    assert(Get(Scan(variants.data(), variants.size(), 0), Field::EntityList).status == Status::Ambiguous);
    const Signature malformed{Field::Health, "bad", "48 QQ", 0, Encoding::Field32};
    assert(Resolve(team, sizeof(team), 0, malformed).status == Status::Invalid);
    assert(Parse("?").size() == 1 && Parse("??").size() == 1 && Parse("4").empty());

    layout = {};
    layout[static_cast<size_t>(Field::GameManager)] = {Status::Resolved, 0x100000};
    layout[static_cast<size_t>(Field::EntityList)] = {Status::Resolved, 0x18};
    layout[static_cast<size_t>(Field::EntityCount)] = {Status::Resolved, 0x20};
    layout[static_cast<size_t>(Field::EntityArray)] = {Status::Resolved, 0x28};
    Memory memory;
    memory.Put(0x100000, uint64_t{0x200000});
    memory.Put(0x200018, uint64_t{0x300000});
    memory.Put(0x300020, int32_t{2});
    memory.Put(0x300028, uint64_t{0x400000});
    memory.Put(0x400000, uint64_t{0x500000});
    memory.Put(0x400008, uint64_t{0x600000});
    auto read = [&](uint64_t address, void* out, size_t bytes) { return memory.Read(address, out, bytes); };
    std::vector<uint64_t> actors;
    assert(ReadEntities(layout, read, actors));
    assert((actors == std::vector<uint64_t>{0x500000,0x600000}));
    memory.changeCount = true;
    assert(!ReadEntities(layout, read, actors) && actors.empty());
    memory.changeCount = false;
    memory.Put(0x300020, int32_t{513});
    assert(!ReadEntities(layout, read, actors));
    memory.Put(0x300020, int32_t{-1});
    assert(!ReadEntities(layout, read, actors));
    memory.Put(0x300020, int32_t{0});
    memory.Put(0x300028, uint64_t{0});
    assert(ReadEntities(layout, read, actors) && actors.empty());
    memory.Put(0x300020, int32_t{2});
    memory.Put(0x300028, uint64_t{0x400000});
    memory.bytes.erase(0x400008);
    assert(!ReadEntities(layout, read, actors) && actors.empty());
    memory.Put(0x400008, uint64_t{0});
    assert(!ReadEntities(layout, read, actors));
    memory.Put(0x100000, uint64_t{0});
    assert(!ReadEntities(layout, read, actors));
}
