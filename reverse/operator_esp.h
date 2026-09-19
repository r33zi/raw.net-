#pragma once
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include "driver.h"
#include "operator_icons.h"

static const struct { uint32_t hash; const char* name; } g_shaderSignatureDB[] = {
    {0xAD7A23C9, "Smoke"},    {0x023A34A5, "Mute"},     {0x537ED196, "Castle"},
    {0x0A95BD37, "Pulse"},    {0xC2F5B301, "Doc"},      {0x564BD5B0, "Rook"},
    {0xAA741E95, "Kapkan"},   {0x6460C576, "Tachanka"}, {0xFF6D64ED, "Jager"},
    {0x37C35982, "Bandit"},   {0x2A2453C6, "Frost"},    {0x1DED78A5, "Valkyrie"},
    {0x77436ACB, "Caveira"},  {0x06B18413, "Echo"},     {0x6F33A925, "Mira"},
    {0x36118D15, "Lesion"},   {0x22F40DDE, "Ela"},      {0xA5477532, "Vigil"},
    {0xA66B111C, "Maestro"},  {0x393E2D19, "Alibi"},    {0xA647FF7C, "Clash"},
    {0x78FC30DB, "Kaid"},     {0xC1C73C31, "Mozzie"},   {0xD8B310E1, "Warden"},
    {0x0ECAFE7B, "Goyo"},     {0xD995E09D, "Wamai"},    {0x3E20B283, "Oryx"},
    {0x90C4FA37, "Melusi"},   {0xEA0E87E7, "Aruni"},    {0x18B5B4A0, "Thunderbird"},
    {0x68A28B1A, "Thorn"},    {0x05A2F528, "Azami"},    {0xEBE3BBCE, "Solis"},
    {0x54626A24, "Fenrir"},   {0x9B48070E, "Tubarao"},  {0xFAD73B13, "Denari"},
    {0x1C22D45E, "Striker"},
    {0xBB7EA310, "Sledge"},   {0x39BD437E, "Thatcher"}, {0x03381273, "Ash"},
    {0xD15FD400, "Thermite"}, {0x7514C29D, "Twitch"},   {0x64FF2308, "Montagne"},
    {0x9DF0850E, "Glaz"},     {0xC7C4EFBE, "Fuze"},     {0xD33349E8, "Blitz"},
    {0x90C6387F, "IQ"},       {0x7BDC6673, "Buck"},     {0xDD70EE12, "Blackbeard"},
    {0x856560D0, "Capitao"},  {0xD5567C80, "Hibana"},   {0xB0EB3A6B, "Jackal"},
    {0x7742AF84, "Ying"},     {0x2174F17C, "Zofia"},    {0x2C227541, "Dokkaebi"},
    {0x54ED9175, "Lion"},     {0xFC1602A7, "Finka"},    {0x35E5EC10, "Maverick"},
    {0x6D99D6C2, "Nomad"},    {0x12AEE127, "Gridlock"}, {0xC34C188F, "Nokk"},
    {0xCA26E9AA, "Amaru"},    {0x58A951DB, "Kali"},     {0x920F91B1, "Iana"},
    {0xD4768DAF, "Ace"},      {0x05234234, "Zero"},     {0x4CB842A9, "Flores"},
    {0x3EECF7E2, "Osa"},      {0xB417EC26, "Sens"},     {0x72D43D17, "Grim"},
    {0x966AB8FE, "Brava"},    {0xB17011E2, "Ram"},      {0x4F804C2E, "Deimos"},
    {0x7228D336, "Rauora"},   {0x856703FF, "Solid Snake"},
    {0xA65DE9A2, "Sentry"},   {0xAB851DC8, "Skopos"},
    {0xF3E5A01F, "Sentry"},
};
static constexpr int g_shaderSigDBCount = sizeof(g_shaderSignatureDB) / sizeof(g_shaderSignatureDB[0]);

static const char* QueryShaderDatabase(uint32_t hash) {
    if (!hash) return nullptr;
    for (int i = 0; i < g_shaderSigDBCount; i++) {
        if (g_shaderSignatureDB[i].hash == hash) return g_shaderSignatureDB[i].name;
    }
    return nullptr;
}

static std::unordered_map<uint64_t, uint32_t> s_shaderSigCache;
static std::mutex s_shaderSigMtx;

static uint32_t ComputeShaderSignature(uint64_t entity) {
    if (!IsValidAddr(entity)) return 0;

    
    {
        std::lock_guard<std::mutex> lk(s_shaderSigMtx);
        auto it = s_shaderSigCache.find(entity);
        if (it != s_shaderSigCache.end())
            return it->second;
    }

    uint64_t p1 = read<uint64_t>(entity + 0x180);
    if (!IsValidAddr(p1)) return 0;
    uint64_t p2 = read<uint64_t>(p1 + 0x30);
    if (!IsValidAddr(p2)) return 0;

    
    uint8_t buf[0x2A8];
    for (int i = 0; i < (int)sizeof(buf); i += 4) {
        uint32_t val = read<uint32_t>(p2 + 0xA0 + i);
        if (val && val != 0x3E4CCCCD && QueryShaderDatabase(val)) {
            std::lock_guard<std::mutex> lk(s_shaderSigMtx);
            s_shaderSigCache[entity] = val;
            return val;
        }
    }
    return 0;
}

static const char* ResolveShaderLabel(uint64_t entity) {
    uint32_t hash = ComputeShaderSignature(entity);
    return QueryShaderDatabase(hash);
}

static void FlushShaderSigCache() {
    std::lock_guard<std::mutex> lk(s_shaderSigMtx);
    s_shaderSigCache.clear();
}

static IDirect3DTexture9* g_shaderResourceTex[128] = {};
static int g_shaderResLoaded = 0;
static bool g_shaderResReady = false;

static bool UploadShaderResource(IDirect3DDevice9* device, const uint8_t* rgba, int w, int h, IDirect3DTexture9** outTex) {
    if (!device || !rgba || !outTex) return false;
    HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, outTex, NULL);
    if (FAILED(hr)) return false;

    D3DLOCKED_RECT lr;
    hr = (*outTex)->LockRect(0, &lr, NULL, D3DLOCK_DISCARD);
    if (FAILED(hr)) { (*outTex)->Release(); *outTex = nullptr; return false; }

    for (int y = 0; y < h; y++) {
        uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
        const uint8_t* src = rgba + y * w * 4;
        for (int x = 0; x < w; x++) {
            
            dst[x*4+0] = src[x*4+2]; 
            dst[x*4+1] = src[x*4+1]; 
            dst[x*4+2] = src[x*4+0]; 
            dst[x*4+3] = src[x*4+3]; 
        }
    }
    (*outTex)->UnlockRect(0);
    return true;
}

static void LoadShaderResources(IDirect3DDevice9* device) {
    if (g_shaderResReady || !device) return;
    int loaded = 0;
    for (int i = 0; i < g_opIconCount && i < 128; i++) {
        if (UploadShaderResource(device, g_opIconTable[i].data, OP_ICON_SIZE, OP_ICON_SIZE, &g_shaderResourceTex[i])) {
            loaded++;
        }
    }
    g_shaderResLoaded = loaded;
    g_shaderResReady = true;
    printf("[ICONS] Loaded %d/%d operator icons as DX9 textures\n", loaded, g_opIconCount);
}

static void ReleaseShaderResources() {
    for (int i = 0; i < 128; i++) {
        if (g_shaderResourceTex[i]) {
            g_shaderResourceTex[i]->Release();
            g_shaderResourceTex[i] = nullptr;
        }
    }
    g_shaderResLoaded = 0;
    g_shaderResReady = false;
}

static ImTextureID QueryShaderResource(const char* opName) {
    if (!opName || !opName[0] || !g_shaderResReady) return nullptr;
    for (int i = 0; i < g_opIconCount && i < g_shaderResLoaded; i++) {
        if (_stricmp(g_opIconTable[i].name, opName) == 0)
            return (ImTextureID)g_shaderResourceTex[i];
    }
    return nullptr;
}
