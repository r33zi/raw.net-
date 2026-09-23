#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <mutex>
#include "skel.h"
#include "skel_autodiscover.h"

struct Vec3B { float x, y, z; };

struct SkeletonBones {
    Vec3B bones[skel::BONE_COUNT];
    bool  hasBone[skel::BONE_COUNT];
    int   total;
};

struct BoneCacheEntry {
    SkeletonBones bones;
    SkeletonBones prev_bones;    // previous frame for temporal smoothing
    uint64_t      last_update_ms;
    uint8_t       consecutive_fails;
    bool          has_prev;      // true if prev_bones is valid
};

inline std::unordered_map<uint64_t, BoneCacheEntry> g_boneCache;
inline std::mutex                                    g_boneCacheMtx;

// Bone discovery performs several pointer walks and bulk reads. Updating it
// at 30 Hz keeps animation smooth while preventing high-refresh overlays from
// issuing the same driver calls multiple times per displayed frame.
static constexpr uint64_t kBoneCacheOkTTL   = 33;
static constexpr uint64_t kBoneCacheFailTTL = 250;

static void FlushBoneCache() {
    std::lock_guard<std::mutex> lk(g_boneCacheMtx);
    g_boneCache.clear();
}

// ═══ SHARED ROTATION ═══
// Every rig-local skeleton path must be re-oriented into the actor's world
// rotation BEFORE being placed at draw_pos. The rotation source is the
// character component's unit rotation quaternion (auto-discovered at
// +0x660/+0x650/+0x670/+0x640/+0x680 — see FindRotQuatOffset in
// r6_entities.h). We convert the quaternion to a row-major 3x3 rotation
// matrix and apply it to each bone's local-space coordinates.
//
// Previous implementation read the 4x4 world matrix at comp+0x240; on the
// current build that offset is stale, which produced skeletons that faced
// the wrong direction. The quaternion is the authoritative rotation.
// ═══ ROTATION DEBUG AID ═══
// Flip switch for field calibration. If — despite the quaternion-derived
// rotation — the skeleton still renders 180° backwards, set kSkelYawFlip
// to true once. (Left in only for the study project.)
static const bool kSkelYawFlip = false;

struct BoneRotationFrame {
    float m[9];   // row-major 3x3 rotation (from quaternion)
    bool  have;
};

static BoneRotationFrame GetBoneRotationFrame(uint64_t entity)
{
    BoneRotationFrame F{};
    if (!skel::ValidPtr(entity)) return F;

    uint64_t comp = 0;
    if (skel::g_componentArrayOffset)
        comp = skel::FindCharacterComponent(entity);
    if (!comp) return F;

    // Read the rotation quaternion (auto-discovers the offset on first use).
    float q[4] = { 0.f, 0.f, 0.f, 1.f };
    if (!GetEntityRotQuat(comp, q)) return F;

    // Convert quaternion (x, y, z, w) to row-major 3x3 rotation matrix.
    // Standard formula — works for any unit quaternion.
    float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2];
    float wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];

    F.m[0] = 1.f - 2.f * (yy + zz);   // [0][0]
    F.m[1] = 2.f * (xy - wz);         // [0][1]
    F.m[2] = 2.f * (xz + wy);         // [0][2]
    F.m[3] = 2.f * (xy + wz);         // [1][0]
    F.m[4] = 1.f - 2.f * (xx + zz);   // [1][1]
    F.m[5] = 2.f * (yz - wx);         // [1][2]
    F.m[6] = 2.f * (xz - wy);         // [2][0]
    F.m[7] = 2.f * (yz + wx);         // [2][1]
    F.m[8] = 1.f - 2.f * (xx + yy);   // [2][2]

    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(F.m[i])) return F;

    F.have = true;
    return F;
}

static inline void TransformBone(float& x, float& y, float& z,
                                 const BoneRotationFrame& F)
{
    if (!F.have) return;
    float nx = x * F.m[0] + y * F.m[1] + z * F.m[2];
    float ny = x * F.m[3] + y * F.m[4] + z * F.m[5];
    float nz = x * F.m[6] + y * F.m[7] + z * F.m[8];
    if (kSkelYawFlip) { nx = -nx; ny = -ny; }   // 180° flip switch
    x = nx; y = ny; z = nz;
}

// ═══ POSITION ANCHOR ═══
// Use the verified paired position. Keep feet alignment enabled: the component
// no longer exposes an independently verified pelvis/root transform.
struct AnchorPos { float x, y, z; bool fromPhysics; };

static inline AnchorPos ResolveAnchor(uint64_t entity, float wx, float wy, float wz)
{
    (void)entity;
    return { wx, wy, wz, false };
}

// PATH A: resolve palette by aligning palette against entity world position.
// Bypasses the char-comp/tag lookup entirely. Works whether palettes are
// stored world- or local-space because we center + translate by draw_pos.
static bool ReadSkeleton_ByPosition(uint64_t entity, float wx, float wy, float wz, SkeletonBones& s) {
    if (!(std::isfinite(wx) && std::isfinite(wy) && std::isfinite(wz))) return false;
    if (fabsf(wx) < 0.01f && fabsf(wy) < 0.01f && fabsf(wz) < 0.01f) return false;

    skel::MatchedPalette mp;
    // Widened from 5m → 8m. R6 palettes are stored around the character's mesh
    // origin (chest area); draw_pos is feet. That gives a ~1.5m Z offset baseline,
    // and light animation adds another 0.5m variance. 5m was cutting valid binds
    // when combined with slight world-position lag.
    if (!skel::ResolvePaletteByPosition(entity, wx, wy, wz, mp, 8.0f)) {
        static uint64_t s_lastLog = 0;
        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - s_lastLog > 3000) {
            s_lastLog = now_ms;
            skel::Log("[BONES] ByPosition: no palette match for entity=0x%llX pos=(%.1f,%.1f,%.1f)\n",
                (unsigned long long)entity, wx, wy, wz);
        }
        return false;
    }

    skel::Vec3f raw[256];
    int nbones = 0;
    if (!skel::ReadPaletteBonesRaw(mp.palette, mp.count, raw, 256, nbones)) return false;
    if (nbones < 17) return false;

    // Center bones around the palette's own centroid — removes any
    // world-space translation baked into the palette.
    skel::Vec3f centroid{};
    for (int i = 0; i < nbones; ++i) { centroid.x += raw[i].x; centroid.y += raw[i].y; centroid.z += raw[i].z; }
    centroid.x /= nbones; centroid.y /= nbones; centroid.z /= nbones;

    skel::Vec3f centered[256];
    for (int i = 0; i < nbones; ++i) {
        centered[i] = { raw[i].x - centroid.x, raw[i].y - centroid.y, raw[i].z - centroid.z };
    }

    // AnvilNext-family engines often store rigs with Y-up; the reference pose
    // assumes Z-up. If Y range dominates, swap Y and Z.
    float mn[3] = {  1e9f,  1e9f,  1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < nbones; ++i) {
        if (centered[i].x < mn[0]) mn[0] = centered[i].x;
        if (centered[i].x > mx[0]) mx[0] = centered[i].x;
        if (centered[i].y < mn[1]) mn[1] = centered[i].y;
        if (centered[i].y > mx[1]) mx[1] = centered[i].y;
        if (centered[i].z < mn[2]) mn[2] = centered[i].z;
        if (centered[i].z > mx[2]) mx[2] = centered[i].z;
    }
    float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];
    bool swapYZ = (sy > sz * 1.3f && sy > sx * 1.3f);
    if (swapYZ) for (int i = 0; i < nbones; ++i) std::swap(centered[i].y, centered[i].z);

    // Solve canonical bone layout against reference pose.
    int idx[skel::BONE_COUNT];
    float err = 0.f;
    const char* pose = "?";
    const int hits = skel::SolveLayoutMulti(centered, nbones, idx, err, &pose);
    if (hits < 10) {
        static uint64_t s_lastLog = 0;
        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - s_lastLog > 3000) {
            s_lastLog = now_ms;
            skel::Log("[BONES] ByPosition: SolveLayout hits=%d err=%.3f (need >=10) ncand=%d spread=(%.2f,%.2f,%.2f)\n",
                hits, err, nbones, sx, sy, sz);
        }
        return false;
    }

    // draw_pos in R6 is the character's FEET, not the pelvis. Palette bones
    // are centered around the pelvis (z-min ~ -0.77, z-max ~ +0.4). If we
    // just add draw_pos.z, the feet end up 0.77m BELOW the ground.
    // Shift the palette so its LOWEST bone sits at draw_pos.z.
    // When we have the PhysicsWorld position (pelvis/root), skip the min_z
    // lift — the physics anchor is already at the rig origin.
    AnchorPos anchor = ResolveAnchor(entity, wx, wy, wz);
    float min_z = 1e9f;
    if (!anchor.fromPhysics) {
        for (int i = 0; i < nbones; i++) if (centered[i].z < min_z) min_z = centered[i].z;
    } else {
        min_z = 0.f;  // physics anchor is already at rig origin
    }

    // Local-space palettes store the rig unrotated (facing world +X in the
    // harness). Re-orient the pose by the actor's full world rotation; world-
    // space palettes already carry the rotation in the bone coordinates.
    BoneRotationFrame F{};
    if (!mp.isWorldSpace)
        F = GetBoneRotationFrame(entity);

    memset(&s, 0, sizeof(s));
    for (int b = 0; b < skel::BONE_COUNT; ++b) {
        if (idx[b] < 0 || idx[b] >= nbones) continue;
        float bx = centered[idx[b]].x;
        float by = centered[idx[b]].y;
        float bz = centered[idx[b]].z - min_z;   // feet-aligned (or 0 if physics)
        TransformBone(bx, by, bz, F);
        s.bones[b].x = bx + anchor.x;
        s.bones[b].y = by + anchor.y;
        s.bones[b].z = bz + anchor.z;
        s.hasBone[b] = true;
        s.total++;
    }

    if (s.total >= 8) {
        static uint64_t s_lastLog = 0;
        static uint64_t s_okCount = 0;
        s_okCount++;
        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - s_lastLog > 2000) {
            s_lastLog = now_ms;
            skel::Log("[BONES] ByPosition OK: entity=0x%llX draw=(%.1f,%.1f,%.1f) centroid=(%.1f,%.1f,%.1f) matchDist=%.2f hits=%d err=%.3f nbones=%d swapYZ=%d rot=%d total OK=%llu\n",
                (unsigned long long)entity, wx, wy, wz,
                centroid.x, centroid.y, centroid.z,
                mp.matchDist, hits, err, nbones, (int)swapYZ, F.have ? 1 : 0, (unsigned long long)s_okCount);
        }
        return true;
    }
    memset(&s, 0, sizeof(s));
    return false;
}

// PATH B: legacy registry path via character-component tag lookup.
// Kept as fallback for when position matching fails (e.g., stale draw_pos).
static bool ReadSkeleton_ByRegistry(uint64_t entity, SkeletonBones& s) {
    skel::Vec3f b[skel::BONE_COUNT];
    uint32_t mask = 0;
    if (!skel::GetBones(entity, b, mask) || !mask) return false;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < skel::BONE_COUNT; ++i) {
        if (mask & (1u << i)) {
            s.bones[i] = { b[i].x, b[i].y, b[i].z };
            s.hasBone[i] = true;
            s.total++;
        }
    }
    return s.total >= 8;
}

// PATH C: auto-discovered char-comp bone read.
// Once skad has locked charTag and worldMatOff by observing which tag+offset
// produces a valid world matrix near draw_pos, we can read bones directly
// from the character component without touching the registry or the (broken)
// palette pointer scheme.
static bool ReadSkeleton_ByAutoDiscover(uint64_t entity, float wx, float wy, float wz, SkeletonBones& s) {
    // Feed each call as a vote toward discovery. Cheap when already locked.
    skad::CastDiscoveryVote(entity, wx, wy, wz);
    if (!skad::g_charTag.load()) return false;  // still hunting

    uint64_t comp = skad::FindCharComp(entity);
    if (!comp) return false;

    skel::Vec3f raw[128];
    int nbones = skad::ReadBonesFromComp(comp, raw, 128);
    if (nbones < 8) {
        static uint64_t s_lastLog = 0;
        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - s_lastLog > 2000) {
            s_lastLog = now_ms;
            skel::Log("[BONES] AutoDiscover: ReadBonesFromComp returned %d bones for comp=0x%llX (need>=8)\n",
                nbones, (unsigned long long)comp);
        }
        return false;
    }

    // Center bones, solve layout, translate by draw_pos.
    skel::Vec3f centroid{};
    for (int i = 0; i < nbones; i++) { centroid.x += raw[i].x; centroid.y += raw[i].y; centroid.z += raw[i].z; }
    centroid.x /= nbones; centroid.y /= nbones; centroid.z /= nbones;

    // Try BOTH orientations (Y-up vs Z-up) and pick the one with more hits.
    // Havok is natively Y-up; our reference pose is Z-up. The old sy>sx*1.3
    // heuristic misfired when arm-spread X was ~= Y.
    auto build_centered = [&](bool swapYZ, skel::Vec3f* out) {
        for (int i = 0; i < nbones; i++) {
            out[i].x = raw[i].x - centroid.x;
            if (swapYZ) {
                out[i].y = raw[i].z - centroid.z;
                out[i].z = raw[i].y - centroid.y;
            } else {
                out[i].y = raw[i].y - centroid.y;
                out[i].z = raw[i].z - centroid.z;
            }
        }
    };

    skel::Vec3f centered_a[128], centered_b[128];
    build_centered(false, centered_a);
    build_centered(true,  centered_b);

    int idx_a[skel::BONE_COUNT], idx_b[skel::BONE_COUNT];
    float err_a = 0.f, err_b = 0.f;
    const char* pose_a = "?"; const char* pose_b = "?";
    const int hits_a = skel::SolveLayoutMulti(centered_a, nbones, idx_a, err_a, &pose_a);
    const int hits_b = skel::SolveLayoutMulti(centered_b, nbones, idx_b, err_b, &pose_b);

    bool useSwap = (hits_b > hits_a) || (hits_b == hits_a && err_b < err_a);
    skel::Vec3f* centered = useSwap ? centered_b : centered_a;
    int*         idx      = useSwap ? idx_b      : idx_a;
    int          hits     = useSwap ? hits_b     : hits_a;
    float        err      = useSwap ? err_b      : err_a;

    // Compute spread for logging.
    float mn[3] = {  1e9f,  1e9f,  1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < nbones; i++) {
        if (centered[i].x < mn[0]) mn[0] = centered[i].x; if (centered[i].x > mx[0]) mx[0] = centered[i].x;
        if (centered[i].y < mn[1]) mn[1] = centered[i].y; if (centered[i].y > mx[1]) mx[1] = centered[i].y;
        if (centered[i].z < mn[2]) mn[2] = centered[i].z; if (centered[i].z > mx[2]) mx[2] = centered[i].z;
    }
    float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];

    // Real skeletons often have many duplicate/near-duplicate bones (finger,
    // cloth, hair) plus the ~17 canonical bones. Accept as low as 4 canonical
    // hits — head/neck/spine/hip alone gives a visible torso stick figure.
    if (hits < 4) {
        static uint64_t s_lastLog = 0;
        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - s_lastLog > 2000) {
            s_lastLog = now_ms;
            skel::Log("[BONES] AutoDiscover: SolveLayout hits_a=%d hits_b=%d err_a=%.3f err_b=%.3f ncand=%d spread(chosen)=(%.2f,%.2f,%.2f) useSwap=%d\n",
                hits_a, hits_b, err_a, err_b, nbones, sx, sy, sz, (int)useSwap);
        }
        return false;
    }

// draw_pos in R6 is the character's FEET, not the pelvis. Palette bones
    // are centered around the pelvis (z-min ~ -0.77, z-max ~ +0.4). If we
    // just add draw_pos.z, the feet end up 0.77m BELOW the ground.
    // Shift the palette so its LOWEST bone sits at draw_pos.z.
    // When we have the PhysicsWorld position (pelvis/root), skip the min_z
    // lift — the physics anchor is already at the rig origin.
    AnchorPos anchor = ResolveAnchor(entity, wx, wy, wz);
    float min_z = 1e9f;
    if (!anchor.fromPhysics) {
        for (int i = 0; i < nbones; i++) if (centered[i].z < min_z) min_z = centered[i].z;
    } else {
        min_z = 0.f;
    }

    // Rig reads from the comp are rig-local — re-orient by the actor's full
    // world rotation so the skeleton tracks the player's facing.
    BoneRotationFrame F = GetBoneRotationFrame(entity);

    memset(&s, 0, sizeof(s));
    for (int b = 0; b < skel::BONE_COUNT; ++b) {
        if (idx[b] < 0 || idx[b] >= nbones) continue;
        float bx = centered[idx[b]].x;
        float by = centered[idx[b]].y;
        float bz = centered[idx[b]].z - min_z;   // feet-aligned (or 0 if physics)
        TransformBone(bx, by, bz, F);
        s.bones[b].x = bx + anchor.x;
        s.bones[b].y = by + anchor.y;
        s.bones[b].z = bz + anchor.z;
        s.hasBone[b] = true;
        s.total++;
    }
    if (s.total >= 4) {
        static uint64_t s_lastLog = 0;
        static uint64_t s_okCount = 0;
        s_okCount++;
        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - s_lastLog > 2000) {
            s_lastLog = now_ms;
            skel::Log("[BONES] AutoDiscover OK: entity=0x%llX comp=0x%llX total=%d hits=%d useSwap=%d rot=%d total_ok=%llu\n",
                (unsigned long long)entity, (unsigned long long)comp, s.total, hits, (int)useSwap, F.have ? 1 : 0, (unsigned long long)s_okCount);
        }
        return true;
    }
    {
        static uint64_t s_lastLog = 0;
        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - s_lastLog > 2000) {
            s_lastLog = now_ms;
            skel::Log("[BONES] AutoDiscover: post-map total=%d hits=%d useSwap=%d ncand=%d (rejected: <4)\n",
                s.total, hits, (int)useSwap, nbones);
        }
    }
    memset(&s, 0, sizeof(s));
    return false;
}

// PATH ZERO: REFERENCE POSE — QUATERNION-ROTATED, PHYSICS-ANCHORED.
// Bulletproof draw. Uses the char comp's rotation quaternion (via
// GetBoneRotationFrame) to orient the pose and the PhysicsWorld position
// (via ResolveAnchor) to place it. Falls back to draw_pos / identity if the
// char comp lookup fails — still visible, just unrotated.
static bool ReadSkeleton_ByFeetAnchor(uint64_t entity, float wx, float wy, float wz, SkeletonBones& s) {
    if (!(std::isfinite(wx) && std::isfinite(wy) && std::isfinite(wz))) return false;
    if (fabsf(wx) < 0.01f && fabsf(wy) < 0.01f && fabsf(wz) < 0.01f) return false;

    BoneRotationFrame F = GetBoneRotationFrame(entity);
    AnchorPos anchor = ResolveAnchor(entity, wx, wy, wz);

    memset(&s, 0, sizeof(s));
    for (const skel::RefBone& rb : skel::kReferencePose_Stand_A) {
        // Rotate local (x,y,z) by the quaternion-derived rotation, then translate.
        float lx = rb.local.x, ly = rb.local.y, lz = rb.local.z;
        TransformBone(lx, ly, lz, F);
        s.bones[rb.bone].x = anchor.x + lx;
        s.bones[rb.bone].y = anchor.y + ly;
        s.bones[rb.bone].z = anchor.z + lz;
        s.hasBone[rb.bone] = true;
        s.total++;
    }
    return s.total > 0;
}

// PATH D: WORLD-MATRIX-ORIENTED SYNTHETIC POSE.
// Once we've locked the character-comp world matrix (tag+offset via skad),
// every character has a valid rotation matrix at that location. We apply
// the matrix to a reference pose. This gives us a POSED skeleton (correct
// orientation, correct height) for EVERY player — animation-agnostic but
// visually correct and universally available.
// PATH D: QUATERNION-ORIENTED SYNTHETIC POSE.
// Uses the char comp's rotation quaternion (GetBoneRotationFrame) to orient
// a reference pose, anchored at the PhysicsWorld position (ResolveAnchor).
// This gives a correctly-facing, correctly-positioned skeleton for every
// player when the animated-palette paths fail. The old +0x240 world-matrix
// read is retired — the quaternion is the authoritative rotation on this build.
static bool ReadSkeleton_ByWorldMatrixPose(uint64_t entity, float wx, float wy, float wz, SkeletonBones& s) {
    BoneRotationFrame F = GetBoneRotationFrame(entity);
    if (!F.have) return false;  // need rotation for this path to be meaningful
    AnchorPos anchor = ResolveAnchor(entity, wx, wy, wz);

    memset(&s, 0, sizeof(s));
    for (const skel::RefBone& rb : skel::kReferencePose_Stand_A) {
        float lx = rb.local.x, ly = rb.local.y, lz = rb.local.z;
        TransformBone(lx, ly, lz, F);
        s.bones[rb.bone].x = anchor.x + lx;
        s.bones[rb.bone].y = anchor.y + ly;
        s.bones[rb.bone].z = anchor.z + lz;
        s.hasBone[rb.bone] = true;
        s.total++;
    }
    static uint64_t s_lastLog = 0;
    static uint64_t s_okCount = 0;
    s_okCount++;
    uint64_t now_ms = skel::NowMicros() / 1000;
    if (now_ms - s_lastLog > 2000) {
        s_lastLog = now_ms;
        skel::Log("[BONES] WMPose OK: entity=0x%llX anchor=(%.1f,%.1f,%.1f) phys=%d total_ok=%llu\n",
            (unsigned long long)entity, anchor.x, anchor.y, anchor.z, (int)anchor.fromPhysics, (unsigned long long)s_okCount);
    }
    return true;
}

// PATH E: DIRECT COMP-IDX LOOKUP + HASH-BASED BONE DISCOVERY.
// Uses the skeleton xref's compIdx to find the character component directly,
// then scans for known bone hashes to identify exact bone positions.
// Falls back to iterating all components when compIdxOff is unknown.
static bool ReadSkeleton_Direct(uint64_t entity, float wx, float wy, float wz, SkeletonBones& s) {
    if (!skel::ValidPtr(entity)) { skel::Log("[BONES-D] bad entity\n"); return false; }
    if (!(std::isfinite(wx) && std::isfinite(wy) && std::isfinite(wz))) return false;
    if (fabsf(wx) < 0.01f && fabsf(wy) < 0.01f && fabsf(wz) < 0.01f) return false;

    // Use FindCharacterComponent with draw_pos so it can auto-discover the
    // char comp tag by matching the live position at +0xB00 against draw_pos.
    uint64_t comp = skel::FindCharacterComponent(entity, wx, wy, wz);
    if (!comp) {
        skel::Log("[BONES-D] no char comp for entity=0x%llX\n", (unsigned long long)entity);
        return false;
    }

    {
        int hash_map[skel::BONE_COUNT];
        skel::Vec3f raw[256];
        int nbones = skad::ScanCompForBonesHash(comp, raw, 256, hash_map);
        if (nbones < 8) {
            skel::Log("[BONES-D] ScanCompForBonesHash returned %d for comp=0x%llX\n",
                nbones, (unsigned long long)comp);
            return false;
        }

        skel::Vec3f centroid{};
        for (int i = 0; i < nbones; i++) {
            centroid.x += raw[i].x; centroid.y += raw[i].y; centroid.z += raw[i].z;
        }
        centroid.x /= nbones; centroid.y /= nbones; centroid.z /= nbones;

        skel::Vec3f centered[256];
        for (int i = 0; i < nbones; i++) {
            centered[i] = { raw[i].x - centroid.x, raw[i].y - centroid.y, raw[i].z - centroid.z };
        }

        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < nbones; i++) {
            if (centered[i].x < mn[0]) mn[0] = centered[i].x;
            if (centered[i].x > mx[0]) mx[0] = centered[i].x;
            if (centered[i].y < mn[1]) mn[1] = centered[i].y;
            if (centered[i].y > mx[1]) mx[1] = centered[i].y;
            if (centered[i].z < mn[2]) mn[2] = centered[i].z;
            if (centered[i].z > mx[2]) mx[2] = centered[i].z;
        }
        float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];
        bool swapYZ = (sy > sz * 1.3f && sy > sx * 1.3f);
        if (swapYZ) {
            for (int i = 0; i < nbones; i++) {
                float tmp = centered[i].y;
                centered[i].y = centered[i].z;
                centered[i].z = tmp;
            }
        }

        float wm[16];
        bool haveWM = skad::ScanCompForWorldMatrix(comp, wx, wy, wz, wm);

        float min_z = 1e9f;
        for (int i = 0; i < nbones; i++) {
            if (centered[i].z < min_z) min_z = centered[i].z;
        }

        memset(&s, 0, sizeof(s));
        for (int b = 0; b < skel::BONE_COUNT; b++) {
            int ri = hash_map[b];
            if (ri < 0 || ri >= nbones) continue;
            float bx = centered[ri].x;
            float by = centered[ri].y;
            float bz = centered[ri].z - min_z;
            if (haveWM) {
                float ox = bx*wm[0]+by*wm[4]+bz*wm[8];
                float oy = bx*wm[1]+by*wm[5]+bz*wm[9];
                float oz = bx*wm[2]+by*wm[6]+bz*wm[10];
                bx=ox; by=oy; bz=oz;
            }
            s.bones[b].x = bx+wx; s.bones[b].y = by+wy; s.bones[b].z = bz+wz;
            s.hasBone[b] = true; s.total++;
        }

        if (s.total >= 4) {
            static uint64_t s_okCount = 0;
            s_okCount++;
            skel::Log("[BONES] Direct OK: entity=0x%llX comp=0x%llX total=%d raw=%d haveWM=%d ok=%llu\n",
                (unsigned long long)entity, (unsigned long long)comp, s.total, nbones, (int)haveWM, (unsigned long long)s_okCount);
            return true;
        }
    }

    skel::Log("[BONES-D] all paths exhausted for entity=0x%llX\n", (unsigned long long)entity);
    return false;
}

static bool ReadSkeleton_Impl(uint64_t entity, float wx, float wy, float wz, SkeletonBones& s) {
    memset(&s, 0, sizeof(s));
    if (!skel::ValidPtr(entity)) return false;

    // Only live animation sources are accepted. Synthetic silhouettes and
    // static bind poses would look plausible but would not track articulation.
    bool r;
    r = ReadSkeleton_Direct(entity, wx, wy, wz, s);
    if (r) return true;
    memset(&s, 0, sizeof(s));

    r = ReadSkeleton_ByRegistry(entity, s);
    if (r) return true;
    memset(&s, 0, sizeof(s));

    r = ReadSkeleton_ByAutoDiscover(entity, wx, wy, wz, s);
    if (r) return true;
    memset(&s, 0, sizeof(s));

    r = ReadSkeleton_ByPosition(entity, wx, wy, wz, s);
    if (r) return true;
    memset(&s, 0, sizeof(s));

    return false;
}

static bool ReadSkeleton(uint64_t entity, float wx, float wy, float wz, SkeletonBones& s) {
    if (!skel::ValidPtr(entity)) { memset(&s, 0, sizeof(s)); return false; }

    const uint64_t now_ms = skel::NowMicros() / 1000;

    {
        std::lock_guard<std::mutex> lk(g_boneCacheMtx);
        auto it = g_boneCache.find(entity);
        if (it != g_boneCache.end()) {
            const bool wasOk = it->second.bones.total > 0;
            const uint64_t failScale = 1 + std::min<uint64_t>(3, it->second.consecutive_fails);
            const uint64_t ttl = wasOk ? kBoneCacheOkTTL : kBoneCacheFailTTL * failScale;
            if (now_ms - it->second.last_update_ms < ttl) {
                s = it->second.bones;
                return wasOk;
            }
        }
    }

    const bool ok = ReadSkeleton_Impl(entity, wx, wy, wz, s);

    {
        std::lock_guard<std::mutex> lk(g_boneCacheMtx);
        auto& slot = g_boneCache[entity];

        // Temporal smoothing: blend new bones with previous frame's bones
        // to reduce jitter. Use exponential moving average with alpha=0.4
        // (favors new data but smooths frame-to-frame noise).
        if (ok && slot.has_prev && slot.bones.total > 0 && s.total > 0) {
            constexpr float kSmoothAlpha = 0.4f;
            const SkeletonBones& prev = slot.bones;
            for (int b = 0; b < skel::BONE_COUNT; ++b) {
                if (!s.hasBone[b] || !prev.hasBone[b]) continue;
                s.bones[b].x = prev.bones[b].x * (1.f - kSmoothAlpha) + s.bones[b].x * kSmoothAlpha;
                s.bones[b].y = prev.bones[b].y * (1.f - kSmoothAlpha) + s.bones[b].y * kSmoothAlpha;
                s.bones[b].z = prev.bones[b].z * (1.f - kSmoothAlpha) + s.bones[b].z * kSmoothAlpha;
            }
        }

        slot.prev_bones        = slot.bones;
        slot.has_prev          = (slot.bones.total > 0);
        slot.bones             = s;
        slot.last_update_ms    = now_ms;
        slot.consecutive_fails = ok ? 0 : (uint8_t)std::min<int>(255, slot.consecutive_fails + 1);
        if (g_boneCache.size() > 128) {
            for (auto it = g_boneCache.begin(); it != g_boneCache.end();) {
                if (now_ms - it->second.last_update_ms > 3000) it = g_boneCache.erase(it);
                else ++it;
            }
        }
    }
    return ok;
}

static Vec3B GetHeadBonePos(const SkeletonBones& s) {
    if (s.hasBone[skel::BONE_HEAD]) return s.bones[skel::BONE_HEAD];
    if (s.hasBone[skel::BONE_NECK]) return s.bones[skel::BONE_NECK];
    return { 0.f, 0.f, 0.f };
}
