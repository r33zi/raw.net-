#include "pasterx.h"
#include "driver.h"
#include "d3d9_x.h"
#include "xor.hpp"
#include <dwmapi.h>
#include <windowsx.h>
#include <vector>
#include <random>
#include <cctype>
#include "Keybind.h"
#include "color.hpp"
#include "json.hpp"
#include "utils.hpp"
#include "offsets.h"
#include "xstring"
#include "r6_entities.h"
#include "skeleton_emu.h"
#include "antitamper.h"

#define color1 (WORD)(0x0001 | 0x0000)
#define color2 (WORD)(0x0002 | 0x0000)
#define color3 (WORD)(0x0003 | 0x0000)
#define color4 (WORD)(0x0004 | 0x0000)
#define color5 (WORD)(0x0005 | 0x0000)
#define color6 (WORD)(0x0006 | 0x0000)
#define color7 (WORD)(0x0007 | 0x0000)
#define color8 (WORD)(0x0008 | 0x0000)
#define COLOR(h, c) SetConsoleTextAttribute(h, c);

static int aimkeypos = 3;
static int aimbone = 1;
int faken_rot = 0;

float BOG_TO_GRD(float BOG) { return (180.f / (float)M_PI) * BOG; }
float GRD_TO_BOG(float GRD) { return ((float)M_PI / 180.f) * GRD; }

bool ShowMenu = true;
bool Esp = false;
bool Esp_box = false;
bool cornered_box = false;
bool Esp_line = false;
bool Aimbot = false;
bool playerTrail = false;
bool Esp_Distance = false;
bool fovcircle = false;
bool square_fov = false;
bool fovcirclefilled = false;
bool fillbox = false;
bool lineheadesp = false;
bool crosshair = false;
bool rainbowMode = false;
bool rainbowBox = false;
bool rainbowTrail = false;
bool rainbowFov = false;
bool rainbowSnaplines = false;
bool Esp_skeleton = false;
bool skeletonAim = false;

float espBoxColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
float espSnaplineColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
float espTrailColor[4] = { 0.0f, 1.0f, 1.0f, 0.7f };
float espDistanceColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
float fovCircleColor[4] = { 1.0f, 1.0f, 1.0f, 0.7f };
float crosshairColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
float aimbotTargetColor[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
float filledBoxColor[4] = { 1.0f, 0.0f, 0.0f, 0.15f };
float espSkeletonColor[4] = { 1.0f, 1.0f, 1.0f, 0.85f };

float boxThickness = 1.5f;
float snaplineThickness = 1.0f;
float trailThickness = 1.5f;
int trailLength = 60;
float fovCircleThickness = 1.0f;
float crosshairSize = 8.0f;
float skeletonThickness = 1.5f;
bool sidewardsEnabled = false;
float sidewardsValue = 10.0f;
bool shaderLabelOverlay = false;
bool shaderIconOverlay = true;
bool depthVisualization = false;
int snaplineOrigin = 2;  
int trailUpdateMs = 33;
bool trailFade = true;
bool espDeathCheck = true;
bool espTeamCheck = true;
int g_weatherMode = WFX_NONE;
int aimTargetMode = 0;
float g_weatherIntensity = 1.0f;
float g_weatherWind = 0.3f;


float ChangerFOV = 80;
ImFont* m_pFont;
float smooth = 5.0f;
static int VisDist = 250;
float AimFOV = 150.0f;
static int aimkey;
static int hitbox;
static int hitboxpos = 0;

DWORD_PTR Uworld;
DWORD_PTR LocalPawn;
DWORD_PTR PlayerState;
DWORD_PTR Localplayer;
DWORD_PTR Rootcomp;
DWORD_PTR PlayerController;
DWORD_PTR Persistentlevel;
uintptr_t PlayerCameraManager;
Vector3 localactorpos;
uint64_t TargetPawn;
int localplayerID;

RECT GameRect = { NULL };
D3DPRESENT_PARAMETERS d3dpp;
DWORD ScreenCenterX;
DWORD ScreenCenterY;
Vector3 LocalRelativeLocation;

struct FBoxSphereBounds {
    struct Vector3 Origin;
    struct Vector3 BoxExtent;
    double SphereRadius;
};

static bool xCreateWindow();
static bool xInitD3d();
static void xMainLoop();
static void xShutdown();
void SubmitDrawCalls();
static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

static HWND Window = NULL;
static HWND g_medalWindow = NULL;
static RECT g_menuBounds = {};
static bool g_menuMouseHeld = false;
static bool g_menuMouseDown[3] = {};
static bool g_menuMousePressed[3] = {};
static bool g_overlayMouseInteractive = false;
static const char* g_overlayClassName = "PasterSixMedalOverlay";
IDirect3D9* p_Object = NULL;
static LPDIRECT3DDEVICE9 D3dDevice = NULL;
static bool g_deviceResetPending = false;

typedef struct { float X, Y, Z; } FVector;
typedef struct { float X, Y; } FVector2D;

inline void K2_DrawLineXD(Vector3 ScreenPositionA, Vector3 ScreenPositionB, float Thickness, ImColor RenderColor) {
    ImGui::GetOverlayDrawList()->AddLine(ImVec2(ScreenPositionA.x, ScreenPositionA.y), ImVec2(ScreenPositionB.x, ScreenPositionB.y), RenderColor, Thickness);
}

struct HandleDisposer {
    using pointer = HANDLE;
    void operator()(HANDLE handle) const {
        if (handle != NULL && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using unique_handle = std::unique_ptr<HANDLE, HandleDisposer>;

static std::uint32_t _GetProcessId(std::string process_name) {
    PROCESSENTRY32 processentry;
    const unique_handle snapshot_handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot_handle.get() == INVALID_HANDLE_VALUE) return 0;
    processentry.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(snapshot_handle.get(), &processentry)) return 0;
    do {
        if (process_name.compare(processentry.szExeFile) == 0)
            return processentry.th32ProcessID;
    } while (Process32Next(snapshot_handle.get(), &processentry) == TRUE);
    return 0;
}

static bool ContainsTextInsensitive(const char* value, const char* needle) {
    if (!value || !needle) return false;

    std::string haystack(value);
    std::string search(needle);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(search.begin(), search.end(), search.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(search) != std::string::npos;
}

static bool IsMedalProcessWindow(HWND candidate) {
    DWORD candidateProcessId = 0;
    GetWindowThreadProcessId(candidate, &candidateProcessId);
    if (candidateProcessId == 0) return false;

    const unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        candidateProcessId));
    if (!process || process.get() == INVALID_HANDLE_VALUE) return false;

    char imagePath[MAX_PATH] = {};
    DWORD imagePathSize = ARRAYSIZE(imagePath);
    if (!QueryFullProcessImageNameA(process.get(), 0, imagePath, &imagePathSize)) return false;

    const char* imageName = strrchr(imagePath, '\\');
    imageName = imageName ? imageName + 1 : imagePath;
    return ContainsTextInsensitive(imageName, "medal");
}

struct MedalOverlaySearch {
    HWND window = NULL;
    int score = -1;
};

static BOOL CALLBACK FindMedalOverlayProc(HWND candidate, LPARAM param) {
    char title[256] = {};
    char className[256] = {};
    GetWindowTextA(candidate, title, ARRAYSIZE(title));
    GetClassNameA(candidate, className, ARRAYSIZE(className));

    const bool hasMedalName = ContainsTextInsensitive(title, "medal") ||
        ContainsTextInsensitive(className, "medal");
    const bool hasOverlayName = ContainsTextInsensitive(title, "overlay") ||
        ContainsTextInsensitive(className, "overlay");
    const bool isMedalProcess = IsMedalProcessWindow(candidate);
    if (!hasOverlayName || !isMedalProcess) return TRUE;

    int score = 0;
    if (isMedalProcess) score += 100;
    if (hasMedalName) score += 40;
    if (IsWindowVisible(candidate)) score += 20;
    if (ContainsTextInsensitive(className, "medaloverlay")) score += 30;
    if (ContainsTextInsensitive(title, "medaloverlay")) score += 30;

    RECT bounds = {};
    if (GetWindowRect(candidate, &bounds)) {
        const LONG width = bounds.right - bounds.left;
        const LONG height = bounds.bottom - bounds.top;
        if (width >= GetSystemMetrics(SM_CXSCREEN) / 2 &&
            height >= GetSystemMetrics(SM_CYSCREEN) / 2)
            score += 10;
    }

    MedalOverlaySearch* search = reinterpret_cast<MedalOverlaySearch*>(param);
    if (score > search->score) {
        search->window = candidate;
        search->score = score;
    }
    return TRUE;
}

static HWND FindMedalOverlay() {
    MedalOverlaySearch search;
    EnumWindows(FindMedalOverlayProc, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

static bool GetGameClientBounds(RECT* bounds) {
    if (!bounds || !IsWindow(hwnd)) return false;

    RECT client = {};
    POINT origin = {};
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return false;

    bounds->left = origin.x;
    bounds->top = origin.y;
    bounds->right = origin.x + (client.right - client.left);
    bounds->bottom = origin.y + (client.bottom - client.top);
    return bounds->right > bounds->left && bounds->bottom > bounds->top;
}

static bool IsGameOrOverlayForeground() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    if (foreground == hwnd || foreground == Window) return true;

    DWORD foregroundProcessId = 0;
    GetWindowThreadProcessId(foreground, &foregroundProcessId);
    return foregroundProcessId != 0 && foregroundProcessId == processID;
}

std::string random_string(std::string::size_type length) {
    static auto& chrs = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#%^&*()";
    thread_local static std::mt19937 rg{ std::random_device{}() };
    thread_local static std::uniform_int_distribution<std::string::size_type> pick(0, sizeof(chrs) - 2);
    std::string s;
    s.reserve(length);
    while (length--) s += chrs[pick(rg)];
    return s + ".exe";
}

void rndmTitle() {
    constexpr int length = 25;
    const auto characters = TEXT("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    TCHAR title[length + 1]{};
    for (int j = 0; j != length; j++)
        title[j] += characters[rand() % 35 + 1];
    SetConsoleTitle(title);
}

static float g_rainbowHue = 0.0f;
static DWORD g_lastRainbowTick = 0;

static ImU32 GetRainbowColor(float alpha = 1.0f, float offset = 0.0f) {
    float h = fmodf(g_rainbowHue + offset, 1.0f);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, 1.0f, 1.0f, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(alpha * 255));
}

static void UpdateRainbow() {
    DWORD now = GetTickCount();
    float dt = (now - g_lastRainbowTick) / 1000.0f;
    g_lastRainbowTick = now;
    g_rainbowHue += dt * 0.3f;
    if (g_rainbowHue > 1.0f) g_rainbowHue -= 1.0f;
}

static ImU32 ColorToU32(const float* col) {
    return IM_COL32((int)(col[0]*255), (int)(col[1]*255), (int)(col[2]*255), (int)(col[3]*255));
}

static ImU32 GetBoxColor(float offset = 0.0f) {
    if (rainbowMode && rainbowBox) return GetRainbowColor(espBoxColor[3], offset);
    return ColorToU32(espBoxColor);
}

static ImU32 GetSnaplineColor(float offset = 0.0f) {
    if (rainbowMode && rainbowSnaplines) return GetRainbowColor(espSnaplineColor[3], offset);
    return ColorToU32(espSnaplineColor);
}

static ImU32 GetTrailColor(float offset = 0.0f) {
    if (rainbowMode && rainbowTrail) return GetRainbowColor(espTrailColor[3], offset);
    return ColorToU32(espTrailColor);
}

static ImU32 GetFovColor() {
    if (rainbowMode && rainbowFov) return GetRainbowColor(fovCircleColor[3]);
    return ColorToU32(fovCircleColor);
}

int main(int argc, const char* argv[]) {
    px33_init_antitamper();
    system("color 3");
    system("cls");
    HANDLE hpStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    
    char _cb1[64]={}, _cb2[64]={}, _cb3[64]={};
    _rcal_get_console_banner(_cb1, _cb2, _cb3);
    printf("%s\n", _cb1);
    printf("%s\n", _cb2);
    printf("%s\n\n", _cb3);
    printf("[+] Initializing mouse controller...\n");
    MouseController::Init();
    printf("[+] Mouse controller OK\n");
    printf("\n[*] Step 1: Looking for game window...\n");
    const char* windowTitles[] = {
        "Rainbow Six", "R6Game", "RainbowSix",
        "Tom Clancy's Rainbow Six  Siege", "Tom Clancy's Rainbow Six Siege", NULL
    };
    int searchAttempts = 0;
    while (hwnd == NULL) {
        for (int i = 0; windowTitles[i] != NULL; i++) {
            hwnd = FindWindowA(0, windowTitles[i]);
            if (hwnd != NULL) { printf("[+] Found: '%s'\n", windowTitles[i]); break; }
        }
        if (hwnd == NULL) {
            hwnd = FindWindowA("UnrealWindow", NULL);
            if (hwnd) { char title[256] = {0}; GetWindowTextA(hwnd, title, 255); printf("[+] UnrealWindow: '%s'\n", title); }
        }
        if (hwnd == NULL) {
            searchAttempts++;
            if (searchAttempts % 10 == 1) printf("[.] Waiting for game... (attempt %d)\n", searchAttempts);
            Sleep(500);
        }
    }

    printf("\n[*] Step 2: Finding game process...\n");
    const char* processNames[] = { "RainbowSix.exe", "rainbowsix.exe", NULL };
    for (int attempt = 0; attempt < 30 && processID == 0; attempt++) {
        for (int i = 0; processNames[i] != NULL; i++) {
            processID = _GetProcessId(processNames[i]);
            if (processID != 0) { printf("[+] Found: '%s' (PID=%lu)\n", processNames[i], processID); break; }
        }
        if (processID == 0) { if (attempt % 5 == 0) printf("[.] Waiting... (%d/30)\n", attempt + 1); Sleep(1000); }
    }
    if (processID == 0) { printf("[!] Game not found!\n"); system("pause"); return 1; }

    printf("\n[*] Step 3: Initializing driver...\n");
    uint64_t module_size = 0;
    if (driver->Init(FALSE)) {
        printf("[+] Driver OK\n");
        driver->Attach(processID);
        base_address = driver->GetModuleBase(L"RainbowSix.exe", &module_size);
        printf("[+] Base: 0x%llX Size: 0x%llX\n", (unsigned long long)base_address, (unsigned long long)module_size);
        if (base_address == 0) {
            base_address = driver->GetModuleBase(L"", &module_size);
            printf("[+] Fallback base: 0x%llX\n", (unsigned long long)base_address);
        }
        if (base_address == 0) { printf("[!] Base address failed!\n"); system("pause"); return 1; }
        uint16_t dosHeader = read<uint16_t>(base_address);
        printf("[+] DOS: 0x%04X %s\n", (unsigned)dosHeader, dosHeader == 0x5A4D ? "(OK)" : "(BAD)");
    } else {
        printf("[!] Driver FAILED!\n"); system("pause"); return 1;
    }

    printf("\n[*] Step 3.5: Scanning R6 structures...\n");
    if (module_size == 0) module_size = 0x18000000;
    if (InitRenderPipeline(base_address, module_size)) printf("[+] R6 scanner OK!\n");
    else printf("[!] R6 scan failed\n");

    g_lastRainbowTick = GetTickCount();
    printf("\n[*] Step 4: Finding Medal overlay...\n");
    if (!xCreateWindow()) {
        printf("[!] Medal overlay was not found. Start Medal, enable its game overlay, then retry.\n");
        xShutdown();
        system("pause");
        return 1;
    }
    printf("[+] Medal overlay acquired\n");
    printf("[*] Step 5: Init DirectX 9...\n");
    if (!xInitD3d()) {
        printf("[!] DirectX 9 initialization failed.\n");
        xShutdown();
        system("pause");
        return 1;
    }
    printf("[+] DirectX 9 OK\n");
    if (g_pipelineReady)
        printf("\n[*] Overlay ready; ESP initialized\n");
    else
        printf("\n[!] Overlay ready; ESP unavailable: %s\n", g_pipelineError);
    Sleep(3000);
    ShowWindow(GetConsoleWindow(), SW_HIDE);
    xMainLoop();
    xShutdown();
    return 0;
}

bool xCreateWindow() {
    for (int attempt = 0; attempt < 40 && !g_medalWindow; ++attempt) {
        g_medalWindow = FindMedalOverlay();
        if (!g_medalWindow) Sleep(250);
    }
    if (!g_medalWindow) return false;

    RECT bounds = {};
    if (!GetGameClientBounds(&bounds)) return false;
    Width = bounds.right - bounds.left;
    Height = bounds.bottom - bounds.top;

    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = OverlayWndProc;
    windowClass.hInstance = GetModuleHandleA(NULL);
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = g_overlayClassName;
    if (!RegisterClassExA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    Window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_LAYERED, g_overlayClassName, _g_overlayWindowTitle,
        WS_POPUP, bounds.left, bounds.top, Width, Height, NULL, NULL,
        windowClass.hInstance, NULL);
    if (!Window) {
        UnregisterClassA(g_overlayClassName, windowClass.hInstance);
        return false;
    }

    const MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(Window, &margins);
    SetLayeredWindowAttributes(Window, 0, 255, LWA_ALPHA);
    ShowWindow(Window, SW_SHOWNOACTIVATE);
    UpdateWindow(Window);
    return true;
}

bool xInitD3d() {
    p_Object = Direct3DCreate9(D3D_SDK_VERSION);
    if (!p_Object) return false;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.BackBufferWidth = Width;
    d3dpp.BackBufferHeight = Height;
    d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
    d3dpp.MultiSampleType = D3DMULTISAMPLE_NONE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.EnableAutoDepthStencil = FALSE;
    d3dpp.hDeviceWindow = Window;
    d3dpp.Windowed = TRUE;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT created = p_Object->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, Window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dpp, &D3dDevice);
    if (FAILED(created))
        created = p_Object->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, Window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &D3dDevice);
    if (FAILED(created)) {
        p_Object->Release();
        p_Object = nullptr;
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    if (!ImGui_ImplWin32_Init(Window)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX9_Init(D3dDevice)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.WindowRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.ChildRounding = 4.0f;
    style.ChildBorderSize = 1.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.FrameRounding = 3.0f;
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabMinSize = 9.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    ImVec4* colors = style.Colors;
    const ImVec4 accent = ImVec4(0.40f, 0.00f, 1.00f, 1.00f);
    const ImVec4 accentHov = ImVec4(0.50f, 0.08f, 1.00f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.48f, 0.48f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.96f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    colors[ImGuiCol_Border] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.28f, 0.25f, 0.58f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.32f, 0.28f, 0.68f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.28f, 0.25f, 0.58f, 1.00f);
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accentHov;
    colors[ImGuiCol_Button] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.00f, 1.00f, 0.55f);
    colors[ImGuiCol_TabActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.40f, 0.00f, 1.00f, 0.15f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.40f, 0.00f, 1.00f, 0.55f);
    colors[ImGuiCol_ResizeGripActive] = accent;

    XorS(font, "C:\\Windows\\Fonts\\tahoma.ttf");
    m_pFont = io.Fonts->AddFontFromFileTTF(font.decrypt(), 13.0f, nullptr, io.Fonts->GetGlyphRangesDefault());
    if (m_pFont == nullptr) m_pFont = io.Fonts->AddFontDefault();
    if (!ImGui_ImplDX9_CreateDeviceObjects()) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    LoadShaderResources(D3dDevice);
    p_Object->Release(); p_Object = nullptr;
    return true;
}

void aimbot(float x, float y) {
    float ScreenCX = (Width / 2.0f);
    float ScreenCY = (Height / 2.0f);
    float AimSpeed = smooth;
    float TargetX = 0, TargetY = 0;
    if (x != 0) {
        if (x > ScreenCX) { TargetX = -(ScreenCX - x); TargetX /= AimSpeed; if (TargetX + ScreenCX > ScreenCX * 2) TargetX = 0; }
        if (x < ScreenCX) { TargetX = x - ScreenCX; TargetX /= AimSpeed; if (TargetX + ScreenCX < 0) TargetX = 0; }
    }
    if (y != 0) {
        if (y > ScreenCY) { TargetY = -(ScreenCY - y); TargetY /= AimSpeed; if (TargetY + ScreenCY > ScreenCY * 2) TargetY = 0; }
        if (y < ScreenCY) { TargetY = y - ScreenCY; TargetY /= AimSpeed; if (TargetY + ScreenCY < 0) TargetY = 0; }
    }
    MouseController::Move_Mouse(static_cast<int>(TargetX), static_cast<int>(TargetY));
}

double GetCrossDistance(double x1, double y1, double x2, double y2) {
    return sqrt(pow((x2 - x1), 2) + pow((y2 - y1), 2));
}

static int g_menuTab = 0;

static void UpdateOverlayInput() {
    ImGuiIO& io = ImGui::GetIO();
    POINT cursor = {};
    const bool cursorValid = GetCursorPos(&cursor) && ScreenToClient(Window, &cursor);
    if (cursorValid)
        io.MousePos = ImVec2(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    else
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);

    static bool previousDown[3] = {};
    const int buttons[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
    g_menuMouseHeld = false;
    for (int button = 0; button < 3; ++button) {
        const bool down = (GetAsyncKeyState(buttons[button]) & 0x8000) != 0;
        if (!ShowMenu) {
            g_menuMouseDown[button] = false;
            g_menuMousePressed[button] = false;
        } else {
            if (down && !previousDown[button] && !g_menuMouseDown[button] &&
                cursorValid && (PtInRect(&g_menuBounds, cursor) ||
                    io.WantCaptureMouse || GetCapture() == Window)) {
                g_menuMouseDown[button] = true;
                g_menuMousePressed[button] = true;
            }
            if (!down) g_menuMouseDown[button] = false;
        }
        io.MouseDown[button] = ShowMenu && (g_menuMouseDown[button] || g_menuMousePressed[button]);
        g_menuMouseHeld |= io.MouseDown[button];
        g_menuMousePressed[button] = false;
        previousDown[button] = down;
    }
}

static void UpdateOverlayInteractivity() {
    POINT cursor = {};
    const bool overMenu = GetCursorPos(&cursor) && ScreenToClient(Window, &cursor) &&
        PtInRect(&g_menuBounds, cursor);
    const bool interactive = ShowMenu && (overMenu || g_menuMouseHeld ||
        GetCapture() == Window || ImGui::GetIO().WantCaptureMouse);
    if (!ShowMenu && GetCapture() == Window) ReleaseCapture();
    if (interactive == g_overlayMouseInteractive) return;

    g_overlayMouseInteractive = interactive;
    LONG_PTR style = GetWindowLongPtr(Window, GWL_EXSTYLE);
    if (interactive)
        style &= ~WS_EX_TRANSPARENT;
    else
        style |= WS_EX_TRANSPARENT;
    SetWindowLongPtr(Window, GWL_EXSTYLE, style);
    SetWindowPos(Window, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static bool ResetD3dDevice() {
    if (!D3dDevice) return false;

    ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT result = D3dDevice->Reset(&d3dpp);
    if (FAILED(result)) return false;

    return ImGui_ImplDX9_CreateDeviceObjects();
}

void SubmitDrawCalls() {
    FlushOverlayPipeline(Width, Height, Esp_box, cornered_box, Esp_line, Esp_Distance, VisDist,
                   playerTrail, Aimbot && !ShowMenu, AimFOV, smooth, hitboxpos,
                   fovcircle, square_fov, crosshair);
}

static bool GetImguiTitleFromServer(const char* endpoint, char* outBuf, int bufSz) {
    if (!endpoint || !outBuf || bufSz < 1) return false;
    HINTERNET hNet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hNet) return false;
    HINTERNET hUrl = InternetOpenUrlA(hNet, endpoint, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) { InternetCloseHandle(hNet); return false; }
    DWORD bytesRead = 0;
    char tmp[512] = {};
    InternetReadFile(hUrl, tmp, sizeof(tmp) - 1, &bytesRead);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    if (bytesRead > 0 && bytesRead < (DWORD)bufSz) {
        memcpy(outBuf, tmp, bytesRead);
        outBuf[bytesRead] = 0;
        return true;
    }
    return false;
}

static const char* FetchRemoteBrandConfig(int configId) {
    static char s_remoteBrand[128] = {};
    if (s_remoteBrand[0]) return s_remoteBrand;
    char url[256];
    snprintf(url, sizeof(url), "https://api.overlay-cfg.net/v2/brand/%d", configId);
    if (GetImguiTitleFromServer(url, s_remoteBrand, sizeof(s_remoteBrand)))
        return s_remoteBrand;
    return nullptr;
}

static bool ValidateOverlayLicense(const char* hwid, const char* key) {
    if (!hwid || !key) return false;
    uint32_t hwidHash = 0x811C9DC5;
    for (int i = 0; hwid[i]; i++) {
        hwidHash ^= (uint8_t)hwid[i];
        hwidHash *= 0x01000193;
    }
    uint32_t keyHash = 0x811C9DC5;
    for (int i = 0; key[i]; i++) {
        keyHash ^= (uint8_t)key[i];
        keyHash *= 0x01000193;
    }
    return (hwidHash ^ keyHash) == 0xA5B3C7D1;
}

static int QueryServerMenuStyle(int userId) {
    static int cached = -1;
    if (cached >= 0) return cached;
    char buf[64] = {};
    char url[256];
    snprintf(url, sizeof(url), "https://api.overlay-cfg.net/v2/style/%d", userId);
    if (GetImguiTitleFromServer(url, buf, sizeof(buf)))
        cached = atoi(buf);
    else
        cached = 0;
    return cached;
}


void render() {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    static bool insertWasDown = false;
    const bool insertIsDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertIsDown && !insertWasDown) ShowMenu = !ShowMenu;
    insertWasDown = insertIsDown;
    UpdateOverlayInteractivity();
    UpdateOverlayInput();
    ImGui::NewFrame();
    UpdateRainbow();

    g_renderSnapshot = g_collectionWorker.Latest();

    if (ShowMenu) {
        static int selectedBone = 0;
        const char* boneItems[] = { "Head", "Neck", "Chest", "Pelvis", "Feet" };
        static const char* snapOriginItems[] = { "Bottom", "Center", "Top" };

        ImGui::SetNextWindowSize(ImVec2(560.0f, 640.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(Width * 0.5f, Height * 0.5f),
            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::Begin(px33_get_menu_title(), &ShowMenu,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        const ImVec2 menuPos = ImGui::GetWindowPos();
        const ImVec2 menuSize = ImGui::GetWindowSize();
        g_menuBounds = { static_cast<LONG>(menuPos.x), static_cast<LONG>(menuPos.y),
            static_cast<LONG>(menuPos.x + menuSize.x), static_cast<LONG>(menuPos.y + menuSize.y) };

        if (ImGui::BeginTabBar("##px33tabs")) {

        
        if (ImGui::BeginTabItem("Aimbot")) {
            g_menuTab = 0;
            ImGui::Spacing();
            ImGui::Checkbox("Enable Aimbot", &Aimbot);
            ImGui::Separator();

            ImGui::PushItemWidth(220.0f);
            ImGui::SliderFloat("FOV", &AimFOV, 20.0f, 800.0f, "%.0f");
            ImGui::SliderFloat("Smoothness", &smooth, 1.0f, 20.0f, "%.1f");
            ImGui::PopItemWidth();

            ImGui::PushItemWidth(220.0f);
            ImGui::Combo("Hitbox", &hitboxpos, boneItems, IM_ARRAYSIZE(boneItems));
            ImGui::PopItemWidth();

            ImGui::PushItemWidth(220.0f);
            static const char* aimModeItems[] = { "Bone", "Closest (Z scan)" };
            ImGui::Combo("Target Mode", &aimTargetMode, aimModeItems, 2);
            ImGui::PopItemWidth();

            ImGui::Spacing();
           // ImGui::Text("Aim Key:");
            ImGui::SameLine();
            
            HotkeyButton(hotkeys::aimkey, ChangeKey, keystatus);

            ImGui::Separator();
            ImGui::Text("FOV Visualization:");
            ImGui::Checkbox("Circle FOV", &fovcircle);
            if (fovcircle) { square_fov = false; fovcirclefilled = false; }
            ImGui::SameLine();
            ImGui::Checkbox("Square FOV", &square_fov);
            if (square_fov) { fovcircle = false; fovcirclefilled = false; }

            ImGui::ColorEdit4("FOV Color", fovCircleColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::SameLine();
            ImGui::PushItemWidth(160.0f);
            ImGui::SliderFloat("FOV Thickness", &fovCircleThickness, 0.5f, 5.0f, "%.1f");
            ImGui::PopItemWidth();

            ImGui::Spacing();
            ImGui::ColorEdit4("Target Color", aimbotTargetColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

            ImGui::Separator();
            ImGui::Checkbox("Crosshair", &crosshair);
            if (crosshair) {
                ImGui::SameLine();
                ImGui::ColorEdit4("##CrosshairCol", crosshairColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                ImGui::PushItemWidth(160.0f);
                ImGui::SliderFloat("Crosshair Size", &crosshairSize, 3.0f, 30.0f, "%.0f");
                ImGui::PopItemWidth();
            }

            ImGui::EndTabItem();
        }

        
        if (ImGui::BeginTabItem("ESP")) {
            g_menuTab = 1;
            ImGui::Spacing();

            
            ImGui::Text("Box ESP");
            ImGui::Checkbox("Enable Box", &Esp_box);
            ImGui::SameLine();
            ImGui::Checkbox("Cornered", &cornered_box);
            ImGui::SameLine();
            ImGui::Checkbox("Filled", &fillbox);

            ImGui::ColorEdit4("Box Color", espBoxColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::SameLine();
            ImGui::PushItemWidth(160.0f);
            ImGui::SliderFloat("Thickness##box", &boxThickness, 0.5f, 5.0f, "%.1f");
            ImGui::PopItemWidth();

            if (fillbox) {
                ImGui::ColorEdit4("Fill Color", filledBoxColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            }

            ImGui::Separator();
            ImGui::Text("Skeleton ESP");
            ImGui::Checkbox("Enable Skeleton", &Esp_skeleton);
            ImGui::SameLine();
            ImGui::Checkbox("Aim at Head Bone", &skeletonAim);
            ImGui::ColorEdit4("Skeleton Color", espSkeletonColor,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::SameLine();
            ImGui::PushItemWidth(160.0f);
            ImGui::SliderFloat("Thickness##skeleton", &skeletonThickness, 0.5f, 5.0f, "%.1f");
            ImGui::PopItemWidth();

            
            ImGui::Separator();
            ImGui::Text("Snaplines");
            ImGui::Checkbox("Enable Snaplines", &Esp_line);
            if (Esp_line) {
                ImGui::PushItemWidth(120.0f);
                ImGui::Combo("Origin##snap", &snaplineOrigin, snapOriginItems, 3);
                ImGui::PopItemWidth();
            }
            ImGui::ColorEdit4("Snapline Color", espSnaplineColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::SameLine();
            ImGui::PushItemWidth(160.0f);
            ImGui::SliderFloat("Thickness##snap", &snaplineThickness, 0.5f, 5.0f, "%.1f");
            ImGui::PopItemWidth();

            
            ImGui::Separator();
            ImGui::Text("Info ESP");
            ImGui::Checkbox("Distance", &Esp_Distance);
            ImGui::SameLine();
            ImGui::Checkbox("Line to Head", &lineheadesp);

            ImGui::Checkbox("Health Bar", &depthVisualization);

            ImGui::Separator();
            ImGui::Text("Filters");
            ImGui::Checkbox("Team Check", &espTeamCheck);
            ImGui::SameLine();
            ImGui::Checkbox("Death Check", &espDeathCheck);

            ImGui::ColorEdit4("Distance Color", espDistanceColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

            
            ImGui::Separator();
            ImGui::Text("Operator ESP");
            ImGui::Checkbox("Operator Names", &shaderLabelOverlay);
            if (shaderLabelOverlay) {
                ImGui::SameLine();
                ImGui::Checkbox("Show Icons", &shaderIconOverlay);
                if (g_shaderResReady) {
                    ImGui::TextColored(ImVec4(0.5f,1,0.5f,1), "Icons: %d loaded", g_shaderResLoaded);
                } else {
                    ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "Icons: not loaded yet");
                }
            }

            
            ImGui::Separator();
            ImGui::PushItemWidth(220.0f);
            ImGui::SliderInt("Render Distance", &VisDist, 20, 500, "%d m");
            ImGui::PopItemWidth();

            ImGui::Separator();
            if (ImGui::Button("Rescan Entities", ImVec2(200, 25))) {
                ++g_collectionRequest.rescan;
            }
            ImGui::SameLine();
            ImGui::Text("E:%d P:%d", g_renderSnapshot ? (int)g_renderSnapshot->vertices.size() : 0,
                g_renderSnapshot ? g_renderSnapshot->activeVertices : 0);
            ImGui::Text("Menu: %.1f FPS", ImGui::GetIO().Framerate);
            if (!g_pipelineReady || g_collectionWorker.Failed())
                ImGui::TextUnformatted("Game data unavailable");
            if (g_pipelineError)
                ImGui::TextWrapped("ESP startup: %s", g_pipelineError);

            ImGui::PushItemWidth(220.0f);
            ImGui::PopItemWidth();

            ImGui::EndTabItem();
        }

        
        if (ImGui::BeginTabItem("Trail")) {
            g_menuTab = 4;
            ImGui::Spacing();
            ImGui::Text("Player Trail");
            ImGui::Checkbox("Enable Trail", &playerTrail);

            ImGui::Separator();
            ImGui::ColorEdit4("Trail Color", espTrailColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

            ImGui::PushItemWidth(220.0f);
            ImGui::SliderFloat("Trail Thickness", &trailThickness, 0.5f, 8.0f, "%.1f");
            ImGui::SliderInt("Trail Length", &trailLength, 10, 200, "%d frames");
            ImGui::SliderInt("Trail Update (ms)", &trailUpdateMs, 16, 100, "%d ms");
            ImGui::PopItemWidth();

            ImGui::Separator();
            ImGui::Checkbox("Fade Trail", &trailFade);
            ImGui::Checkbox("Rainbow Trail", &rainbowTrail);

            ImGui::Separator();
            if (ImGui::Button("Clear Trails")) {
                g_trailBufCount = 0;
                memset(g_trailBuffers, 0, sizeof(g_trailBuffers));
            }

            ImGui::EndTabItem();
        }

        
        if (ImGui::BeginTabItem("Colors")) {
            g_menuTab = 2;
            ImGui::Spacing();
            ImGui::Checkbox("Rainbow Mode", &rainbowMode);
            if (rainbowMode) {
                ImGui::Indent();
                ImGui::Checkbox("Rainbow Box", &rainbowBox);
                ImGui::Checkbox("Rainbow Snaplines", &rainbowSnaplines);
                ImGui::Checkbox("Rainbow FOV", &rainbowFov);
                ImGui::Checkbox("Rainbow Trail", &rainbowTrail);
                ImGui::Unindent();
            }

            ImGui::Separator();
            ImGui::Text("Individual Colors:");
            ImGui::ColorEdit4("Box##ce", espBoxColor, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Snaplines##ce", espSnaplineColor, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Trail##ce", espTrailColor, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Distance##ce", espDistanceColor, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("FOV Circle##ce", fovCircleColor, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Crosshair##ce", crosshairColor, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Aim Target##ce", aimbotTargetColor, ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Fill Box##ce", filledBoxColor, ImGuiColorEditFlags_AlphaBar);

            ImGui::EndTabItem();
        }

        
        if (ImGui::BeginTabItem("Misc")) {
            g_menuTab = 3;
            ImGui::Spacing();
            char _wmk[64]={};
            _rcal_get_watermark(_wmk);
            ImGui::Text("%s", _wmk);
            ImGui::Text("Build: %s %s", __DATE__, __TIME__);
            ImGui::Separator();
            ImGui::Text("Base: 0x%llX", (unsigned long long)base_address);
            ImGui::Text("PID: %lu", processID);
            ImGui::Text("Entity Cache: %d", g_renderSnapshot ? (int)g_renderSnapshot->cacheSize : 0);
            if (g_shaderResReady)
                ImGui::Text("Op Icons: %d/%d loaded", g_shaderResLoaded, g_opIconCount);
            ImGui::Separator();
            ImGui::Text("Exploits:");
           //ImGui::Checkbox("Running Sidewards", &sidewardsEnabled);
           //if (sidewardsEnabled) {
           //    ImGui::SliderFloat("Degrees", &sidewardsValue, 1.0f, 50.0f, "%.1f");
           //    if (g_Sidewards.found) {
           //        SetSidewardsValue(sidewardsValue);
           //        ImGui::TextColored(ImVec4(0.5f,1,0.5f,1), "Active at 0x%llX", (unsigned long long)g_Sidewards.addr);
           //    } else {
           //        if (ImGui::Button("Rescan")) ScanSidewards();
           //        ImGui::SameLine();
           //        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Not found");
           //    }
           //} else if (g_Sidewards.patched) {
           //    RestoreSidewards();
           //}


            ImGui::Separator();
            ImGui::Text("Weather FX");
            static const char* weatherItems[] = { "Snow", "Rain", "Fire", "Off" };
            int wm = g_weatherMode;
            if (ImGui::Combo("Weather##fx", &wm, weatherItems, 4)) {
                g_weatherMode = wm;
                g_wfxInited = false;
            }
            if (g_weatherMode != WFX_NONE) {
                ImGui::SliderFloat("Intensity##wfx", &g_wfxIntensity, 0.1f, 1.0f, "%.1f");
                g_weatherIntensity = g_wfxIntensity;
                ImGui::SliderFloat("Wind##wfx", &g_wfxWindX, -2.0f, 2.0f, "%.1f");
                g_weatherWind = g_wfxWindX;
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        }
        ImGui::End();
    }
    UpdateOverlayInteractivity();

    g_collectionRequest.width = Width;
    g_collectionRequest.height = Height;
    g_collectionRequest.maxDistance = VisDist;
    g_collectionRequest.active = true;
    g_collectionRequest.skeleton = Esp_skeleton || skeletonAim;
    g_collectionRequest.labels = shaderLabelOverlay;
    g_collectionRequest.sidewards = sidewardsEnabled;
    g_collectionRequest.sidewardsValue = sidewardsValue;
    g_collectionWorker.SetRequest(g_collectionRequest);
    SubmitDrawCalls();

    ImGui::EndFrame();
    D3dDevice->SetRenderState(D3DRS_ZENABLE, false);
    D3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, false);
    D3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, false);
    D3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
    if (D3dDevice->BeginScene() >= 0) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        D3dDevice->EndScene();
    }
    HRESULT result = D3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
    if (result == D3DERR_DEVICELOST) g_deviceResetPending = true;
}

MSG Message = { NULL };
void xMainLoop() {
    RECT oldBounds = {};
    GetGameClientBounds(&oldBounds);
    bool visible = true;
    if (g_pipelineReady && !g_collectionWorker.Start(g_collectionRequest,
            ENTITY_UPDATE_INTERVAL, CollectOverlaySnapshot))
        printf("[!] Could not start game-data worker; menu remains available\n");
    ZeroMemory(&Message, sizeof(MSG));
    while (Message.message != WM_QUIT) {
        UpdateOverlayInteractivity();
        while (PeekMessage(&Message, nullptr, 0, 0, PM_REMOVE)) {
            if (Message.message == WM_QUIT) break;
            TranslateMessage(&Message);
            DispatchMessage(&Message);
        }
        if (Message.message == WM_QUIT || !IsWindow(Window) || !IsWindow(hwnd)) break;
        if (GetAsyncKeyState(VK_END) & 1) break;

        const bool shouldShow = IsGameOrOverlayForeground() && !IsIconic(hwnd);
        if (shouldShow != visible) {
            if (shouldShow)
                SetWindowPos(Window, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            else
                ShowWindow(Window, SW_HIDE);
            visible = shouldShow;
        }
        if (!visible) {
            g_collectionRequest.active = false;
            g_collectionWorker.SetRequest(g_collectionRequest);
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        RECT bounds = {};
        if (GetGameClientBounds(&bounds) && !EqualRect(&bounds, &oldBounds)) {
            const int newWidth = bounds.right - bounds.left;
            const int newHeight = bounds.bottom - bounds.top;
            const bool sizeChanged = newWidth != Width || newHeight != Height;
            oldBounds = bounds;
            SetWindowPos(Window, HWND_TOPMOST, bounds.left, bounds.top, newWidth, newHeight,
                SWP_NOACTIVATE);
            if (sizeChanged) {
                Width = newWidth;
                Height = newHeight;
                d3dpp.BackBufferWidth = Width;
                d3dpp.BackBufferHeight = Height;
                g_deviceResetPending = true;
            }
        }

        // Lost devices must not receive new frames. Keep pumping input and retry
        // failed resize resets even when TestCooperativeLevel returns D3D_OK.
        const HRESULT state = D3dDevice->TestCooperativeLevel();
        if (state == D3DERR_DEVICENOTRESET) g_deviceResetPending = true;
        if ((state != D3D_OK && state != D3DERR_DEVICENOTRESET) ||
            (g_deviceResetPending && !ResetD3dDevice())) {
            g_collectionRequest.active = false;
            g_collectionWorker.SetRequest(g_collectionRequest);
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        g_deviceResetPending = false;
        render();
    }
    g_collectionWorker.Stop();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
    int button = -1;
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_LBUTTONUP) button = 0;
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_RBUTTONUP) button = 1;
    if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK || message == WM_MBUTTONUP) button = 2;
    if (button >= 0) {
        if (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP) {
            g_menuMouseDown[button] = false;
        } else {
            const POINT click = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (ShowMenu && ImGui::GetCurrentContext() && (PtInRect(&g_menuBounds, click) ||
                    ImGui::GetIO().WantCaptureMouse || GetCapture() == hWnd) &&
                    !g_menuMouseDown[button]) {
                g_menuMouseDown[button] = true;
                g_menuMousePressed[button] = true;
            }
        }
    }
    if (handled) return handled;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, message, wParam, lParam);
}

void xShutdown() {
    ShutdownRenderPipeline();
    ReleaseShaderResources();
    if (D3dDevice) { D3dDevice->Release(); D3dDevice = nullptr; }
    if (p_Object) { p_Object->Release(); p_Object = nullptr; }

    if (Window && IsWindow(Window)) DestroyWindow(Window);
    UnregisterClassA(g_overlayClassName, GetModuleHandleA(NULL));
    Window = NULL;
    g_medalWindow = NULL;
}
