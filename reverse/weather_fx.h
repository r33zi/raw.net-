#pragma once
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include "Imgui/imgui.h"

#define WFX_MAX_PARTICLES 800
#define WFX_SNOW  0
#define WFX_RAIN  1
#define WFX_FIRE  2
#define WFX_NONE  3

struct WfxParticle {
    float wx, wy, wz;
    float vx, vy, vz;
    float size;
    float alpha;
    float life;
    float maxLife;
};

static WfxParticle g_wfxParticles[WFX_MAX_PARTICLES];
static int g_wfxMode = WFX_NONE;
static bool g_wfxInited = false;
static float g_wfxIntensity = 1.0f;
static float g_wfxWindX = 0.3f;
static float g_wfxWindY = 0.0f;
static float g_wfxSpawnRadius = 30.0f;
static float g_wfxSpawnHeight = 25.0f;
static float g_wfxDepth = 35.0f;


static float wfx_randf() { return (float)rand() / (float)RAND_MAX; }
static float wfx_randf_range(float lo, float hi) { return lo + wfx_randf() * (hi - lo); }

static void WfxSpawnParticle(WfxParticle& p, float cx, float cy, float cz, int mode) {
    float angle = wfx_randf() * 6.28318f;
    float dist = sqrtf(wfx_randf()) * g_wfxSpawnRadius;
    p.wx = cx + cosf(angle) * dist;
    p.wy = cy + sinf(angle) * dist;
    p.life = 0.0f;

    if (mode == WFX_SNOW) {
        p.wz = cz + wfx_randf_range(3.0f, g_wfxSpawnHeight);
        p.vx = wfx_randf_range(-0.5f, 0.5f) + g_wfxWindX;
        p.vy = wfx_randf_range(-0.5f, 0.5f) + g_wfxWindY;
        p.vz = wfx_randf_range(-1.2f, -3.0f);
        p.size = wfx_randf_range(1.5f, 4.5f);
        p.alpha = wfx_randf_range(0.4f, 0.9f);
        p.maxLife = 30.0f;
    } else if (mode == WFX_RAIN) {
        p.wz = cz + wfx_randf_range(5.0f, g_wfxSpawnHeight);
        p.vx = g_wfxWindX * 4.0f + wfx_randf_range(-0.3f, 0.3f);
        p.vy = g_wfxWindY * 4.0f + wfx_randf_range(-0.3f, 0.3f);
        p.vz = wfx_randf_range(-20.0f, -32.0f);
        p.size = wfx_randf_range(0.6f, 1.8f);
        p.alpha = wfx_randf_range(0.15f, 0.5f);
        p.maxLife = 10.0f;
    } else if (mode == WFX_FIRE) {
        float fdist = sqrtf(wfx_randf()) * 12.0f;
        p.wx = cx + cosf(angle) * fdist;
        p.wy = cy + sinf(angle) * fdist;
        p.wz = cz + wfx_randf_range(-2.0f, 3.0f);
        p.vx = wfx_randf_range(-1.5f, 1.5f);
        p.vy = wfx_randf_range(-1.5f, 1.5f);
        p.vz = wfx_randf_range(2.0f, 8.0f);
        p.size = wfx_randf_range(2.0f, 6.0f);
        p.alpha = wfx_randf_range(0.6f, 1.0f);
        p.maxLife = wfx_randf_range(1.5f, 4.0f);
    }
}

static void WfxInit(float cx, float cy, float cz) {
    int mode = g_wfxMode;
    for (int i = 0; i < WFX_MAX_PARTICLES; i++) {
        WfxSpawnParticle(g_wfxParticles[i], cx, cy, cz, mode);
        if (mode == WFX_SNOW)
            g_wfxParticles[i].wz = cz + wfx_randf_range(-g_wfxDepth, g_wfxSpawnHeight);
        else if (mode == WFX_RAIN)
            g_wfxParticles[i].wz = cz + wfx_randf_range(-g_wfxDepth * 0.5f, g_wfxSpawnHeight);
        else
            g_wfxParticles[i].life = wfx_randf() * g_wfxParticles[i].maxLife;
    }
    g_wfxInited = true;
}

static void WfxRender3D(Vec3 cam, float dt, int W, int H, ImDrawList* dl) {
    if (g_wfxMode == WFX_NONE) return;
    if (!g_wfxInited) WfxInit(cam.x, cam.y, cam.z);



    int mode = g_wfxMode;
    int count = (int)(WFX_MAX_PARTICLES * g_wfxIntensity);
    if (count > WFX_MAX_PARTICLES) count = WFX_MAX_PARTICLES;
    if (count < 30) count = 30;

    for (int i = 0; i < count; i++) {
        WfxParticle& p = g_wfxParticles[i];

        p.wx += p.vx * dt;
        p.wy += p.vy * dt;
        p.wz += p.vz * dt;
        p.life += dt;



        if (mode == WFX_SNOW) {
            p.vx += wfx_randf_range(-2.0f, 2.0f) * dt;
            p.vy += wfx_randf_range(-2.0f, 2.0f) * dt;
            float sway = sinf(p.life * 1.5f + (float)i * 0.07f) * 0.5f;
            p.wx += sway * dt;
        }

        if (mode == WFX_FIRE) {
            p.vx += wfx_randf_range(-3.0f, 3.0f) * dt;
            p.vy += wfx_randf_range(-3.0f, 3.0f) * dt;
            p.vz += wfx_randf_range(0.5f, 2.0f) * dt;
            p.size *= (1.0f + 0.3f * dt);
        }

        float dx = p.wx - cam.x;
        float dy = p.wy - cam.y;
        float dz = p.wz - cam.z;
        float hDist = sqrtf(dx*dx + dy*dy);
        float totalDist = sqrtf(dx*dx + dy*dy + dz*dz);

        bool respawn = false;
        if (mode == WFX_SNOW || mode == WFX_RAIN) {
            if (dz < -g_wfxDepth) respawn = true;
            if (hDist > g_wfxSpawnRadius * 1.6f) respawn = true;
        } else {
            if (p.life > p.maxLife) respawn = true;
            if (totalDist > 20.0f) respawn = true;
        }

        if (respawn) {
            WfxSpawnParticle(p, cam.x, cam.y, cam.z, mode);
            continue;
        }

        Vec3 worldPos = {p.wx, p.wy, p.wz};
        Vec3 screenPos = {};
        if (!W2S(worldPos, screenPos, W, H)) continue;
        if (screenPos.z < 0.01f) continue;

        float distFade = 1.0f;
        if (totalDist > g_wfxSpawnRadius * 0.6f) {
            distFade = 1.0f - ((totalDist - g_wfxSpawnRadius * 0.6f) / (g_wfxSpawnRadius * 1.0f));
            if (distFade < 0.0f) distFade = 0.0f;
        }

        float perspScale = 800.0f / (screenPos.z + 0.5f);
        if (perspScale < 0.2f) perspScale = 0.2f;
        if (perspScale > 12.0f) perspScale = 12.0f;

        if (mode == WFX_SNOW) {
            float sz = p.size * perspScale;
            if (sz < 0.8f) continue;
            if (sz > 10.0f) sz = 10.0f;
            float fa = p.alpha * distFade;
            int a = (int)(fa * 255.0f);
            if (a < 3) continue;
            ImU32 col = IM_COL32(235, 240, 255, a);
            dl->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), sz, col, 8);
            if (sz > 2.0f) {
                ImU32 glow = IM_COL32(255, 255, 255, a / 4);
                dl->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), sz * 1.8f, glow, 8);
            }
        } else if (mode == WFX_RAIN) {
            float lineLen = fabsf(p.vz) * 0.018f;
            Vec3 tailWorld = {p.wx - p.vx * lineLen, p.wy - p.vy * lineLen, p.wz - p.vz * lineLen};
            Vec3 tailScreen = {};
            if (!W2S(tailWorld, tailScreen, W, H)) continue;

            float thick = p.size * perspScale * 0.4f;
            if (thick < 0.3f) continue;
            if (thick > 3.5f) thick = 3.5f;

            float fa = p.alpha * distFade;
            int a = (int)(fa * 255.0f);
            if (a < 3) continue;
            ImU32 col = IM_COL32(170, 195, 230, a);
            dl->AddLine(ImVec2(tailScreen.x, tailScreen.y),
                        ImVec2(screenPos.x, screenPos.y), col, thick);
            if (thick > 1.0f) {
                ImU32 hl = IM_COL32(210, 230, 255, a / 3);
                dl->AddLine(ImVec2(tailScreen.x, tailScreen.y),
                            ImVec2(screenPos.x, screenPos.y), hl, thick * 0.25f);
            }
        } else if (mode == WFX_FIRE) {
            float lifePct = p.life / p.maxLife;
            float sz = p.size * perspScale * (1.0f - lifePct * 0.5f);
            if (sz < 0.5f) continue;
            if (sz > 16.0f) sz = 16.0f;

            float fa = p.alpha * distFade * (1.0f - lifePct);
            int a = (int)(fa * 255.0f);
            if (a < 3) continue;

            int r_core, g_core, b_core;
            if (lifePct < 0.3f) {
                r_core = 255; g_core = (int)(200 + 55 * (1.0f - lifePct/0.3f)); b_core = (int)(80 * (1.0f - lifePct/0.3f));
            } else if (lifePct < 0.6f) {
                float t = (lifePct - 0.3f) / 0.3f;
                r_core = 255; g_core = (int)(200 - 120 * t); b_core = 0;
            } else {
                float t = (lifePct - 0.6f) / 0.4f;
                r_core = (int)(255 - 100 * t); g_core = (int)(80 - 60 * t); b_core = 0;
            }

            ImU32 glow = IM_COL32(255, 150, 30, a / 3);
            dl->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), sz * 2.0f, glow, 10);

            ImU32 core = IM_COL32(r_core, g_core, b_core, a);
            dl->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), sz, core, 10);

            ImU32 hotCenter = IM_COL32(255, 255, (int)(200 * (1.0f - lifePct)), a > 128 ? a : 128);
            dl->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), sz * 0.35f, hotCenter, 8);

            if (sz > 3.0f && wfx_randf() > 0.7f) {
                float sparkAngle = wfx_randf() * 6.28f;
                float sparkDist = sz * wfx_randf_range(1.2f, 2.5f);
                float sx2 = screenPos.x + cosf(sparkAngle) * sparkDist;
                float sy2 = screenPos.y + sinf(sparkAngle) * sparkDist;
                ImU32 spark = IM_COL32(255, 200, 50, a / 2);
                dl->AddCircleFilled(ImVec2(sx2, sy2), sz * 0.2f, spark, 4);
            }
        }
    }
}
