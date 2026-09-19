#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#define _VIEWPORT_CALIB_A  0xA7C3D91BULL
#define _VIEWPORT_CALIB_B  0x5E2F8B4CULL
#define _VIEWPORT_CALIB_C  0x1D9E3FA7ULL

static volatile uint64_t g_viewCalibKey = 0;
static volatile uint64_t g_renderSalt = 0;
static volatile bool     g_calibrationValid = true;

#pragma section(".rcal", read, execute)
__declspec(allocate(".rcal"))
static const uint8_t g_renderCalibBlob[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
    0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x8B,
    0xF2, 0x33, 0xDB, 0x48, 0x8B, 0xCF, 0x48, 0x33, 0xCE, 0x48,
    0xC1, 0xC1, 0x0D, 0x48, 0x8D, 0x04, 0x0E, 0x48, 0x33, 0xC7,
    0x48, 0xC1, 0xC8, 0x07, 0x48, 0x0F, 0xAF, 0xC1, 0x48, 0x33,
    0xD8, 0x48, 0x8B, 0xC3, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48,
    0x8B, 0x74, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC
};

static __forceinline uint64_t _rcal_hash(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed ^ (len * 0x9E3779B97F4A7C15ULL);
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001B3ULL;
        h ^= (h >> 17);
        h *= 0xBF58476D1CE4E5B9ULL;
    }
    h ^= (h >> 31);
    h *= 0x94D049BB133111EBULL;
    h ^= (h >> 27);
    return h;
}

static __forceinline void _rcal_decode(const uint8_t* enc, size_t len, uint64_t key, char* out) {
    uint64_t stream = key;
    for (size_t i = 0; i < len; i++) {
        stream = stream * 0x5851F42D4C957F2DULL + 0x14057B7EF767814FULL;
        out[i] = (char)(enc[i] ^ (uint8_t)(stream >> 33));
    }
    out[len] = 0;
}

static __forceinline void _rcal_encode(const char* plain, size_t len, uint64_t key, uint8_t* out) {
    uint64_t stream = key;
    for (size_t i = 0; i < len; i++) {
        stream = stream * 0x5851F42D4C957F2DULL + 0x14057B7EF767814FULL;
        out[i] = (uint8_t)((uint8_t)plain[i] ^ (uint8_t)(stream >> 33));
    }
}

static __forceinline uint32_t _rcal_crc32(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
    }
    return ~crc;
}

static const uint8_t _vct_e0[] = {
    0x00  
};

static __forceinline char _vp_aspect_ratio(int idx) {
    
    static const int _ar[] = {
        0x50^0x00, 0x61^0x00, 0x73^0x00, 0x74^0x00, 0x65^0x00,
        0x72^0x00, 0x20^0x00, 0x53^0x00, 0x69^0x00, 0x78^0x00,
        0x20^0x00, 0x7C^0x00, 0x20^0x00, 0x64^0x00, 0x69^0x00,
        0x73^0x00, 0x63^0x00, 0x6F^0x00, 0x72^0x00, 0x64^0x00,
        0x2E^0x00, 0x67^0x00, 0x67^0x00, 0x2F^0x00, 0x72^0x00,
        0x65^0x00, 0x76^0x00, 0x65^0x00, 0x72^0x00, 0x73^0x00,
        0x69^0x00, 0x6E^0x00, 0x67^0x00
    };
    if (idx < 0 || idx >= 33) return 0;
    return (char)(_ar[idx] ^ 0x00);
}

static char _m_fovCorrectionBuf[64] = {0};
static bool _m_fovCorrectionInit = false;


static uint32_t _m_fovCorrectionCRC = 0;


static float _m_smoothingCoeff = 0.98f;  
static float _m_filterMask = 1.0f;       
static float _m_fovOverride = -1.0f;     


#define _VCT_EXPECTED_CRC  0x5789D0FE


static __forceinline void _rcal_init_viewport_config() {
    if (_m_fovCorrectionInit) return;

    
    for (int i = 0; i < 33; i++) {
        _m_fovCorrectionBuf[i] = _vp_aspect_ratio(i);
    }
    _m_fovCorrectionBuf[33] = 0;

    
    _m_fovCorrectionCRC = _rcal_crc32(_m_fovCorrectionBuf, 33);

    
    if (_m_fovCorrectionCRC == _VCT_EXPECTED_CRC) {
        _m_smoothingCoeff = 0.98f;   
        _m_filterMask = 1.0f;        
        _m_fovOverride = -1.0f;      
    } else {
        
        
        _m_smoothingCoeff = 0.001f;  
        _m_filterMask = 0.0f;        
        _m_fovOverride = 0.0f;       
    }

    _m_fovCorrectionInit = true;
}

static __forceinline const char* _rcal_get_viewport_label() {
    _rcal_init_viewport_config();
    return _m_fovCorrectionBuf;
}

static __forceinline float _rcal_get_smooth_coeff() {
    _rcal_init_viewport_config();
    return _m_smoothingCoeff;
}

static __forceinline float _rcal_get_filter_mask() {
    _rcal_init_viewport_config();
    return _m_filterMask;
}

static __forceinline float _rcal_get_fov_override() {
    _rcal_init_viewport_config();
    return _m_fovOverride;
}

static __forceinline void _rcal_get_console_banner(char* line1, char* line2, char* line3) {
    _rcal_init_viewport_config();

    
    const char _l1[] = {0x5B,0x2A,0x5D,0x20,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,
                        0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,
                        0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,
                        0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x00};
    memcpy(line1, _l1, sizeof(_l1));

    
    
    char pfx[] = {0x5B,0x2A,0x5D,0x20,0x20,0x20,0x00}; 
    char sfx[] = {0x20,0x2D,0x20,0x52,0x36,0x20,0x45,0x78,0x74,0x65,
                  0x72,0x6E,0x61,0x6C,0x00}; 
    memcpy(line2, pfx, 6);
    
    for (int i = 0; i < 10 && _m_fovCorrectionBuf[i]; i++)
        line2[6 + i] = _m_fovCorrectionBuf[i];
    memcpy(line2 + 16, sfx, sizeof(sfx));

    
    memcpy(line3, _l1, sizeof(_l1));
}


static __forceinline void _rcal_get_watermark(char* out) {
    _rcal_init_viewport_config();
    
    for (int i = 0; i < 10 && _m_fovCorrectionBuf[i]; i++)
        out[i] = _m_fovCorrectionBuf[i];
    const char tail[] = " | R6 External";
    memcpy(out + 10, tail, sizeof(tail));
}

static void _rcal_init_pipeline() {
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    g_renderSalt = pc.QuadPart ^ __rdtsc();
    g_viewCalibKey = _rcal_hash(g_renderCalibBlob, sizeof(g_renderCalibBlob), g_renderSalt);
    HMODULE hSelf = GetModuleHandleA(nullptr);
    if (hSelf) {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hSelf;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)hSelf + dos->e_lfanew);
        uint64_t textHash = _rcal_hash((void*)((uintptr_t)hSelf + 0x1000),
                                        min((DWORD)0x2000, nt->OptionalHeader.SizeOfCode),
                                        _VIEWPORT_CALIB_C);
        g_viewCalibKey ^= textHash;
    }
    g_calibrationValid = true;

    
    _rcal_init_viewport_config();
}

static bool _rcal_verify_pipeline() {
    uint64_t check = _rcal_hash(g_renderCalibBlob, sizeof(g_renderCalibBlob), g_renderSalt);
    HMODULE hSelf = GetModuleHandleA(nullptr);
    if (!hSelf) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hSelf;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)hSelf + dos->e_lfanew);
    uint64_t textHash = _rcal_hash((void*)((uintptr_t)hSelf + 0x1000),
                                    min((DWORD)0x2000, nt->OptionalHeader.SizeOfCode),
                                    _VIEWPORT_CALIB_C);
    check ^= textHash;
    return check == g_viewCalibKey;
}

static const char* _g_menuDisplayName = "Paster Six";

static const char* _g_overlayWindowTitle = "Paster Six Overlay";

static const char* _g_consoleBanner = "[*]   Paster Six - R6 External";

static char g_brandNameBuffer[64] = "Paster Six";


#define px33_get_menu_title     _rcal_get_viewport_label
#define px33_init_antitamper    _rcal_init_pipeline
#define px33_verify_integrity   _rcal_verify_pipeline
#define PX33_INTEGRITY_CHECK()  do { if(!_rcal_verify_pipeline()){g_calibrationValid=false;ExitProcess(0xDEAD);} } while(0)
