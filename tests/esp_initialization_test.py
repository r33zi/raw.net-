"""Compile production initialization with synthetic discovery/allocation results.

This covers merge-sensitive startup decisions without a Windows SDK or driver.
It does not verify the live signature matches or the full application build.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(signature):
    source = (ROOT / "reverse/r6_entities.h").read_text()
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = r'''
#include "game_signatures.h"
#include "offsets.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_set>
using DWORD = uint32_t;
using LPVOID = void*;
constexpr int MEM_COMMIT = 1, MEM_RESERVE = 2, PAGE_READWRITE = 4;
static bool g_pipelineReady = false;
static const char* g_pipelineError = nullptr;
static uint64_t g_frameSyncAddr = 0, g_imageBase = 0, g_imageSize = 0;
static uint64_t g_ShellPage = 0, g_RingAddr = 0, g_pViewDataPtr = 0, g_projectionAddr = 0;
static void* g_registryScanThread = nullptr;
static game_signatures::Layout g_gameLayout{};
static const char* g_entityScanError = nullptr;
static bool headersValid = true, cacheValid = true;
struct CallTarget { uint64_t targetVA; bool hasTestAlAl; };
static std::vector<CallTarget> calls;
struct Driver {
    uint64_t allocation = 0x400000;
    int attempts = 0;
    uint64_t AllocMemory(size_t, int, int) { ++attempts; return allocation; }
} memory;
static Driver* driver = &memory;
struct { uint64_t skelFuncVA, headHashRefVA, neckHashRefVA; unsigned pairCount; } g_SkelXref{};
static void ValidateBuild118144515Entries(uint64_t, uint64_t) {}
static std::vector<int> GetPESections(uint64_t) { return headersValid ? std::vector<int>{1} : std::vector<int>{}; }
static bool CacheTextSection(uint64_t, const std::vector<int>&) { return cacheValid; }
static void ScanConfiguredPointers(uint64_t) {}
static void ScanGameLayout() {}
static std::vector<CallTarget> FindEntityFunctionCalls(uint64_t) { return calls; }
static void FindRound() {}
static bool IsValidAddr(uint64_t value) { return value > 0x10000; }
template<class T> T read(uint64_t) { return {}; }
static void RefreshConfiguredProjection() {}
static uint64_t ScanForViewTrans(uint64_t, uint64_t) { return 0; }
static bool ScanSkelXref(uint64_t) { return false; }
static bool ReadSkeletonRotation(uint64_t, float*) { return false; }
static bool ReadSkeletonAnchor(uint64_t, void*) { return false; }
namespace skel {
static bool (*g_readRotQuatFn)(uint64_t, float*) = nullptr;
static bool (*g_readPhysPosFn)(uint64_t, void*) = nullptr;
static void ScanRegistry() {}
}
static void* CreateThread(void*, size_t, DWORD (*)(LPVOID), void*, DWORD, void*) { return nullptr; }
'''
source += function("static bool FailRenderPipeline(") + "\n"
source += function("static bool InitRenderPipeline(") + "\n"
source += r'''
static void Reset(bool manager) {
    headersValid = cacheValid = true;
    g_pipelineReady = true; // Initialization must clear stale success on failure.
    g_pipelineError = "previous failure";
    g_frameSyncAddr = g_ShellPage = g_RingAddr = 0;
    g_pViewDataPtr = g_projectionAddr = 0;
    memory.allocation = 0x400000;
    memory.attempts = 0;
    calls.clear();
    g_entityScanError = nullptr;
    g_gameLayout = {};
    if (manager) {
        using namespace game_signatures;
        for (auto field : {Field::GameManager, Field::EntityList, Field::EntityCount, Field::EntityArray})
            g_gameLayout[static_cast<size_t>(field)] = {Status::Resolved, 0x200000};
    }
}
static void Failed(const char* reason) {
    assert(!InitRenderPipeline(0x10000000, 0x1000));
    assert(!g_pipelineReady && g_pipelineError && std::strcmp(g_pipelineError, reason) == 0);
}
static void Ready() {
    assert(InitRenderPipeline(0x10000000, 0x1000));
    assert(g_pipelineReady && g_pipelineError == nullptr);
}
int main() {
    Reset(true);
    headersValid = false;
    Failed("PE headers unreadable or invalid");
    assert(memory.attempts == 0);
    Reset(true);
    cacheValid = false;
    Failed("code section missing or unreadable; see [SCAN] log");
    for (const char* reason : {"entity anchor not found; signature update required",
            "entity anchor ambiguous; signature update required",
            "entity anchor found without a usable call target"}) {
        Reset(false);
        g_entityScanError = reason;
        Failed(reason);
        assert(memory.attempts == 0);
        Reset(true);
        g_entityScanError = reason;
        Ready(); // Manager-only startup survives capture discovery failure.
        assert(memory.attempts == 0 && g_RingAddr == 0);
    }
    Reset(false);
    calls = {{0x10001000,true},{0x10002000,true}};
    Failed("multiple entity targets; matching build signature required");
    assert(memory.attempts == 0);
    Reset(true);
    calls = {{0x10001000,true},{0x10002000,true}};
    Ready();
    assert(memory.attempts == 0 && g_frameSyncAddr == 0);
    Reset(false);
    calls = {{0x10001000,true}};
    memory.allocation = 0;
    Failed("capture allocation failed");
    Reset(true);
    calls = {{0x10001000,true}};
    memory.allocation = 0;
    Ready(); // An optional capture allocation must not disable GameManager.
    assert(memory.attempts == 1 && g_RingAddr == 0);
    Reset(false);
    calls = {{0x10001000,true}};
    Ready(); // Capture-only startup remains supported.
    assert(memory.attempts == 1 && g_RingAddr == memory.allocation + 0x1000);
    Reset(true);
    calls = {{0x10001000,true}};
    Ready();
    assert(memory.attempts == 1 && g_RingAddr == memory.allocation + 0x1000);
}
'''
with tempfile.TemporaryDirectory(prefix="esp-initialization-") as directory:
    test = Path(directory) / "initialization.cpp"
    test.write_text(source)
    binary = Path(directory) / "initialization"
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I" + str(ROOT / "reverse"), str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("ESP initialization regression checks passed")
