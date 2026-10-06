#include "build_118144515.h"
#include <cassert>
#include <sstream>
#include <string>
#include <vector>

int main() {
    using namespace Build118144515;
    // Independently parse fixtures and test every entry with two ASLR bases.
    for (uint64_t base : {0x140000000ULL, 0x7FF600000000ULL}) {
        for (const auto& entry : CodeEntries) {
            std::vector<uint8_t> bytes;
            std::vector<bool> wildcards;
            std::istringstream tokens(entry.signature);
            std::string token;
            while (tokens >> token) {
                wildcards.push_back(token == "??");
                bytes.push_back(token == "??" ? 0xA5 :
                    static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
            }
            size_t reads = 0;
            auto reader = [&](uint64_t address, void* out, size_t size) {
                ++reads;
                assert(address == base + entry.rva);
                assert(size == bytes.size());
                std::memcpy(out, bytes.data(), size);
                return true;
            };
            const uint64_t imageSize = entry.rva + bytes.size();
            assert(ValidateCodeEntry(entry, base, imageSize, reader));
            for (size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] ^= 0xFF;
                assert(ValidateCodeEntry(entry, base, imageSize, reader) == wildcards[i]);
                bytes[i] ^= 0xFF;
            }
            reads = 0;
            assert(!ValidateCodeEntry(entry, base, imageSize - 1, reader));
            assert(!ValidateCodeEntry(entry, base, entry.rva, reader));
            assert(!ValidateCodeEntry(entry, 0, imageSize, reader));
            assert(!ValidateCodeEntry(entry, UINT64_MAX - imageSize + 1, imageSize, reader));
            assert(reads == 0); // Invalid ranges must never reach the memory reader.
            assert(!ValidateCodeEntry(entry, base, imageSize,
                [](uint64_t, void*, size_t) { return false; }));
        }
    }
    // Invalid patterns and truncated reads must fail, including all-zero code.
    for (const char* pattern : {"", "0", "GG", "?0", "00-00", "00 "}) {
        assert(!ValidateCodeEntry({"invalid", 0, pattern}, 1, 100,
            [](uint64_t, void* out, size_t size) {
                std::memset(out, 0, size);
                return true;
            }));
    }
    assert(!ValidateCodeEntry({"null", 0, nullptr}, 1, 100,
        [](uint64_t, void*, size_t) { assert(false); return true; }));
    assert(!ValidateCodeEntry({"unreadable", 0, "00"}, 1, 100,
        [](uint64_t, void*, size_t) { return false; }));
}
