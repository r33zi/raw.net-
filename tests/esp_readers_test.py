"""Exercise production reader functions with synthetic memory on Linux.

Extract only the named function definitions to avoid pulling in the Windows
SDK/driver. This does not replace a build of the complete Windows application.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = r'''
#include "overlay_projection.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <limits>
struct Vec3 { float x,y,z; };
struct Matrix4x4 { float m[16]; };
static uint64_t g_imageBase = 0x10000000, g_imageSize = 0x1000;
static uint64_t g_pViewDataPtr = 0x500000, g_projectionAddr = 0;
static Matrix4x4 g_frameProjection{};
static Vec3 g_frameCamera{};
static bool g_frameProjectionValid = false;
namespace OFFSETS {
constexpr uint64_t ViewProjectionOffset = 0x250, CameraPositionOffset = 0x190;
}
struct Memory {
    std::map<uint64_t, unsigned char> bytes;
    int ReadProcessMemory(uint64_t a, void* out, size_t n) {
        for (size_t i=0; i<n; ++i) if (!bytes.count(a+i)) return -1;
        for (size_t i=0; i<n; ++i) static_cast<unsigned char*>(out)[i] = bytes.at(a+i);
        return 0;
    }
    template<class T> void Put(uint64_t a, const T& value) {
        for (size_t i=0; i<sizeof(T); ++i)
            bytes[a+i] = reinterpret_cast<const unsigned char*>(&value)[i];
    }
} memory;
static Memory* driver = &memory;
template<class T> T read(uint64_t a) {
    T v{}; driver->ReadProcessMemory(a, &v, sizeof(v)); return v;
}
bool IsValidAddr(uint64_t a) { return a>0x10000 && a<0x7FFFFFFFFFFF; }
namespace skel {
static uint64_t g_componentArrayOffset = 0xE0;
static uint32_t g_compIdxOff = 0x1EF;
bool ValidPtr(uint64_t a) { return IsValidAddr(a); }
bool ReadRaw(uint64_t a, void* p, size_t n) { return driver->ReadProcessMemory(a,p,n)==0; }
template<class T> T Read(uint64_t a) { return read<T>(a); }
'''
source += function("reverse/skel.h", "inline uint64_t FindIndexedCharacterComponent(")
source += r'''
}
static uint64_t lastComponent = 0;
bool GetPhysWorldPos(uint64_t component, Vec3& out) {
    lastComponent = component; out = {1,2,3}; return true;
}
'''
for name in ("static bool ReadActorOrigin(", "static void RefreshConfiguredProjection(",
             "static void CaptureProjectionFrame("):
    source += function("reverse/r6_entities.h", name) + "\n"
source += r'''
int main() {
    const uint64_t actor=0x200000, list=0x300000, component=0x400000, camera=0x600000;
    Vec3 position{};
    // Discovery changes both offsets. No legacy fields exist in this fixture.
    skel::g_componentArrayOffset = 0xD8;
    skel::g_compIdxOff = 0x1FB;
    memory.Put(actor+0xD8,list);
    memory.Put(actor+0x1FB,uint8_t{7});
    memory.Put(list+7*8,component);
    assert(ReadActorOrigin(actor,position) && lastComponent==component);
    memory.Put(actor+0x1FB,uint8_t{255});
    assert(!ReadActorOrigin(actor,position));
    memory.bytes.erase(actor+0x1FB); // Failed selector read must not imply index 0.
    memory.Put(list,component);
    assert(!ReadActorOrigin(actor,position));
    memory.Put(actor+0x1FB,uint8_t{7});
    memory.Put(list+7*8,g_imageBase+0x20);
    assert(!ReadActorOrigin(actor,position));
    memory.Put(actor+0xD8,uint64_t{0});
    assert(!ReadActorOrigin(actor,position));

    const Matrix4x4 projection{{1,0,0,0, 0,0,0,1, 0,1,0,0, 0,0,1,0}};
    memory.Put(g_pViewDataPtr,camera);
    memory.Put(camera+0x250,projection);
    memory.Put(camera+0x190,Vec3{0,0,0}); // Camera at origin is valid.
    CaptureProjectionFrame();
    assert(g_frameProjectionValid && g_projectionAddr==camera);
    memory.bytes.erase(camera+0x190);
    CaptureProjectionFrame();
    assert(!g_frameProjectionValid);
    memory.Put(camera+0x190,Vec3{std::numeric_limits<float>::quiet_NaN(),1,2});
    CaptureProjectionFrame();
    assert(!g_frameProjectionValid);
    memory.Put(camera+0x190,Vec3{1,2,3});
    memory.Put(camera+0x250,Matrix4x4{});
    CaptureProjectionFrame();
    assert(!g_frameProjectionValid);
    memory.Put(camera+0x250,projection);
    CaptureProjectionFrame();
    assert(g_frameProjectionValid);
    memory.Put(g_pViewDataPtr,uint64_t{0});
    CaptureProjectionFrame();
    assert(!g_frameProjectionValid && g_projectionAddr==0);
}
'''
with tempfile.TemporaryDirectory(prefix="esp-readers-") as directory:
    test = Path(directory) / "readers.cpp"
    test.write_text(source)
    binary = Path(directory) / "readers"
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I" + str(ROOT / "reverse"), str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("ESP reader regression checks passed")
