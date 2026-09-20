#pragma once
//
// Runtime auto-discovery of R6's character-component tag, world-matrix offset,
// and bone-source pointer. Independent of hardcoded engine offsets.
//
// Design:
//   1. For each entity, we know the component list lives at entity+g_componentArrayOffset
//      (already discovered by ScanSkelXref). The list has ~8 component pointers.
//   2. Each comp has a 32-bit type-tag at comp-0x08. The tag distinguishes what
//      component type it is (character/physics/render/audio/etc).
//   3. We iterate all 8 comps, try to read a world matrix at each of several
//      candidate offsets (0x240, 0x200, 0x180, 0x100, 0x80). A valid world
//      matrix is an orthonormal 3x3 rotation with a translation close to
//      draw_pos.
//   4. When we find a comp+offset combo whose translation matches draw_pos,
//      we've identified BOTH the character-component tag AND the world-matrix
//      offset for this build. Lock both globally.
//   5. Once locked, subsequent lookups go straight to (comp, offset) with no
//      further scanning.
//   6. With the character comp identified, we scan its data area for pointers
//      that lead to bone-shaped data (SkeletonScore). The pointer offset that
//      passes becomes the "bone-source offset" and we cache the mapping.
//

#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include <atomic>
#include <vector>
#include <unordered_map>
#include "skel.h"

namespace skad {

    // Locked after auto-discovery converges. Zero means still hunting.
    inline std::atomic<uint32_t> g_charTag{0};
    inline std::atomic<uint32_t> g_worldMatOff{0};
    inline std::atomic<uint32_t> g_boneSrcOff{0};       // offset on comp to a bone-array pointer
    inline std::atomic<uint32_t> g_boneStride{0x40};
    inline std::atomic<uint32_t> g_boneTrans{0x30};

    // Candidates observed from field-tag dumps (see r6_skel log SKEL-TAG lines).
    // Also include 0x0c63 for backward compatibility.
    static const uint32_t kCandidateTags[] = {
        0x000000D2, 0x000000D3,
        0x000004D2, 0x000004D3,
        0x00000143, 0x00000153,
        0x00000272, 0x00000273,
        0x00000093,
        0x000035F2, 0x000035F3,
        0x00011ED2, 0x00011ED3,
        0x00000C63,  // legacy
    };
    static const uint32_t kCandidateWMOffs[] = {
        0x0240, 0x0200, 0x01C0, 0x0180, 0x0140, 0x0100, 0x00E0, 0x00C0, 0x0080, 0x0040
    };

    struct TagVote { uint32_t tag; uint32_t wmoff; uint32_t votes; };
    inline std::vector<TagVote> g_votes;
    inline std::mutex           g_votesMx;
    inline uint64_t             g_lastDiscoverLog_ms = 0;

    // Weak but self-consistent world-matrix validator. Row-major 4x4:
    // rows 0..2 are unit-length, row 3 is (tx,ty,tz,1). Translation must be
    // finite and (optionally) near a given target.
    static bool ValidateWorldMat(const float m[16], float tx_want, float ty_want, float tz_want, float* out_dist = nullptr) {
        for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) return false;
        float tx = m[12], ty = m[13], tz = m[14];
        for (int r = 0; r < 3; r++) {
            float l = sqrtf(m[r*4+0]*m[r*4+0] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2]);
            if (!std::isfinite(l) || fabsf(l - 1.0f) > 0.05f) return false;
        }
        float dx = tx - tx_want, dy = ty - ty_want, dz = tz - tz_want;
        float d  = sqrtf(dx*dx + dy*dy + dz*dz);
        if (out_dist) *out_dist = d;
        if (!std::isfinite(d)) return false;
        return d < 3.0f;  // char root within 3m of entity draw pos
    }

    // Read comp header tag at comp - 0x08 as uint32 (masked to 20 bits since
    // the low bit varies per team/state and we've seen up to 0x11ED3).
    static inline uint32_t ReadCompTag(uint64_t comp) {
        return skel::Read<uint32_t>(comp - 0x08);
    }

    // Dump the first ~0x400 bytes of each candidate comp so we can SEE the
    // layout and infer where the world matrix lives.
    static void DumpCompLayout(uint64_t entity, float wx, float wy, float wz) {
        static int s_dumped_entities = 0;
        if (s_dumped_entities >= 2) return;
        if (!skel::g_componentArrayOffset || !skel::ValidPtr(entity)) return;
        uint64_t list = skel::Read<uint64_t>(entity + skel::g_componentArrayOffset);
        if (!skel::ValidPtr(list)) return;
        s_dumped_entities++;

        skel::Log("[COMP-DUMP] ==== entity=0x%llX draw=(%.1f,%.1f,%.1f) ====\n",
            (unsigned long long)entity, wx, wy, wz);
        for (uint64_t i = 0; i < 10; i++) {
            uint64_t c = skel::Read<uint64_t>(list + i * 8);
            if (!skel::ValidPtr(c)) { if (c == 0 && i > 4) break; continue; }
            uint32_t tag = ReadCompTag(c);
            uint8_t buf[0x400];
            int rd = driver->ReadProcessMemory(c, buf, 0x400);
            skel::Log("[COMP-DUMP] --- comp[%llu] @ 0x%llX tag=0x%08X read=%s ---\n",
                (unsigned long long)i, (unsigned long long)c, tag, rd == 0 ? "OK" : "FAIL");
            if (rd != 0) continue;
            // Show floats every 4 bytes that look distance-like
            for (uint32_t off = 0; off < 0x400; off += 4) {
                float f = *(float*)(buf + off);
                if (!std::isfinite(f)) continue;
                if (fabsf(f) < 1e-3f || fabsf(f) > 1e5f) continue;
                // Look for X/Y/Z clusters near draw_pos
                float dx = f - wx, dy = f - wy, dz = f - wz;
                if (fabsf(dx) < 0.5f || fabsf(dy) < 0.5f || fabsf(dz) < 0.5f) {
                    const char* which = fabsf(dx)<0.5f?"~wx":fabsf(dy)<0.5f?"~wy":"~wz";
                    skel::Log("[COMP-DUMP]   +0x%03X = %.3f  %s\n", off, f, which);
                }
            }
            // Also, look for orthonormal 3x3 rotation matrix candidates
            for (uint32_t off = 0; off + 48 <= 0x400; off += 4) {
                float* rows = (float*)(buf + off);
                bool ok = true;
                for (int r = 0; r < 3 && ok; r++) {
                    if (!std::isfinite(rows[r*4+0]) || !std::isfinite(rows[r*4+1]) || !std::isfinite(rows[r*4+2])) { ok = false; break; }
                    float l = sqrtf(rows[r*4+0]*rows[r*4+0] + rows[r*4+1]*rows[r*4+1] + rows[r*4+2]*rows[r*4+2]);
                    if (!std::isfinite(l) || fabsf(l - 1.f) > 0.05f) { ok = false; break; }
                }
                if (!ok) continue;
                // Translation follows at rows[12..14]
                float tx = rows[12], ty = rows[13], tz = rows[14];
                if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) continue;
                if (fabsf(tx) < 0.01f && fabsf(ty) < 0.01f && fabsf(tz) < 0.01f) continue;
                float dx = tx - wx, dy = ty - wy, dz = tz - wz;
                float d = sqrtf(dx*dx + dy*dy + dz*dz);
                skel::Log("[COMP-DUMP]   ORTHO@+0x%03X  trans=(%.1f,%.1f,%.1f)  dist=%.2f\n",
                    off, tx, ty, tz, d);
            }
        }
        skel::Log("[COMP-DUMP] ==== end dump ====\n");
    }

    // For an entity we know has a real world position, try to identify the
    // character comp + world matrix offset. Casts votes into g_votes.
    static void CastDiscoveryVote(uint64_t entity, float wx, float wy, float wz) {
        if (g_charTag.load() && g_worldMatOff.load()) return;  // already locked
        if (!skel::ValidPtr(entity)) return;
        if (!(std::isfinite(wx) && std::isfinite(wy) && std::isfinite(wz))) return;
        if (fabsf(wx) < 0.01f && fabsf(wy) < 0.01f && fabsf(wz) < 0.01f) return;

        static std::atomic<uint64_t> s_lastSweepUs{0};
        const uint64_t nowUs = skel::NowMicros();
        uint64_t previousUs = s_lastSweepUs.load(std::memory_order_relaxed);
        if (previousUs && nowUs - previousUs < 250000) return;
        if (!s_lastSweepUs.compare_exchange_strong(previousUs, nowUs,
                std::memory_order_acq_rel, std::memory_order_relaxed)) return;

        // One-time diagnostic dump per session.
        DumpCompLayout(entity, wx, wy, wz);

        // Try the locked offset first, then fall back to a sweep if it fails.
        // This handles the case where the scanner found the wrong compArr offset.
        static const uint64_t kFallbackOffsets[] = {
            0xC0, 0xD0, 0x370, 0xA0, 0xB0, 0xE0, 0xF0, 0x100, 0x180, 0x200
        };
        uint64_t offsets[12];
        int nOff = 0;
        if (skel::g_componentArrayOffset)
            offsets[nOff++] = skel::g_componentArrayOffset;
        for (auto fb : kFallbackOffsets) {
            if (fb != skel::g_componentArrayOffset && nOff < 12)
                offsets[nOff++] = fb;
        }

        for (int oi = 0; oi < nOff; oi++) {
            uint64_t list = skel::Read<uint64_t>(entity + offsets[oi]);
            if (!skel::ValidPtr(list)) continue;

            // Check if this list has at least 4 valid pointers (a real component list).
            int validPtrs = 0;
            for (uint64_t j = 0; j < 8; j++) {
                uint64_t c = skel::Read<uint64_t>(list + j * 8);
                if (skel::ValidPtr(c)) validPtrs++;
            }
            if (validPtrs < 3) continue;

            // This offset looks like a valid component list — update if better.
            if (offsets[oi] != skel::g_componentArrayOffset) {
                skel::Log("[AUTO-DISC] compArr corrected: +0x%llX -> +0x%llX (validPtrs=%d)\n",
                    (unsigned long long)skel::g_componentArrayOffset,
                    (unsigned long long)offsets[oi], validPtrs);
                skel::g_componentArrayOffset = offsets[oi];
            }

            // Also try FINER-grained wmoff sweep (every 0x10 bytes across the whole
            // comp) since our coarse candidate list may miss the actual offset.
            static int  s_fine_sweeps = 0;
            const bool  do_fine = (s_fine_sweeps < 8);
            if (do_fine) s_fine_sweeps++;

            for (uint64_t i = 0; i < 32; i++) {
                uint64_t c = skel::Read<uint64_t>(list + i * 8);
                if (!skel::ValidPtr(c)) { if (c == 0 && i > 4) break; continue; }
                uint32_t tag = ReadCompTag(c);
                if (tag == 0 || tag > 0x00FFFFFF) continue;

                auto try_wmoff = [&](uint32_t wmoff) {
                    float m[16];
                    if (driver->ReadProcessMemory(c + wmoff, m, sizeof(m)) != 0) return;
                    float d = 0.f;
                    if (!ValidateWorldMat(m, wx, wy, wz, &d)) return;
                    std::lock_guard<std::mutex> lk(g_votesMx);
                    bool bumped = false;
                    for (auto& v : g_votes) {
                        if (v.tag == tag && v.wmoff == wmoff) { v.votes++; bumped = true; break; }
                    }
                    if (!bumped) g_votes.push_back({ tag, wmoff, 1 });
                };

                for (uint32_t wmoff : kCandidateWMOffs) try_wmoff(wmoff);
                if (do_fine) {
                    for (uint32_t wmoff = 0x10; wmoff < 0x1000; wmoff += 0x10) try_wmoff(wmoff);
                }
            }
            break;  // found a valid list, stop trying offsets
        }

        uint64_t now_ms = skel::NowMicros() / 1000;
        if (now_ms - g_lastDiscoverLog_ms > 2000) {
            g_lastDiscoverLog_ms = now_ms;
            std::lock_guard<std::mutex> lk(g_votesMx);
            std::sort(g_votes.begin(), g_votes.end(),
                      [](const TagVote& a, const TagVote& b){ return a.votes > b.votes; });
            skel::Log("[AUTO-DISC] entity=0x%llX draw=(%.1f,%.1f,%.1f) votes so far:\n",
                      (unsigned long long)entity, wx, wy, wz);
            int shown = 0;
            for (auto& v : g_votes) {
                skel::Log("[AUTO-DISC]   tag=0x%08X  wmoff=+0x%04X  votes=%u\n",
                          v.tag, v.wmoff, v.votes);
                if (++shown >= 8) break;
            }
            // Lock if a candidate has clear dominance and enough evidence
            if (!g_votes.empty() && g_votes[0].votes >= 5 &&
                (g_votes.size() < 2 || g_votes[0].votes > g_votes[1].votes * 2))
            {
                g_charTag.store(g_votes[0].tag);
                g_worldMatOff.store(g_votes[0].wmoff);
                skel::Log("[AUTO-DISC] *** LOCKED: charTag=0x%08X  wmOff=+0x%04X  (votes=%u) ***\n",
                          g_votes[0].tag, g_votes[0].wmoff, g_votes[0].votes);
            }
        }
    }

    // Find the character comp inside an entity's comp list, given locked tag.
    static uint64_t FindCharComp(uint64_t entity) {
        uint32_t tag = g_charTag.load();
        if (!tag || !skel::g_componentArrayOffset || !skel::ValidPtr(entity)) return 0;
        uint64_t list = skel::Read<uint64_t>(entity + skel::g_componentArrayOffset);
        if (!skel::ValidPtr(list)) return 0;
        for (uint64_t i = 0; i < 32; i++) {
            uint64_t c = skel::Read<uint64_t>(list + i * 8);
            if (!skel::ValidPtr(c)) { if (c == 0 && i > 4) break; continue; }
            if (ReadCompTag(c) == tag) return c;
        }
        return 0;
    }

    // Read the world matrix for a comp, using the locked offset. Returns
    // rotation rows + translation. Validates orthonormality.
    static bool ReadWorldMatrix(uint64_t comp, float m[16]) {
        uint32_t off = g_worldMatOff.load();
        if (!off) return false;
        if (driver->ReadProcessMemory(comp + off, m, sizeof(float) * 16) != 0) return false;
        for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) return false;
        // Sanity: at least the first row should be near unit-length
        float l = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        if (!std::isfinite(l) || l < 0.5f || l > 1.5f) return false;
        return true;
    }

    // Apply row-major 4x4 world matrix to a local point.
    // Layout: m[0..3]=row0, m[4..7]=row1, m[8..11]=row2, m[12..14]=translation.
    static inline void TransformByWM(const float m[16], float lx, float ly, float lz, float& ox, float& oy, float& oz) {
        ox = lx * m[0] + ly * m[4] + lz * m[8]  + m[12];
        oy = lx * m[1] + ly * m[5] + lz * m[9]  + m[13];
        oz = lx * m[2] + ly * m[6] + lz * m[10] + m[14];
    }

    // Scan the character comp's own memory for a pointer that leads to
    // bone-shaped data. Serialized behind a mutex so parallel discovery
    // doesn't produce alternating "LOCKED" results with different offsets.
    inline std::mutex g_boneDiscoverMx;
    static bool AutoDiscoverBoneSource(uint64_t comp) {
        if (g_boneSrcOff.load()) return true;
        std::lock_guard<std::mutex> _lk(g_boneDiscoverMx);
        if (g_boneSrcOff.load()) return true;  // recheck under lock
        if (!skel::ValidPtr(comp)) return false;

        // Scan a reasonable window of the comp for pointer fields.
        constexpr uint32_t kScanBytes = 0x2000;
        static thread_local std::vector<uint8_t> cbuf;
        cbuf.resize(kScanBytes);
        if (driver->ReadProcessMemory(comp, cbuf.data(), kScanBytes) != 0) return false;

        struct Cand { uint32_t off; uint32_t stride; uint32_t trans; uint32_t count; float score; };
        Cand best{0, 0, 0, 0, 0.f};

        static const uint32_t strides[]   = { 0x20, 0x30, 0x40, 0x48, 0x60 };
        static const uint32_t trans_off[] = { 0x00, 0x10, 0x20, 0x30 };

        for (uint32_t off = 0x40; off + 8 <= kScanBytes; off += 8) {
            uint64_t ptr = *(uint64_t*)(cbuf.data() + off);
            if (!skel::ValidPtr(ptr)) continue;
            for (uint32_t st : strides) {
                for (uint32_t tr : trans_off) {
                    if (tr + 12 > st) continue;
                    constexpr uint32_t nTry = 80;
                    static thread_local std::vector<uint8_t> pbuf;
                    pbuf.resize((size_t)st * nTry);
                    if (driver->ReadProcessMemory(ptr, pbuf.data(), (uint32_t)pbuf.size()) != 0) continue;
                    skel::Vec3f pts[128]; int n = 0;
                    for (uint32_t i = 0; i < nTry && n < 128; i++) {
                        skel::Vec3f p;
                        memcpy(&p, pbuf.data() + (size_t)i * st + tr, 12);
                        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) break;
                        if (fabsf(p.x) > 50000.f || fabsf(p.y) > 50000.f || fabsf(p.z) > 50000.f) break;
                        pts[n++] = p;
                    }
                    if (n < 17) continue;
                    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
                    for (int i = 0; i < n; i++) {
                        if (pts[i].x < mn[0]) mn[0] = pts[i].x; if (pts[i].x > mx[0]) mx[0] = pts[i].x;
                        if (pts[i].y < mn[1]) mn[1] = pts[i].y; if (pts[i].y > mx[1]) mx[1] = pts[i].y;
                        if (pts[i].z < mn[2]) mn[2] = pts[i].z; if (pts[i].z > mx[2]) mx[2] = pts[i].z;
                    }
                    float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];
                    float spread = fmaxf(sx, fmaxf(sy, sz));
                    float score = skel::SkeletonScore(pts, n, spread, (uint32_t)n);
                    if (score > best.score) {
                        best = { off, st, tr, (uint32_t)n, score };
                    }
                }
            }
        }

        if (best.score <= 0.f) return false;
        g_boneSrcOff.store(best.off);
        g_boneStride.store(best.stride);
        g_boneTrans.store(best.trans);
        skel::Log("[AUTO-DISC] BONE-SRC LOCKED: compOff=+0x%X stride=0x%X trans=0x%X n=%u score=%.2f\n",
                  best.off, best.stride, best.trans, best.count, best.score);

        // Dump the first 20 bones so we can see the shape.
        {
            uint64_t ptr = skel::Read<uint64_t>(comp + best.off);
            static thread_local std::vector<uint8_t> pbuf;
            pbuf.resize((size_t)best.stride * 30);
            if (skel::ValidPtr(ptr) &&
                driver->ReadProcessMemory(ptr, pbuf.data(), (uint32_t)pbuf.size()) == 0) {
                skel::Vec3f pts[30]; int n = 0;
                for (uint32_t i = 0; i < 30; i++) {
                    skel::Vec3f v;
                    memcpy(&v, pbuf.data() + (size_t)i * best.stride + best.trans, 12);
                    if (!std::isfinite(v.x)) break;
                    pts[n++] = v;
                }
                skel::Vec3f cent{};
                for (int i = 0; i < n; i++) { cent.x += pts[i].x; cent.y += pts[i].y; cent.z += pts[i].z; }
                if (n) { cent.x /= n; cent.y /= n; cent.z /= n; }
                skel::Log("[AUTO-DISC] BONE-DUMP: n=%d centroid=(%.2f,%.2f,%.2f)\n", n, cent.x, cent.y, cent.z);
                float mn[3] = { 1e9f,1e9f,1e9f }, mx[3] = { -1e9f,-1e9f,-1e9f };
                for (int i = 0; i < n; i++) {
                    float lx = pts[i].x - cent.x, ly = pts[i].y - cent.y, lz = pts[i].z - cent.z;
                    if (lx<mn[0]) mn[0]=lx; if (lx>mx[0]) mx[0]=lx;
                    if (ly<mn[1]) mn[1]=ly; if (ly>mx[1]) mx[1]=ly;
                    if (lz<mn[2]) mn[2]=lz; if (lz>mx[2]) mx[2]=lz;
                    skel::Log("[AUTO-DISC]   bone[%02d] local=(%+7.3f, %+7.3f, %+7.3f)\n", i, lx, ly, lz);
                }
                skel::Log("[AUTO-DISC] BONE-DUMP: local extent x=[%.2f..%.2f] y=[%.2f..%.2f] z=[%.2f..%.2f]  spread=(%.2f,%.2f,%.2f)\n",
                    mn[0], mx[0], mn[1], mx[1], mn[2], mx[2],
                    mx[0]-mn[0], mx[1]-mn[1], mx[2]-mn[2]);
            }
        }
        return true;
    }

    // Candidates observed across sessions (score-ranked). We try each in
    // order per-read since which one has valid data varies by comp state.
    struct BoneSrc { uint32_t off, stride, trans; };
    static const BoneSrc kBoneSourceCandidates[] = {
        { 0x0C48, 0x40, 0x30 },  // discovered — 21 bones, score 13.73 (looked human)
        { 0x1978, 0x40, 0x10 },  // discovered — 52 bones, score 116 (dense, maybe matrix palette)
        { 0x0E38, 0x48, 0x00 },  // discovered — 62 bones, score 35
        // hkQsTransform native layout (Havok):
        { 0x0080, 0x30, 0x00 },
        { 0x0100, 0x30, 0x00 },
        { 0x0200, 0x30, 0x00 },
        { 0x0400, 0x30, 0x00 },
        { 0x0800, 0x30, 0x00 },
        // hkTransformf native (4x4 float matrix, translation last row):
        { 0x0080, 0x40, 0x30 },
        { 0x0100, 0x40, 0x30 },
        { 0x0200, 0x40, 0x30 },
    };

    // Read one candidate slot; returns n>=8 with human-sized spread, or 0.
    static int TryReadBonesAt(uint64_t comp, uint32_t off, uint32_t stride, uint32_t trans,
                              skel::Vec3f* out, int max_out) {
        uint64_t ptr = skel::Read<uint64_t>(comp + off);
        if (!skel::ValidPtr(ptr)) return 0;

        constexpr uint32_t kMaxBones = 128;
        static thread_local std::vector<uint8_t> pbuf;
        pbuf.resize((size_t)stride * kMaxBones);
        if (driver->ReadProcessMemory(ptr, pbuf.data(), (uint32_t)pbuf.size()) != 0) return 0;

        int n = 0;
        for (uint32_t i = 0; i < kMaxBones && n < max_out; i++) {
            skel::Vec3f v;
            memcpy(&v, pbuf.data() + (size_t)i * stride + trans, 12);
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) break;
            if (fabsf(v.x) > 1e5f || fabsf(v.y) > 1e5f || fabsf(v.z) > 1e5f) return 0;
            out[n++] = v;
        }
        if (n < 8) return 0;

        // Human-sized 3D spread check. A real skeleton has meaningful extent
        // on ALL THREE axes. Flat data (spread ~ 0 on any axis) is UI, texture
        // atlas, projected coordinates — reject.
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < n; i++) {
            if (out[i].x < mn[0]) mn[0] = out[i].x; if (out[i].x > mx[0]) mx[0] = out[i].x;
            if (out[i].y < mn[1]) mn[1] = out[i].y; if (out[i].y > mx[1]) mx[1] = out[i].y;
            if (out[i].z < mn[2]) mn[2] = out[i].z; if (out[i].z > mx[2]) mx[2] = out[i].z;
        }
        float sx = mx[0]-mn[0], sy = mx[1]-mn[1], sz = mx[2]-mn[2];
        float maxSpread = fmaxf(sx, fmaxf(sy, sz));
        float minSpread = fminf(sx, fminf(sy, sz));
        if (maxSpread < 0.3f || maxSpread > 5.0f) return 0;  // human-height range
        if (minSpread < 0.15f) return 0;                     // reject flat/2D data
        return n;
    }

    // Per-comp cache: once we've found a working (off, stride, trans) for a
    // specific character component, remember it. Different comps often have
    // bone data at different offsets, so a global lock is insufficient.
    struct PerCompSlot { uint32_t off, stride, trans; uint64_t last_ok_ms; };
    inline std::unordered_map<uint64_t, PerCompSlot> g_perComp;
    inline std::mutex g_perCompMx;

    static int ScanCompForBones(uint64_t comp, skel::Vec3f* out, int max_out,
                                uint32_t& out_off, uint32_t& out_stride, uint32_t& out_trans) {
        // Brute-force scan: every 8 bytes across the first 0x2000 of comp,
        // try each (stride, trans) combo. Return first that yields valid bones.
        constexpr size_t kScanBytes = 0x2000;
        static thread_local std::vector<uint8_t> cbuf;
        cbuf.resize(kScanBytes);
        if (driver->ReadProcessMemory(comp, cbuf.data(), (uint32_t)kScanBytes) != 0) return 0;

        static const uint32_t strides[]   = { 0x30, 0x40, 0x48, 0x60 };
        static const uint32_t trans_off[] = { 0x00, 0x10, 0x20, 0x30 };

        int bestN = 0;
        float bestScore = 0.f;
        for (uint32_t off = 0x40; off + 8 <= kScanBytes; off += 8) {
            uint64_t ptr = *(uint64_t*)(cbuf.data() + off);
            if (!skel::ValidPtr(ptr)) continue;
            for (uint32_t st : strides) {
                for (uint32_t tr : trans_off) {
                    if (tr + 12 > st) continue;
                    skel::Vec3f tmp[128];
                    int n = TryReadBonesAt(comp, off, st, tr, tmp, 128);
                    if (n < 8) continue;
                    // Score by spread; keep the best.
                    float mn[3] = { 1e9f,1e9f,1e9f }, mx[3] = { -1e9f,-1e9f,-1e9f };
                    for (int i = 0; i < n; i++) {
                        if (tmp[i].x < mn[0]) mn[0] = tmp[i].x; if (tmp[i].x > mx[0]) mx[0] = tmp[i].x;
                        if (tmp[i].y < mn[1]) mn[1] = tmp[i].y; if (tmp[i].y > mx[1]) mx[1] = tmp[i].y;
                        if (tmp[i].z < mn[2]) mn[2] = tmp[i].z; if (tmp[i].z > mx[2]) mx[2] = tmp[i].z;
                    }
                    float spread = fmaxf(mx[0]-mn[0], fmaxf(mx[1]-mn[1], mx[2]-mn[2]));
                    // Prefer spreads near 1.7m (adult human height).
                    float score = (float)n / (fabsf(spread - 1.7f) + 0.5f);
                    if (score > bestScore) {
                        bestScore = score;
                        bestN = n;
                        out_off = off; out_stride = st; out_trans = tr;
                        for (int i = 0; i < n && i < max_out; i++) out[i] = tmp[i];
                        bestN = (n < max_out) ? n : max_out;
                    }
                }
            }
        }
        return bestN;
    }

    // HASH-BASED BONE SCANNER — scan comp memory for known bone hash values.
    // Each bone entry starts with a uint32_t hash identifying the bone type.
    // Find the hashes, determine stride, then read positions at the right offset.
    // out_hash_map[bone_index] = position in out[] array. Returns number of bones found.
    static int ScanCompForBonesHash(uint64_t comp, skel::Vec3f* out, int max_out,
                                     int out_hash_map[skel::BONE_COUNT]) {
        auto isKnownHash = [](uint32_t value) -> int {
            return skel::BoneHashToIndex(value);
        };

        // ═══ FAST PATH — IDA-derived layout (Y10+) ═══
        // From RE of sub_7FF7E71B8B60's caller:
        //   mov  rdi, [rsi+1A8h]   ; rdi = bone_array_ptr = *(char_comp + 0x1A8)
        //   add  rdi, 30h          ; stride = 0x30 (48 bytes per bone entry)
        // Read the array pointer, then walk 512 max bones dereferencing.
        {
            uint64_t bonesPtr = 0;
            if (driver->ReadProcessMemory(comp + 0x1A8, &bonesPtr, sizeof(bonesPtr)) == 0 &&
                skel::ValidPtr(bonesPtr))
            {
                constexpr uint32_t kMaxBones   = 256;
                constexpr uint32_t kEntry      = 0x30;
                constexpr uint32_t kFastBytes  = kMaxBones * kEntry;
                static thread_local std::vector<uint8_t> fbuf;
                fbuf.resize(kFastBytes);
                if (driver->ReadProcessMemory(bonesPtr, fbuf.data(), kFastBytes) == 0) {
                    // Scan first entry to find hash offset within a 48-byte block.
                    // Then verify by checking next entries at the same intra-block offset.
                    int bestHashOff = -1, bestHashScore = 0;
                    for (uint32_t off = 0; off + 4 <= kEntry; off += 4) {
                        int score = 0;
                        for (uint32_t i = 0; i < kMaxBones; i++) {
                            uint32_t v = *(uint32_t*)(fbuf.data() + i*kEntry + off);
                            if (isKnownHash(v) >= 0) score++;
                            if (v == 0) break;  // hit array end (zero-terminated)
                        }
                        if (score > bestHashScore) { bestHashScore = score; bestHashOff = (int)off; }
                    }

                    if (bestHashScore >= 6) {
                        // Found the hash offset within an entry — now find the position offset.
                        // Position is a Vec3f (12 bytes) at some offset in the entry.
                        int bestTransOff = -1, bestTransScore = 0;
                        for (uint32_t tr = 0; tr + 12 <= kEntry; tr += 4) {
                            if ((int)tr == bestHashOff || (int)tr == bestHashOff - 4) continue; // skip hash slot
                            int score = 0, checked = 0;
                            for (uint32_t i = 0; i < kMaxBones && checked < 12; i++) {
                                uint32_t v = *(uint32_t*)(fbuf.data() + i*kEntry + bestHashOff);
                                if (isKnownHash(v) < 0) continue;
                                checked++;
                                float x = *(float*)(fbuf.data() + i*kEntry + tr);
                                float y = *(float*)(fbuf.data() + i*kEntry + tr + 4);
                                float z = *(float*)(fbuf.data() + i*kEntry + tr + 8);
                                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
                                float r = sqrtf(x*x + y*y + z*z);
                                if (r > 100000.f) continue;
                                score++;
                            }
                            if (score > bestTransScore) { bestTransScore = score; bestTransOff = (int)tr; }
                        }

                        if (bestTransOff >= 0 && bestTransScore >= 4) {
                            // We have the full layout of the BIND POSE (static rest positions).
                            // Learn the hash → linear-index mapping from it.
                            memset(out_hash_map, 0xFF, sizeof(int) * skel::BONE_COUNT);
                            int   boneCount = 0;
                            int   idxOf[skel::BONE_COUNT];
                            for (int b = 0; b < skel::BONE_COUNT; b++) idxOf[b] = -1;
                            for (uint32_t i = 0; i < kMaxBones; i++) {
                                uint32_t hv = *(uint32_t*)(fbuf.data() + i*kEntry + bestHashOff);
                                if (hv == 0) { boneCount = i; break; }
                                int bidx = isKnownHash(hv);
                                if (bidx >= 0 && bidx < skel::BONE_COUNT && idxOf[bidx] < 0)
                                    idxOf[bidx] = (int)i;
                                boneCount = (int)i + 1;
                            }
                            if (boneCount < 8) return 0;

                            // ═══ REGISTRY-BOUND ANIMATED PALETTE ═══
                            // Try to match this character to a live rig in the skinning
                            // registry. If matched, use the ANIMATED bone matrices
                            // (updated every frame by the animation blender) instead of
                            // static bind pose.
                            skel::RegistrySlot rig{};
                            if (skel::BindRigToPlayer(comp, rig)) {
                                // Palette is an array of 4x4 matrices (0x40 stride,
                                // translation at m[3][0..2] = offset 0x30 within each entry).
                                const uint32_t paletteBytes = rig.count * (uint32_t)skel::kBoneStride;
                                if (paletteBytes <= 0x20000) {
                                    static thread_local std::vector<uint8_t> pbuf;
                                    pbuf.resize(paletteBytes);
                                    if (driver->ReadProcessMemory(rig.palette, pbuf.data(), paletteBytes) == 0) {
                                        int nbones = 0;
                                        for (int b = 0; b < skel::BONE_COUNT; b++) {
                                            int slot = idxOf[b];
                                            if (slot < 0 || slot >= (int)rig.count) continue;
                                            const uint8_t* e = pbuf.data() + slot * skel::kBoneStride + skel::kBoneTranslate;
                                            float x = *(float*)(e + 0);
                                            float y = *(float*)(e + 4);
                                            float z = *(float*)(e + 8);
                                            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
                                            out[nbones] = { x, y, z };
                                            out_hash_map[b] = nbones;
                                            nbones++;
                                        }
                                        if (nbones >= 8) return nbones;  // ANIMATED
                                    }
                                }
                            }

                            // The hash table also contains a static bind pose, but it is
                            // intentionally not rendered. Continue to the live-palette
                            // discovery paths so every accepted result is articulated.
                        }
                    }
                }
            }
        }

        // ═══ SLOW-PATH FALLBACK — legacy inline scan ═══
        // Used when the +0x1A8 dereference doesn't yield hits (older build layout
        // or gadget/prop component that doesn't follow the character schema).
        constexpr uint32_t kScanBytes = 0x20000;
        static thread_local std::vector<uint8_t> cbuf;
        cbuf.resize(kScanBytes);
        if (driver->ReadProcessMemory(comp, cbuf.data(), kScanBytes) != 0) return 0;

        const uint8_t* buf = cbuf.data();

        // Step 1: Find all bone hash locations in the buffer.
        struct HashHit { uint32_t hash; uint32_t offset; int bone_idx; };
        HashHit hits[64];
        int nhits = 0;

        for (uint32_t off = 0; off + 4 <= kScanBytes; off += 4) {
            uint32_t val = *(uint32_t*)(buf + off);
            int bidx = isKnownHash(val);
            if (bidx >= 0) {
                if (nhits < 64) hits[nhits++] = { val, off, bidx };
            }
        }

        if (nhits < 8) return 0;  // need at least 8 hash hits to be confident

        // Step 2: Determine stride — distance between consecutive hash entries.
        // Sort hits by offset, then find the most common stride.
        for (int i = 0; i < nhits - 1; i++)
            for (int j = i + 1; j < nhits; j++)
                if (hits[i].offset > hits[j].offset) { auto t = hits[i]; hits[i] = hits[j]; hits[j] = t; }

        // Compute strides between consecutive hits.
        uint32_t stride_hist[256] = {};
        for (int i = 0; i < nhits - 1; i++) {
            uint32_t d = hits[i + 1].offset - hits[i].offset;
            if (d >= 8 && d <= 256) stride_hist[d / 4]++;
        }
        // Find the stride with the most hits.
        uint32_t best_stride = 0;
        int best_stride_count = 0;
        for (uint32_t s = 2; s < 64; s++) {
            if (stride_hist[s] > best_stride_count) {
                best_stride_count = stride_hist[s];
                best_stride = s * 4;
            }
        }
        if (best_stride < 8 || best_stride > 256) return 0;

        // Step 3: Determine position offset within each entry.
        // The position (Vec3f) should be at some offset from the hash.
        // Try common offsets: +4, +8, +12, +16, +20, +24, +28, +32, +36, +40, +44, +48.
        // Score each by how many entries yield finite, reasonable positions.
        int best_trans = -1;
        int best_trans_score = 0;
        for (uint32_t tr = 4; tr + 12 <= best_stride; tr += 4) {
            int score = 0;
            for (int i = 0; i < nhits; i++) {
                uint32_t pos_off = hits[i].offset + tr;
                if (pos_off + 12 > kScanBytes) continue;
                float x = *(float*)(buf + pos_off);
                float y = *(float*)(buf + pos_off + 4);
                float z = *(float*)(buf + pos_off + 8);
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
                // Accept either local-space (small radius) OR world-space (any
                // finite value within map extents). The old 100m cap dropped
                // valid world-space palettes on large R6 maps.
                float r = sqrtf(x * x + y * y + z * z);
                if (r > 100000.0f) continue;  // beyond map extents = garbage
                score++;
            }
            if (score > best_trans_score) {
                best_trans_score = score;
                best_trans = tr;
            }
        }
        if (best_trans < 0) return 0;

        // Step 4: Read positions and map to bone indices.
        memset(out_hash_map, 0xFF, sizeof(int) * skel::BONE_COUNT);  // -1 = unmapped
        int nbones = 0;
        for (int i = 0; i < nhits && nbones < max_out; i++) {
            uint32_t pos_off = hits[i].offset + best_trans;
            if (pos_off + 12 > kScanBytes) continue;
            float x = *(float*)(buf + pos_off);
            float y = *(float*)(buf + pos_off + 4);
            float z = *(float*)(buf + pos_off + 8);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;

            out[nbones] = { x, y, z };
            if (hits[i].bone_idx >= 0 && hits[i].bone_idx < skel::BONE_COUNT) {
                out_hash_map[hits[i].bone_idx] = nbones;
            }
            nbones++;
        }

        return nbones;
    }

    // Read the character-comp's bone palette. Uses per-comp cache; scans on
    // first sight of a comp; falls back to the hardcoded candidates.
    static int ReadBonesFromComp(uint64_t comp, skel::Vec3f* out, int max_out) {
        uint64_t now_ms = skel::NowMicros() / 1000;

        // Fast path: use per-comp cached (off, stride, trans).
        {
            std::lock_guard<std::mutex> lk(g_perCompMx);
            auto it = g_perComp.find(comp);
            if (it != g_perComp.end()) {
                int n = TryReadBonesAt(comp, it->second.off, it->second.stride, it->second.trans, out, max_out);
                if (n > 0) { it->second.last_ok_ms = now_ms; return n; }
                // Cache went stale for this comp — fall through and rediscover.
                g_perComp.erase(it);
            }
        }

        // Try the known-good hardcoded candidates first.
        for (const auto& c : kBoneSourceCandidates) {
            int n = TryReadBonesAt(comp, c.off, c.stride, c.trans, out, max_out);
            if (n > 0) {
                {
                    std::lock_guard<std::mutex> lk(g_perCompMx);
                    g_perComp[comp] = { c.off, c.stride, c.trans, now_ms };
                }
                static uint64_t s_lastLog = 0;
                if (now_ms - s_lastLog > 3000) {
                    s_lastLog = now_ms;
                    skel::Log("[SKAD-READ] comp=0x%llX cand +0x%X stride=0x%X trans=0x%X -> %d bones\n",
                        (unsigned long long)comp, c.off, c.stride, c.trans, n);
                }
                return n;
            }
        }

        // Nothing in the hardcoded list works — scan this specific comp.
        // Rate-limit to avoid scanning every failing entity every frame.
        static thread_local uint64_t tl_lastScan_ms = 0;
        if (now_ms - tl_lastScan_ms < 500) return 0;
        tl_lastScan_ms = now_ms;

        uint32_t o, st, tr;
        int n = ScanCompForBones(comp, out, max_out, o, st, tr);
        if (n > 0) {
            {
                std::lock_guard<std::mutex> lk(g_perCompMx);
                g_perComp[comp] = { o, st, tr, now_ms };
            }
            skel::Log("[SKAD-SCAN] comp=0x%llX BRUTE FOUND +0x%X stride=0x%X trans=0x%X -> %d bones\n",
                (unsigned long long)comp, o, st, tr, n);
        }
        return n;
    }

    // Brute-force scan a component for a world matrix (orthonormal rotation + translation near draw_pos).
    static bool ScanCompForWorldMatrix(uint64_t comp, float wx, float wy, float wz, float m[16]) {
        constexpr uint32_t kScanBytes = 0x1000;
        static thread_local std::vector<uint8_t> cbuf;
        cbuf.resize(kScanBytes);
        if (driver->ReadProcessMemory(comp, cbuf.data(), kScanBytes) != 0) return false;

        for (uint32_t off = 0x40; off + 64 <= kScanBytes; off += 4) {
            float* candidate = (float*)(cbuf.data() + off);
            float d = 0;
            if (ValidateWorldMat(candidate, wx, wy, wz, &d)) {
                memcpy(m, candidate, 64);
                return true;
            }
        }
        return false;
    }

} // namespace skad
