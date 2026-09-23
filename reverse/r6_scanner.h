#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "driver.h"
#include "offsets.h"

static std::vector<int> ParsePattern(const char* pat) {
    std::vector<int> v;
    const char* p = pat;
    while (*p) {
        while (*p==' ') ++p;
        if (!*p) break;
        if (p[0]=='?') { v.push_back(-1); ++p; if (*p=='?') ++p; }
        else { v.push_back((int)strtoul(p, const_cast<char**>(&p), 16)); }
    }
    return v;
}

static size_t ScanBufFirst(const uint8_t* buf, size_t sz, const std::vector<int>& pat) {
    if (pat.empty() || sz < pat.size()) return SIZE_MAX;
    size_t lim = sz - pat.size();
    for (size_t i = 0; i <= lim; ++i) {
        bool ok = true;
        for (size_t j = 0; j < pat.size(); ++j)
            if (pat[j] != -1 && buf[i+j] != (uint8_t)pat[j]) { ok = false; break; }
        if (ok) return i;
    }
    return SIZE_MAX;
}

static bool PatternMatchesAt(const uint8_t* buf, size_t sz, size_t offset,
    const std::vector<int>& pat) {
    if (!buf || pat.empty() || offset > sz || pat.size() > sz - offset) return false;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] != -1 && buf[offset + i] != static_cast<uint8_t>(pat[i]))
            return false;
    }
    return true;
}

struct TextSectionCache {
    std::vector<uint8_t> data;
    uint64_t textBase = 0, textSize = 0;
    bool valid = false;
};
static TextSectionCache g_textCache;

// Resolve the pointer variable referenced by a seven-byte RIP-relative load
// at the requested offset within a configured signature match.
static uint64_t ScanSigRipRelative(const char* pattern, int instructionOffset,
    uint64_t moduleBase) {
    if (!g_textCache.valid || !pattern || instructionOffset < 0) return 0;
    const auto parsed = ParsePattern(pattern);
    const size_t match = ScanBufFirst(g_textCache.data.data(),
        g_textCache.data.size(), parsed);
    if (match == SIZE_MAX) return 0;

    const size_t instruction = match + (size_t)instructionOffset;
    if (instruction > g_textCache.data.size() ||
        g_textCache.data.size() - instruction < 7) return 0;

    int32_t displacement = 0;
    memcpy(&displacement, g_textCache.data.data() + instruction + 3,
        sizeof(displacement));
    const uint64_t instructionEnd = g_textCache.textBase + instruction + 7;
    const uint64_t pointerAddress = instructionEnd + displacement;
    printf("[SIG] match RVA=0x%llX -> pointer=0x%llX\n",
        (unsigned long long)(g_textCache.textBase + match - moduleBase),
        (unsigned long long)pointerAddress);
    return pointerAddress;
}

static uint64_t g_pViewDataPtr = 0;
static uint64_t g_pCameraManagerPtr = 0;
static uint64_t g_pInGameFlag = 0;
static uint64_t g_actorCaller = 0;

static void ScanConfiguredPointers(uint64_t moduleBase) {
    g_pViewDataPtr = ScanSigRipRelative(
        OFFSETS::ViewMatrixSignature, 0, moduleBase);
    g_pCameraManagerPtr = ScanSigRipRelative(
        OFFSETS::CameraOneSignature, 0, moduleBase);
    if (!g_pCameraManagerPtr) {
        g_pCameraManagerPtr = ScanSigRipRelative(
            OFFSETS::CameraTwoSignature, 0, moduleBase);
    }
    g_pInGameFlag = ScanSigRipRelative(OFFSETS::InGameFlagSignature,
        OFFSETS::InGameFlagInstructionOffset, moduleBase);

    const auto actorCallerPattern = ParsePattern(OFFSETS::ActorCallerSignature);
    const size_t actorCallerMatch = ScanBufFirst(g_textCache.data.data(),
        g_textCache.data.size(), actorCallerPattern);
    if (actorCallerMatch != SIZE_MAX)
        g_actorCaller = g_textCache.textBase + actorCallerMatch;

    printf("[SIG] ViewData=0x%llX CameraManager=0x%llX "
           "InGameFlag=0x%llX ActorCaller=0x%llX\n",
        (unsigned long long)g_pViewDataPtr,
        (unsigned long long)g_pCameraManagerPtr,
        (unsigned long long)g_pInGameFlag,
        (unsigned long long)g_actorCaller);
}

struct PESection { char name[9]; uint64_t va, vsz; };

static std::vector<PESection> GetPESections(uint64_t base) {
    std::vector<PESection> s;
    IMAGE_DOS_HEADER dos={}; driver->ReadProcessMemory(base, &dos, sizeof(dos));
    if (dos.e_magic != 0x5A4D) return s;
    IMAGE_NT_HEADERS64 nt={}; driver->ReadProcessMemory(base+dos.e_lfanew, &nt, sizeof(nt));
    if (nt.Signature != 0x4550) return s;
    uint64_t off = base+dos.e_lfanew+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    for (int i=0; i<nt.FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh={}; driver->ReadProcessMemory(off+i*sizeof(sh), &sh, sizeof(sh));
        PESection p={}; memcpy(p.name,sh.Name,8); p.name[8]=0; p.va=sh.VirtualAddress; p.vsz=sh.Misc.VirtualSize;
        s.push_back(p);
    }
    printf("[SCAN] %zu PE sections\n", s.size());
    return s;
}

static bool CacheTextSection(uint64_t base, const std::vector<PESection>& secs) {
    for (auto& s : secs) {
        if (strncmp(s.name,".text",5)) continue;
        g_textCache.textBase = base + s.va; g_textCache.textSize = s.vsz;
        printf("[SCAN] .text VA=0x%llX sz=0x%llX\n",(unsigned long long)g_textCache.textBase,(unsigned long long)g_textCache.textSize);
        g_textCache.data.resize((size_t)s.vsz);
        const size_t CH = 0x100000; size_t done = 0;
        while (done < (size_t)s.vsz) {
            size_t rd = min(CH, (size_t)s.vsz - done);
            if (driver->ReadProcessMemory(g_textCache.textBase+done, g_textCache.data.data()+done, (uint32_t)rd) != 0)
                { g_textCache.valid=false; return false; }
            done += rd;
        }
        g_textCache.valid = true;
        printf("[SCAN] Cached %zu bytes\n", g_textCache.data.size());
        return true;
    }
    return false;
}

struct CallTarget { uint64_t callVA, targetVA, anchorVA; bool hasTestAlAl; };

static std::vector<CallTarget> FindEntityFunctionCalls(uint64_t moduleBase) {
    std::vector<CallTarget> res;
    if (!g_textCache.valid) return res;
    const uint8_t* t = g_textCache.data.data();
    size_t sz = (size_t)g_textCache.textSize;
    uint64_t tb = g_textCache.textBase;
    const auto anchorPattern = ParsePattern(OFFSETS::EntityFunctionCallsSignature);
    printf("[ENTITY-SCAN] Scanning %zu bytes for configured entity-call anchor...\n", sz);
    int anchors = 0;
    for (size_t i = 0; i + anchorPattern.size() <= sz; i++) {
        if (!PatternMatchesAt(t, sz, i, anchorPattern)) continue;
        anchors++;
        uint64_t aVA = tb + i;
        if (anchors<=5) printf("[ENTITY-SCAN]   Anchor #%d VA=0x%llX (RVA=0x%llX)\n",
            anchors,(unsigned long long)aVA,(unsigned long long)(aVA-moduleBase));
        size_t bt = 0x80, st = (i>bt)?(i-bt):0;
        for (size_t j=st; j+5<=i; j++) {
            if (t[j]!=0xE8) continue;
            int32_t r32 = *(int32_t*)&t[j+1];
            uint64_t cVA = tb+j, tgt = cVA+5+r32;
            if (tgt<moduleBase||tgt>moduleBase+0x20000000) continue;
            if (tgt<tb||tgt>=tb+sz) continue;
            bool ht = (j+7<sz && t[j+5]==0x84 && t[j+6]==0xC0);
            bool dup=false; for(auto&r:res) if(r.targetVA==tgt&&r.anchorVA==aVA){dup=true;break;}
            if(dup) continue;
            res.push_back({cVA,tgt,aVA,ht});
            if(res.size()<=20) printf("[ENTITY-SCAN]     CALL RVA=0x%llX -> RVA=0x%llX%s\n",
                (unsigned long long)(cVA-moduleBase),(unsigned long long)(tgt-moduleBase),
                ht?" [test al,al] <-- ENTITY FUNC":"");
        }
    }
    printf("[ENTITY-SCAN] %d anchors, %zu calls\n", anchors, res.size());
    if (anchors < 1 || anchors > 2) {
        printf("[ENTITY-SCAN] Anchor missing or ambiguous; refusing to hook\n");
        res.clear();
    }
    std::sort(res.begin(),res.end(),[](auto&a,auto&b){return a.hasTestAlAl>b.hasTestAlAl;});
    return res;
}

static uint64_t ScanForViewTrans(uint64_t moduleBase, uint64_t moduleSize) {
    printf("[W2S] Scanning PAGE_READWRITE regions for ViewTranslation...\n");
    static const auto pat = ParsePattern(OFFSETS::ViewAnchorSignature);
    extern DWORD processID;
    HANDLE hP = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, processID);
    if (!hP) { printf("[W2S] OpenProcess failed\n"); return 0; }
    const size_t CH = 0x100000;
    std::vector<uint8_t> buf;
    size_t regs=0, bytes=0;
    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi={};
    while (VirtualQueryEx(hP,(LPCVOID)addr,&mbi,sizeof(mbi))) {
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State==MEM_COMMIT && (mbi.Protect==PAGE_READWRITE||mbi.Protect==PAGE_EXECUTE_READWRITE)
            && mbi.RegionSize>=pat.size()) {
            regs++;
            uint64_t rBase=(uint64_t)(uintptr_t)mbi.BaseAddress; size_t rSz=mbi.RegionSize;
            size_t done=0;
            while (done<rSz) {
                size_t rd=rSz-done; if(rd>CH)rd=CH;
                if(buf.size()<rd)buf.resize(rd);
                if(driver->ReadProcessMemory(rBase+done,buf.data(),(uint32_t)rd)!=0){done+=CH;continue;}
                size_t off=ScanBufFirst(buf.data(),rd,pat);
                if(off!=SIZE_MAX) {
                    uint64_t vt=rBase+done+off-0x2A4;
                    printf("[W2S] FOUND: vt=0x%llX (%zu regions, %zu bytes)\n",
                        (unsigned long long)vt,regs,bytes);
                    CloseHandle(hP); return vt;
                }
                bytes+=rd;
                size_t adv=(rd>pat.size())?(rd-pat.size()+1):rd;
                done+=adv;
            }
        }
        if(next<=addr)break; addr=next;
    }
    CloseHandle(hP);
    printf("[W2S] NOT FOUND (%zu regions, %zu bytes)\n",regs,bytes);
    return 0;
}


struct SkelXrefInfo {
    uint64_t skelFuncVA;
    uint64_t headHashRefVA;
    uint64_t neckHashRefVA;
    uint32_t pairCount;
    bool valid;
};
static SkelXrefInfo g_SkelXref = {};

static constexpr uint32_t HEAD_BONE_HASH = 0x07C159A2;
static constexpr uint32_t NECK_BONE_HASH = 0x8023796D;
static constexpr size_t BONE_HASH_PAIR_MAX_DIST = 0x100;

struct RtFunc { uint32_t begin, end, unwind; };
static std::vector<RtFunc> g_pdataEntries;
static bool g_pdataLoaded = false;

static bool LoadPdata(uint64_t base) {
    if (g_pdataLoaded) return !g_pdataEntries.empty();
    g_pdataLoaded = true;
    IMAGE_DOS_HEADER dos={};
    if (driver->ReadProcessMemory(base, &dos, sizeof(dos)) != 0) return false;
    if (dos.e_magic != 0x5A4D) return false;
    IMAGE_NT_HEADERS64 nt={};
    if (driver->ReadProcessMemory(base+dos.e_lfanew, &nt, sizeof(nt)) != 0) return false;
    if (nt.Signature != 0x4550) return false;
    uint32_t pdataRVA = nt.OptionalHeader.DataDirectory[3].VirtualAddress;
    uint32_t pdataSize = nt.OptionalHeader.DataDirectory[3].Size;
    if (!pdataRVA || !pdataSize || pdataSize % sizeof(RtFunc) != 0) return false;
    size_t count = pdataSize / sizeof(RtFunc);
    g_pdataEntries.resize(count);
    const size_t CHUNK = 0x100000;
    size_t done = 0;
    while (done < (size_t)pdataSize) {
        size_t rd = pdataSize - done; if (rd > CHUNK) rd = CHUNK;
        if (driver->ReadProcessMemory(base + pdataRVA + done,
            ((uint8_t*)g_pdataEntries.data()) + done, (uint32_t)rd) != 0) {
            g_pdataEntries.clear();
            return false;
        }
        done += rd;
    }
    printf("[SKEL-SCAN] Loaded %zu .pdata entries\n", count);
    return true;
}

static bool LookupFuncBounds(uint64_t moduleBase, uint32_t rva, uint32_t& outBegin, uint32_t& outEnd) {
    int lo = 0, hi = (int)g_pdataEntries.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rva < g_pdataEntries[mid].begin) hi = mid - 1;
        else if (rva >= g_pdataEntries[mid].end) lo = mid + 1;
        else { outBegin = g_pdataEntries[mid].begin; outEnd = g_pdataEntries[mid].end; return true; }
    }
    for (auto& rf : g_pdataEntries) {
        if (rva >= rf.begin && rva < rf.end) {
            outBegin = rf.begin; outEnd = rf.end; return true;
        }
    }
    return false;
}

static std::vector<size_t> FindImm32Refs(const uint8_t* text, size_t textSize, uint32_t target) {
    std::vector<size_t> hits;
    if (!text || textSize < sizeof(target)) return hits;

    for (size_t i = 0; i + sizeof(target) <= textSize; ++i) {
        uint32_t value = 0;
        memcpy(&value, text + i, sizeof(value));
        if (value == target) hits.push_back(i);
    }
    return hits;
}

struct SkelHashCandidate {
    uint32_t funcBeginRVA;
    uint32_t funcEndRVA;
    size_t headRefOffset;
    size_t neckRefOffset;
    uint32_t pairCount;
};

static bool ScanSkelXref(uint64_t moduleBase) {
    g_SkelXref = {};
    if (!g_textCache.valid || !LoadPdata(moduleBase)) return false;

    const uint8_t* text = g_textCache.data.data();
    const size_t textSize = g_textCache.data.size();
    const uint64_t textBase = g_textCache.textBase;
    const auto headRefs = FindImm32Refs(text, textSize, HEAD_BONE_HASH);
    const auto neckRefs = FindImm32Refs(text, textSize, NECK_BONE_HASH);

    printf("[SKEL-SCAN] HEAD hash 0x%08X: %zu hit(s)\n", HEAD_BONE_HASH, headRefs.size());
    printf("[SKEL-SCAN] NECK hash 0x%08X: %zu hit(s)\n", NECK_BONE_HASH, neckRefs.size());

    std::vector<SkelHashCandidate> candidates;
    for (size_t headOffset : headRefs) {
        const size_t pairLo = headOffset > BONE_HASH_PAIR_MAX_DIST
            ? headOffset - BONE_HASH_PAIR_MAX_DIST : 0;
        const size_t remaining = textSize - 1 - headOffset;
        const size_t pairHi = remaining < BONE_HASH_PAIR_MAX_DIST
            ? textSize - 1 : headOffset + BONE_HASH_PAIR_MAX_DIST;
        auto neckIt = std::lower_bound(neckRefs.begin(), neckRefs.end(), pairLo);

        for (; neckIt != neckRefs.end() && *neckIt <= pairHi; ++neckIt) {
            const uint64_t headVA = textBase + headOffset;
            const uint64_t neckVA = textBase + *neckIt;
            if (headVA < moduleBase || neckVA < moduleBase) continue;

            const uint64_t headRVA64 = headVA - moduleBase;
            const uint64_t neckRVA64 = neckVA - moduleBase;
            if (headRVA64 > UINT32_MAX || neckRVA64 > UINT32_MAX) continue;

            uint32_t headBegin = 0, headEnd = 0;
            uint32_t neckBegin = 0, neckEnd = 0;
            if (!LookupFuncBounds(moduleBase, (uint32_t)headRVA64, headBegin, headEnd) ||
                !LookupFuncBounds(moduleBase, (uint32_t)neckRVA64, neckBegin, neckEnd) ||
                headBegin != neckBegin || headEnd != neckEnd) {
                continue;
            }

            auto candidateIt = std::find_if(candidates.begin(), candidates.end(),
                [headBegin](const SkelHashCandidate& candidate) {
                    return candidate.funcBeginRVA == headBegin;
                });
            if (candidateIt == candidates.end()) {
                candidates.push_back({headBegin, headEnd, headOffset, *neckIt, 1});
            } else {
                ++candidateIt->pairCount;
            }
        }
    }

    printf("[SKEL-SCAN] %zu function candidate(s) contain a HEAD/NECK pair within 0x%zX bytes\n",
        candidates.size(), BONE_HASH_PAIR_MAX_DIST);
    for (const auto& candidate : candidates) {
        printf("[SKEL-SCAN]   func=RVA 0x%X HEAD=RVA 0x%llX NECK=RVA 0x%llX pairs=%u\n",
            candidate.funcBeginRVA,
            (unsigned long long)(textBase + candidate.headRefOffset - moduleBase),
            (unsigned long long)(textBase + candidate.neckRefOffset - moduleBase),
            candidate.pairCount);
    }

    if (candidates.size() != 1) {
        printf("[SKEL-SCAN] NOT FOUND: expected one hash-pair function, got %zu\n", candidates.size());
        return false;
    }

    const auto& candidate = candidates.front();
    g_SkelXref.skelFuncVA = moduleBase + candidate.funcBeginRVA;
    g_SkelXref.headHashRefVA = textBase + candidate.headRefOffset;
    g_SkelXref.neckHashRefVA = textBase + candidate.neckRefOffset;
    g_SkelXref.pairCount = candidate.pairCount;
    g_SkelXref.valid = true;
    printf("[SKEL-SCAN] FOUND: func=0x%llX HEAD=0x%llX NECK=0x%llX pairs=%u\n",
        (unsigned long long)g_SkelXref.skelFuncVA,
        (unsigned long long)g_SkelXref.headHashRefVA,
        (unsigned long long)g_SkelXref.neckHashRefVA,
        g_SkelXref.pairCount);
    return true;
}

struct SidewardsInfo {
    uint64_t addr;
    float origValue;
    bool found;
    bool patched;
};
static SidewardsInfo g_Sidewards = {};

static bool ScanSidewards() {
    printf("[SIDEWARDS] Scanning heap for sidewards value...\n");
    g_Sidewards.found = false;
    g_Sidewards.patched = false;
    const uint8_t pattern[] = { 0x66, 0x66, 0x26, 0x3F, 0x33, 0x33, 0xB3, 0x3E, 0x00, 0x00 };
    extern DWORD processID;
    HANDLE hP = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processID);
    if (!hP) return false;
    const size_t CH = 0x100000;
    std::vector<uint8_t> buf(CH);
    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    while (VirtualQueryEx(hP, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && 
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            mbi.RegionSize >= sizeof(pattern)) {
            uint64_t rBase = (uint64_t)(uintptr_t)mbi.BaseAddress;
            size_t rSz = mbi.RegionSize;
            size_t done = 0;
            while (done < rSz) {
                size_t rd = rSz - done; if (rd > CH) rd = CH;
                if (driver->ReadProcessMemory(rBase + done, buf.data(), (uint32_t)rd) != 0) { done += CH; continue; }
                for (size_t j = 0; j + sizeof(pattern) <= rd; j++) {
                    bool match = true;
                    for (size_t k = 0; k < sizeof(pattern); k++) {
                        if (buf[j + k] != pattern[k]) { match = false; break; }
                    }
                    if (match) {
                        g_Sidewards.addr = rBase + done + j;
                        g_Sidewards.origValue = *(float*)&buf[j];
                        g_Sidewards.found = true;
                        printf("[SIDEWARDS] FOUND at 0x%llX (value=%.4f)\n",
                            (unsigned long long)g_Sidewards.addr, g_Sidewards.origValue);
                        CloseHandle(hP);
                        return true;
                    }
                }
                done += (rd > sizeof(pattern)) ? (rd - sizeof(pattern) + 1) : rd;
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    CloseHandle(hP);
    printf("[SIDEWARDS] NOT FOUND\n");
    return false;
}

static void SetSidewardsValue(float value) {
    if (!g_Sidewards.found) return;
    uint8_t buf[4];
    memcpy(buf, &value, 4);
    driver->WriteProcessMemory((PVOID)buf, (PVOID)g_Sidewards.addr, 4);
    g_Sidewards.patched = true;
}

static void RestoreSidewards() {
    if (!g_Sidewards.found || !g_Sidewards.patched) return;
    uint8_t buf[4];
    memcpy(buf, &g_Sidewards.origValue, 4);
    driver->WriteProcessMemory((PVOID)buf, (PVOID)g_Sidewards.addr, 4);
    g_Sidewards.patched = false;
}
