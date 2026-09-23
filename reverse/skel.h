#pragma once


#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include "driver.h"
#include "r6_bone_ids.h"

extern DWORD processID;

namespace skel
{

    // No-op — logging removed. Call sites remain compilable.
    inline void Log(const char*, ...) {}
    inline void CloseLog() {}


    constexpr uint16_t kCharacterTag = 0x0c63;
    constexpr uint64_t kWorldMatrix  = 0x240;
    constexpr uint64_t kLivePosition = 0xB00;


    inline bool ReadRaw(uint64_t address, void* out, size_t size)
    {
        return driver->ReadProcessMemory(address, out, (uint32_t)size) == 0;
    }

    template <class T> inline T Read(uint64_t address)
    {
        T v{};
        ReadRaw(address, &v, sizeof(T));
        return v;
    }

    inline uint64_t NowMicros()
    {
        static LARGE_INTEGER freq = [](){ LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        uint64_t q = (uint64_t)c.QuadPart, f = (uint64_t)freq.QuadPart;
        return (q / f) * 1000000ULL + ((q % f) * 1000000ULL) / f;
    }

    inline bool ValidPtr(uint64_t p) { return p > 0x10000 && p < 0x7FFFFFFFFFFF; }


    struct Vec3f {
        float x = 0, y = 0, z = 0;
        Vec3f operator-(const Vec3f& o) const { return { x - o.x, y - o.y, z - o.z }; }
        float Length() const { return sqrtf(x * x + y * y + z * z); }
    };

    // Bone-entry layout. Stride is 0x40 (64 bytes per entry — confirmed by the
    // 1.0f store at +0x3C in the bone sigs). The translation vec3 begins at
    // +0x30; a matched store at +0x38 writes its Z component, not another
    // vec3. ScanBoneSigs confirms this layout at startup.
    inline uint64_t kBoneStride    = 0x40;
    inline uint64_t kBoneTranslate = 0x30;
    // Widened from 0x8000 → 0x40000 (32KB → 256KB). Newer R6 arenas are larger
    // than the initial reverse and the tighter window was dropping valid binds
    // when the char comp and skinning context landed in different sub-arenas.
    constexpr uint64_t kRigAllocWindow = 0x40000;
    // Bone-count sanity bounds — character rigs sit here; gadgets/props fall outside.
    constexpr uint32_t kMinCharBones   = 24;
    constexpr uint32_t kMaxCharBones   = 200;

    struct RegistrySlot {
        uint32_t seq_begin;
        uint32_t count;
        uint64_t palette;
        uint64_t context;
        uint32_t seq_end;
        uint32_t reserved;
    };
    static_assert(sizeof(RegistrySlot) == 0x20, "slot layout");

    constexpr uint32_t kRegistryCapacity    = 64;
    constexpr uint64_t kRegistrySlotsOffset = 0x20;

    inline uint64_t g_registryBase = 0;
    inline std::mutex g_registryScanMutex;

    struct RetainedRig { RegistrySlot slot; uint64_t last_seen_ms; };
    inline std::vector<RetainedRig> g_retained;
    inline uint64_t                 g_lastPollUs = 0;

    constexpr uint64_t kPollIntervalUs = 1000;
    // Bumped from 400ms → 1200ms. Momentary registry read gaps (thread contention,
    // page walk) were dropping rigs briefly then requiring a full re-bind cycle.
    // 1.2s covers realistic transient gaps without holding stale rigs from dead actors.
    constexpr uint64_t kRetainMs       = 1200;

    inline const std::vector<RetainedRig>& PollRegistry()
    {
        const uint64_t now_us = NowMicros();
        if (g_lastPollUs && (now_us - g_lastPollUs) < kPollIntervalUs)
            return g_retained;
        g_lastPollUs = now_us;

        RegistrySlot slots[kRegistryCapacity]{};
        if (!g_registryBase ||
            !ReadRaw(g_registryBase + kRegistrySlotsOffset, slots, sizeof(slots)))
            return g_retained;

        const uint64_t now_ms = now_us / 1000;

        for (const RegistrySlot& s : slots)
        {
            if (!s.seq_begin || s.seq_begin != s.seq_end) continue;
            if (s.count < 8 || s.count > 512)             continue;
            if (!ValidPtr(s.palette) || !ValidPtr(s.context)) continue;

            auto it = std::find_if(g_retained.begin(), g_retained.end(),
                [&](const RetainedRig& r) { return r.slot.context == s.context; });

            if (it == g_retained.end()) {
                g_retained.push_back({ s, now_ms });
                continue;
            }

            if (s.seq_begin > it->slot.seq_begin) it->slot = s;
            it->last_seen_ms = now_ms;
        }

        g_retained.erase(std::remove_if(g_retained.begin(), g_retained.end(),
            [&](const RetainedRig& r) { return now_ms - r.last_seen_ms >= kRetainMs; }),
            g_retained.end());

        return g_retained;
    }


    inline uint64_t g_componentArrayOffset = 0xE0;
    inline uint32_t g_compIdxOff = 0x1EF;

    // TAG-DISCOVERY DIAGNOSTIC — dumps 16 bytes before each of the first N pointers
    // in a candidate component list, so the real character-component tag stands out
    // by being repeated across multiple pointers in the same list.
    inline void DumpTagCandidates(uint64_t list, int maxPtrs = 8)
    {
        static int s_dumps = 0;
        if (s_dumps >= 8) return;  // cap total spam
        s_dumps++;
        Log("[SKEL-TAG] ==== dump list=0x%llX ====\n", (unsigned long long)list);
        for (uint64_t i = 0; i < 40 && maxPtrs > 0; i++) {
            uint64_t c = Read<uint64_t>(list + i * 8);
            if (!ValidPtr(c)) continue;
            uint8_t hdr[16] = {};
            ReadRaw(c - 0x10, hdr, 16);
            uint16_t t_m10 = *(uint16_t*)(hdr + 0);
            uint16_t t_m0E = *(uint16_t*)(hdr + 2);
            uint16_t t_m0C = *(uint16_t*)(hdr + 4);
            uint16_t t_m0A = *(uint16_t*)(hdr + 6);
            uint16_t t_m08 = *(uint16_t*)(hdr + 8);
            uint16_t t_m06 = *(uint16_t*)(hdr + 10);
            uint16_t t_m04 = *(uint16_t*)(hdr + 12);
            uint16_t t_m02 = *(uint16_t*)(hdr + 14);
            uint32_t d_m10 = *(uint32_t*)(hdr + 0);
            uint32_t d_m08 = *(uint32_t*)(hdr + 8);
            Log("[SKEL-TAG] idx=%llu comp=0x%llX  hdr[-10..-01]: %04X %04X %04X %04X | %04X %04X %04X %04X  (dw-10=0x%08X dw-08=0x%08X)\n",
                (unsigned long long)i, (unsigned long long)c,
                t_m10, t_m0E, t_m0C, t_m0A,
                t_m08, t_m06, t_m04, t_m02,
                d_m10, d_m08);
            maxPtrs--;
        }
    }

    inline uint16_t g_discoveredCharTag = 0;  // auto-discovered; 0 = not found yet

    // Scan a component list for a component whose world-matrix translation is
    // near draw_pos (if provided) OR that has a valid live position at +0xB00.
    // This replaces the hardcoded 0x0c63 tag lookup, which is stale on current
    // builds. When we find the char comp, we lock the tag so future lookups
    // are fast. If draw_pos is (0,0,0), we accept any comp with a valid +0xB00.
    inline uint64_t ScanListForCharacter(uint64_t list, float dx = 0.f, float dy = 0.f, float dz = 0.f)
    {
        if (!ValidPtr(list)) return 0;
        bool haveDraw = (dx != 0.f || dy != 0.f || dz != 0.f);

        for (uint64_t i = 0; i < 200; i++) {
            uint64_t c = Read<uint64_t>(list + i * 8);
            if (!ValidPtr(c)) { if (c == 0) break; continue; }

            // Fast path: if we already know the tag, use it.
            if (g_discoveredCharTag) {
                uint16_t tag = Read<uint16_t>(c - 0x08);
                if (tag == g_discoveredCharTag) return c;
                continue;
            }

            // Discovery: check if this comp has a valid live position at +0xB00.
            // The char comp is the ONLY comp with a world-space position there.
            Vec3f live{};
            if (!ReadRaw(c + kLivePosition, &live, sizeof(live))) continue;
            if (!std::isfinite(live.x) || !std::isfinite(live.y) || !std::isfinite(live.z)) continue;
            if (live.x == 0.f && live.y == 0.f && live.z == 0.f) continue;

            if (haveDraw) {
                // Verify it's near the draw position (within 5m).
                float ddx = live.x - dx, ddy = live.y - dy, ddz = live.z - dz;
                float dist2 = ddx*ddx + ddy*ddy + ddz*ddz;
                if (dist2 > 25.f) continue;  // > 5m away, not the char comp
            }

            // Lock the tag.
            uint16_t tag = Read<uint16_t>(c - 0x08);
            if (tag == 0 || tag > 0xFFFF) continue;
            g_discoveredCharTag = tag;
            printf("[SKEL] Char comp tag discovered: 0x%04X (comp=0x%llX idx=%d live=%.1f,%.1f,%.1f)\n",
                tag, (unsigned long long)c, (int)i, live.x, live.y, live.z);
            return c;
        }
        return 0;
    }

    inline uint64_t FindCharacterComponent(uint64_t entity, float dx = 0.f, float dy = 0.f, float dz = 0.f)
    {
        if (!ValidPtr(entity)) return 0;
        g_componentArrayOffset = 0xE0;
        g_compIdxOff = 0x1EF;
        const uint64_t list = Read<uint64_t>(entity + g_componentArrayOffset);
        const uint8_t index = Read<uint8_t>(entity + g_compIdxOff);
        if (!ValidPtr(list)) return 0;
        const uint64_t component = Read<uint64_t>(list + (uint64_t)index * 8);
        if (!ValidPtr(component) ||
            (component >= g_imageBase && component - g_imageBase < g_imageSize)) return 0;
        return component;
    }


    struct Claim { uint64_t comp; uint64_t gap; uint64_t at_ms; };
    inline std::unordered_map<uint64_t, Claim> g_ctxOwner;
    constexpr uint64_t kClaimExpiryMs = 500;

    inline bool ClaimContext(uint64_t ctx, uint64_t comp, uint64_t gap, uint64_t now_ms)
    {
        auto it = g_ctxOwner.find(ctx);
        if (it != g_ctxOwner.end()) {
            const bool expired = (now_ms - it->second.at_ms) >= kClaimExpiryMs;
            const bool mine    = it->second.comp == comp;
            if (!expired && !mine && it->second.gap <= gap) return false;
        }
        g_ctxOwner[ctx] = { comp, gap, now_ms };
        return true;
    }

    // Persistent comp → context memory. Once a comp has been bound to a context
    // and that context is still live in the current registry poll, we prefer it
    // over any distance-based re-scoring. Kills flicker from re-binds when two
    // rigs land in the same allocation window.
    struct StickyBind { uint64_t context; uint64_t last_used_ms; };
    inline std::unordered_map<uint64_t, StickyBind> g_stickyBind;
    constexpr uint64_t kStickyExpireMs = 3000;

    inline bool BindRigToPlayer(uint64_t comp, RegistrySlot& out)
    {
        if (!comp) return false;
        const auto&    rigs   = PollRegistry();
        const uint64_t now_ms = NowMicros() / 1000;

        // Fast path: if we already have a good bind and that context is still
        // live in the registry poll, keep using it.
        auto sb = g_stickyBind.find(comp);
        if (sb != g_stickyBind.end() && (now_ms - sb->second.last_used_ms) < kStickyExpireMs) {
            for (const RetainedRig& r : rigs) {
                if (r.slot.context != sb->second.context) continue;
                if (r.slot.count < kMinCharBones || r.slot.count > kMaxCharBones) break;
                out = r.slot;
                sb->second.last_used_ms = now_ms;
                return true;
            }
        }

        // Slow path: score every candidate rig against this comp.
        std::vector<const RegistrySlot*> candidates;
        for (const RetainedRig& r : rigs) {
            if (r.slot.context >= comp) continue;
            if (comp - r.slot.context >= kRigAllocWindow) continue;
            // Character-only bone-count filter — cuts gadget/prop skinning noise.
            if (r.slot.count < kMinCharBones || r.slot.count > kMaxCharBones) continue;
            candidates.push_back(&r.slot);
        }

        std::sort(candidates.begin(), candidates.end(),
            [&](const RegistrySlot* a, const RegistrySlot* b) {
                return (comp - a->context) < (comp - b->context);
            });

        for (const RegistrySlot* c : candidates) {
            if (!ClaimContext(c->context, comp, comp - c->context, now_ms)) continue;
            out = *c;
            g_stickyBind[comp] = { c->context, now_ms };
            return true;
        }

        // Cleanup expired sticky binds so the map doesn't grow unbounded.
        if ((now_ms & 0x3FF) == 0) {  // ~once/sec at 1kHz poll
            for (auto it = g_stickyBind.begin(); it != g_stickyBind.end(); ) {
                if (now_ms - it->second.last_used_ms > kStickyExpireMs)
                    it = g_stickyBind.erase(it);
                else ++it;
            }
        }
        return false;
    }


    struct WorldMatrix { float m[4][4]; bool valid = false; };

    // Build a world matrix from the char comp's rotation quaternion (auto-
    // discovered at +0x660/+0x650/etc) and the PhysicsWorld position (auto-
    // discovered by matching the +0xB00 live position). Falls back to the
    // legacy +0x240 matrix / +0xB00 position if the auto-discovery hasn't
    // converged yet. The +0x240 matrix is STALE on the current build — the
    // quaternion is the authoritative rotation source.
    //
    // We can't call GetEntityRotQuat / GetPhysWorldPos directly (they're in
    // r6_entities.h, included after us), so we use function pointers set by
    // r6_entities.h at startup.
    inline bool (*g_readRotQuatFn)(uint64_t, float*) = nullptr;
    inline bool (*g_readPhysPosFn)(uint64_t, void*) = nullptr;  // void* = Vec3*

    inline WorldMatrix ReadWorldMatrix(uint64_t comp)
    {
        WorldMatrix wm{};

        static uint64_t s_lastDiag = 0;
        uint64_t now_ms = NowMicros() / 1000;
        bool diag = (now_ms - s_lastDiag > 2000);
        if (diag) s_lastDiag = now_ms;

        // ── Rotation: from quaternion if available, else +0x240 ──
        float q[4] = { 0.f, 0.f, 0.f, 1.f };
        bool haveQuat = false;
        if (g_readRotQuatFn && g_readRotQuatFn(comp, q)) {
            haveQuat = true;
        }

        if (diag) {
            printf("[WM-DIAG] comp=0x%llX haveQuat=%d q=(%.3f,%.3f,%.3f,%.3f) rotFn=%p\n",
                (unsigned long long)comp, (int)haveQuat, q[0], q[1], q[2], q[3], (void*)g_readRotQuatFn);
        }

        if (haveQuat) {
            // Quaternion (x, y, z, w) → row-major 4x4 rotation.
            float xx = q[0]*q[0], yy = q[1]*q[1], zz = q[2]*q[2];
            float xy = q[0]*q[1], xz = q[0]*q[2], yz = q[1]*q[2];
            float wx = q[3]*q[0], wy = q[3]*q[1], wz = q[3]*q[2];
            wm.m[0][0] = 1.f - 2.f*(yy+zz);  wm.m[0][1] = 2.f*(xy-wz);      wm.m[0][2] = 2.f*(xz+wy);      wm.m[0][3] = 0.f;
            wm.m[1][0] = 2.f*(xy+wz);        wm.m[1][1] = 1.f - 2.f*(xx+zz);  wm.m[1][2] = 2.f*(yz-wx);      wm.m[1][3] = 0.f;
            wm.m[2][0] = 2.f*(xz-wy);        wm.m[2][1] = 2.f*(yz+wx);        wm.m[2][2] = 1.f - 2.f*(xx+yy);  wm.m[2][3] = 0.f;
        } else {
            // Fallback: read the legacy +0x240 matrix rotation block.
            if (!comp || !ReadRaw(comp + kWorldMatrix, wm.m, sizeof(wm.m))) return wm;
            if (diag) printf("[WM-DIAG] FALLBACK to +0x240 matrix\n");
        }

        // ── Translation: PhysicsWorld position if available, else +0xB00 ──
        Vec3f live{};
        bool havePhys = false;
        if (g_readPhysPosFn && g_readPhysPosFn(comp, &live)) {
            havePhys = true;
        }
        if (!havePhys) {
            if (!ReadRaw(comp + kLivePosition, &live, sizeof(live))) return wm;
        }
        if (!std::isfinite(live.x) || !std::isfinite(live.y) || !std::isfinite(live.z))
            return wm;

        if (diag) {
            printf("[WM-DIAG] havePhys=%d pos=(%.1f,%.1f,%.1f)\n", (int)havePhys, live.x, live.y, live.z);
        }

        wm.m[3][0] = live.x; wm.m[3][1] = live.y; wm.m[3][2] = live.z; wm.m[3][3] = 1.f;

        // Validate
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(wm.m[r][c])) return wm;
        if (fabsf(wm.m[3][3] - 1.f) > 1e-3f) return wm;
        for (int r = 0; r < 3; ++r) {
            const float len = sqrtf(wm.m[r][0] * wm.m[r][0] +
                                    wm.m[r][1] * wm.m[r][1] +
                                    wm.m[r][2] * wm.m[r][2]);
            if (!std::isfinite(len) || fabsf(len - 1.f) > 0.02f) return wm;
        }
        wm.valid = true;
        return wm;
    }

    inline Vec3f ToWorld(const WorldMatrix& wm, const Vec3f& b)
    {
        return {
            b.x * wm.m[0][0] + b.y * wm.m[1][0] + b.z * wm.m[2][0] + wm.m[3][0],
            b.x * wm.m[0][1] + b.y * wm.m[1][1] + b.z * wm.m[2][1] + wm.m[3][1],
            b.x * wm.m[0][2] + b.y * wm.m[1][2] + b.z * wm.m[2][2] + wm.m[3][2]
        };
    }

    inline int ReadPaletteBones(const uint8_t* buf, size_t size, uint32_t count,
                                Vec3f* out, int max_out)
    {
        int n = 0;
        for (uint32_t i = 0; i < count && n < max_out; ++i) {
            const size_t at = (size_t)i * kBoneStride + kBoneTranslate;
            if (at + 12 > size) break;
            Vec3f v; memcpy(&v, buf + at, 12);
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) continue;
            out[n++] = v;
        }
        return n;
    }

    enum {
        BONE_HEAD = 0, BONE_NECK, BONE_SPINE,
        BONE_L_SHOULDER, BONE_L_ELBOW, BONE_L_HAND,
        BONE_R_SHOULDER, BONE_R_ELBOW, BONE_R_HAND,
        BONE_L_HIP, BONE_L_KNEE, BONE_L_ANKLE, BONE_L_FOOT,
        BONE_R_HIP, BONE_R_KNEE, BONE_R_ANKLE, BONE_R_FOOT,
        BONE_COUNT
    };

    // Map an engine bone hash to the overlay's compact drawing-bone index.
    // The arm/leg entries identify the start of each segment, so forearm and
    // lower-leg IDs supply the elbow and knee joints respectively.
    inline int BoneHashToIndex(uint32_t h) {
        switch (h) {
        case BoneHash(BipedBoneID::BONE_HEAD):          return BONE_HEAD;
        case BoneHash(BipedBoneID::BONE_NECK):          return BONE_NECK;
        case BoneHash(BipedBoneID::BONE_SPINE):         return BONE_SPINE;
        case BoneHash(BipedBoneID::BONE_LEFTSHOULDER):  return BONE_L_SHOULDER;
        case BoneHash(BipedBoneID::BONE_LEFTFOREARM):   return BONE_L_ELBOW;
        case BoneHash(BipedBoneID::BONE_LEFTHAND):      return BONE_L_HAND;
        case BoneHash(BipedBoneID::BONE_RIGHTSHOULDER): return BONE_R_SHOULDER;
        case BoneHash(BipedBoneID::BONE_RIGHTFOREARM):  return BONE_R_ELBOW;
        case BoneHash(BipedBoneID::BONE_RIGHTHAND):     return BONE_R_HAND;
        case BoneHash(BipedBoneID::BONE_LEFTUPLEG):     return BONE_L_HIP;
        case BoneHash(BipedBoneID::BONE_LEFTLEG):       return BONE_L_KNEE;
        case BoneHash(BipedBoneID::BONE_LEFTFOOT):      return BONE_L_ANKLE;
        case BoneHash(BipedBoneID::BONE_LEFTTOEBASE):   return BONE_L_FOOT;
        case BoneHash(BipedBoneID::BONE_RIGHTUPLEG):    return BONE_R_HIP;
        case BoneHash(BipedBoneID::BONE_RIGHTLEG):      return BONE_R_KNEE;
        case BoneHash(BipedBoneID::BONE_RIGHTFOOT):     return BONE_R_ANKLE;
        case BoneHash(BipedBoneID::BONE_RIGHTTOEBASE):  return BONE_R_FOOT;
        default:                                         return -1;
        }
    }

    static const std::pair<int, int> kConnections[] = {
        { BONE_HEAD, BONE_NECK }, { BONE_NECK, BONE_SPINE },
        { BONE_NECK, BONE_L_SHOULDER }, { BONE_L_SHOULDER, BONE_L_ELBOW }, { BONE_L_ELBOW, BONE_L_HAND },
        { BONE_NECK, BONE_R_SHOULDER }, { BONE_R_SHOULDER, BONE_R_ELBOW }, { BONE_R_ELBOW, BONE_R_HAND },
        { BONE_SPINE, BONE_L_HIP }, { BONE_L_HIP, BONE_L_KNEE }, { BONE_L_KNEE, BONE_L_ANKLE }, { BONE_L_ANKLE, BONE_L_FOOT },
        { BONE_SPINE, BONE_R_HIP }, { BONE_R_HIP, BONE_R_KNEE }, { BONE_R_KNEE, BONE_R_ANKLE }, { BONE_R_ANKLE, BONE_R_FOOT },
    };
    constexpr int kNumConnections = (int)(sizeof(kConnections) / sizeof(kConnections[0]));

    struct RefBone { int bone; Vec3f local; float radius; };

    // R6-verified reference poses. Same enum order (HEAD, NECK, SPINE, L{shoulder,elbow,hand,hip,knee,ankle,foot}, R{...}).
    // SolveLayoutMulti tries all three and picks whichever fits best per palette.
    static const RefBone kReferencePose_Stand_A[] = {
        { BONE_HEAD,       {  0.005f,  0.154f, 1.602f }, 0.40f },
        { BONE_NECK,       { -0.050f, -0.044f, 1.354f }, 0.40f },
        { BONE_SPINE,      { -0.037f, -0.190f, 0.889f }, 0.40f },
        { BONE_L_SHOULDER, { -0.223f,  0.035f, 1.345f }, 0.50f },
        { BONE_L_ELBOW,    { -0.344f,  0.132f, 1.066f }, 0.65f },
        { BONE_L_HAND,     { -0.139f,  0.362f, 1.105f }, 0.85f },
        { BONE_R_SHOULDER, {  0.084f, -0.146f, 1.389f }, 0.50f },
        { BONE_R_ELBOW,    {  0.224f, -0.151f, 1.090f }, 0.65f },
        { BONE_R_HAND,     {  0.181f,  0.166f, 1.046f }, 0.85f },
        { BONE_L_HIP,      { -0.171f,  0.028f, 0.740f }, 0.50f },
        { BONE_L_KNEE,     { -0.197f,  0.197f, 0.558f }, 0.60f },
        { BONE_L_ANKLE,    { -0.295f,  0.128f, 0.301f }, 0.55f },
        { BONE_L_FOOT,     { -0.341f,  0.058f, 0.065f }, 0.70f },
        { BONE_R_HIP,      {  0.084f, -0.147f, 0.733f }, 0.50f },
        { BONE_R_KNEE,     {  0.210f, -0.094f, 0.524f }, 0.60f },
        { BONE_R_ANKLE,    {  0.206f, -0.254f, 0.314f }, 0.55f },
        { BONE_R_FOOT,     {  0.182f, -0.408f, 0.076f }, 0.70f },
    };
    static const RefBone kReferencePose_Stand_Rifle[] = {
        { BONE_HEAD,       {  0.00f,  0.16f, 1.60f }, 0.40f },
        { BONE_NECK,       { -0.08f, -0.07f, 1.37f }, 0.40f },
        { BONE_SPINE,      { -0.08f, -0.20f, 0.90f }, 0.40f },
        { BONE_L_SHOULDER, { -0.23f,  0.07f, 1.35f }, 0.50f },
        { BONE_L_ELBOW,    { -0.15f,  0.31f, 1.12f }, 0.65f },
        { BONE_L_HAND,     {  0.02f,  0.48f, 1.21f }, 0.85f },
        { BONE_R_SHOULDER, {  0.05f, -0.20f, 1.38f }, 0.50f },
        { BONE_R_ELBOW,    {  0.20f, -0.07f, 1.11f }, 0.65f },
        { BONE_R_HAND,     {  0.11f,  0.25f, 1.18f }, 0.85f },
        { BONE_L_HIP,      { -0.21f,  0.01f, 0.75f }, 0.50f },
        { BONE_L_KNEE,     { -0.23f,  0.17f, 0.57f }, 0.60f },
        { BONE_L_ANKLE,    { -0.31f,  0.12f, 0.30f }, 0.55f },
        { BONE_L_FOOT,     { -0.34f,  0.07f, 0.06f }, 0.70f },
        { BONE_R_HIP,      {  0.05f, -0.17f, 0.74f }, 0.50f },
        { BONE_R_KNEE,     {  0.16f, -0.14f, 0.53f }, 0.60f },
        { BONE_R_ANKLE,    {  0.18f, -0.28f, 0.31f }, 0.55f },
        { BONE_R_FOOT,     {  0.18f, -0.41f, 0.06f }, 0.70f },
    };
    static const RefBone kReferencePose_Crouch[] = {
        { BONE_HEAD,       {  0.03f,  0.07f, 1.07f }, 0.40f },
        { BONE_NECK,       { -0.06f, -0.16f, 0.86f }, 0.40f },
        { BONE_SPINE,      { -0.12f, -0.43f, 0.38f }, 0.40f },
        { BONE_L_SHOULDER, { -0.21f, -0.04f, 0.80f }, 0.50f },
        { BONE_L_ELBOW,    { -0.13f,  0.20f, 0.57f }, 0.65f },
        { BONE_L_HAND,     {  0.04f,  0.36f, 0.68f }, 0.85f },
        { BONE_R_SHOULDER, {  0.08f, -0.29f, 0.90f }, 0.50f },
        { BONE_R_ELBOW,    {  0.18f, -0.21f, 0.59f }, 0.65f },
        { BONE_R_HAND,     {  0.12f,  0.12f, 0.66f }, 0.85f },
        { BONE_L_HIP,      { -0.20f, -0.13f, 0.42f }, 0.50f },
        { BONE_L_KNEE,     { -0.19f,  0.21f, 0.49f }, 0.60f },
        { BONE_L_ANKLE,    { -0.23f,  0.02f, 0.28f }, 0.55f },
        { BONE_L_FOOT,     { -0.25f, -0.14f, 0.06f }, 0.70f },
        { BONE_R_HIP,      {  0.03f, -0.29f, 0.42f }, 0.50f },
        { BONE_R_KNEE,     {  0.26f, -0.05f, 0.46f }, 0.60f },
        { BONE_R_ANKLE,    {  0.17f, -0.23f, 0.29f }, 0.55f },
        { BONE_R_FOOT,     {  0.06f, -0.39f, 0.06f }, 0.70f },
    };
    // Default alias — kept for legacy callers (SolveLayout uses it below).
    static const RefBone (&kReferencePose)[17] = kReferencePose_Stand_A;

    struct Layout { int idx[BONE_COUNT]; };
    inline std::unordered_map<uint64_t, Layout> g_layoutByContext;

    constexpr float kAcceptError = 1.50f;

    // Solve against a specific reference pose (17 bones). Used by SolveLayoutMulti.
    inline int SolveLayoutPose(const RefBone* pose, const Vec3f* cand, int ncand,
                               int out[BONE_COUNT], float& out_err)
    {
        int   best_hits = 0;
        float best_err  = 1e9f;
        int   best[BONE_COUNT];
        for (int i = 0; i < BONE_COUNT; ++i) best[i] = -1;

        for (int q = 0; q < 4; ++q)
        {
            const float ang = q * 1.57079633f;
            const float cs = cosf(ang), sn = sinf(ang);

            int   idx[BONE_COUNT];
            bool  used[256] = {};
            float err   = 0.f;
            int   hits  = 0;
            for (int i = 0; i < BONE_COUNT; ++i) idx[i] = -1;

            for (int rbi = 0; rbi < BONE_COUNT; ++rbi)
            {
                const RefBone& rb = pose[rbi];
                const Vec3f want{ rb.local.x * cs - rb.local.y * sn,
                                  rb.local.x * sn + rb.local.y * cs,
                                  rb.local.z };
                int   pick = -1;
                float pick_d = rb.radius;

                for (int c = 0; c < ncand && c < 256; ++c) {
                    if (used[c]) continue;
                    const float d = (cand[c] - want).Length();
                    if (d < pick_d) { pick_d = d; pick = c; }
                }
                if (pick < 0) continue;
                used[pick]     = true;
                idx[rb.bone]   = pick;
                err           += pick_d;
                ++hits;
            }

            if (hits > best_hits || (hits == best_hits && err < best_err)) {
                best_hits = hits;
                best_err  = err / (hits ? hits : 1);
                memcpy(best, idx, sizeof(idx));
            }
        }

        memcpy(out, best, sizeof(best));
        out_err = best_err;
        return best_hits;
    }

    // Legacy alias: default pose.
    inline int SolveLayout(const Vec3f* cand, int ncand, int out[BONE_COUNT], float& out_err) {
        return SolveLayoutPose(kReferencePose, cand, ncand, out, out_err);
    }

    // Try all three R6 poses (Stand_A, Stand_Rifle, Crouch). Returns the best hit-count.
    inline int SolveLayoutMulti(const Vec3f* cand, int ncand, int out[BONE_COUNT],
                                float& out_err, const char** out_pose_name = nullptr)
    {
        static const RefBone* const poses[] = { kReferencePose_Stand_A, kReferencePose_Stand_Rifle, kReferencePose_Crouch };
        static const char* const     names[] = { "Stand_A", "Stand_Rifle", "Crouch" };

        int best_hits = 0;
        float best_err = 1e9f;
        int best[BONE_COUNT];
        int best_pose = 0;
        for (int i = 0; i < BONE_COUNT; ++i) best[i] = -1;

        for (int p = 0; p < 3; ++p) {
            int idx[BONE_COUNT];
            float err = 0.f;
            int hits = SolveLayoutPose(poses[p], cand, ncand, idx, err);
            if (hits > best_hits || (hits == best_hits && err < best_err)) {
                best_hits = hits;
                best_err  = err;
                best_pose = p;
                memcpy(best, idx, sizeof(idx));
            }
        }
        memcpy(out, best, sizeof(best));
        out_err = best_err;
        if (out_pose_name) *out_pose_name = names[best_pose];
        return best_hits;
    }

    struct Donor { uint32_t count; float seg[kNumConnections]; Layout layout; };
    inline std::vector<Donor> g_donors;

    constexpr float kSegTolerance = 0.08f;
    constexpr float kSegMinLength = 0.02f;

    inline bool ComputeSegments(const Vec3f* cand, int ncand,
                                const int idx[BONE_COUNT], float out[kNumConnections])
    {
        for (int i = 0; i < kNumConnections; ++i) {
            const int a = idx[kConnections[i].first];
            const int b = idx[kConnections[i].second];
            if (a < 0 || b < 0 || a >= ncand || b >= ncand) return false;
            const float len = (cand[a] - cand[b]).Length();
            if (!std::isfinite(len)) return false;
            out[i] = len;
        }
        return true;
    }

    inline void RememberDonor(const Vec3f* cand, int ncand, uint32_t count,
                              const int idx[BONE_COUNT])
    {
        Donor d{}; d.count = count;
        if (!ComputeSegments(cand, ncand, idx, d.seg)) return;
        memcpy(d.layout.idx, idx, sizeof(d.layout.idx));

        for (const Donor& e : g_donors) {
            if (e.count != count) continue;
            bool same = true;
            for (int i = 0; i < kNumConnections && same; ++i) {
                if (e.seg[i] < kSegMinLength) continue;
                if (fabsf(d.seg[i] - e.seg[i]) / e.seg[i] > kSegTolerance) same = false;
            }
            if (same) return;
        }
        g_donors.push_back(d);
    }

    inline bool AdoptFromDonor(const Vec3f* cand, int ncand, uint32_t count,
                               int out[BONE_COUNT])
    {
        for (const Donor& d : g_donors) {
            if (d.count != count) continue;

            float seg[kNumConnections];
            if (!ComputeSegments(cand, ncand, d.layout.idx, seg)) continue;

            bool ok = true; int compared = 0;
            for (int i = 0; i < kNumConnections && ok; ++i) {
                if (d.seg[i] < kSegMinLength) continue;
                ++compared;
                if (fabsf(seg[i] - d.seg[i]) / d.seg[i] > kSegTolerance) ok = false;
            }
            if (!ok || compared < kNumConnections / 2) continue;

            memcpy(out, d.layout.idx, sizeof(int) * BONE_COUNT);
            return true;
        }
        return false;
    }


    inline std::mutex g_mutex;

    inline uint64_t g_bindSuccessCount = 0;
    inline uint64_t g_bindFailCount    = 0;
    inline uint64_t g_lastReset_ms     = 0;

    inline bool ScanRegistry();

    inline bool GetBones(uint64_t entity, Vec3f out_world[BONE_COUNT], uint32_t& out_mask)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        out_mask = 0;

        if (!g_registryBase) {
            static uint64_t s_lastRescan_ms = 0;
            const uint64_t now_ms = NowMicros() / 1000;
            if (now_ms - s_lastRescan_ms >= 2000) {
                s_lastRescan_ms = now_ms;
                ScanRegistry();
            }
            if (!g_registryBase) { Log("[SKEL] GetBones: no registry\n"); return false; }
        }

        const uint64_t comp = FindCharacterComponent(entity);
        if (!comp) { Log("[SKEL] GetBones: no char comp\n"); g_bindFailCount++; return false; }

        RegistrySlot rig{};
        if (!BindRigToPlayer(comp, rig)) { Log("[SKEL] GetBones: no rig bind comp=0x%llX\n", (unsigned long long)comp); g_bindFailCount++; return false; }
        g_bindSuccessCount++;

        const WorldMatrix wm = ReadWorldMatrix(comp);
        if (!wm.valid) { Log("[SKEL] GetBones: world matrix invalid comp=0x%llX\n", (unsigned long long)comp); return false; }

        // ═══ HASH-BASED ANIMATED BONE PATH ═══
        // Read bind pose from comp+0x1A8 to learn hash→index mapping, then use
        // that mapping to directly index into the animated registry palette.
        // This bypasses the layout solver entirely and gets real animated bones.
        {
            // Read bind pose from comp+0x1A8. The bind pose entries may use a
            // different stride than the animated palette (0x40). Try 0x30
            // first (legacy), then 0x40 (matches the animated palette stride).
            uint64_t bonesPtr = 0;
            if (ReadRaw(comp + 0x1A8, &bonesPtr, sizeof(bonesPtr)) && ValidPtr(bonesPtr)) {
                constexpr uint32_t kMaxBones = 256;
                static thread_local std::vector<uint8_t> fbuf;
                bool bindOk = false;
                uint32_t kEntry = 0x30;

                // Try both strides; the one that yields more known hash hits wins.
                for (uint32_t tryEntry : { 0x30u, 0x40u }) {
                    kEntry = tryEntry;
                    fbuf.resize(kMaxBones * kEntry);
                    if (!ReadRaw(bonesPtr, fbuf.data(), kMaxBones * kEntry)) continue;
                    // Find hash offset within entries
                    auto isKnownHash = [](uint32_t v) -> int {
                        return BoneHashToIndex(v);
                    };

                    int bestHashOff = -1, bestHashScore = 0;
                    for (uint32_t off = 0; off + 4 <= kEntry; off += 4) {
                        int score = 0;
                        for (uint32_t i = 0; i < kMaxBones; i++) {
                            uint32_t v = *(uint32_t*)(fbuf.data() + i * kEntry + off);
                            if (isKnownHash(v) >= 0) score++;
                            if (v == 0) break;
                        }
                        if (score > bestHashScore) { bestHashScore = score; bestHashOff = (int)off; }
                    }

                    if (bestHashScore >= 6) {
                        // Find position offset within entries
                        int bestTransOff = -1, bestTransScore = 0;
                        for (uint32_t tr = 0; tr + 12 <= kEntry; tr += 4) {
                            if ((int)tr == bestHashOff || (int)tr == bestHashOff - 4) continue;
                            int score = 0, checked = 0;
                            for (uint32_t i = 0; i < kMaxBones && checked < 12; i++) {
                                uint32_t v = *(uint32_t*)(fbuf.data() + i * kEntry + bestHashOff);
                                if (isKnownHash(v) < 0) continue;
                                checked++;
                                float x = *(float*)(fbuf.data() + i * kEntry + tr);
                                float y = *(float*)(fbuf.data() + i * kEntry + tr + 4);
                                float z = *(float*)(fbuf.data() + i * kEntry + tr + 8);
                                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
                                float r = sqrtf(x*x + y*y + z*z);
                                if (r > 100000.f) continue;
                                score++;
                            }
                            if (score > bestTransScore) { bestTransScore = score; bestTransOff = (int)tr; }
                        }

                        if (bestTransOff >= 0 && bestTransScore >= 4) {
                            // Learn hash→index mapping from bind pose
                            int idxOf[BONE_COUNT];
                            for (int b = 0; b < BONE_COUNT; b++) idxOf[b] = -1;
                            for (uint32_t i = 0; i < kMaxBones; i++) {
                                uint32_t hv = *(uint32_t*)(fbuf.data() + i * kEntry + bestHashOff);
                                if (hv == 0) break;
                                int bidx = isKnownHash(hv);
                                if (bidx >= 0 && bidx < BONE_COUNT && idxOf[bidx] < 0)
                                    idxOf[bidx] = (int)i;
                            }

                            // Read animated palette using hash mapping
                            const size_t bytes = (size_t)rig.count * kBoneStride;
                            static thread_local std::vector<uint8_t> palette;
                            palette.resize(bytes);
                            if (ReadRaw(rig.palette, palette.data(), palette.size())) {
                                int nbones = 0;
                                for (int b = 0; b < BONE_COUNT; b++) {
                                    int slot = idxOf[b];
                                    if (slot < 0 || slot >= (int)rig.count) continue;
                                    const uint8_t* e = palette.data() + (size_t)slot * kBoneStride + kBoneTranslate;
                                    float x = *(float*)(e + 0);
                                    float y = *(float*)(e + 4);
                                    float z = *(float*)(e + 8);
                                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;

                                    // Check if palette is world-space
                                    float dx = x - wm.m[3][0];
                                    float dy = y - wm.m[3][1];
                                    float dz = z - wm.m[3][2];
                                    float distToLive = sqrtf(dx*dx + dy*dy + dz*dz);
                                    bool worldSpace = (distToLive < 3.0f);

                                    if (worldSpace) {
                                        out_world[b] = { x, y, z };
                                    } else {
                                        out_world[b] = ToWorld(wm, { x, y, z });
                                    }
                                    out_mask |= (1u << b);
                                    nbones++;
                                }
                                Log("[SKEL] GetBones: HASH path OK %d bones (hits=%d)\n", nbones, bestHashScore);
                                if (out_mask != 0) return true;
                            }
                        }
                    }
                }
            }
        }

        // ═══ FALLBACK: LAYOUT SOLVER PATH ═══
        const size_t bytes = (size_t)rig.count * kBoneStride;
        static thread_local std::vector<uint8_t> palette;
        palette.resize(bytes);
        if (!ReadRaw(rig.palette, palette.data(), palette.size())) { Log("[SKEL] GetBones: palette read fail\n"); return false; }

        Vec3f cand[256];
        const int ncand = ReadPaletteBones(palette.data(), palette.size(),
                                           rig.count, cand, 256);
        if (ncand <= 0) { Log("[SKEL] GetBones: no bones in palette\n"); return false; }

        Vec3f palCentroid{};
        for (int i = 0; i < ncand; ++i) {
            palCentroid.x += cand[i].x;
            palCentroid.y += cand[i].y;
            palCentroid.z += cand[i].z;
        }
        palCentroid.x /= ncand;
        palCentroid.y /= ncand;
        palCentroid.z /= ncand;
        const float dx = palCentroid.x - wm.m[3][0];
        const float dy = palCentroid.y - wm.m[3][1];
        const float dz = palCentroid.z - wm.m[3][2];
        const float distToLive = sqrtf(dx*dx + dy*dy + dz*dz);
        const bool paletteIsWorldSpace = (distToLive < 3.0f);

        int idx[BONE_COUNT];
        bool have = false;

        auto cached = g_layoutByContext.find(rig.context);
        if (cached != g_layoutByContext.end()) {
            memcpy(idx, cached->second.idx, sizeof(idx));
            have = true;
        }
        if (!have && AdoptFromDonor(cand, ncand, rig.count, idx)) {
            Layout l; memcpy(l.idx, idx, sizeof(idx));
            g_layoutByContext[rig.context] = l;
            have = true;
        }
        if (!have) {
            float err = 0.f;
            const char* pose = "?";
            const int hits = SolveLayoutMulti(cand, ncand, idx, err, &pose);
            Log("[SKEL] GetBones: layout solve pose=%s hits=%d err=%.3f worldSpace=%d dist=%.1f\n",
                pose, hits, err, paletteIsWorldSpace ? 1 : 0, distToLive);
            if (hits >= 8 && err < kAcceptError) {
                Layout l; memcpy(l.idx, idx, sizeof(idx));
                g_layoutByContext[rig.context] = l;
                RememberDonor(cand, ncand, rig.count, idx);
                have = true;
            } else {
                return false;
            }
        }

        for (int b = 0; b < BONE_COUNT; ++b) {
            if (idx[b] < 0 || idx[b] >= ncand) continue;
            if (paletteIsWorldSpace) {
                out_world[b] = cand[idx[b]];
            } else {
                out_world[b] = ToWorld(wm, cand[idx[b]]);
            }
            out_mask |= (1u << b);
        }
        return out_mask != 0;
    }

    template <class W2SFn, class DrawLineFn>
    inline void DrawSkeleton(const Vec3f bones[BONE_COUNT], uint32_t mask,
                             W2SFn w2s, DrawLineFn line)
    {
        for (const auto& c : kConnections) {
            if (!(mask & (1u << c.first)) || !(mask & (1u << c.second))) continue;
            const auto a = w2s(bones[c.first]);
            const auto b = w2s(bones[c.second]);
            if (a.x == 0.f && a.y == 0.f) continue;
            if (b.x == 0.f && b.y == 0.f) continue;
            line(a, b);
        }
    }



    inline bool ScanRegistry()
    {
        std::lock_guard<std::mutex> scanLock(g_registryScanMutex);
        if (g_registryBase) return true;
        if (!processID)     return false;

        HANDLE hP = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processID);
        if (!hP) { Log("[SKEL] OpenProcess failed (err=%lu)\n", GetLastError()); return false; }

        constexpr size_t CHUNK = 0x100000;
        std::vector<uint8_t> buf;
        size_t regions_scanned = 0, bytes_scanned = 0, hits = 0;
        uintptr_t addr = 0;
        MEMORY_BASIC_INFORMATION mbi{};

        auto slot_valid = [](const RegistrySlot& s) {
            if (!s.seq_begin || s.seq_begin != s.seq_end) return false;
            if (s.count < 8 || s.count > 512) return false;
            if (!ValidPtr(s.palette) || !ValidPtr(s.context)) return false;
            return true;
        };

        while (VirtualQueryEx(hP, (LPCVOID)addr, &mbi, sizeof(mbi))) {
            uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
                mbi.RegionSize >= sizeof(RegistrySlot) * kRegistryCapacity)
            {
                regions_scanned++;
                uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress;
                size_t sz = mbi.RegionSize;

                size_t done = 0;
                while (done < sz) {
                    size_t rd = sz - done;
                    if (rd > CHUNK) rd = CHUNK;
                    if (buf.size() < rd) buf.resize(rd);
                    if (driver->ReadProcessMemory(base + done, buf.data(), (uint32_t)rd) != 0) {
                        done += CHUNK; continue;
                    }
                    bytes_scanned += rd;


                    for (size_t off = 0; off + sizeof(RegistrySlot) * 4 <= rd; off += 8) {
                        const RegistrySlot* s0 = (const RegistrySlot*)(buf.data() + off);
                        int valid_in_run = 0;
                        for (int i = 0; i < (int)kRegistryCapacity; ++i) {
                            if (off + sizeof(RegistrySlot) * (i + 1) > rd) break;
                            if (slot_valid(s0[i])) valid_in_run++;
                        }
                        if (valid_in_run >= 12) {

                            uint64_t slots_addr = base + done + off;
                            g_registryBase = slots_addr - kRegistrySlotsOffset;
                            hits++;
                            Log("[SKEL] Registry FOUND: base=0x%llX slots=0x%llX validSlots=%d/%u region=0x%llX+%zu\n",
                                (unsigned long long)g_registryBase, (unsigned long long)slots_addr,
                                valid_in_run, kRegistryCapacity,
                                (unsigned long long)base, sz);
                            CloseHandle(hP);
                            return true;
                        }
                    }

                    size_t adv = (rd > sizeof(RegistrySlot) * kRegistryCapacity)
                                   ? (rd - sizeof(RegistrySlot) * kRegistryCapacity + 8) : rd;
                    done += adv;
                }
            }
            if (next <= addr) break;
            addr = next;
        }

        CloseHandle(hP);
        Log("[SKEL] Registry NOT FOUND (scanned %zu regions, %zu bytes, %zu hits)\n",
               regions_scanned, bytes_scanned, hits);
        return false;
    }

    inline bool Ready() { return g_registryBase != 0; }




    struct PaletteSnapshot {
        Vec3f    centroid;
        uint64_t palette;
        uint64_t context;
        uint32_t count;
        float    spread;
    };
    inline std::vector<PaletteSnapshot> g_paletteSnapshots;
    inline std::mutex                    g_snapMtx;
    inline uint64_t                      g_lastSnap_ms = 0;
    inline uint64_t                      g_snapCounter = 0;
    inline bool                          g_snapWorldSpace = true;

    inline void RefreshPaletteSnapshots()
    {
        uint64_t now_ms = NowMicros() / 1000;
        if (g_lastSnap_ms && (now_ms - g_lastSnap_ms) < 33) return;
        g_lastSnap_ms = now_ms;
        if (!g_registryBase) return;

        RegistrySlot slots[kRegistryCapacity]{};
        if (!ReadRaw(g_registryBase + kRegistrySlotsOffset, slots, sizeof(slots))) {
            static uint64_t s_lastReadFail = 0;
            if (now_ms - s_lastReadFail > 2000) {
                s_lastReadFail = now_ms;
                Log("[SKEL-SNAP] read of registry slots FAILED at 0x%llX\n",
                    (unsigned long long)(g_registryBase + kRegistrySlotsOffset));
            }
            return;
        }

        std::vector<PaletteSnapshot> fresh;
        fresh.reserve(64);


        int nz_seq = 0, seq_match = 0, count_ok = 0, ptr_ok = 0, palette_read_ok = 0, spread_ok = 0;
        for (const RegistrySlot& s : slots) {
            if (s.seq_begin) nz_seq++;
            if (s.seq_begin && s.seq_begin == s.seq_end) seq_match++;
            if (s.count >= 8 && s.count <= 512) count_ok++;
            if (ValidPtr(s.palette)) ptr_ok++;

            if (!s.seq_begin || s.seq_begin != s.seq_end) continue;
            if (s.count < 8 || s.count > 512) continue;
            if (!ValidPtr(s.palette)) continue;

            const uint32_t stride = (uint32_t)kBoneStride;
            const uint32_t trans  = (uint32_t)kBoneTranslate;
            const size_t   bytes  = (size_t)s.count * stride;
            if (bytes > 0x20000) continue;

            static thread_local std::vector<uint8_t> pbuf;
            pbuf.resize(bytes);
            if (!ReadRaw(s.palette, pbuf.data(), bytes)) continue;
            palette_read_ok++;

            Vec3f centroid{}; int n = 0;
            float minx =  1e9f, miny =  1e9f, minz =  1e9f;
            float maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
            for (uint32_t i = 0; i < s.count; ++i) {
                Vec3f v;
                memcpy(&v, pbuf.data() + (size_t)i * stride + trans, 12);
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) continue;
                if (fabsf(v.x) > 100000.f || fabsf(v.y) > 100000.f || fabsf(v.z) > 100000.f) continue;
                centroid.x += v.x; centroid.y += v.y; centroid.z += v.z;
                if (v.x < minx) minx = v.x; if (v.x > maxx) maxx = v.x;
                if (v.y < miny) miny = v.y; if (v.y > maxy) maxy = v.y;
                if (v.z < minz) minz = v.z; if (v.z > maxz) maxz = v.z;
                ++n;
            }
            if (n < 8) continue;
            centroid.x /= n; centroid.y /= n; centroid.z /= n;

            float sx = maxx - minx, sy = maxy - miny, sz = maxz - minz;
            float spread = fmaxf(sx, fmaxf(sy, sz));
            if (spread < 0.4f || spread > 500.f) continue;
            spread_ok++;

            fresh.push_back({ centroid, s.palette, s.context, s.count, spread });
        }

        if ((g_snapCounter % 60) == 0) {
            Log("[SKEL-SNAP-STAGE] nz_seq=%d seq_match=%d count_ok=%d ptr_ok=%d palette_read_ok=%d spread_ok=%d slot0.seq=%u slot0.cnt=%u slot0.pal=0x%llX slot0.ctx=0x%llX\n",
                nz_seq, seq_match, count_ok, ptr_ok, palette_read_ok, spread_ok,
                slots[0].seq_begin, slots[0].count,
                (unsigned long long)slots[0].palette, (unsigned long long)slots[0].context);
        }


        int nearOrigin = 0;
        for (const auto& p : fresh) {
            float m2 = p.centroid.x*p.centroid.x + p.centroid.y*p.centroid.y + p.centroid.z*p.centroid.z;
            if (m2 < 100.f) nearOrigin++;
        }
        bool worldSpace = fresh.empty() ? true : (nearOrigin * 2 < (int)fresh.size());


        {
            std::lock_guard<std::mutex> lk(g_snapMtx);
            g_paletteSnapshots = std::move(fresh);
            g_snapWorldSpace   = worldSpace;
            g_snapCounter++;
        }

        if ((g_snapCounter % 60) == 1) {
            Log("[SKEL-SNAP] n=%zu worldSpace=%d nearOrigin=%d/%zu",
                g_paletteSnapshots.size(), worldSpace ? 1 : 0, nearOrigin, g_paletteSnapshots.size());
            for (size_t i = 0; i < g_paletteSnapshots.size() && i < 4; ++i) {
                const auto& p = g_paletteSnapshots[i];
                Log(" | slot%zu cent=(%.1f,%.1f,%.1f) cnt=%u spread=%.2f", i,
                    p.centroid.x, p.centroid.y, p.centroid.z, p.count, p.spread);
            }
            Log("\n");
        }
    }


    inline bool ReadPaletteBonesRaw(uint64_t palette, uint32_t count,
                                    Vec3f* out, int max_out, int& out_count)
    {
        const uint32_t stride = (uint32_t)kBoneStride;
        const uint32_t trans  = (uint32_t)kBoneTranslate;
        size_t bytes = (size_t)count * stride;
        if (count < 17 || bytes > 0x20000 || max_out < 17) return false;
        static thread_local std::vector<uint8_t> pbuf;
        pbuf.resize(bytes);
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (!ReadRaw(palette, pbuf.data(), bytes)) continue;
            int n = 0;
            float minX = FLT_MAX, maxX = -FLT_MAX;
            float minY = FLT_MAX, maxY = -FLT_MAX;
            float minZ = FLT_MAX, maxZ = -FLT_MAX;
            for (uint32_t index = 0; index < count && n < max_out; ++index) {
                const uint8_t* entry = pbuf.data() + (size_t)index * stride;
                float homogeneous = 0.f, padding[3]{};
                memcpy(&homogeneous, entry + 0x3C, sizeof(float));
                for (int row = 0; row < 3; ++row)
                    memcpy(&padding[row], entry + row * 0x10 + 0xC, sizeof(float));
                if (!std::isfinite(homogeneous) || !std::isfinite(padding[0]) ||
                    !std::isfinite(padding[1]) || !std::isfinite(padding[2]) ||
                    fabsf(homogeneous - 1.f) > 0.001f ||
                    fabsf(padding[0]) > 0.001f || fabsf(padding[1]) > 0.001f ||
                    fabsf(padding[2]) > 0.001f) break;
                Vec3f value;
                memcpy(&value, entry + trans, sizeof(value));
                if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
                    !std::isfinite(value.z)) break;
                out[n++] = value;
                minX = (std::min)(minX, value.x); maxX = (std::max)(maxX, value.x);
                minY = (std::min)(minY, value.y); maxY = (std::max)(maxY, value.y);
                minZ = (std::min)(minZ, value.z); maxZ = (std::max)(maxZ, value.z);
            }
            const float width = (std::max)(maxX - minX, maxY - minY);
            const float height = maxZ - minZ;
            if (n >= 17 && height >= 1.4f && height <= 2.1f &&
                width < 1.f && height >= 1.6f * width) {
                out_count = n;
                return true;
            }
        }
        out_count = 0;
        return false;
    }


    inline bool GetPaletteForWorldPos(float wx, float wy, float wz,
                                      Vec3f* out_bones, int max_out, int& out_count,
                                      float maxMatchDist = 3.0f)
    {
        RefreshPaletteSnapshots();
        std::lock_guard<std::mutex> lk(g_snapMtx);
        if (g_paletteSnapshots.empty() || !g_snapWorldSpace) return false;

        int best = -1; float bestD = maxMatchDist;
        Vec3f target{ wx, wy, wz };
        for (size_t i = 0; i < g_paletteSnapshots.size(); ++i) {
            Vec3f d = g_paletteSnapshots[i].centroid - target;
            float dd = d.Length();
            if (dd < bestD) { bestD = dd; best = (int)i; }
        }
        if (best < 0) return false;
        const auto& snap = g_paletteSnapshots[best];
        return ReadPaletteBonesRaw(snap.palette, snap.count, out_bones, max_out, out_count);
    }

    // Position-matched palette resolver that works regardless of local/world
    // storage. Bypasses the character-component / tag lookup entirely.
    //   - If palettes are world-space: centroid == world position of the rig.
    //     Nearest centroid to draw_pos is this entity's rig.
    //   - If palettes are local-space: centroids all cluster near origin.
    //     We can't match by position — return the first plausible palette
    //     that isn't already claimed by another entity (allocation-order
    //     fallback), which is often correct for the local player.
    // Either way, downstream centers the bones around the palette's centroid
    // and translates by draw_pos, so world position is always draw_pos-anchored
    // and rotation comes from the palette itself.
    struct MatchedPalette {
        Vec3f    centroid;
        uint64_t palette;
        uint64_t context;
        uint32_t count;
        bool     isWorldSpace;
        float    matchDist;
    };

    inline std::unordered_map<uint64_t, uint64_t> g_ctxToEntity;  // palette context -> owning entity
    inline uint64_t g_ctxToEntity_lastReset_ms = 0;

    inline bool ResolvePaletteByPosition(uint64_t entity, float wx, float wy, float wz,
                                         MatchedPalette& out, float maxMatchDist = 4.0f)
    {
        RefreshPaletteSnapshots();
        std::lock_guard<std::mutex> lk(g_snapMtx);
        if (g_paletteSnapshots.empty()) return false;

        const uint64_t now_ms = NowMicros() / 1000;
        // reset context->entity claims every second so palettes can re-bind
        // when entities die / respawn.
        if (now_ms - g_ctxToEntity_lastReset_ms > 1000) {
            g_ctxToEntity.clear();
            g_ctxToEntity_lastReset_ms = now_ms;
        }

        if (g_snapWorldSpace) {
            int best = -1; float bestD = maxMatchDist;
            for (size_t i = 0; i < g_paletteSnapshots.size(); ++i) {
                const auto& p = g_paletteSnapshots[i];
                float dx = p.centroid.x - wx, dy = p.centroid.y - wy, dz = p.centroid.z - wz;
                float dd = sqrtf(dx*dx + dy*dy + dz*dz);
                if (dd < bestD) { bestD = dd; best = (int)i; }
            }
            if (best < 0) return false;
            const auto& snap = g_paletteSnapshots[best];
            out = { snap.centroid, snap.palette, snap.context, snap.count, true, bestD };
            g_ctxToEntity[snap.context] = entity;
            return true;
        }

        // Local-space path: pick first unclaimed palette. Not order-ideal but
        // gives us SOMETHING to draw; correctness will follow when we identify
        // char-comp offsets.
        for (const auto& snap : g_paletteSnapshots) {
            auto it = g_ctxToEntity.find(snap.context);
            if (it != g_ctxToEntity.end() && it->second != entity) continue;
            out = { snap.centroid, snap.palette, snap.context, snap.count, false, 0.f };
            g_ctxToEntity[snap.context] = entity;
            return true;
        }
        return false;
    }




    inline uint64_t g_directRigOff       = 0;
    inline uint32_t g_directRigStride    = 0;
    inline uint32_t g_directRigTrans     = 0;
    inline bool     g_directRigIsWorld   = false;
    inline uint32_t g_directRigCount     = 0;


    struct DirectCache { Layout layout; bool has = false; };
    inline std::unordered_map<uint64_t, DirectCache> g_directLayoutByComp;


    inline uint64_t g_lastAutodetectMs = 0;
    inline uint64_t g_autodetectAttempts = 0;




    inline uint64_t g_rawRigOff       = 0;
    inline uint32_t g_rawRigStride    = 0;
    inline uint32_t g_rawRigTrans     = 0;
    inline uint32_t g_rawRigCount     = 0;
    inline bool     g_rawRigIsWorld   = false;
    inline float    g_rawRigScoreBest = 0.f;


    inline float SkeletonScore(const Vec3f* pts, int n, float spread, uint32_t count)
    {
        if (n < 20 || n > 150) return 0.f;
        if (spread < 1.2f || spread > 3.5f) return 0.f;


        float sumMinD = 0.f; int contrib = 0;
        for (int i = 0; i < n; ++i) {
            float best = 1e9f;
            for (int j = 0; j < n; ++j) {
                if (i == j) continue;
                float dx = pts[i].x - pts[j].x;
                float dy = pts[i].y - pts[j].y;
                float dz = pts[i].z - pts[j].z;
                float d = sqrtf(dx*dx + dy*dy + dz*dz);
                if (d < best) best = d;
            }
            if (best < 1.f) { sumMinD += best; contrib++; }
        }
        if (contrib < n / 2) return 0.f;
        float avgNN = sumMinD / contrib;

        if (avgNN < 0.005f || avgNN > 0.4f) return 0.f;


        float score = 1.f / (avgNN + 0.02f);

        if (spread >= 1.5f && spread <= 2.2f) score *= 2.f;

        if (count >= 40 && count <= 120) score *= 1.5f;
        return score;
    }

    inline bool ScanCompForRawRig(uint64_t comp)
    {
        constexpr size_t CHUNK = 0x4000;
        static thread_local std::vector<uint8_t> cbuf;
        cbuf.resize(CHUNK);
        if (!ReadRaw(comp, cbuf.data(), CHUNK)) return false;

        const uint32_t strides[]   = { 0x30, 0x40, 0x48, 0x60 };
        const uint32_t trans_off[] = { 0x00, 0x10, 0x20, 0x30 };
        constexpr uint32_t kNTry   = 80;


        struct Hit { uint64_t off; uint32_t stride, trans, count; float spread; float score; bool isWorld; };
        Hit best{0,0,0,0,0.f,0.f,false};
        int nCandidates = 0;

        for (size_t off = 0x40; off + 8 <= CHUNK; off += 8) {
            uint64_t palette = *(uint64_t*)(cbuf.data() + off);
            if (!ValidPtr(palette)) continue;

            for (uint32_t stride : strides) {
                for (uint32_t trans : trans_off) {
                    if (trans + 12 > stride) continue;

                    static thread_local std::vector<uint8_t> pbuf;
                    pbuf.resize((size_t)stride * kNTry);
                    if (!ReadRaw(palette, pbuf.data(), pbuf.size())) continue;

                    Vec3f pos[128]; int n = 0;
                    for (uint32_t i = 0; i < kNTry && n < 128; ++i) {
                        Vec3f p;
                        memcpy(&p, pbuf.data() + (size_t)i * stride + trans, 12);
                        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) break;
                        if (fabsf(p.x) > 50000.f || fabsf(p.y) > 50000.f || fabsf(p.z) > 50000.f) break;
                        pos[n++] = p;
                    }
                    if (n < 20) continue;

                    float mn[3] = { 1e9f,1e9f,1e9f }, mx[3] = {-1e9f,-1e9f,-1e9f };
                    Vec3f centroid{};
                    for (int i = 0; i < n; ++i) {
                        centroid.x += pos[i].x; centroid.y += pos[i].y; centroid.z += pos[i].z;
                        if (pos[i].x<mn[0]) mn[0]=pos[i].x; if (pos[i].x>mx[0]) mx[0]=pos[i].x;
                        if (pos[i].y<mn[1]) mn[1]=pos[i].y; if (pos[i].y>mx[1]) mx[1]=pos[i].y;
                        if (pos[i].z<mn[2]) mn[2]=pos[i].z; if (pos[i].z>mx[2]) mx[2]=pos[i].z;
                    }
                    centroid.x /= n; centroid.y /= n; centroid.z /= n;
                    float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];
                    float spread = fmaxf(sx, fmaxf(sy, sz));


                    float score = SkeletonScore(pos, n, spread, (uint32_t)n);
                    if (score <= 0.f) continue;
                    nCandidates++;

                    if (score > best.score) {
                        best = { (uint64_t)off, stride, trans, (uint32_t)n, spread, score,
                                 fabsf(centroid.x) > 10.f || fabsf(centroid.y) > 10.f || fabsf(centroid.z) > 10.f };
                    }
                }
            }
        }

        if (best.score <= 0.f) {
            Log("[SKEL-RAW] no candidate found at comp 0x%llX (0 scored)\n", (unsigned long long)comp);
            return false;
        }

        g_rawRigOff       = best.off;
        g_rawRigStride    = best.stride;
        g_rawRigTrans     = best.trans;
        g_rawRigCount     = best.count;
        g_rawRigIsWorld   = best.isWorld;
        g_rawRigScoreBest = best.score;

        Log("[SKEL-RAW] LOCKED: compOff=+0x%llX stride=0x%X trans=0x%X n=%u spread=%.2f score=%.2f world=%d (from %d scored candidates)\n",
            (unsigned long long)best.off, best.stride, best.trans, best.count,
            best.spread, best.score, best.isWorld ? 1 : 0, nCandidates);
        return true;
    }


    inline uint64_t g_entityRigOff = 0;
    inline uint32_t g_entityRigStride = 0;
    inline uint32_t g_entityRigTrans = 0;
    inline uint32_t g_entityRigCount = 0;
    inline bool     g_entityRigWorld = false;
    inline bool     g_entityRigTried = false;
    inline DWORD    g_entityRigAttempt = 0;

    inline bool ScanEntityForRig(uint64_t entity)
    {
        if (g_entityRigOff) return true;
        DWORD now = GetTickCount();
        if (g_entityRigTried && (now - g_entityRigAttempt < 3000)) return false;
        g_entityRigTried = true;
        g_entityRigAttempt = now;

        if (!ValidPtr(entity)) return false;

        constexpr size_t CHUNK = 0x2000;
        static thread_local std::vector<uint8_t> ebuf;
        ebuf.resize(CHUNK);
        if (!ReadRaw(entity, ebuf.data(), CHUNK)) { Log("[SKEL-ENT] read fail entity=0x%llX\n", (unsigned long long)entity); return false; }

        const uint32_t strides[]   = { 0x20, 0x30, 0x40, 0x48, 0x60 };
        const uint32_t trans_off[] = { 0x00, 0x10, 0x20, 0x30 };
        constexpr uint32_t kNTry   = 80;

        struct Hit { uint64_t off; uint32_t stride, trans, count; float spread, score; bool isWorld; };
        Hit best{0,0,0,0,0.f,0.f,false};
        int nCandidates = 0;

        for (size_t off = 0x20; off + 8 <= CHUNK; off += 8) {
            uint64_t palette = *(uint64_t*)(ebuf.data() + off);
            if (!ValidPtr(palette)) continue;

            for (uint32_t stride : strides) {
                for (uint32_t trans : trans_off) {
                    if (trans + 12 > stride) continue;

                    static thread_local std::vector<uint8_t> pbuf;
                    pbuf.resize((size_t)stride * kNTry);
                    if (!ReadRaw(palette, pbuf.data(), pbuf.size())) continue;

                    Vec3f pos[128]; int n = 0;
                    for (uint32_t i = 0; i < kNTry && n < 128; ++i) {
                        Vec3f p;
                        memcpy(&p, pbuf.data() + (size_t)i * stride + trans, 12);
                        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) break;
                        if (fabsf(p.x) > 50000.f || fabsf(p.y) > 50000.f || fabsf(p.z) > 50000.f) break;
                        pos[n++] = p;
                    }
                    if (n < 17) continue;

                    float mn[3] = {1e9f,1e9f,1e9f}, mx[3] = {-1e9f,-1e9f,-1e9f};
                    Vec3f centroid{};
                    for (int i = 0; i < n; ++i) {
                        centroid.x += pos[i].x; centroid.y += pos[i].y; centroid.z += pos[i].z;
                        if (pos[i].x<mn[0]) mn[0]=pos[i].x; if (pos[i].x>mx[0]) mx[0]=pos[i].x;
                        if (pos[i].y<mn[1]) mn[1]=pos[i].y; if (pos[i].y>mx[1]) mx[1]=pos[i].y;
                        if (pos[i].z<mn[2]) mn[2]=pos[i].z; if (pos[i].z>mx[2]) mx[2]=pos[i].z;
                    }
                    centroid.x /= n; centroid.y /= n; centroid.z /= n;
                    float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];
                    float spread = fmaxf(sx, fmaxf(sy, sz));
                    if (spread < 0.4f || spread > 500.f) continue;

                    float score = SkeletonScore(pos, n, spread, (uint32_t)n);
                    if (score <= 0.f) continue;
                    nCandidates++;

                    if (score > best.score) {
                        best = { (uint64_t)off, stride, trans, (uint32_t)n, spread, score,
                                 fabsf(centroid.x) > 10.f || fabsf(centroid.y) > 10.f || fabsf(centroid.z) > 10.f };
                    }
                }
            }
        }

        if (best.score <= 0.f) {
            Log("[SKEL-ENT] no rig at entity 0x%llX (scanned %d candidates)\n", (unsigned long long)entity, nCandidates);
            return false;
        }

        g_entityRigOff    = best.off;
        g_entityRigStride  = best.stride;
        g_entityRigTrans   = best.trans;
        g_entityRigCount   = best.count;
        g_entityRigWorld   = best.isWorld;
        Log("[SKEL-ENT] RIG LOCKED: entityOff=+0x%llX stride=0x%X trans=0x%X n=%u spread=%.2f score=%.2f world=%d (%d candidates)\n",
            (unsigned long long)best.off, best.stride, best.trans, best.count, best.spread, best.score, (int)best.isWorld, nCandidates);
        return true;
    }

    inline bool GetBonesFromEntity(uint64_t entity, Vec3f* out_bones, int max_out, int& out_count)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        out_count = 0;
        if (!ValidPtr(entity)) return false;

        if (!g_entityRigOff) {
            if (!ScanEntityForRig(entity)) return false;
        }

        uint64_t palette = Read<uint64_t>(entity + g_entityRigOff);
        if (!ValidPtr(palette)) { g_entityRigOff = 0; return false; }

        static thread_local std::vector<uint8_t> pbuf;
        size_t bytes = (size_t)g_entityRigStride * g_entityRigCount;
        pbuf.resize(bytes);
        if (!ReadRaw(palette, pbuf.data(), bytes)) return false;

        int n = 0;
        for (uint32_t i = 0; i < g_entityRigCount && n < max_out; ++i) {
            Vec3f p;
            memcpy(&p, pbuf.data() + (size_t)i * g_entityRigStride + g_entityRigTrans, 12);
            if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
                out_bones[n++] = p;
        }
        out_count = n;
        Log("[SKEL-ENT] GetBonesFromEntity: %d bones read (count=%u)\n", n, g_entityRigCount);
        return n >= 17;
    }


    inline bool GetBonesRaw(uint64_t entity, Vec3f* out_bones, int max_out, int& out_count)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        out_count = 0;

        const uint64_t comp = FindCharacterComponent(entity);
        if (!comp) return false;

        if (!g_rawRigOff) {

            static uint64_t s_lastScan_ms = 0;
            uint64_t now_ms = NowMicros() / 1000;
            if (s_lastScan_ms && (now_ms - s_lastScan_ms) < 1500) return false;
            s_lastScan_ms = now_ms;
            if (!ScanCompForRawRig(comp)) return false;
        }

        uint64_t palette = Read<uint64_t>(comp + g_rawRigOff);
        if (!ValidPtr(palette)) {

            g_rawRigOff = 0;
            return false;
        }

        static thread_local std::vector<uint8_t> pbuf;
        size_t bytes = (size_t)g_rawRigStride * g_rawRigCount;
        pbuf.resize(bytes);
        if (!ReadRaw(palette, pbuf.data(), bytes)) return false;

        int n = 0;
        for (uint32_t i = 0; i < g_rawRigCount && n < max_out; ++i) {
            Vec3f v;
            memcpy(&v, pbuf.data() + (size_t)i * g_rawRigStride + g_rawRigTrans, 12);
            if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z))
                out_bones[n++] = v;
        }
        out_count = n;
        return n >= 20;
    }

    inline bool AutodetectDirectRig(uint64_t comp)
    {

        uint64_t now_ms = NowMicros() / 1000;
        if (g_lastAutodetectMs && (now_ms - g_lastAutodetectMs) < 1500) return false;
        g_lastAutodetectMs = now_ms;
        g_autodetectAttempts++;

        constexpr size_t CHUNK = 0x4000;
        static thread_local std::vector<uint8_t> cbuf;
        cbuf.resize(CHUNK);
        if (!ReadRaw(comp, cbuf.data(), CHUNK)) {
            Log("[SKEL-DIRECT] comp read FAIL at 0x%llX (attempt %llu)\n",
                (unsigned long long)comp, (unsigned long long)g_autodetectAttempts);
            return false;
        }

        const uint32_t strides[]   = { 0x20, 0x30, 0x40, 0x48, 0x60 };
        const uint32_t trans_off[] = { 0x00, 0x10, 0x20, 0x30 };
        constexpr uint32_t kNTry   = 48;

        uint32_t stat_ptrs = 0, stat_valid = 0, stat_read = 0, stat_spread = 0, stat_solver = 0;

        for (size_t off = 0x40; off + 8 <= CHUNK; off += 8) {
            uint64_t palette = *(uint64_t*)(cbuf.data() + off);
            stat_ptrs++;
            if (!ValidPtr(palette)) continue;
            stat_valid++;

            for (uint32_t stride : strides) {
                for (uint32_t trans : trans_off) {
                    if (trans + 12 > stride) continue;

                    static thread_local std::vector<uint8_t> pbuf;
                    pbuf.resize((size_t)stride * kNTry);
                    if (!ReadRaw(palette, pbuf.data(), pbuf.size())) continue;
                    stat_read++;

                    Vec3f pos[64]; int n = 0;
                    for (uint32_t i = 0; i < kNTry && n < 64; ++i) {
                        Vec3f p;
                        memcpy(&p, pbuf.data() + (size_t)i * stride + trans, 12);
                        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) break;
                        if (fabsf(p.x) > 50000.f || fabsf(p.y) > 50000.f || fabsf(p.z) > 50000.f) break;
                        pos[n++] = p;
                    }
                    if (n < 17) continue;

                    float mn[3] = {  1e9f,  1e9f,  1e9f };
                    float mx[3] = { -1e9f, -1e9f, -1e9f };
                    for (int i = 0; i < n; ++i) {
                        if (pos[i].x < mn[0]) mn[0] = pos[i].x; if (pos[i].x > mx[0]) mx[0] = pos[i].x;
                        if (pos[i].y < mn[1]) mn[1] = pos[i].y; if (pos[i].y > mx[1]) mx[1] = pos[i].y;
                        if (pos[i].z < mn[2]) mn[2] = pos[i].z; if (pos[i].z > mx[2]) mx[2] = pos[i].z;
                    }
                    float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];
                    float maxS = fmaxf(sx, fmaxf(sy, sz));

                    if (maxS < 0.4f || maxS > 500.f) continue;
                    stat_spread++;

                    Vec3f centroid{};
                    for (int i = 0; i < n; ++i) { centroid.x += pos[i].x; centroid.y += pos[i].y; centroid.z += pos[i].z; }
                    centroid.x /= n; centroid.y /= n; centroid.z /= n;

                    Vec3f centered[64];
                    for (int i = 0; i < n; ++i)
                        centered[i] = { pos[i].x - centroid.x, pos[i].y - centroid.y, pos[i].z - centroid.z };


                    if (sy > sz * 1.3f && sy > sx * 1.3f)
                        for (int i = 0; i < n; ++i) std::swap(centered[i].y, centered[i].z);


                    float scale = 1.7f / maxS;
                    Vec3f scaled[64];
                    for (int i = 0; i < n; ++i)
                        scaled[i] = { centered[i].x * scale, centered[i].y * scale, centered[i].z * scale };

                    int idx[BONE_COUNT]; float err = 0.f;
                    int hits = SolveLayout(scaled, n, idx, err);
                    if (hits < 10) continue;
                    stat_solver++;
                    if (err > 0.35f) continue;

                    g_directRigOff     = off;
                    g_directRigStride  = stride;
                    g_directRigTrans   = trans;
                    g_directRigCount   = (uint32_t)n;
                    g_directRigIsWorld = (fabsf(centroid.x) > 10.f || fabsf(centroid.y) > 10.f || fabsf(centroid.z) > 10.f);

                    Log("[SKEL-DIRECT] rig autodetected: compOff=+0x%llX stride=0x%X trans=0x%X n=%d hits=%d/%d err=%.3f world=%d spread=(%.2f,%.2f,%.2f) scale=%.3f (attempt %llu)\n",
                        (unsigned long long)off, stride, trans, n, hits, BONE_COUNT, err,
                        g_directRigIsWorld ? 1 : 0, sx, sy, sz, scale, (unsigned long long)g_autodetectAttempts);
                    return true;
                }
            }
        }
        Log("[SKEL-DIRECT] no rig at comp 0x%llX (attempt %llu): ptrs=%u valid=%u read=%u spread=%u solver=%u\n",
            (unsigned long long)comp, (unsigned long long)g_autodetectAttempts,
            stat_ptrs, stat_valid, stat_read, stat_spread, stat_solver);
        return false;
    }

    inline uint64_t g_lastDbg_ms = 0;

    inline bool GetBonesDirect(uint64_t entity, Vec3f out_world[BONE_COUNT], uint32_t& out_mask)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        out_mask = 0;

        const uint64_t comp = FindCharacterComponent(entity);

        uint64_t now_ms = NowMicros() / 1000;
        if (!g_directRigOff && (now_ms - g_lastDbg_ms) > 3000) {
            g_lastDbg_ms = now_ms;
            Log("[SKEL-DIRECT] GetBonesDirect entity=0x%llX comp=0x%llX compArrOff=+0x%llX\n",
                (unsigned long long)entity, (unsigned long long)comp,
                (unsigned long long)g_componentArrayOffset);
        }
        if (!comp) return false;


        if (!g_directRigOff) {
            if (!AutodetectDirectRig(comp)) return false;
        }


        uint64_t palette = Read<uint64_t>(comp + g_directRigOff);
        if (!ValidPtr(palette)) {


            if (!AutodetectDirectRig(comp)) return false;
            palette = Read<uint64_t>(comp + g_directRigOff);
            if (!ValidPtr(palette)) return false;
        }

        const uint32_t nTry = g_directRigCount ? g_directRigCount : 40;
        static thread_local std::vector<uint8_t> pbuf;
        pbuf.resize((size_t)g_directRigStride * nTry);
        if (!ReadRaw(palette, pbuf.data(), pbuf.size())) return false;

        Vec3f raw[64]; int n = 0;
        for (uint32_t i = 0; i < nTry && n < 64; ++i) {
            Vec3f p;
            memcpy(&p, pbuf.data() + (size_t)i * g_directRigStride + g_directRigTrans, 12);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) break;
            raw[n++] = p;
        }
        if (n < 12) return false;


        Vec3f centroid{};
        for (int i = 0; i < n; ++i) { centroid.x += raw[i].x; centroid.y += raw[i].y; centroid.z += raw[i].z; }
        centroid.x /= n; centroid.y /= n; centroid.z /= n;

        Vec3f centered[64];
        for (int i = 0; i < n; ++i)
            centered[i] = { raw[i].x - centroid.x, raw[i].y - centroid.y, raw[i].z - centroid.z };



        static thread_local bool tl_swapYZ = false;
        {
            float mn[3] = {  1e9f,  1e9f,  1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
            for (int i = 0; i < n; ++i) {
                if (centered[i].x < mn[0]) mn[0] = centered[i].x; if (centered[i].x > mx[0]) mx[0] = centered[i].x;
                if (centered[i].y < mn[1]) mn[1] = centered[i].y; if (centered[i].y > mx[1]) mx[1] = centered[i].y;
                if (centered[i].z < mn[2]) mn[2] = centered[i].z; if (centered[i].z > mx[2]) mx[2] = centered[i].z;
            }
            float sy = mx[1]-mn[1], sz = mx[2]-mn[2], sx = mx[0]-mn[0];
            tl_swapYZ = (sy > sz * 1.3f && sy > sx * 1.3f);
            if (tl_swapYZ) for (int i = 0; i < n; ++i) std::swap(centered[i].y, centered[i].z);
        }


        int idx[BONE_COUNT]; bool have = false;

        auto cit = g_directLayoutByComp.find(comp);
        if (cit != g_directLayoutByComp.end() && cit->second.has) {
            memcpy(idx, cit->second.layout.idx, sizeof(idx));
            have = true;
        }
        if (!have) {
            float err = 0;
            int hits = SolveLayout(centered, n, idx, err);
            if (hits >= 12 && err < 0.9f) {
                DirectCache dc; memcpy(dc.layout.idx, idx, sizeof(idx)); dc.has = true;
                g_directLayoutByComp[comp] = dc;
                have = true;
            }
        }
        if (!have) return false;


        // Resolve world transform ONCE per call, not per bone.
        // Also: do NOT re-subtract the centroid — palette bones are already
        // rig-local, subtracting again collapses the skeleton toward the origin.
        WorldMatrix wm{};
        bool wmReady = false;
        if (!g_directRigIsWorld) {
            wm = ReadWorldMatrix(comp);
            wmReady = wm.valid;
        }

        for (int b = 0; b < BONE_COUNT; ++b) {
            if (idx[b] < 0 || idx[b] >= n) continue;
            Vec3f p = raw[idx[b]];
            if (tl_swapYZ) std::swap(p.y, p.z);
            if (g_directRigIsWorld) {
                out_world[b] = p;
            } else if (wmReady) {
                out_world[b] = ToWorld(wm, p);
            } else {
                // No usable world matrix and bones aren't world-space —
                // don't emit. Emitting rig-local coords as world is what
                // gave the "all bones clustered at origin" failure mode.
                continue;
            }
            out_mask |= (1u << b);
        }
        return out_mask != 0;
    }


    inline void SyntheticSkeleton(float wx, float wy, float wz, Vec3f out[BONE_COUNT])
    {
        for (int i = 0; i < BONE_COUNT; ++i) out[i] = {};
        for (const RefBone& rb : kReferencePose) {
            out[rb.bone] = { wx + rb.local.x, wy + rb.local.y, wz + rb.local.z };
        }
    }

    inline void RegistryHealthTick(bool had_players)
    {
        const uint64_t now_ms = NowMicros() / 1000;
        if (!g_lastReset_ms) g_lastReset_ms = now_ms;


        if (had_players && (now_ms - g_lastReset_ms) > 15000 &&
            g_bindSuccessCount == 0 && g_bindFailCount > 30 && g_registryBase != 0)
        {
            Log("[SKEL] Registry 0x%llX yielded 0 bindings over 15s / %llu attempts — discarding and re-scanning.\n",
                (unsigned long long)g_registryBase, (unsigned long long)g_bindFailCount);
            g_registryBase = 0;
            g_retained.clear();
            g_layoutByContext.clear();
            g_ctxOwner.clear();
            g_bindSuccessCount = 0;
            g_bindFailCount    = 0;
            g_lastReset_ms     = now_ms;
        }
    }
}
