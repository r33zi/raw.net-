#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cfloat>
#include <cmath>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
struct Vec3 { float x, y, z; };

#include "driver.h"
#include "r6_scanner.h"
#include "Imgui/imgui.h"
#include "skeleton_emu.h"
#include "antitamper.h"
#include "operator_esp.h"
struct Matrix4x4 { float m[16]; };
enum class ActorStatus { VALID, DEAD_1, DEAD_2, TEAM, LOCAL, INVALID };

static bool IsValidAddr(uint64_t p){return p>0x10000ULL&&p<0x7FFFFFFFFFFFULL;}

static bool ValidateWorldCoord(Vec3 v){
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
        return false;
    if(fabsf(v.x)>=500000.f||fabsf(v.y)>=500000.f||fabsf(v.z)>=500000.f)
        return false;
    int s=(fabsf(v.x)>2.f?1:0)+(fabsf(v.y)>2.f?1:0)+(fabsf(v.z)>2.f?1:0);
    return s>=2;
}

// Character-component transform fields drift between game builds. Resolve
// the physics anchor from the live +0xB00 position and discover the rotation
// quaternion from the small set of layouts observed by the animation reader.
static std::atomic<uint32_t> g_physWorldOff{0};
static std::atomic<uint32_t> g_rotQuatOff{0};
static std::mutex g_physWorldDiscoveryMtx;
static uint32_t g_physWorldCandidateOff = 0;
static Vec3 g_physWorldCandidateLive{};
static uint32_t g_physWorldCandidateConfirmations = 0;

static bool FindPhysWorldOffset(uint64_t component) {
    if (g_physWorldOff.load(std::memory_order_acquire)) return true;
    if (!IsValidAddr(component)) return false;

    Vec3 live{};
    if (driver->ReadProcessMemory(component + 0xB00, &live, sizeof(live)) != 0 ||
        !ValidateWorldCoord(live)) return false;

    float bestDistanceSq = FLT_MAX;
    uint32_t bestOffset = 0;
    for (uint32_t offset = 0x80; offset + sizeof(Vec3) <= 0x2F0; offset += 4) {
        Vec3 candidate{};
        if (driver->ReadProcessMemory(component + offset, &candidate, sizeof(candidate)) != 0)
            continue;
        if (!ValidateWorldCoord(candidate)) continue;
        const float dx = candidate.x - live.x;
        const float dy = candidate.y - live.y;
        const float dz = candidate.z - live.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq < bestDistanceSq) {
            bestDistanceSq = distanceSq;
            bestOffset = offset;
        }
    }
    if (!bestOffset || bestDistanceSq > 9.0f) return false;
    std::lock_guard<std::mutex> lock(g_physWorldDiscoveryMtx);
    if (g_physWorldOff.load(std::memory_order_acquire)) return true;

    const float moveX = live.x - g_physWorldCandidateLive.x;
    const float moveY = live.y - g_physWorldCandidateLive.y;
    const float moveZ = live.z - g_physWorldCandidateLive.z;
    const float movementSq = moveX * moveX + moveY * moveY + moveZ * moveZ;
    if (g_physWorldCandidateOff != bestOffset) {
        g_physWorldCandidateOff = bestOffset;
        g_physWorldCandidateLive = live;
        g_physWorldCandidateConfirmations = 1;
        return false;
    }
    if (movementSq < 0.01f) return false;

    g_physWorldCandidateLive = live;
    if (++g_physWorldCandidateConfirmations >= 2) {
        g_physWorldOff.store(bestOffset, std::memory_order_release);
        printf("[SKEL] Physics anchor offset: +0x%X\n", bestOffset);
        return true;
    }
    return false;
}

static bool FindRotQuatOffset(uint64_t component) {
    if (g_rotQuatOff.load(std::memory_order_acquire)) return true;
    if (!IsValidAddr(component)) return false;

    static constexpr uint32_t candidates[] = { 0x660, 0x650, 0x670, 0x640, 0x680 };
    for (uint32_t offset : candidates) {
        float q[4]{};
        if (driver->ReadProcessMemory(component + offset, q, sizeof(q)) != 0) continue;
        bool finite = true;
        for (float value : q)
            finite = finite && std::isfinite(value) && fabsf(value) <= 1.01f;
        if (!finite) continue;
        const float magnitudeSq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (magnitudeSq < 0.95f || magnitudeSq > 1.05f) continue;
        uint32_t expected = 0;
        if (g_rotQuatOff.compare_exchange_strong(expected, offset,
                std::memory_order_release, std::memory_order_relaxed)) {
            printf("[SKEL] Rotation quaternion offset: +0x%X\n", offset);
        }
        return true;
    }
    return false;
}

static bool GetEntityRotQuat(uint64_t component, float* out) {
    if (!out) return false;
    uint32_t offset = g_rotQuatOff.load(std::memory_order_acquire);
    if (!offset && (!FindRotQuatOffset(component) ||
            !(offset = g_rotQuatOff.load(std::memory_order_acquire)))) return false;
    if (driver->ReadProcessMemory(component + offset, out, sizeof(float) * 4) != 0)
        return false;
    const float magnitudeSq = out[0] * out[0] + out[1] * out[1] +
        out[2] * out[2] + out[3] * out[3];
    if (!std::isfinite(magnitudeSq) || magnitudeSq < 0.8f || magnitudeSq > 1.2f)
        return false;
    const float inverseLength = 1.0f / sqrtf(magnitudeSq);
    for (int i = 0; i < 4; ++i) out[i] *= inverseLength;
    return true;
}

static bool GetPhysWorldPos(uint64_t component, Vec3& out) {
    Vec3 live{};
    if (driver->ReadProcessMemory(component + 0xB00, &live, sizeof(live)) != 0 ||
        !ValidateWorldCoord(live)) return false;

    uint32_t offset = g_physWorldOff.load(std::memory_order_acquire);
    if (offset) {
        Vec3 candidate{};
        if (driver->ReadProcessMemory(component + offset, &candidate, sizeof(candidate)) == 0 &&
            ValidateWorldCoord(candidate)) {
            const float dx = candidate.x - live.x;
            const float dy = candidate.y - live.y;
            const float dz = candidate.z - live.z;
            if (dx * dx + dy * dy + dz * dz <= 9.0f) {
                out = candidate;
                return true;
            }
        }
        std::lock_guard<std::mutex> lock(g_physWorldDiscoveryMtx);
        uint32_t staleOffset = offset;
        if (g_physWorldOff.compare_exchange_strong(staleOffset, 0,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            g_physWorldCandidateOff = 0;
            g_physWorldCandidateLive = {};
            g_physWorldCandidateConfirmations = 0;
        }
    }

    if (FindPhysWorldOffset(component)) {
        offset = g_physWorldOff.load(std::memory_order_acquire);
        Vec3 candidate{};
        if (offset &&
            driver->ReadProcessMemory(component + offset, &candidate, sizeof(candidate)) == 0 &&
            ValidateWorldCoord(candidate)) {
            const float dx = candidate.x - live.x;
            const float dy = candidate.y - live.y;
            const float dz = candidate.z - live.z;
            if (dx * dx + dy * dy + dz * dz <= 9.0f) {
                out = candidate;
                return true;
            }
        }
    }

    out = live;
    return true;
}

static bool ReadSkeletonRotation(uint64_t component, float* out) {
    return GetEntityRotQuat(component, out);
}

static bool ReadSkeletonAnchor(uint64_t component, void* out) {
    return out && GetPhysWorldPos(component, *static_cast<Vec3*>(out));
}

#include "r6_bones.h"

struct OverlayVertex {
    uint64_t instance;
    Vec3 position;
    ActorStatus status;
    bool isPlayer;
    float distance;
    Vec3 screenPos;
    bool onScreen;
    bool hasBones;
    SkeletonBones bones;
    Vec3 headScreenPos;
    bool headOnScreen;
    char operatorName[32];
    int  hp;
};

struct RenderSyncEntry {
    Vec3 position{};
    Vec3 smoothed_position{};
    uint64_t filter_byte = 0;
    std::chrono::steady_clock::time_point last_seen{};
    std::chrono::steady_clock::time_point position_time{};
};

extern bool Aimbot;
extern bool fillbox;
extern float boxThickness;
extern float snaplineThickness;
extern float fovCircleThickness;
extern float crosshairSize;
extern float espBoxColor[4];
extern float espSnaplineColor[4];
extern float espDistanceColor[4];
extern float fovCircleColor[4];
extern float crosshairColor[4];
extern float aimbotTargetColor[4];
extern float filledBoxColor[4];
extern bool rainbowMode;
extern bool rainbowBox;
extern bool rainbowFov;
extern bool rainbowSnaplines;
extern bool Esp_skeleton;
extern bool skeletonAim;
extern float espSkeletonColor[4];
extern float skeletonThickness;

extern ImU32 GetBoxColor(float offset);
extern ImU32 GetSnaplineColor(float offset);
extern ImU32 GetFovColor();
extern ImU32 ColorToU32(const float* col);
extern void aimbot(float x, float y);

static FILE* g_log = nullptr;
static void DBG(const char* fmt, ...) {
    if (!g_log) g_log = fopen("C:\\r6_render_calib.log", "w");
    va_list a;
    if (g_log) { va_start(a,fmt); vfprintf(g_log,fmt,a); fprintf(g_log,"\n"); fflush(g_log); va_end(a); }
    va_start(a,fmt); vprintf(fmt,a); printf("\n"); va_end(a);
}

static uint64_t g_projectionAddr = 0;
static Matrix4x4 g_frameProjection{};
static Vec3 g_frameCamera{};
static bool g_frameProjectionValid = false;
static uint64_t g_frameSyncAddr = 0;
static uint64_t g_imageBase = 0;
static uint64_t g_ShellPage = 0;
static uint64_t g_RingAddr = 0;
static bool     g_frameSyncActive = false;
static uint8_t  g_OrigBytes[32] = {};
static int      g_PatchLen = 0;
static size_t   g_ShellSize = 0;
static DWORD    g_frameSyncStart = 0;
static bool     g_syncComplete = false;
static HANDLE   g_registryScanThread = NULL;
static constexpr DWORD COLLECT_MS = 499;

static std::vector<OverlayVertex> g_vertexBuffer;
static std::mutex g_Mtx;
static int g_vtxCount = 0, g_activeVtx = 0;
static std::unordered_set<uint64_t> g_capturedFrames;
static std::mutex g_frameMtx;
static std::atomic<uint64_t> g_totalFrames{0};
static uint64_t g_ReadIdx = 0;
static constexpr size_t RING_SZ = 256;
static uint64_t g_RoundPtr = 0;
static bool g_RoundFound = false;

static DWORD g_lastEntityUpdate = 0;
// Keep costly driver reads and skeleton reconstruction off the render cadence.
// The overlay (including the menu) still renders at the presentation rate while
// entity data is refreshed often enough for smooth interpolation.
static constexpr DWORD ENTITY_UPDATE_INTERVAL = 33;


static std::unordered_map<uint64_t, RenderSyncEntry> g_syncMap;
static std::mutex g_syncMapMtx;
static constexpr auto k_syncMaxAge = std::chrono::seconds(30);
static constexpr float k_viewportHeight = 1.72f;
static constexpr float k_minRenderDist = 0.1f;

static bool DrvProtect(uint64_t a, size_t s, uint32_t p, uint32_t* old) {
    return driver->ProtectMemory(a, s, p, old);
}
static bool DrvWriteRaw(const void* src, uint64_t dst, size_t sz) {
    return driver->WriteProcessMemory((PVOID)src, (PVOID)dst, (DWORD)sz) == 0;
}
static bool DrvWriteExec(const void* src, uint64_t dst, size_t sz) {
    uint32_t old = 0;
    DrvProtect(dst, sz, PAGE_EXECUTE_READWRITE, &old);
    bool ok = DrvWriteRaw(src, dst, sz);
    uint32_t tmp = 0;
    DrvProtect(dst, sz, old ? old : PAGE_EXECUTE_READ, &tmp);
    return ok;
}

static int FindBoundary(const uint8_t* c, int minB) {
    int p = 0;
    while (p < 32) {
        uint8_t b = c[p];
        bool rex = (b >= 0x40 && b <= 0x4F);
        if (rex) { p++; b = c[p]; }
        if (b >= 0x50 && b <= 0x5F) { p++; if (p >= minB) return p; continue; }
        if (b == 0x90 || b == 0xCC || b == 0xC3) { p++; if (p >= minB) return p; continue; }
        if (b==0x83||b==0x81||b==0x89||b==0x8B||b==0x8D||b==0x01||b==0x29||
            b==0x31||b==0x33||b==0x39||b==0x3B||b==0x85||b==0x87) {
            uint8_t m = c[p+1]; uint8_t mod = (m>>6)&3; uint8_t rm = m&7;
            p += 2;
            if (mod!=3) { if (rm==4) p++; if (mod==0&&rm==5) p+=4; else if (mod==1) p++; else if (mod==2) p+=4; }
            if (b==0x83) p++; else if (b==0x81) p+=4;
            if (p >= minB) return p; continue;
        }
        if (b == 0x0F) { p++; uint8_t m2 = c[p+1]; uint8_t mod=(m2>>6)&3; uint8_t rm=m2&7;
            p += 2; if (mod!=3) { if(rm==4)p++; if(mod==0&&rm==5)p+=4; else if(mod==1)p++; else if(mod==2)p+=4; }
            if (p >= minB) return p; continue; }
        p++; if (p >= minB) return p;
    }
    return p;
}

static void RefreshConfiguredProjection() {
    if (!g_pViewDataPtr) return;
    const uint64_t viewData = read<uint64_t>(g_pViewDataPtr);
    if (IsValidAddr(viewData)) g_projectionAddr = viewData;
}

static void CaptureProjectionFrame() {
    RefreshConfiguredProjection();
    g_frameProjection = {};
    g_frameCamera = {};
    g_frameProjectionValid = false;
    if (!IsValidAddr(g_projectionAddr)) return;

    if (driver->ReadProcessMemory(
            g_projectionAddr + OFFSETS::ViewProjectionOffset,
            &g_frameProjection, sizeof(g_frameProjection)) != 0) return;
    driver->ReadProcessMemory(
        g_projectionAddr + OFFSETS::CameraPositionOffset,
        &g_frameCamera, sizeof(g_frameCamera));
    g_frameProjectionValid = true;
}

static Matrix4x4 QueryProjectionMatrix() {
    return g_frameProjectionValid ? g_frameProjection : Matrix4x4{};
}

static Vec3 QueryCameraOrigin() {
    return g_frameProjectionValid ? g_frameCamera : Vec3{};
}
static bool W2S(const Vec3& w, Vec3& s, int W, int H) {
    if (!g_frameProjectionValid) return false;
    Matrix4x4 v=QueryProjectionMatrix();
    float ww=v.m[3]*w.x+v.m[7]*w.y+v.m[11]*w.z+v.m[15];
    if (ww<0.001f) return false;
    s.x=(W*.5f)*(w.x*v.m[0]+w.y*v.m[4]+w.z*v.m[8]+v.m[12])/ww+W*.5f;
    s.y=-(H*.5f)*(w.x*v.m[1]+w.y*v.m[5]+w.z*v.m[9]+v.m[13])/ww+H*.5f;
    s.z=ww; return s.x>=0&&s.y>=0&&s.x<=W&&s.y<=H;
}

#include "weather_fx.h"

static bool ValidatePtr(uint64_t e){
    if(!IsValidAddr(e))return false;if(!IsValidAddr(read<uint64_t>(e)))return false;
    int id=read<int>(e+0x1C);return id>0&&id<1000;
}
static uint64_t ReadStencilBuffer(uint64_t e){if(!IsValidAddr(e))return 0;return read<uint64_t>(e+0xB8);}
static uint8_t StencilByte4(uint64_t fb){return(uint8_t)((fb>>32)&0xFF);}
static uint8_t StencilByte3(uint64_t fb){return(uint8_t)((fb>>24)&0xFF);}
static bool IsActiveViewport(uint64_t e){return((ReadStencilBuffer(e)>>52)&0xFFF)==0x2C8;}
static bool IsActiveStencil(uint64_t fb){return ((fb>>52)&0xFFF)==0x2C8;}
static bool IsClearedStencil(uint64_t fb){
    uint8_t b4=StencilByte4(fb);
    return b4==0x84||b4==0x82||b4==0x80;
}
static bool IsStencilCleared(uint64_t e){
    uint64_t fb=ReadStencilBuffer(e);
    if(IsClearedStencil(fb))return true;
    uint64_t bf=read<uint64_t>(e+0xB0);
    if(((bf>>24)&0xFF)==0xFA)return true;
    return false;
}
static bool IsSharedStencil(uint64_t fb){uint8_t b=StencilByte4(fb);return b==0x00||b==0x02;}
static bool IsSharedViewport(uint64_t e){return IsSharedStencil(ReadStencilBuffer(e));}
static bool ValidateDepthStencil(uint64_t fb){
    
    
    
    float fmask = _rcal_get_filter_mask();
    if (fmask < 0.5f) {
        
        return IsSharedStencil(fb) && !IsClearedStencil(fb);
    }
    if(IsClearedStencil(fb))return false;
    if(StencilByte4(fb)==0x00)return false;
    if(IsSharedStencil(fb))return false;
    return true;
}
static bool ValidateStencilMask(uint64_t e){
    return ValidateDepthStencil(ReadStencilBuffer(e));
}

static inline uint64_t mba_dec(uint64_t x) {
    return (x & 0xFFFFFFFFFFFFULL) ^ (0x100010001ULL * ((x >> 48) & 0xFFFFULL));
}

static bool TryPlainPos(uint64_t actor, Vec3& out) {
    uint32_t offsets[] = { 0x50, 0x60 };
    for (int i = 0; i < 2; i++) {
        Vec3 v = read<Vec3>(actor + offsets[i]);
        if (!ValidateWorldCoord(v)) continue;
        out = v;
        return true;
    }
    return false;
}

static bool TryEncryptedPos(uint64_t actor, Vec3& out) {
    uint32_t roots[] = { 0x20, 0x30 };
    for (int ri = 0; ri < 2; ri++) {
        uint64_t slot = read<uint64_t>(actor + roots[ri]);
        if (!slot || !IsValidAddr(slot)) continue;

        uint64_t val = read<uint64_t>(slot);
        if (!val) continue;

        uint64_t d = 0;
        bool ok = true;
        for (int step = 0; step < 4; ++step) {
            d = mba_dec(val);
            if (step == 3) break;
            if (!d || !IsValidAddr(d)) { ok = false; break; }
            val = read<uint64_t>(d);
            if (!val) { ok = false; break; }
        }
        if (!ok || !d || !IsValidAddr(d)) continue;

        uint32_t node_offsets[] = { 0x30, 0x00 };
        for (int ni = 0; ni < 2; ni++) {
            Vec3 v = read<Vec3>(d + node_offsets[ni]);
            if (!ValidateWorldCoord(v)) continue;
            out = v;
            return true;
        }
    }
    return false;
}

static bool ReadActorOrigin(uint64_t actor, Vec3& out) {
    if (!actor || !IsValidAddr(actor)) return false;

    uint16_t flag_5e = read<uint16_t>(actor + 0x5E);
    uint16_t flag_6e = read<uint16_t>(actor + 0x6E);

    bool enc_5e = flag_5e == 0;
    bool enc_6e = flag_6e == 0;
    bool want_encrypted = enc_5e || enc_6e;
    bool want_plain = (flag_5e != 0) && (flag_6e != 0);

    if (want_encrypted) {
        if (TryEncryptedPos(actor, out)) return true;
        return false;
    }

    if (want_plain) {
        if (TryPlainPos(actor, out)) return true;
    }

    if (TryEncryptedPos(actor, out)) return true;
    return TryPlainPos(actor, out);
}

static Vec3 ResolveViewportOrigin(uint64_t e) {
    Vec3 pos{};
    if (ReadActorOrigin(e, pos))
        return pos;
    return {};
}

static void SyncFrameState(RenderSyncEntry& entry, const Vec3& new_pos, std::chrono::steady_clock::time_point now) {
    if (!ValidateWorldCoord(new_pos))
        return;
    entry.position = new_pos;
    entry.position_time = now;
    entry.smoothed_position = new_pos;
}

static Vec3 InterpolateFrameCoord(RenderSyncEntry& entry, float frame_dt) {
    (void)frame_dt;
    if (!ValidateWorldCoord(entry.position))
        return {};
    return entry.position;
}

static void FlushSyncBuffer() {
    std::lock_guard<std::mutex> lock(g_syncMapMtx);
    g_syncMap.clear();
    FlushShaderSigCache();
    FlushBoneCache();
}

static int ReadRound() {
    if (!g_RoundPtr) return -1;
    uint64_t b=read<uint64_t>(g_RoundPtr); if(!IsValidAddr(b))return -1;
    uint64_t p1=read<uint64_t>(b+0x40); if(!IsValidAddr(p1))return -1;
    uint64_t p2=read<uint64_t>(p1+0x48); if(!IsValidAddr(p2))return -1;
    uint64_t p3=read<uint64_t>(p2+0x78); if(!IsValidAddr(p3))return -1;
    uint64_t p4=read<uint64_t>(p3+0x18); if(!IsValidAddr(p4))return -1;
    uint64_t p5=read<uint64_t>(p4+0x90); if(!IsValidAddr(p5))return -1;
    uint64_t p6=read<uint64_t>(p5+0x38); if(!IsValidAddr(p6))return -1;
    int st=read<int>(p6+0x348); return (st>=0&&st<=5)?st:-1;
}
static void FindRound() {
    if (g_RoundFound||!g_textCache.valid) return;
    const uint8_t* t=g_textCache.data.data(); size_t sz=(size_t)g_textCache.textSize;
    uint64_t tb=g_textCache.textBase;
    for (size_t i=0;i+15<sz;i++) {
        if(t[i]!=0xE8) continue; if((t[i+12]&0xF0)!=0x40) continue;
        int32_t r=*(int32_t*)&t[i+8]; uint64_t c=tb+i+12+(int64_t)r;
        if(!IsValidAddr(c)) continue;
        uint64_t b=read<uint64_t>(c); if(!b||!IsValidAddr(b)) continue;
        uint64_t p1=read<uint64_t>(b+0x40); if(!IsValidAddr(p1)) continue;
        uint64_t p2=read<uint64_t>(p1+0x48); if(!IsValidAddr(p2)) continue;
        uint64_t p3=read<uint64_t>(p2+0x78); if(!IsValidAddr(p3)) continue;
        uint64_t p4=read<uint64_t>(p3+0x18); if(!IsValidAddr(p4)) continue;
        uint64_t p5=read<uint64_t>(p4+0x90); if(!IsValidAddr(p5)) continue;
        uint64_t p6=read<uint64_t>(p5+0x38); if(!IsValidAddr(p6)) continue;
        int st=read<int>(p6+0x348);
        if(st>=0&&st<=5) { g_RoundPtr=c; g_RoundFound=true; return; }
    }
}

static void PollFrameRing() {
    if (!g_RingAddr) return;
    uint64_t wi=read<uint64_t>(g_RingAddr);
    while (g_ReadIdx < wi) {
        uint64_t idx=g_ReadIdx&(RING_SZ-1);
        uint64_t ep=read<uint64_t>(g_RingAddr+0x10+idx*8);
        if (IsValidAddr(ep)&&ValidatePtr(ep)) { std::lock_guard<std::mutex> l(g_frameMtx); g_capturedFrames.insert(ep); g_totalFrames++; }
        g_ReadIdx++;
    }
}

static bool AttachFrameSync() {
    if (g_frameSyncActive||!g_frameSyncAddr||!g_ShellPage) return false;
    for (int i=0;i<32;i++) g_OrigBytes[i]=read<uint8_t>(g_frameSyncAddr+i);
    g_PatchLen = FindBoundary(g_OrigBytes, 14);
    if (g_PatchLen<14||g_PatchLen>30) return false;
    uint8_t sc[80]={}; int p=0;
    sc[p++]=0x50; sc[p++]=0x52;
    sc[p++]=0x48; sc[p++]=0xB8;
    *(uint64_t*)&sc[p]=g_RingAddr; p+=8;
    sc[p++]=0x48; sc[p++]=0x8B; sc[p++]=0x10;
    sc[p++]=0x0F; sc[p++]=0xB6; sc[p++]=0xD2;
    sc[p++]=0x48; sc[p++]=0x89; sc[p++]=0x4C; sc[p++]=0xD0; sc[p++]=0x10;
    sc[p++]=0x48; sc[p++]=0xFF; sc[p++]=0x00;
    sc[p++]=0x5A; sc[p++]=0x58;
    memcpy(&sc[p],g_OrigBytes,g_PatchLen); p+=g_PatchLen;
    sc[p++]=0xFF; sc[p++]=0x25; sc[p++]=0; sc[p++]=0; sc[p++]=0; sc[p++]=0;
    *(uint64_t*)&sc[p]=g_frameSyncAddr+g_PatchLen; p+=8;
    g_ShellSize=p;
    if (!DrvWriteRaw(sc, g_ShellPage, p)) return false;
    uint32_t old=0; DrvProtect(g_ShellPage, 4096, PAGE_EXECUTE_READWRITE, &old);
    uint64_t zero=0; DrvWriteRaw(&zero, g_RingAddr, 8); g_ReadIdx=0;
    uint8_t hook[32]={};
    hook[0]=0xFF; hook[1]=0x25; hook[2]=0; hook[3]=0; hook[4]=0; hook[5]=0;
    *(uint64_t*)&hook[6]=g_ShellPage;
    for(int i=14;i<g_PatchLen;i++) hook[i]=0x90;
    if (!DrvWriteExec(hook, g_frameSyncAddr, g_PatchLen)) return false;
    g_frameSyncActive=true; g_frameSyncStart=GetTickCount();
    printf("[HOOK] INSTALLED - .text patched (will restore in %dms)\n", COLLECT_MS);
    return true;
}

static bool DetachFrameSync() {
    if (!g_frameSyncActive) return false;
    if (!DrvWriteExec(g_OrigBytes, g_frameSyncAddr, g_PatchLen)) return false;
    g_frameSyncActive=false;
    printf("[HOOK] REMOVED - .text restored after %dms\n", GetTickCount()-g_frameSyncStart);
return true;
}


static constexpr int TRAIL_MAX_ENTITIES = 32;
static constexpr int TRAIL_MAX_POINTS = 200;
struct TrailBuffer {
    uint64_t entityId;
    Vec3 points[TRAIL_MAX_POINTS];
    int count;
    int writeIdx;
    DWORD lastUpdate;
};
static TrailBuffer g_trailBuffers[TRAIL_MAX_ENTITIES] = {};
static int g_trailBufCount = 0;

static TrailBuffer* AllocTrailBuffer(uint64_t entityId) {
    for (int i = 0; i < g_trailBufCount; i++)
        if (g_trailBuffers[i].entityId == entityId) return &g_trailBuffers[i];
    if (g_trailBufCount < TRAIL_MAX_ENTITIES) {
        TrailBuffer* t = &g_trailBuffers[g_trailBufCount++];
        t->entityId = entityId;
        t->count = 0;
        t->writeIdx = 0;
        t->lastUpdate = 0;
        return t;
    }
    int oldest = 0; DWORD oldestT = UINT_MAX;
    for (int i = 0; i < TRAIL_MAX_ENTITIES; i++)
        if (g_trailBuffers[i].lastUpdate < oldestT) { oldestT = g_trailBuffers[i].lastUpdate; oldest = i; }
    TrailBuffer* t = &g_trailBuffers[oldest];
    t->entityId = entityId;
    t->count = 0;
    t->writeIdx = 0;
    return t;
}

static void AppendTrailSample(TrailBuffer* t, Vec3 pos) {
    extern int trailUpdateMs;
    DWORD now = GetTickCount();
    if (now - t->lastUpdate < (DWORD)trailUpdateMs) return;
    t->lastUpdate = now;
    t->points[t->writeIdx] = pos;
    t->writeIdx = (t->writeIdx + 1) % TRAIL_MAX_POINTS;
    if (t->count < TRAIL_MAX_POINTS) t->count++;
}

static bool InitRenderPipeline(uint64_t base, uint64_t size) {
    g_imageBase=base;
    auto secs=GetPESections(base);
    if(secs.empty()) return false;
    if(!CacheTextSection(base,secs)) return false;
    ScanConfiguredPointers(base);
    auto calls=FindEntityFunctionCalls(base);

    // Prefer the supplied actor capture site when its preceding MOV bytes
    // match this build. Retain signature-based discovery as a safe fallback.
    if (OFFSETS::ActorMovRva + sizeof(OFFSETS::ActorMovBytes) <= size &&
        OFFSETS::ActorPatchRva + 32 <= size) {
        uint8_t actorMov[sizeof(OFFSETS::ActorMovBytes)]{};
        if (driver->ReadProcessMemory(base + OFFSETS::ActorMovRva,
                actorMov, sizeof(actorMov)) == 0 &&
            memcmp(actorMov, OFFSETS::ActorMovBytes, sizeof(actorMov)) == 0) {
            g_frameSyncAddr = base + OFFSETS::ActorPatchRva;
            printf("[ENTITY-SCAN] Using configured actor patch RVA 0x%llX\n",
                (unsigned long long)OFFSETS::ActorPatchRva);
        } else {
            printf("[ENTITY-SCAN] Configured actor MOV validation failed; using signature fallback\n");
        }
    }
    for(auto& c:calls) if(!g_frameSyncAddr&&c.hasTestAlAl){g_frameSyncAddr=c.targetVA;break;}
    if(!g_frameSyncAddr&&!calls.empty()) g_frameSyncAddr=calls[0].targetVA;
    if(!g_frameSyncAddr) return false;
    KAllocRequest ar={}; ar.ProcessId=driver->ProcessId; ar.Size=8192;
    ar.AllocationType=MEM_COMMIT|MEM_RESERVE; ar.Protect=PAGE_READWRITE;
    ULONG ret=0;
    GetNtApi().Ioctl(driver->Handle(),IOCTL_ALLOC_MEMORY,&ar,sizeof(ar),&ar,sizeof(ar),&ret);
    g_ShellPage=ar.Address;
    if(!g_ShellPage) return false;
    g_RingAddr=g_ShellPage+0x100;
    FindRound();
    if (!g_pViewDataPtr && OFFSETS::ViewMatrixRva + sizeof(uint64_t) <= size) {
        const uint64_t configuredViewPtr = base + OFFSETS::ViewMatrixRva;
        if (IsValidAddr(read<uint64_t>(configuredViewPtr))) {
            g_pViewDataPtr = configuredViewPtr;
            printf("[W2S] Using configured view pointer RVA 0x%llX\n",
                (unsigned long long)OFFSETS::ViewMatrixRva);
        }
    }
    RefreshConfiguredProjection();
    if (!g_projectionAddr) g_projectionAddr=ScanForViewTrans(base,size);
    if (ScanSkelXref(base)) {
        printf("[R6] Skeleton hash function: func=0x%llX HEAD=0x%llX NECK=0x%llX pairs=%u\n",
            (unsigned long long)g_SkelXref.skelFuncVA,
            (unsigned long long)g_SkelXref.headHashRefVA,
            (unsigned long long)g_SkelXref.neckHashRefVA,
            g_SkelXref.pairCount);
    } else {
        printf("[R6] Skeleton hash function not found or ambiguous\n");
    }

    skel::g_readRotQuatFn = ReadSkeletonRotation;
    skel::g_readPhysPosFn = ReadSkeletonAnchor;
    g_registryScanThread = CreateThread(NULL, 0, [](LPVOID) -> DWORD {
        skel::ScanRegistry();
        return 0;
    }, NULL, 0, NULL);

    CreateThread(NULL, 0, [](LPVOID) -> DWORD { ScanSidewards(); return 0; }, NULL, 0, NULL);

    printf("[R6] Position system: ReadActorOrigin (encrypted+plain, driver read)\n");
    return true;
}

static void ShutdownRenderPipeline() {
    if (g_registryScanThread) {
        if (WaitForSingleObject(g_registryScanThread, 2000) == WAIT_OBJECT_0)
            CloseHandle(g_registryScanThread);
        g_registryScanThread = NULL;
    }
    RestoreSidewards();
    if(g_frameSyncActive) DetachFrameSync();
    FlushSyncBuffer();
    skel::CloseLog();
    if(g_log){fclose(g_log);g_log=nullptr;}
}

static void PollSyncBuffer(int W, int H, int maxD) {
    DWORD now_tick = GetTickCount();
    if (now_tick - g_lastEntityUpdate < ENTITY_UPDATE_INTERVAL) return;
    g_lastEntityUpdate = now_tick;

    std::lock_guard<std::mutex> lk(g_Mtx);
    g_vertexBuffer.clear(); g_vtxCount = 0; g_activeVtx = 0;

    static DWORD s_posDbg = 0;
    bool posDbg = (now_tick - s_posDbg > 5000);
    if (posDbg) s_posDbg = now_tick;
    if (!g_RoundFound) FindRound();
    int rs = g_RoundFound ? ReadRound() : 3;

    static int s_lastRoundState = -1;
    extern bool sidewardsEnabled;
    extern float sidewardsValue;

    bool isGameplay = (rs == 2 || rs == 3);
    bool wasGameplay = (s_lastRoundState == 2 || s_lastRoundState == 3);
    bool newRound = (isGameplay && !wasGameplay) || (rs == 2 && s_lastRoundState == 3);

    if (newRound) {
        printf("[ROUND] New round (rs=%d, prev=%d), waiting before hook...\\n", rs, s_lastRoundState);
        CreateThread(NULL, 0, [](LPVOID) -> DWORD { ScanSidewards(); return 0; }, NULL, 0, NULL);
        if (sidewardsEnabled && g_Sidewards.found) {
            SetSidewardsValue(sidewardsValue);
        }
        g_syncComplete = false;
        FlushSyncBuffer();
        { std::lock_guard<std::mutex> l(g_frameMtx); g_capturedFrames.clear(); }
    }

    if (!isGameplay && wasGameplay) {
        g_syncComplete = false;
        FlushSyncBuffer();
        { std::lock_guard<std::mutex> l(g_frameMtx); g_capturedFrames.clear(); }
    }

    s_lastRoundState = rs;

    static DWORD s_hookDelayStart = 0;
    static DWORD s_hookDelayMs = 0;

    if (newRound) {
        s_hookDelayStart = now_tick;
        s_hookDelayMs = 4000 + (rand() % 2001);
        printf("[HOOK] Delay %dms before patching\\n", s_hookDelayMs);
    }

    bool delayPassed = (s_hookDelayStart > 0 && (now_tick - s_hookDelayStart) >= s_hookDelayMs);
    if (!g_frameSyncActive && !g_syncComplete && isGameplay && delayPassed) {
        AttachFrameSync();
        s_hookDelayStart = 0;
    }
    if (g_frameSyncActive) { PollFrameRing(); if (GetTickCount() - g_frameSyncStart > COLLECT_MS) { DetachFrameSync(); g_syncComplete = true; } }

    std::vector<uint64_t> cap;
    {
        std::lock_guard<std::mutex> l(g_frameMtx);
        for (auto it = g_capturedFrames.begin(); it != g_capturedFrames.end(); ++it) {
            if (IsValidAddr(*it)) cap.push_back(*it);
        }
    }

    Vec3 cam = QueryCameraOrigin();
    auto now = std::chrono::steady_clock::now();
    float frame_dt = 1.f / 60.f;
    {

        static DWORD s_prevTick = 0;
        if (s_prevTick != 0) {
            float raw = (float)(now_tick - s_prevTick) / 1000.f;
            if (raw > 0.f && raw < 0.25f) frame_dt = raw;
        }
        s_prevTick = now_tick;
    }


    {
        std::lock_guard<std::mutex> clock(g_syncMapMtx);


        for (uint64_t ea : cap) {
            uint64_t fb = ReadStencilBuffer(ea);
            if (!IsActiveStencil(fb) && !ValidateDepthStencil(fb)) {
                g_syncMap.erase(ea);
                continue;
            }

            auto& entry = g_syncMap[ea];
            entry.filter_byte = fb;
            entry.last_seen = now;

            Vec3 position{};
            if (ReadActorOrigin(ea, position))
                SyncFrameState(entry, position, now);
        }


        for (auto it = g_syncMap.begin(); it != g_syncMap.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last_seen);
            if (age > k_syncMaxAge) {
                it = g_syncMap.erase(it);
                continue;
            }
            ++it;
        }


        for (auto it = g_syncMap.begin(); it != g_syncMap.end(); ++it) {
            uint64_t ea = it->first;
            auto& entry = it->second;

            if (!ValidateDepthStencil(entry.filter_byte))
                continue;


            Vec3 draw_pos = entry.position;
            if (!ValidateWorldCoord(draw_pos))
                continue;

            float d = sqrtf(
                (draw_pos.x - cam.x) * (draw_pos.x - cam.x) +
                (draw_pos.y - cam.y) * (draw_pos.y - cam.y) +
                (draw_pos.z - cam.z) * (draw_pos.z - cam.z));

            if (d < k_minRenderDist || d >(float)maxD)
                continue;

            Vec3 sp = {};
            bool on = W2S(draw_pos, sp, W, H);

            OverlayVertex e = {};
            e.instance = ea;
            e.position = draw_pos;
            e.status = IsClearedStencil(entry.filter_byte) ? ActorStatus::DEAD_1 : ActorStatus::VALID;
            e.isPlayer = IsActiveStencil(entry.filter_byte);
            e.distance = d;
            e.screenPos = sp;
            e.onScreen = on;
            e.hasBones = false;
            e.headOnScreen = false;
            e.hp = 100;
            e.operatorName[0] = '\0';

            if ((Esp_skeleton || skeletonAim) && e.isPlayer &&
                ReadSkeleton(ea, draw_pos.x, draw_pos.y, draw_pos.z, e.bones) &&
                e.bones.total > 0) {
                e.hasBones = true;
                const Vec3B head = GetHeadBonePos(e.bones);
                const Vec3 headWorld = { head.x, head.y, head.z };
                e.headOnScreen = W2S(headWorld, e.headScreenPos, W, H);
            }


            const char* opName = ResolveShaderLabel(ea);
            if (opName) {
                strncpy(e.operatorName, opName, sizeof(e.operatorName) - 1);
                e.operatorName[sizeof(e.operatorName) - 1] = '\0';
            }

            if (posDbg && g_vtxCount < 3) {
                printf("[POS] entity=0x%llX pos=(%.1f,%.1f,%.1f) smooth=(%.1f,%.1f,%.1f) dist=%.0f on=%d\n",
                    (unsigned long long)ea,
                    entry.position.x, entry.position.y, entry.position.z,
                    draw_pos.x, draw_pos.y, draw_pos.z,
                    d, on ? 1 : 0);
            }

            g_vertexBuffer.push_back(e);
            g_vtxCount++;
            if (e.isPlayer) g_activeVtx++;
        }
    }
}

static void FlushOverlayPipeline(bool box, bool corner, bool line, bool dist, int visDist,
                            bool trail, bool aimEnabled, float aimFov, float aimSmooth,
                            int hitboxSel, bool fovCircle, bool squareFov, bool xhair) {
    int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
    CaptureProjectionFrame();
    PollSyncBuffer(W, H, visDist);
    std::lock_guard<std::mutex> lk(g_Mtx);
    ImDrawList* dl = ImGui::GetOverlayDrawList();
    if (!dl) return;

    float bestAimDist = 1e30f;
    float bestAimX = 0, bestAimY = 0;
    bool hasAimTarget = false;

    extern float boxThickness;
    extern float snaplineThickness;
    extern float fovCircleThickness;
    extern float crosshairSize;
    extern float espBoxColor[4];
    extern float espSnaplineColor[4];
    extern float espDistanceColor[4];
    extern float fovCircleColor[4];
    extern float crosshairColor[4];
    extern float aimbotTargetColor[4];
    extern float filledBoxColor[4];
    extern float espTrailColor[4];
    extern float trailThickness;
    extern int trailLength;
    extern bool fillbox;
    extern bool lineheadesp;
    extern bool rainbowMode;
    extern bool rainbowBox;
    extern bool rainbowFov;
    extern bool rainbowSnaplines;
    extern bool rainbowTrail;

    for (auto& e : g_vertexBuffer) {
        if (!e.onScreen) continue;
        float sx = e.screenPos.x, sy = e.screenPos.y;
        float entOffset = (float)(e.instance & 0xFF) / 255.0f;

        ImU32 boxCol = GetBoxColor(entOffset);
        ImU32 snapCol = GetSnaplineColor(entOffset);

        
        Vec3 headScr = e.headScreenPos;
        bool headOn = e.hasBones && e.headOnScreen;
        if (!headOn) {
            const Vec3 headPos = {e.position.x, e.position.y, e.position.z + k_viewportHeight};
            headOn = W2S(headPos, headScr, W, H);
        }

        if (box && headOn) {
            float boxH = fabsf(sy - headScr.y);
            if (boxH < 4.f) continue;
            float boxW = boxH * 0.45f;
            float top = fminf(sy, headScr.y);
            float bot = fmaxf(sy, headScr.y);
            float left = sx - boxW * 0.5f;
            float right = sx + boxW * 0.5f;

            if (fillbox) {
                dl->AddRectFilled({left, top}, {right, bot}, ColorToU32(filledBoxColor));
            }
            if (corner) {
                
                float w = right - left, h = bot - top, f = 0.25f;
                dl->AddLine({left,top},{left+w*f,top},boxCol,boxThickness);
                dl->AddLine({left,top},{left,top+h*f},boxCol,boxThickness);
                dl->AddLine({right,top},{right-w*f,top},boxCol,boxThickness);
                dl->AddLine({right,top},{right,top+h*f},boxCol,boxThickness);
                dl->AddLine({left,bot},{left+w*f,bot},boxCol,boxThickness);
                dl->AddLine({left,bot},{left,bot-h*f},boxCol,boxThickness);
                dl->AddLine({right,bot},{right-w*f,bot},boxCol,boxThickness);
                dl->AddLine({right,bot},{right,bot-h*f},boxCol,boxThickness);
            } else {
                dl->AddRect({left, top}, {right, bot}, boxCol, 0, 0, boxThickness);
            }
        }

        if (line) {
            extern int snaplineOrigin;
            ImVec2 from;
            if (snaplineOrigin == 0) from = ImVec2((float)W/2, 0);           
            else if (snaplineOrigin == 1) from = ImVec2((float)W/2, (float)H/2); 
            else from = ImVec2((float)W/2, (float)H);                        
            dl->AddLine(from, {sx, sy}, snapCol, snaplineThickness);
        }

        if (lineheadesp && headOn) {
            dl->AddLine({sx, sy}, {(float)headScr.x, (float)headScr.y}, IM_COL32(255,255,0,200), 1.0f);
        }

        if (Esp_skeleton && e.hasBones) {
            const ImU32 skeletonColor = ColorToU32(espSkeletonColor);
            const SkeletonBones& bones = e.bones;
            auto drawBone = [&](int first, int second) {
                if (!bones.hasBone[first] || !bones.hasBone[second]) return;
                const Vec3 firstWorld = {
                    bones.bones[first].x, bones.bones[first].y, bones.bones[first].z
                };
                const Vec3 secondWorld = {
                    bones.bones[second].x, bones.bones[second].y, bones.bones[second].z
                };
                Vec3 firstScreen{}, secondScreen{};
                if (W2S(firstWorld, firstScreen, W, H) && W2S(secondWorld, secondScreen, W, H)) {
                    dl->AddLine({firstScreen.x, firstScreen.y},
                        {secondScreen.x, secondScreen.y}, skeletonColor, skeletonThickness);
                }
            };
            for (int i = 0; i < skel::kNumConnections; ++i) {
                drawBone(skel::kConnections[i].first, skel::kConnections[i].second);
            }
        }

        if (dist) {
            char dt[32]; snprintf(dt, 32, "%.0fm", e.distance);
            dl->AddText({sx - 10 + 1, sy + 3 + 1}, IM_COL32(0, 0, 0, 200), dt);
            dl->AddText({sx - 10, sy + 3}, ColorToU32(espDistanceColor), dt);
        }

        extern bool shaderLabelOverlay;
        extern bool shaderIconOverlay;
        extern bool depthVisualization;
        if (shaderLabelOverlay && e.isPlayer && headOn) {
            float cx = sx;
            float ty = fminf(sy, headScr.y);
            float boxH = fabsf(sy - headScr.y);
            if (boxH < 4.f) boxH = 16.f;

            if (shaderIconOverlay && e.operatorName[0]) {
                
                float iconSz = boxH * 0.35f;
                if (iconSz < 16.0f) iconSz = 16.0f;
                if (iconSz > 48.0f) iconSz = 48.0f;
                ImTextureID iconTex = QueryShaderResource(e.operatorName);
                if (iconTex) {
                    float iconX = cx - iconSz * 0.5f;
                    float iconY = ty - iconSz - 3.0f;
                    dl->AddImage(iconTex, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
                } else {
                    
                    ImVec2 tsz = ImGui::CalcTextSize(e.operatorName);
                    float textX = cx - tsz.x * 0.5f;
                    float textY = ty - 15.0f;
                    dl->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0,0,0,200), e.operatorName);
                    dl->AddText(ImVec2(textX, textY), IM_COL32(255,200,50,255), e.operatorName);
                }
            } else if (e.operatorName[0]) {
                
                ImVec2 tsz = ImGui::CalcTextSize(e.operatorName);
                float textX = cx - tsz.x * 0.5f;
                float textY = ty - 15.0f;
                dl->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0,0,0,200), e.operatorName);
                dl->AddText(ImVec2(textX, textY), IM_COL32(255,200,50,255), e.operatorName);
            } else {
                
                char opLabel[32];
                snprintf(opLabel, 32, "P%d", (int)(e.instance & 0xFF));
                ImVec2 tsz = ImGui::CalcTextSize(opLabel);
                float textX = cx - tsz.x * 0.5f;
                float textY = ty - 15.0f;
                dl->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0,0,0,200), opLabel);
                dl->AddText(ImVec2(textX, textY), IM_COL32(200,200,200,200), opLabel);
            }
        }

        
        if (depthVisualization && headOn) {
            float top_y = fminf(sy, headScr.y);
            float bot_y = fmaxf(sy, headScr.y);
            float boxH_hp = bot_y - top_y;
            if (boxH_hp >= 4.f) {
                float boxW_hp = boxH_hp * 0.45f;
                float barX = sx - boxW_hp * 0.5f - 6.0f;
                float barW = 3.0f;
                float hpFrac = (float)e.hp / 100.0f;
                if (hpFrac > 1.0f) hpFrac = 1.0f;
                if (hpFrac < 0.0f) hpFrac = 0.0f;
                float filledH = boxH_hp * hpFrac;
                int rr = (int)(255.0f * (1.0f - hpFrac));
                int gg = (int)(255.0f * hpFrac);
                dl->AddRectFilled(ImVec2(barX, top_y), ImVec2(barX + barW, bot_y), IM_COL32(20,20,20,180));
                dl->AddRectFilled(ImVec2(barX, bot_y - filledH), ImVec2(barX + barW, bot_y), IM_COL32(rr,gg,0,220));
                dl->AddRect(ImVec2(barX, top_y), ImVec2(barX + barW, bot_y), IM_COL32(0,0,0,200));
            }
        }

        if (trail && e.isPlayer) {
            TrailBuffer* tr = AllocTrailBuffer(e.instance);
            AppendTrailSample(tr, e.position);
            int maxPts = (trailLength < tr->count) ? trailLength : tr->count;
            if (maxPts > 1) {
                for (int ti = 0; ti < maxPts - 1; ti++) {
                    int idx0 = (tr->writeIdx - maxPts + ti + TRAIL_MAX_POINTS) % TRAIL_MAX_POINTS;
                    int idx1 = (idx0 + 1) % TRAIL_MAX_POINTS;
                    Vec3 s0 = {}, s1 = {};
                    if (W2S(tr->points[idx0], s0, W, H) && W2S(tr->points[idx1], s1, W, H)) {
                        extern bool trailFade;
                        float alpha = trailFade ? (float)(ti + 1) / (float)maxPts : 1.0f;
                        ImU32 tc;
                        if (rainbowMode && rainbowTrail) {
                            float hue = fmodf((float)ti / (float)maxPts + entOffset, 1.0f);
                            float r, g, b;
                            ImGui::ColorConvertHSVtoRGB(hue, 1.0f, 1.0f, r, g, b);
                            tc = IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),(int)(alpha*espTrailColor[3]*255));
                        } else {
                            tc = IM_COL32((int)(espTrailColor[0]*255),(int)(espTrailColor[1]*255),
                                         (int)(espTrailColor[2]*255),(int)(alpha*espTrailColor[3]*255));
                        }
                        dl->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), tc, trailThickness);
                    }
                }
            }
        }

        if (aimEnabled && e.isPlayer) {
            float effectiveFov = aimFov;
            float fovOvr = _rcal_get_fov_override();
            if (fovOvr >= 0.0f) effectiveFov = fovOvr;

            extern int aimTargetMode;
            if (aimTargetMode == 1) {
                for (float zOff = 0.2f; zOff <= 1.5f; zOff += 0.1f) {
                    Vec3 scanWorld = {e.position.x, e.position.y, e.position.z + zOff};
                    Vec3 scanScr = {};
                    if (!W2S(scanWorld, scanScr, W, H)) continue;
                    float dx = scanScr.x - W / 2.0f;
                    float dy = scanScr.y - H / 2.0f;
                    float crossDist = sqrtf(dx * dx + dy * dy);
                    if (crossDist < effectiveFov && crossDist < bestAimDist) {
                        bestAimDist = crossDist;
                        bestAimX = scanScr.x;
                        bestAimY = scanScr.y;
                        hasAimTarget = true;
                    }
                }
            } else {
                Vec3 aimScr = {};
                bool aimOn = false;
                if (skeletonAim) {
                    if (e.hasBones && e.headOnScreen) {
                        aimScr = e.headScreenPos;
                        aimOn = true;
                    }
                } else {
                    AimTarget at = GetAimPosition(e.instance, (float)e.position.x,
                        (float)e.position.y, (float)e.position.z, hitboxSel);
                    const Vec3 aimWorld = {at.x, at.y, at.z};
                    aimOn = W2S(aimWorld, aimScr, W, H);
                }
                if (aimOn) {
                    float dx = (float)aimScr.x - W / 2.0f;
                    float dy = (float)aimScr.y - H / 2.0f;
                    float crossDist = sqrtf(dx * dx + dy * dy);
                    if (crossDist < effectiveFov && crossDist < bestAimDist) {
                        bestAimDist = crossDist;
                        bestAimX = (float)aimScr.x;
                        bestAimY = (float)aimScr.y;
                        hasAimTarget = true;
                    }
                }
            }
        }
    }

    if (aimEnabled && hasAimTarget) {
        dl->AddCircle(ImVec2(bestAimX, bestAimY), 6.0f, ColorToU32(aimbotTargetColor), 12, 2.0f);
        bool aimKeyPressed = false;
        extern int hotkeys_aimkey_val();
        if (hotkeys::aimkey > 0)
            aimKeyPressed = (GetAsyncKeyState(hotkeys::aimkey) & 0x8000) != 0;
        else
            aimKeyPressed = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (aimKeyPressed) {
            extern void aimbot(float x, float y);
            aimbot(bestAimX, bestAimY);
        }
    }

    if (fovCircle) {
        extern float AimFOV;
        dl->AddCircle(ImVec2((float)W/2, (float)H/2), AimFOV, GetFovColor(), 64, fovCircleThickness);
    }
    if (squareFov) {
        extern float AimFOV;
        float half = AimFOV;
        dl->AddRect(ImVec2(W/2.0f-half, H/2.0f-half), ImVec2(W/2.0f+half, H/2.0f+half), GetFovColor(), 0, 0, fovCircleThickness);
    }
    if (xhair) {
        float cx = W/2.0f, cy = H/2.0f, cs = crosshairSize;
        ImU32 xc = ColorToU32(crosshairColor);
        dl->AddLine(ImVec2(cx-cs, cy), ImVec2(cx+cs, cy), xc, 1.5f);
        dl->AddLine(ImVec2(cx, cy-cs), ImVec2(cx, cy+cs), xc, 1.5f);
    }


    {
        extern int g_weatherMode;
        if (g_weatherMode != WFX_NONE && g_projectionAddr) {
            g_wfxMode = g_weatherMode;
            Vec3 cam_w = QueryCameraOrigin();
            if (fabsf(cam_w.x) > 0.1f || fabsf(cam_w.y) > 0.1f) {
                float wdt = 1.f / 60.f;
                {
                    static DWORD s_wprev = 0;
                    DWORD wnow = GetTickCount();
                    if (s_wprev) {
                        float raw = (float)(wnow - s_wprev) / 1000.f;
                        if (raw > 0.f && raw < 0.25f) wdt = raw;
                    }
                    s_wprev = wnow;
                }
                WfxRender3D(cam_w, wdt, W, H, dl);
            }
        } else {
            g_wfxMode = WFX_NONE;
            g_wfxInited = false;
        }
    }

    char info[256];
    snprintf(info, 256, "P:%d E:%d Hook:%s Rnd:%d Cache:%d",
        g_activeVtx, g_vtxCount, g_frameSyncActive ? "ON" : (g_syncComplete ? "done" : "wait"),
        g_RoundFound ? ReadRound() : -1,
        (int)g_syncMap.size());
    dl->AddText({10, 10}, IM_COL32(0, 255, 0, 200), info);
}
