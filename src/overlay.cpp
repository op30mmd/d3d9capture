#include "overlay.h"
#include "capture.h"
#include "recorder.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include <atomic>
#include <mutex>
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

enum class OverlayRenderer
{
    None,
    D3D9,
    D3D11
};

static std::atomic<bool> g_OverlayInitialized{ false };

// Overlay_Init and Overlay_InitDXGI can both be entered from two threads at
// once: the DLL's worker thread installs hooks the moment it finds a device or
// swap chain (including via the memory scan), while the game's render thread
// reaches the same init from inside Present. Testing g_OverlayInitialized alone
// leaves a window several milliseconds wide -- all of CreateContext, both
// backend inits and the WndProc hook -- in which both threads pass the check.
//
// Running the WndProc hook twice is the dangerous part: the second
// SetWindowLongPtr returns the HookedWndProc we already installed and stores it
// as g_OriginalWndProc, so CallWindowProc recurses into itself and blows the
// stack on the next window message. That reads as a random startup crash with
// nothing pointing at the overlay.
static std::mutex g_OverlayInitMtx;
static std::atomic<OverlayRenderer> g_ActiveRenderer{ OverlayRenderer::None };

static HWND g_OverlayHwnd = nullptr;
static WNDPROC g_OriginalWndProc = nullptr;
static std::atomic<bool> g_ShowMenu{ false };

static ID3D11Device* g_pD3D11Device = nullptr;
static ID3D11DeviceContext* g_pD3D11Context = nullptr;
static ID3D11RenderTargetView* g_pD3D11RTV = nullptr;

// ── Mini-HUD and Customization state ─────────────────────────────────────────
static bool  s_ShowMiniHud = false;
static int   s_MiniHudPos = 0; // 0: Top-Right, 1: Top-Left, 2: Bottom-Right, 3: Bottom-Left
static bool  s_MiniHudFps = true;
static bool  s_MiniHudLatency = true;
static bool  s_MiniHudRes = true;

// ── Video Recording settings ─────────────────────────────────────────────────
static int   s_RecFpsIdx = 0;     // 0: 30 FPS, 1: 60 FPS, 2: 24 FPS
static int   s_RecBitrateIdx = 1; // 0: 4000, 1: 8000, 2: 12000, 3: 16000, 4: 24000 kbps

static int   s_SelectedTheme = 0; // 0: Emerald (GTA), 1: Cyberpunk, 2: Dark, 3: Light, 4: Classic
static float s_WindowAlpha = 0.92f;
static bool  s_ShowDemoWindow = false;
static bool  s_ShowMetricsWindow = false;

// ── Performance history for graphs ───────────────────────────────────────────
static constexpr int HISTORY_SIZE = 120;
static float s_PresentFpsHistory[HISTORY_SIZE] = {};
static float s_LatencyHistory[HISTORY_SIZE] = {};
static int   s_HistoryOffset = 0;
static DWORD s_LastHistoryTick = 0;

bool Overlay_IsMenuOpen()
{
    return g_ShowMenu.load();
}

// ── Mouse Wheel Interception State ───────────────────────────────────────────
static std::atomic<float> s_PendingWheelX{ 0.0f };
static std::atomic<float> s_PendingWheelY{ 0.0f };
static DWORD s_LastWheelTick = 0;
static float s_LastWheelY = 0.0f;
static std::mutex s_WheelMtx;
static HHOOK s_MouseHook = nullptr;

void Overlay_AddMouseWheel(float wheelX, float wheelY)
{
    if (wheelX == 0.0f && wheelY == 0.0f)
        return;

    DWORD now = GetTickCount();
    {
        std::lock_guard<std::mutex> lk(s_WheelMtx);
        if ((now - s_LastWheelTick) < 20 && s_LastWheelY == wheelY)
        {
            return;
        }
        s_LastWheelTick = now;
        s_LastWheelY = wheelY;
    }

    float curY = s_PendingWheelY.load(std::memory_order_relaxed);
    while (!s_PendingWheelY.compare_exchange_weak(curY, curY + wheelY, std::memory_order_relaxed)) {}

    float curX = s_PendingWheelX.load(std::memory_order_relaxed);
    while (!s_PendingWheelX.compare_exchange_weak(curX, curX + wheelX, std::memory_order_relaxed)) {}
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_ShowMenu.load())
    {
        if (wParam == WM_MOUSEWHEEL)
        {
            auto ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            if (ms)
            {
                short delta = static_cast<short>(HIWORD(ms->mouseData));
                if (delta != 0)
                {
                    Overlay_AddMouseWheel(0.0f, static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA));
                }
            }
            return 1;
        }
        else if (wParam == WM_MOUSEHWHEEL)
        {
            auto ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            if (ms)
            {
                short delta = static_cast<short>(HIWORD(ms->mouseData));
                if (delta != 0)
                {
                    Overlay_AddMouseWheel(static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA), 0.0f);
                }
            }
            return 1;
        }
    }
    return CallNextHookEx(s_MouseHook, nCode, wParam, lParam);
}

static void SetMenuState(bool open)
{
    g_ShowMenu.store(open);
    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = open;
        if (open && g_OverlayHwnd)
        {
            POINT pt;
            if (GetCursorPos(&pt) && ScreenToClient(g_OverlayHwnd, &pt))
            {
                io.AddMousePosEvent((float)pt.x, (float)pt.y);
            }
        }
        else if (!open)
        {
            io.AddMouseButtonEvent(0, false);
            io.AddMouseButtonEvent(1, false);
            io.AddMouseButtonEvent(2, false);
        }
    }
    if (open)
    {
        ReleaseCapture();
        ClipCursor(nullptr);
        if (!s_MouseHook)
        {
            HMODULE hMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&LowLevelMouseProc), &hMod);
            s_MouseHook = SetWindowsHookExA(WH_MOUSE_LL, LowLevelMouseProc, hMod, 0);
        }
    }
    else
    {
        if (s_MouseHook)
        {
            UnhookWindowsHookEx(s_MouseHook);
            s_MouseHook = nullptr;
        }
    }
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_INSERT)
        {
            SetMenuState(!g_ShowMenu.load());
            return 1;
        }
        else if (wParam == VK_F9)
        {
            if (Recorder_IsRecording())
            {
                Recorder_Stop();
            }
            else
            {
                uint32_t fps = (s_RecFpsIdx == 0) ? 30 : ((s_RecFpsIdx == 1) ? 60 : 24);
                uint32_t bitrates[] = { 4000, 8000, 12000, 16000, 24000 };
                Recorder_Start(nullptr, fps, bitrates[s_RecBitrateIdx]);
            }
            return 1;
        }
    }

    if (g_ShowMenu.load())
    {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        if (msg == WM_MOUSEWHEEL)
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta != 0)
            {
                Overlay_AddMouseWheel(0.0f, static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA));
            }
            return 1;
        }
        if (msg == WM_MOUSEHWHEEL)
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta != 0)
            {
                Overlay_AddMouseWheel(static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA), 0.0f);
            }
            return 1;
        }

        bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                          (msg >= WM_NCMOUSEMOVE && msg <= 0x00AD);
        if (isMouseMsg)
            return 1;

        if (msg == WM_INPUT)
        {
            RAWINPUT raw = {};
            UINT rawSize = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER)) != (UINT)-1)
            {
                if (raw.header.dwType == RIM_TYPEMOUSE)
                {
                    if (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                    {
                        short delta = static_cast<short>(raw.data.mouse.usButtonData);
                        if (delta != 0)
                        {
                            Overlay_AddMouseWheel(0.0f, static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA));
                        }
                    }
                    else if (raw.data.mouse.usButtonFlags & RI_MOUSE_HWHEEL)
                    {
                        short delta = static_cast<short>(raw.data.mouse.usButtonData);
                        if (delta != 0)
                        {
                            Overlay_AddMouseWheel(static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA), 0.0f);
                        }
                    }
                }
            }
            DefWindowProcA(hWnd, msg, wParam, lParam);
            return 0;
        }

        if (msg == WM_SETCURSOR)
        {
            SetCursor(nullptr);
            return 1;
        }

        if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)
        {
            return 1;
        }
    }

    return CallWindowProc(g_OriginalWndProc, hWnd, msg, wParam, lParam);
}

static const char* GetFormatName(UINT32 fmt)
{
    if (g_ActiveRenderer.load() == OverlayRenderer::D3D11)
    {
        switch (static_cast<DXGI_FORMAT>(fmt))
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:      return "DXGI_FORMAT_R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM:      return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM:   return "DXGI_FORMAT_R10G10B10A2_UNORM (HDR)";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "DXGI_FORMAT_R16G16B16A16_FLOAT (64bpp HDR)";
        default:                              return "DXGI Custom / Unknown Format";
        }
    }
    else
    {
        switch (static_cast<D3DFORMAT>(fmt))
        {
        case D3DFMT_X8R8G8B8:                 return "D3DFMT_X8R8G8B8 (32bpp RGB)";
        case D3DFMT_A8R8G8B8:                 return "D3DFMT_A8R8G8B8 (32bpp ARGB)";
        case D3DFMT_A2R10G10B10:              return "D3DFMT_A2R10G10B10 (10-bit HDR)";
        case D3DFMT_R5G6B5:                   return "D3DFMT_R5G6B5 (16bpp 565)";
        case D3DFMT_X1R5G5B5:                 return "D3DFMT_X1R5G5B5 (16bpp 555)";
        case D3DFMT_A1R5G5B5:                 return "D3DFMT_A1R5G5B5 (16bpp 1555)";
        default:                              return "D3D9 Custom / Unknown Format";
        }
    }
}

static void OpenDirectoryInExplorer(const char* dir)
{
    CreateDirectoryA(dir, nullptr);
    ShellExecuteA(nullptr, "explore", dir, nullptr, nullptr, SW_SHOWNORMAL);
}

static void ApplyTheme(int themeIdx)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    switch (themeIdx)
    {
    case 0: // Emerald / GTA Style
    {
        ImGui::StyleColorsDark();
        colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_Header]               = ImVec4(0.18f, 0.50f, 0.30f, 0.55f);
        colors[ImGuiCol_HeaderHovered]        = ImVec4(0.22f, 0.65f, 0.38f, 0.80f);
        colors[ImGuiCol_HeaderActive]         = ImVec4(0.15f, 0.75f, 0.40f, 1.00f);
        colors[ImGuiCol_Button]               = ImVec4(0.14f, 0.45f, 0.26f, 0.65f);
        colors[ImGuiCol_ButtonHovered]        = ImVec4(0.18f, 0.60f, 0.35f, 0.85f);
        colors[ImGuiCol_ButtonActive]         = ImVec4(0.12f, 0.70f, 0.38f, 1.00f);
        colors[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.15f, 0.14f, 0.70f);
        colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.25f, 0.22f, 0.80f);
        colors[ImGuiCol_FrameBgActive]        = ImVec4(0.20f, 0.35f, 0.28f, 0.90f);
        colors[ImGuiCol_TitleBg]              = ImVec4(0.07f, 0.12f, 0.09f, 1.00f);
        colors[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.25f, 0.16f, 1.00f);
        colors[ImGuiCol_CheckMark]            = ImVec4(0.25f, 0.90f, 0.50f, 1.00f);
        colors[ImGuiCol_SliderGrab]           = ImVec4(0.22f, 0.75f, 0.42f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.28f, 0.95f, 0.55f, 1.00f);
        colors[ImGuiCol_Tab]                  = ImVec4(0.10f, 0.22f, 0.15f, 0.80f);
        colors[ImGuiCol_TabHovered]           = ImVec4(0.18f, 0.55f, 0.32f, 0.85f);
        colors[ImGuiCol_TabActive]            = ImVec4(0.15f, 0.45f, 0.28f, 1.00f);
        colors[ImGuiCol_PlotLines]            = ImVec4(0.25f, 0.90f, 0.50f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]     = ImVec4(0.35f, 1.00f, 0.65f, 1.00f);
        style.WindowRounding = 6.0f;
        style.FrameRounding  = 4.0f;
        style.GrabRounding   = 4.0f;
        break;
    }
    case 1: // Cyberpunk / Neon Cyan
    {
        ImGui::StyleColorsDark();
        colors[ImGuiCol_WindowBg]             = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
        colors[ImGuiCol_PopupBg]              = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
        colors[ImGuiCol_Header]               = ImVec4(0.10f, 0.40f, 0.55f, 0.55f);
        colors[ImGuiCol_HeaderHovered]        = ImVec4(0.15f, 0.55f, 0.75f, 0.80f);
        colors[ImGuiCol_HeaderActive]         = ImVec4(0.20f, 0.70f, 0.95f, 1.00f);
        colors[ImGuiCol_Button]               = ImVec4(0.12f, 0.35f, 0.50f, 0.65f);
        colors[ImGuiCol_ButtonHovered]        = ImVec4(0.18f, 0.50f, 0.70f, 0.85f);
        colors[ImGuiCol_ButtonActive]         = ImVec4(0.22f, 0.65f, 0.90f, 1.00f);
        colors[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.14f, 0.18f, 0.70f);
        colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.16f, 0.20f, 0.28f, 0.80f);
        colors[ImGuiCol_FrameBgActive]        = ImVec4(0.20f, 0.28f, 0.40f, 0.90f);
        colors[ImGuiCol_TitleBg]              = ImVec4(0.06f, 0.10f, 0.15f, 1.00f);
        colors[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.22f, 0.35f, 1.00f);
        colors[ImGuiCol_CheckMark]            = ImVec4(0.00f, 0.85f, 0.95f, 1.00f);
        colors[ImGuiCol_SliderGrab]           = ImVec4(0.15f, 0.65f, 0.85f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.25f, 0.85f, 1.00f, 1.00f);
        colors[ImGuiCol_Tab]                  = ImVec4(0.10f, 0.18f, 0.25f, 0.80f);
        colors[ImGuiCol_TabHovered]           = ImVec4(0.15f, 0.40f, 0.60f, 0.85f);
        colors[ImGuiCol_TabActive]            = ImVec4(0.12f, 0.35f, 0.52f, 1.00f);
        colors[ImGuiCol_PlotLines]            = ImVec4(0.00f, 0.85f, 1.00f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]     = ImVec4(0.30f, 0.95f, 1.00f, 1.00f);
        style.WindowRounding = 5.0f;
        style.FrameRounding  = 3.0f;
        style.GrabRounding   = 3.0f;
        break;
    }
    case 2: // Dark
        ImGui::StyleColorsDark();
        break;
    case 3: // Light
        ImGui::StyleColorsLight();
        break;
    case 4: // Classic
        ImGui::StyleColorsClassic();
        break;
    }
    colors[ImGuiCol_WindowBg].w = 1.00f;
    colors[ImGuiCol_PopupBg].w  = 1.00f;
    style.Alpha = 1.00f;
}

static void UpdateHistory(const CaptureStats& stats)
{
    DWORD now = GetTickCount();
    if (now - s_LastHistoryTick >= 50)
    {
        s_PresentFpsHistory[s_HistoryOffset] = stats.presentFps;
        s_LatencyHistory[s_HistoryOffset]    = stats.readbackMs;
        s_HistoryOffset = (s_HistoryOffset + 1) % HISTORY_SIZE;
        s_LastHistoryTick = now;
    }
}

static void RenderMiniHud(const CaptureStats& stats, int positionPreset, bool showFps, bool showLatency, bool showRes)
{
    ImGuiIO& io = ImGui::GetIO();
    float margin = 12.0f;
    ImVec2 windowPos;
    ImVec2 windowPivot;

    switch (positionPreset)
    {
    case 0:
        windowPos = ImVec2(io.DisplaySize.x - margin, margin);
        windowPivot = ImVec2(1.0f, 0.0f);
        break;
    case 1:
        windowPos = ImVec2(margin, margin);
        windowPivot = ImVec2(0.0f, 0.0f);
        break;
    case 2:
        windowPos = ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin);
        windowPivot = ImVec2(1.0f, 1.0f);
        break;
    case 3:
        windowPos = ImVec2(margin, io.DisplaySize.y - margin);
        windowPivot = ImVec2(0.0f, 1.0f);
        break;
    default:
        windowPos = ImVec2(io.DisplaySize.x - margin, margin);
        windowPivot = ImVec2(1.0f, 0.0f);
        break;
    }

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPivot);
    ImGui::SetNextWindowBgAlpha(0.70f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;

    if (!g_ShowMenu.load())
        flags |= ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##d3dcapture_MiniHUD", nullptr, flags))
    {
        bool hasItem = false;
        RecorderStats recStats = {};
        Recorder_GetStats(&recStats);
        if (recStats.isRecording)
        {
            if (recStats.isPaused)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[PAUSED %02u:%02u]",
                    (unsigned)(recStats.durationMs / 60000), (unsigned)((recStats.durationMs / 1000) % 60));
            }
            else
            {
                bool blink = ((GetTickCount() / 500) % 2) == 0;
                ImGui::TextColored(blink ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f) : ImVec4(0.6f, 0.1f, 0.1f, 1.0f),
                    "[REC %02u:%02u]",
                    (unsigned)(recStats.durationMs / 60000), (unsigned)((recStats.durationMs / 1000) % 60));
            }
            hasItem = true;
        }

        if (!stats.captureEnabled)
        {
            if (hasItem) ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "[CAPTURE PAUSED]");
            hasItem = true;
        }

        if (showFps)
        {
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%.1f FPS", stats.presentFps);
            hasItem = true;
        }

        if (showLatency && stats.readbackMs > 0.0f)
        {
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%.1f ms", stats.readbackMs);
            hasItem = true;
        }

        if (showRes && stats.width > 0)
        {
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%ux%u", stats.width, stats.height);
            hasItem = true;
        }
    }
    ImGui::End();
}

static void RenderControlCenter(const CaptureStats& stats)
{
    if (!g_ShowMenu.load()) return;

    ImGui::SetNextWindowSize(ImVec2(580, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(s_WindowAlpha);
    bool menuOpen = g_ShowMenu.load();
    if (ImGui::Begin("d3dcapture - Control Center", &menuOpen, ImGuiWindowFlags_NoCollapse))
    {
        if (!stats.captureEnabled)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "[CAPTURE PAUSED]");
            ImGui::SameLine();
            ImGui::TextDisabled("| Press INSERT to toggle menu | F9 to Start/Stop Recording");
        }
        else
        {
            ImGui::TextDisabled("Press INSERT to toggle menu | F9 to Start/Stop Recording");
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("ControlTabs", ImGuiTabBarFlags_None))
        {
            if (ImGui::BeginTabItem("Dashboard"))
            {
                ImGui::Spacing();
                if (ImGui::BeginTable("MetricsTable", 2, ImGuiTableFlags_BordersInnerV))
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("Game FPS:"); ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "%.1f FPS", stats.presentFps);

                    ImGui::Text("Capture Rate:"); ImGui::SameLine();
                    if (stats.targetFps > 0)
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%.1f FPS (Capped at %d)", stats.captureFps, stats.targetFps);
                    else
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%.1f FPS (Uncapped)", stats.captureFps);

                    ImGui::Text("DMA Latency:"); ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "%.2f ms", stats.readbackMs);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("Resolution: %u x %u", stats.width, stats.height);
                    ImGui::Text("Format: %s", GetFormatName(stats.format));
                    ImGui::Text("Presents: %llu | Captured: %llu",
                                static_cast<unsigned long long>(stats.presentCalls),
                                static_cast<unsigned long long>(stats.capturedFrames));

                    ImGui::EndTable();
                }

                ImGui::Separator();
                ImGui::Spacing();

                float minFps = 999.0f, maxFps = 0.0f;
                float totalLat = 0.0f, maxLat = 0.0f;
                int latSamples = 0;
                for (int i = 0; i < HISTORY_SIZE; ++i)
                {
                    float f = s_PresentFpsHistory[i];
                    if (f > 0.0f)
                    {
                        if (f < minFps) minFps = f;
                        if (f > maxFps) maxFps = f;
                    }
                    float l = s_LatencyHistory[i];
                    if (l > 0.0f)
                    {
                        totalLat += l;
                        latSamples++;
                        if (l > maxLat) maxLat = l;
                    }
                }
                if (minFps > maxFps) minFps = 0.0f;
                float avgLat = (latSamples > 0) ? (totalLat / latSamples) : 0.0f;
                float plotFpsMax = (maxFps > 60.0f) ? (maxFps * 1.15f) : 75.0f;
                float plotLatMax = (maxLat > 8.0f) ? (maxLat * 1.25f) : 10.0f;

                ImGui::Text("FPS History (Current: %.1f | Min: %.1f | Max: %.1f):", stats.presentFps, minFps, maxFps);
                ImGui::PlotLines("##FpsPlot", s_PresentFpsHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, 0.0f, plotFpsMax, ImVec2(-1, 65.0f));

                ImGui::Spacing();
                ImGui::Text("Readback Latency History (Current: %.2f ms | Avg: %.2f ms):", stats.readbackMs, avgLat);
                ImGui::PlotLines("##LatPlot", s_LatencyHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, 0.0f, plotLatMax, ImVec2(-1, 65.0f));

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Capture & Recording"))
            {
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "Native In-Game Video Recorder (H.264 MP4):");

                RecorderStats recStats = {};
                Recorder_GetStats(&recStats);

                if (!recStats.isRecording)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.58f, 0.30f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.72f, 0.38f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.85f, 0.45f, 1.0f));
                    if (ImGui::Button("[REC] Start Recording (F9)", ImVec2(230, 36)))
                    {
                        uint32_t fps = (s_RecFpsIdx == 0) ? 30 : ((s_RecFpsIdx == 1) ? 60 : 24);
                        uint32_t bitrates[] = { 4000, 8000, 12000, 16000, 24000 };
                        Recorder_Start(nullptr, fps, bitrates[s_RecBitrateIdx]);
                    }
                    ImGui::PopStyleColor(3);
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.16f, 0.16f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.22f, 0.22f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.10f, 0.10f, 1.0f));
                    if (ImGui::Button("[STOP] Stop Recording (F9)", ImVec2(200, 36)))
                    {
                        Recorder_Stop();
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::SameLine();
                    if (recStats.isPaused)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.82f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.98f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.75f, 1.00f, 1.0f));
                        if (ImGui::Button("[>] Resume", ImVec2(130, 36)))
                        {
                            Recorder_Resume();
                        }
                        ImGui::PopStyleColor(3);
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.58f, 0.10f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.70f, 0.15f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.80f, 0.10f, 1.0f));
                        if (ImGui::Button("[||] Pause", ImVec2(130, 36)))
                        {
                            Recorder_Pause();
                        }
                        ImGui::PopStyleColor(3);
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Open Folder", ImVec2(120, 36)))
                {
                    OpenDirectoryInExplorer("C:\\d3d9capture\\recordings");
                }

                ImGui::Spacing();
                if (recStats.isRecording)
                {
                    unsigned totalSec = static_cast<unsigned>(recStats.durationMs / 1000);
                    unsigned hrs = totalSec / 3600;
                    unsigned mins = (totalSec % 3600) / 60;
                    unsigned secs = totalSec % 60;

                    bool blink = ((GetTickCount() / 500) % 2) == 0;
                    if (recStats.isPaused)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[PAUSED]");
                    }
                    else
                    {
                        ImGui::TextColored(blink ? ImVec4(1.0f, 0.25f, 0.25f, 1.0f) : ImVec4(0.6f, 0.1f, 0.1f, 1.0f), "[REC]");
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Duration: %02u:%02u:%02u  |  Frames: %llu  |  Res: %ux%u",
                        hrs, mins, secs,
                        static_cast<unsigned long long>(recStats.recordedFrames),
                        recStats.width, recStats.height);

                    ImGui::TextDisabled("Saving to: %s", recStats.outputPath);
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Status: Ready to record (Hardware-accelerated H.264 MP4)");
                    if (recStats.outputPath[0] != '\0')
                    {
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Last Saved: %s", recStats.outputPath);
                    }
                }

                ImGui::Spacing();
                ImGui::SetNextItemWidth(140);
                const char* fpsItems[] = { "30 FPS", "60 FPS", "24 FPS (Cinematic)" };
                ImGui::Combo("Video FPS", &s_RecFpsIdx, fpsItems, IM_ARRAYSIZE(fpsItems));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                const char* bitrateItems[] = { "4 Mbps (Compact)", "8 Mbps (Standard)", "12 Mbps (High Quality)", "16 Mbps (Crisp)", "24 Mbps (Master)" };
                ImGui::Combo("Bitrate", &s_RecBitrateIdx, bitrateItems, IM_ARRAYSIZE(bitrateItems));
                ImGui::TextDisabled("Shortcut: Press F9 at any time while playing to Start / Stop recording.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "GPU Capture Engine:");

                bool capEnabled = stats.captureEnabled;
                if (ImGui::Checkbox("Enable GPU Frame Capture (Master Switch)", &capEnabled))
                {
                    Capture_SetEnabled(capEnabled);
                }
                ImGui::TextDisabled("Unchecking halts GPU staging and readback DMA completely.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Capture Framerate Limiter (Throttle):");
                int curTarget = stats.targetFps;
                const char* targetItems[] = { "Uncapped (Match Game FPS)", "60 FPS", "30 FPS", "24 FPS (Cinematic)", "15 FPS (Low Bandwidth)" };
                int targetValues[] = { 0, 60, 30, 24, 15 };
                int curSel = 0;
                for (int i = 0; i < 5; ++i)
                {
                    if (targetValues[i] == curTarget) { curSel = i; break; }
                }
                ImGui::SetNextItemWidth(260);
                if (ImGui::Combo("##TargetFps", &curSel, targetItems, IM_ARRAYSIZE(targetItems)))
                {
                    Capture_SetTargetFps(targetValues[curSel]);
                }
                ImGui::TextDisabled("Limits GPU readback frequency while allowing the game to render at maximum uncapped FPS.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Instant High-Res Screenshot:");
                if (ImGui::Button("Take High-Res Screenshot (BMP)", ImVec2(240, 30)))
                {
                    Capture_TriggerSnapshot();
                }
                ImGui::SameLine();
                if (ImGui::Button("Open Screenshots Folder", ImVec2(180, 30)))
                {
                    OpenDirectoryInExplorer("C:\\d3d9capture\\screenshots");
                }

                if (stats.lastScreenshotPath[0] != '\0')
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Last Saved: %s", stats.lastScreenshotPath);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Frame Burst Dumper (Diagnostic Tool):");
                static int s_DumpCountToRequest = 30;
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("Frames to Dump", &s_DumpCountToRequest);
                if (s_DumpCountToRequest < 1) s_DumpCountToRequest = 1;
                if (s_DumpCountToRequest > 1000) s_DumpCountToRequest = 1000;

                if (stats.dumpQuotaRemaining > 0)
                {
                    float prog = 1.0f - (static_cast<float>(stats.dumpQuotaRemaining) / static_cast<float>(s_DumpCountToRequest));
                    if (prog < 0.0f) prog = 0.0f;
                    if (prog > 1.0f) prog = 1.0f;
                    ImGui::ProgressBar(prog, ImVec2(240, 24));
                    ImGui::SameLine();
                    ImGui::Text("%d remaining", stats.dumpQuotaRemaining);
                }
                else
                {
                    if (ImGui::Button("Start Burst Dump", ImVec2(140, 28)))
                    {
                        Capture_SetDumpQuota(s_DumpCountToRequest);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Open Dumps Folder", ImVec2(150, 28)))
                    {
                        OpenDirectoryInExplorer("C:\\d3d9capture\\dumps");
                    }
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Appearance & HUD"))
            {
                ImGui::Spacing();
                ImGui::Text("Overlay Theme:");
                ImGui::SetNextItemWidth(260);
                if (ImGui::Combo("##ThemeCombo", &s_SelectedTheme, "Emerald (GTA Style)\0Cyberpunk (Cyan)\0Dark (Default)\0Light\0Classic\0\0"))
                {
                    ApplyTheme(s_SelectedTheme);
                }

                ImGui::Spacing();
                ImGui::Text("Window Background Opacity:");
                ImGui::SetNextItemWidth(260);
                ImGui::SliderFloat("##WindowAlpha", &s_WindowAlpha, 0.10f, 1.00f, "%.2f");
                ImGui::TextDisabled("Adjusts transparency of window panels while keeping UI text and widgets sharp.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Compact Mini-HUD (Pinned to screen):");
                ImGui::Checkbox("Show Mini-HUD when menu is closed", &s_ShowMiniHud);
                if (s_ShowMiniHud)
                {
                    ImGui::SetNextItemWidth(200);
                    ImGui::Combo("Position", &s_MiniHudPos, "Top-Right\0Top-Left\0Bottom-Right\0Bottom-Left\0\0");
                    ImGui::Checkbox("Show FPS", &s_MiniHudFps);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Latency", &s_MiniHudLatency);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Resolution", &s_MiniHudRes);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("ImGui Debugging Tools:");
                ImGui::Checkbox("Show ImGui Demo Window", &s_ShowDemoWindow);
                ImGui::SameLine();
                ImGui::Checkbox("Show ImGui Metrics", &s_ShowMetricsWindow);

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Info & Help"))
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "d3dcapture Inter-Process Architecture");
                ImGui::BulletText("Shared Memory: Local\\D3D9CaptureShm (Capacity: 126 MB)");
                ImGui::BulletText("Ready Event:   Local\\D3D9CaptureReady (Signals when new frame is written)");
                ImGui::BulletText("Done Event:    Local\\D3D9CaptureDone (Backpressure from consumer)");
                ImGui::BulletText("DirectInput 8: VTable Hook on IDirectInputDevice8::GetDeviceState & GetDeviceData");
                ImGui::BulletText("Mouse Policy:  Blocked while menu is open to prevent camera turns / weapon fires");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Companion Tools & Usage");
                ImGui::Text("1. View live capture:");
                ImGui::TextDisabled("   bin\\shm_reader.exe");
                ImGui::Text("2. Hardware video recording (NVENC / x264):");
                ImGui::TextDisabled("   bin\\shm_reader.exe --record output.mp4");
                ImGui::Text("3. Python / OpenCV reading:");
                ImGui::TextDisabled("   import mmap; shm = mmap.mmap(-1, 7680*4320*4, 'Local\\\\D3D9CaptureShm')");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Tip: You can drag any window or panel by clicking its header.");
                ImGui::Text("Press INSERT to close this menu.");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (!menuOpen)
    {
        SetMenuState(false);
    }
}

// ── D3D9 Overlay Initialization and Rendering ──────────────────────────────────
void Overlay_Init(IDirect3DDevice9* pDev)
{
    Log("[overlay] Overlay_Init entry pDev=%p", pDev);
    if (g_OverlayInitialized.load()) return;

    std::lock_guard<std::mutex> initLock(g_OverlayInitMtx);
    if (g_OverlayInitialized.load())
    {
        Log("[overlay] Another thread completed init first; nothing to do");
        return;
    }

    D3DDEVICE_CREATION_PARAMETERS cp = {};
    HRESULT hrcp = pDev->GetCreationParameters(&cp);
    Log("[overlay] GetCreationParameters hr=0x%08lX hwnd=%p", hrcp, cp.hFocusWindow);
    if (FAILED(hrcp))
    {
        Log("[overlay] Failed to get creation parameters");
        return;
    }

    HWND targetHwnd = cp.hFocusWindow;
    IDirect3DSwapChain9* pChain = nullptr;
    if (SUCCEEDED(pDev->GetSwapChain(0, &pChain)) && pChain)
    {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(pChain->GetPresentParameters(&pp)) && pp.hDeviceWindow)
        {
            targetHwnd = pp.hDeviceWindow;
        }
        pChain->Release();
    }

    g_OverlayHwnd = targetHwnd;
    if (!g_OverlayHwnd)
    {
        Log("[overlay] target window is null");
        return;
    }

    Log("[overlay] Calling CreateContext");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDebugIgnoreFocusLoss = true;

    ApplyTheme(s_SelectedTheme);

    Log("[overlay] Calling ImGui_ImplWin32_Init");
    if (!ImGui_ImplWin32_Init(g_OverlayHwnd))
    {
        Log("[overlay] ImGui_ImplWin32_Init failed");
        return;
    }
    Log("[overlay] Calling ImGui_ImplDX9_Init");
    if (!ImGui_ImplDX9_Init(pDev))
    {
        Log("[overlay] ImGui_ImplDX9_Init failed");
        return;
    }

    Log("[overlay] Hooking WndProc");
    g_OriginalWndProc = (WNDPROC)SetWindowLongPtrA(g_OverlayHwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    Log("[overlay] Initialized successfully for D3D9. Menu toggled with INSERT.");

    g_ActiveRenderer.store(OverlayRenderer::D3D9);
    g_OverlayInitialized.store(true);
}

// ── D3D11 / DXGI Overlay Initialization and Rendering ─────────────────────────
static bool CreateRenderTargetViewDXGI(IDXGISwapChain* pSwapChain)
{
    if (g_pD3D11RTV) return true;
    if (!g_pD3D11Device) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
    if (FAILED(hr) || !pBackBuffer) return false;

    hr = g_pD3D11Device->CreateRenderTargetView(pBackBuffer, nullptr, &g_pD3D11RTV);
    pBackBuffer->Release();
    return SUCCEEDED(hr);
}

static void CleanupRenderTargetViewDXGI()
{
    if (g_pD3D11RTV)
    {
        g_pD3D11RTV->Release();
        g_pD3D11RTV = nullptr;
    }
}

void Overlay_InitDXGI(IDXGISwapChain* pSwapChain)
{
    Log("[overlay] Overlay_InitDXGI entry pSwapChain=%p", pSwapChain);
    if (g_OverlayInitialized.load()) return;

    std::lock_guard<std::mutex> initLock(g_OverlayInitMtx);
    if (g_OverlayInitialized.load())
    {
        Log("[overlay] Another thread completed init first; nothing to do");
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(pSwapChain->GetDesc(&desc)) || !desc.OutputWindow)
    {
        Log("[overlay] Failed to get swap chain desc / OutputWindow");
        return;
    }

    ID3D11Device* pDev = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pDev))) || !pDev)
    {
        Log("[overlay] Failed to get ID3D11Device from swap chain");
        return;
    }

    ID3D11DeviceContext* pCtx = nullptr;
    pDev->GetImmediateContext(&pCtx);
    if (!pCtx)
    {
        pDev->Release();
        Log("[overlay] Failed to get ID3D11DeviceContext");
        return;
    }

    g_OverlayHwnd = desc.OutputWindow;
    g_pD3D11Device = pDev;
    g_pD3D11Context = pCtx;

    Log("[overlay] Calling CreateContext (DXGI)");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDebugIgnoreFocusLoss = true;

    ApplyTheme(s_SelectedTheme);

    Log("[overlay] Calling ImGui_ImplWin32_Init");
    if (!ImGui_ImplWin32_Init(g_OverlayHwnd))
    {
        Log("[overlay] ImGui_ImplWin32_Init failed");
        return;
    }

    Log("[overlay] Calling ImGui_ImplDX11_Init");
    if (!ImGui_ImplDX11_Init(g_pD3D11Device, g_pD3D11Context))
    {
        Log("[overlay] ImGui_ImplDX11_Init failed");
        return;
    }

    Log("[overlay] Hooking WndProc");
    g_OriginalWndProc = (WNDPROC)SetWindowLongPtrA(g_OverlayHwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    Log("[overlay] Initialized successfully for D3D11 / DXGI. Menu toggled with INSERT.");

    g_ActiveRenderer.store(OverlayRenderer::D3D11);
    g_OverlayInitialized.store(true);
}

void Overlay_Shutdown()
{
    if (!g_OverlayInitialized.load()) return;

    if (g_OverlayHwnd && g_OriginalWndProc)
    {
        SetWindowLongPtrA(g_OverlayHwnd, GWLP_WNDPROC, (LONG_PTR)g_OriginalWndProc);
        g_OriginalWndProc = nullptr;
    }

    if (s_MouseHook)
    {
        UnhookWindowsHookEx(s_MouseHook);
        s_MouseHook = nullptr;
    }

    if (g_ActiveRenderer.load() == OverlayRenderer::D3D9)
    {
        ImGui_ImplDX9_Shutdown();
    }
    else if (g_ActiveRenderer.load() == OverlayRenderer::D3D11)
    {
        CleanupRenderTargetViewDXGI();
        ImGui_ImplDX11_Shutdown();
        if (g_pD3D11Context) { g_pD3D11Context->Release(); g_pD3D11Context = nullptr; }
        if (g_pD3D11Device) { g_pD3D11Device->Release(); g_pD3D11Device = nullptr; }
    }

    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_ActiveRenderer.store(OverlayRenderer::None);
    g_OverlayInitialized.store(false);
    Log("[overlay] Shutdown complete");
}

void Overlay_OnPreReset()
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D9) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
}

void Overlay_OnPostReset(IDirect3DDevice9*)
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D9) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

void Overlay_OnPreResetDXGI()
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D11) return;
    CleanupRenderTargetViewDXGI();
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

void Overlay_OnPostResetDXGI(IDXGISwapChain* pSwapChain)
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D11) return;
    ImGui_ImplDX11_CreateDeviceObjects();
    if (pSwapChain)
        CreateRenderTargetViewDXGI(pSwapChain);
}

void Overlay_OnPresent(IDirect3DDevice9* pDev)
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D9) return;

    static bool s_PrevF9 = false;
    bool curF9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (curF9 && !s_PrevF9)
    {
        if (Recorder_IsRecording())
        {
            Recorder_Stop();
        }
        else
        {
            uint32_t fps = (s_RecFpsIdx == 0) ? 30 : ((s_RecFpsIdx == 1) ? 60 : 24);
            uint32_t bitrates[] = { 4000, 8000, 12000, 16000, 24000 };
            Recorder_Start(nullptr, fps, bitrates[s_RecBitrateIdx]);
        }
    }
    s_PrevF9 = curF9;

    if (!g_ShowMenu.load() && !s_ShowMiniHud && !Recorder_IsRecording()) return;

    ImGuiIO& io = ImGui::GetIO();
    io.AddFocusEvent(true);

    D3DVIEWPORT9 vp = {};
    if (SUCCEEDED(pDev->GetViewport(&vp)) && vp.Width > 0 && vp.Height > 0)
    {
        io.DisplaySize = ImVec2(static_cast<float>(vp.Width), static_cast<float>(vp.Height));
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    if (vp.Width > 0 && vp.Height > 0)
    {
        io.DisplaySize = ImVec2(static_cast<float>(vp.Width), static_cast<float>(vp.Height));
    }

    if (g_ShowMenu.load())
    {
        io.MouseDrawCursor = true;
        if (g_OverlayHwnd)
        {
            POINT pt;
            if (GetCursorPos(&pt) && ScreenToClient(g_OverlayHwnd, &pt))
            {
                RECT cr = {};
                GetClientRect(g_OverlayHwnd, &cr);
                float cw = static_cast<float>(cr.right - cr.left);
                float ch = static_cast<float>(cr.bottom - cr.top);
                if (cw > 0.0f && ch > 0.0f && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
                {
                    float mx = static_cast<float>(pt.x) * (io.DisplaySize.x / cw);
                    float my = static_cast<float>(pt.y) * (io.DisplaySize.y / ch);
                    io.AddMousePosEvent(mx, my);
                }
                else
                {
                    io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
                }
            }
        }

        static bool prevL = false, prevR = false, prevM = false;
        bool curL = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool curR = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        bool curM = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;

        if (curL != prevL) { io.AddMouseButtonEvent(0, curL); prevL = curL; }
        if (curR != prevR) { io.AddMouseButtonEvent(1, curR); prevR = curR; }
        if (curM != prevM) { io.AddMouseButtonEvent(2, curM); prevM = curM; }
    }
    else
    {
        io.MouseDrawCursor = false;
    }

    float wy = s_PendingWheelY.exchange(0.0f);
    float wx = s_PendingWheelX.exchange(0.0f);
    if (g_ShowMenu.load() && (wy != 0.0f || wx != 0.0f))
    {
        io.AddMouseWheelEvent(wx, wy);
    }

    ImGui::NewFrame();

    CaptureStats stats = {};
    Capture_GetStats(&stats);
    UpdateHistory(stats);

    if (!g_ShowMenu.load() && Recorder_IsRecording() && !s_ShowMiniHud)
    {
        RecorderStats recStats = {};
        Recorder_GetStats(&recStats);
        unsigned totalSec = static_cast<unsigned>(recStats.durationMs / 1000);
        unsigned mins = totalSec / 60;
        unsigned secs = totalSec % 60;

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 14.0f, 14.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.70f);
        ImGuiWindowFlags recFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##d3dcapture_RecIndicator", nullptr, recFlags))
        {
            if (recStats.isPaused)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[PAUSED %02u:%02u]", mins, secs);
            }
            else
            {
                bool blink = ((GetTickCount() / 500) % 2) == 0;
                ImGui::TextColored(blink ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f) : ImVec4(0.6f, 0.1f, 0.1f, 1.0f),
                    "[REC %02u:%02u]", mins, secs);
            }
        }
        ImGui::End();
    }

    if (s_ShowMiniHud)
    {
        RenderMiniHud(stats, s_MiniHudPos, s_MiniHudFps, s_MiniHudLatency, s_MiniHudRes);
    }

    RenderControlCenter(stats);

    if (s_ShowDemoWindow)
        ImGui::ShowDemoWindow(&s_ShowDemoWindow);
    if (s_ShowMetricsWindow)
        ImGui::ShowMetricsWindow(&s_ShowMetricsWindow);

    ImGui::Render();

    IDirect3DSurface9* pBackBuffer = nullptr;
    HRESULT hrBB = pDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);

    IDirect3DSurface9* pOldRT = nullptr;
    pDev->GetRenderTarget(0, &pOldRT);

    if (SUCCEEDED(hrBB) && pBackBuffer)
    {
        if (pOldRT != pBackBuffer)
            pDev->SetRenderTarget(0, pBackBuffer);
    }

    bool sceneBeganHere = false;
    HRESULT hrScene = pDev->BeginScene();
    if (SUCCEEDED(hrScene))
    {
        sceneBeganHere = true;
    }
    else if (hrScene != D3DERR_INVALIDCALL)
    {
        if (pOldRT) { if (pOldRT != pBackBuffer) pDev->SetRenderTarget(0, pOldRT); pOldRT->Release(); }
        if (pBackBuffer) pBackBuffer->Release();
        return;
    }

    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    if (sceneBeganHere)
    {
        pDev->EndScene();
    }

    if (pOldRT)
    {
        if (pOldRT != pBackBuffer)
            pDev->SetRenderTarget(0, pOldRT);
        pOldRT->Release();
    }
    if (pBackBuffer)
    {
        pBackBuffer->Release();
    }
}

void Overlay_OnPresentDXGI(IDXGISwapChain* pSwapChain)
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D11)
    {
        if (pSwapChain && !g_OverlayInitialized.load())
            Overlay_InitDXGI(pSwapChain);
        if (!g_OverlayInitialized.load()) return;
    }

    static bool s_PrevF9 = false;
    bool curF9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (curF9 && !s_PrevF9)
    {
        if (Recorder_IsRecording())
        {
            Recorder_Stop();
        }
        else
        {
            uint32_t fps = (s_RecFpsIdx == 0) ? 30 : ((s_RecFpsIdx == 1) ? 60 : 24);
            uint32_t bitrates[] = { 4000, 8000, 12000, 16000, 24000 };
            Recorder_Start(nullptr, fps, bitrates[s_RecBitrateIdx]);
        }
    }
    s_PrevF9 = curF9;

    if (!g_ShowMenu.load() && !s_ShowMiniHud && !Recorder_IsRecording()) return;

    ImGuiIO& io = ImGui::GetIO();
    io.AddFocusEvent(true);

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(pSwapChain->GetDesc(&desc)) && desc.BufferDesc.Width > 0 && desc.BufferDesc.Height > 0)
    {
        io.DisplaySize = ImVec2(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    if (desc.BufferDesc.Width > 0 && desc.BufferDesc.Height > 0)
    {
        io.DisplaySize = ImVec2(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
    }

    if (g_ShowMenu.load())
    {
        io.MouseDrawCursor = true;
        if (g_OverlayHwnd)
        {
            POINT pt;
            if (GetCursorPos(&pt) && ScreenToClient(g_OverlayHwnd, &pt))
            {
                RECT cr = {};
                GetClientRect(g_OverlayHwnd, &cr);
                float cw = static_cast<float>(cr.right - cr.left);
                float ch = static_cast<float>(cr.bottom - cr.top);
                if (cw > 0.0f && ch > 0.0f && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
                {
                    float mx = static_cast<float>(pt.x) * (io.DisplaySize.x / cw);
                    float my = static_cast<float>(pt.y) * (io.DisplaySize.y / ch);
                    io.AddMousePosEvent(mx, my);
                }
                else
                {
                    io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
                }
            }
        }

        static bool prevL = false, prevR = false, prevM = false;
        bool curL = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool curR = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        bool curM = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;

        if (curL != prevL) { io.AddMouseButtonEvent(0, curL); prevL = curL; }
        if (curR != prevR) { io.AddMouseButtonEvent(1, curR); prevR = curR; }
        if (curM != prevM) { io.AddMouseButtonEvent(2, curM); prevM = curM; }
    }
    else
    {
        io.MouseDrawCursor = false;
    }

    float wy = s_PendingWheelY.exchange(0.0f);
    float wx = s_PendingWheelX.exchange(0.0f);
    if (g_ShowMenu.load() && (wy != 0.0f || wx != 0.0f))
    {
        io.AddMouseWheelEvent(wx, wy);
    }

    ImGui::NewFrame();

    CaptureStats stats = {};
    Capture_GetStats(&stats);
    UpdateHistory(stats);

    if (!g_ShowMenu.load() && Recorder_IsRecording() && !s_ShowMiniHud)
    {
        RecorderStats recStats = {};
        Recorder_GetStats(&recStats);
        unsigned totalSec = static_cast<unsigned>(recStats.durationMs / 1000);
        unsigned mins = totalSec / 60;
        unsigned secs = totalSec % 60;

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 14.0f, 14.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.70f);
        ImGuiWindowFlags recFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##d3dcapture_RecIndicator", nullptr, recFlags))
        {
            if (recStats.isPaused)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[PAUSED %02u:%02u]", mins, secs);
            }
            else
            {
                bool blink = ((GetTickCount() / 500) % 2) == 0;
                ImGui::TextColored(blink ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f) : ImVec4(0.6f, 0.1f, 0.1f, 1.0f),
                    "[REC %02u:%02u]", mins, secs);
            }
        }
        ImGui::End();
    }

    if (s_ShowMiniHud)
    {
        RenderMiniHud(stats, s_MiniHudPos, s_MiniHudFps, s_MiniHudLatency, s_MiniHudRes);
    }

    RenderControlCenter(stats);

    if (s_ShowDemoWindow)
        ImGui::ShowDemoWindow(&s_ShowDemoWindow);
    if (s_ShowMetricsWindow)
        ImGui::ShowMetricsWindow(&s_ShowMetricsWindow);

    ImGui::Render();

    if (CreateRenderTargetViewDXGI(pSwapChain) && g_pD3D11Context)
    {
        g_pD3D11Context->OMSetRenderTargets(1, &g_pD3D11RTV, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}
