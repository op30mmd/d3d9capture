/**
 * overlay.cpp  —  In-game ImGui overlay and Control Center
 *
 * Supports both Direct3D 9 (GTA IV) and Direct3D 11 / DXGI (GTA V).
 * Provides a comprehensive in-game control suite:
 *   - Dashboard: Game metrics, system status, quick video recorder, live FPS/Latency/Audio plots.
 *   - Video Recording: Custom filename prefix, FPS/bitrate combos, HW acceleration, auto-stop timer, hotkeys.
 *   - Audio & Sound: WASAPI hook status, software volume gain slider, AAC bitrate, peak meter, test tone generator.
 *   - Live Logs: Real-time scrolling in-game console with tag filtering, search, notepad viewer, and clear actions.
 *   - Appearance & HUD: 6 themes (including Midnight Purple & Emerald), Mini-HUD items, screen recording beacon.
 *   - System & GPU: GPU adapter name & VRAM, DirectX backend, process RAM working set, shared memory IPC status.
 *   - Info & Help: Inter-process shared memory architecture and companion tools.
 */

#include "overlay.h"
#include "capture.h"
#include "recorder.h"
#include "audio.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"

#include <atomic>
#include <mutex>
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

enum class OverlayRenderer
{
    None,
    D3D9,
    D3D11
};

static std::atomic<bool> g_OverlayInitialized{ false };
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
static bool  s_MiniHudAudio = true;
static bool  s_MiniHudClock = true;
static bool  s_MiniHudRam = false;
static float s_MiniHudAlpha = 0.70f;
static bool  s_ShowRecIndicator = true; // Pulsing red recording beacon in screen corner

// ── Video Recording settings ─────────────────────────────────────────────────
static int   s_RecFpsIdx = 3;        // 0: 24, 1: 30, 2: 48, 3: 60, 4: 90, 5: 120 FPS
static int   s_RecBitrateIdx = 1;    // 0: 4000, 1: 8000, 2: 12000, 3: 16000, 4: 24000, 5: 35000, 6: 50000 kbps
static int   s_RecAudioBitrateIdx = 1; // 0: 128 kbps, 1: 192 kbps, 2: 256 kbps, 3: 320 kbps
static int   s_AutoStopMinutes = 0;  // 0: Disabled, 1: 1m, 5: 5m, 10: 10m, 15: 15m, 30: 30m, 60: 60m
static bool  s_HardwareEncoding = true;
static char  s_CustomPrefix[64] = "";

// ── Hotkey settings ──────────────────────────────────────────────────────────
static int   s_MenuHotkeyIdx = 0;   // 0: INSERT, 1: HOME, 2: END, 3: F11, 4: GRAVE/TILDE
static int   s_RecordHotkeyIdx = 0; // 0: F9, 1: F10, 2: F11, 3: F12, 4: F8, 5: SCROLL LOCK, 6: PAUSE

// ── Theme and UI state ───────────────────────────────────────────────────────
static int   s_SelectedTheme = 0; // 0: Emerald (GTA), 1: Cyberpunk, 2: Midnight Purple, 3: Dark, 4: Light, 5: Classic
static float s_WindowAlpha = 0.94f;
static bool  s_ShowDemoWindow = false;
static bool  s_ShowMetricsWindow = false;

// ── Audio state ──────────────────────────────────────────────────────────────
static float s_AudioVolSlider = 1.0f;
static DWORD s_TestToneTriggerTick = 0;

// ── Log Viewer state ─────────────────────────────────────────────────────────
static int   s_LogCategoryFilter = 0; // 0: All, 1: [rec], 2: [audio], 3: [d3d], 4: [capture], 5: Errors
static char  s_LogSearch[64] = "";
static bool  s_LogAutoScroll = true;

// ── Hardware & System Info Cache ─────────────────────────────────────────────
static char   s_GpuName[128] = "Auto-detecting GPU...";
static UINT64 s_GpuVramMb = 0;

// ── Performance history for graphs ───────────────────────────────────────────
static constexpr int HISTORY_SIZE = 120;
static float s_PresentFpsHistory[HISTORY_SIZE] = {};
static float s_LatencyHistory[HISTORY_SIZE] = {};
static float s_AudioPeakHistory[HISTORY_SIZE] = {};
static int   s_HistoryOffset = 0;
static DWORD s_LastHistoryTick = 0;

bool Overlay_IsMenuOpen()
{
    return g_ShowMenu.load();
}

static void GetMemoryStats(size_t* outWorkingSetMb, size_t* outPeakWorkingSetMb)
{
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        if (outWorkingSetMb) *outWorkingSetMb = pmc.WorkingSetSize / (1024 * 1024);
        if (outPeakWorkingSetMb) *outPeakWorkingSetMb = pmc.PeakWorkingSetSize / (1024 * 1024);
    }
    else
    {
        if (outWorkingSetMb) *outWorkingSetMb = 0;
        if (outPeakWorkingSetMb) *outPeakWorkingSetMb = 0;
    }
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

static void TriggerRecordingToggle()
{
    if (Recorder_IsRecording() || Recorder_IsStarting())
    {
        Recorder_Stop();
    }
    else
    {
        static const uint32_t fpsTable[] = { 24, 30, 48, 60, 90, 120 };
        static const uint32_t bitrateTable[] = { 4000, 8000, 12000, 16000, 24000, 35000, 50000 };
        static const uint32_t audioBitrates[] = { 128, 192, 256, 320 };

        uint32_t fps = (s_RecFpsIdx >= 0 && s_RecFpsIdx < 6) ? fpsTable[s_RecFpsIdx] : 30;
        uint32_t bit = (s_RecBitrateIdx >= 0 && s_RecBitrateIdx < 7) ? bitrateTable[s_RecBitrateIdx] : 8000;
        uint32_t aBit = (s_RecAudioBitrateIdx >= 0 && s_RecAudioBitrateIdx < 4) ? audioBitrates[s_RecAudioBitrateIdx] : 192;

        Recorder_SetAudioBitrate(aBit);
        Recorder_SetHardwareAccel(s_HardwareEncoding);

        char customPath[MAX_PATH] = "";
        if (s_CustomPrefix[0] != '\0')
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            _snprintf_s(customPath, sizeof(customPath), _TRUNCATE,
                "C:\\d3d9capture\\recordings\\%s_%04u%02u%02u_%02u%02u%02u.mp4",
                s_CustomPrefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        }
        CaptureStats cap = {};
        Capture_GetStats(&cap);
        Recorder_Start(customPath[0] ? customPath : nullptr, fps, bit, cap.width, cap.height);
    }
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static const int s_RecordKeys[] = { VK_F9, VK_F10, VK_F11, VK_F12, VK_F8, VK_SCROLL, VK_PAUSE };
    static const int s_MenuKeys[]   = { VK_INSERT, VK_HOME, VK_END, VK_F11, VK_OEM_3 };

    int menuKey = (s_MenuHotkeyIdx >= 0 && s_MenuHotkeyIdx < 5) ? s_MenuKeys[s_MenuHotkeyIdx] : VK_INSERT;
    int recKey  = (s_RecordHotkeyIdx >= 0 && s_RecordHotkeyIdx < 7) ? s_RecordKeys[s_RecordHotkeyIdx] : VK_F9;
    if (msg == WM_CLOSE || msg == WM_DESTROY)
    {
        if (Recorder_GetAutoSaveOnExit() && Recorder_IsRecording())
        {
            Log("[overlay] Window close message (0x%04X) received while recording. Auto-saving video...", msg);
            Recorder_StopSync(5000);
        }
    }

    if (msg == WM_KEYDOWN)
    {
        if (wParam == static_cast<WPARAM>(menuKey))
        {
            SetMenuState(!g_ShowMenu.load());
            return 1;
        }
        else if (wParam == static_cast<WPARAM>(recKey))
        {
            TriggerRecordingToggle();
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

static void OpenFileInNotepad(const char* filePath)
{
    ShellExecuteA(nullptr, "open", "notepad.exe", filePath, nullptr, SW_SHOWNORMAL);
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
    case 2: // Midnight Purple
    {
        ImGui::StyleColorsDark();
        colors[ImGuiCol_WindowBg]             = ImVec4(0.09f, 0.07f, 0.13f, 1.00f);
        colors[ImGuiCol_PopupBg]              = ImVec4(0.09f, 0.07f, 0.13f, 1.00f);
        colors[ImGuiCol_Header]               = ImVec4(0.42f, 0.18f, 0.65f, 0.55f);
        colors[ImGuiCol_HeaderHovered]        = ImVec4(0.55f, 0.25f, 0.85f, 0.80f);
        colors[ImGuiCol_HeaderActive]         = ImVec4(0.68f, 0.32f, 0.98f, 1.00f);
        colors[ImGuiCol_Button]               = ImVec4(0.38f, 0.16f, 0.60f, 0.65f);
        colors[ImGuiCol_ButtonHovered]        = ImVec4(0.50f, 0.22f, 0.78f, 0.85f);
        colors[ImGuiCol_ButtonActive]         = ImVec4(0.62f, 0.28f, 0.92f, 1.00f);
        colors[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.12f, 0.22f, 0.70f);
        colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.18f, 0.34f, 0.80f);
        colors[ImGuiCol_FrameBgActive]        = ImVec4(0.32f, 0.22f, 0.44f, 0.90f);
        colors[ImGuiCol_TitleBg]              = ImVec4(0.12f, 0.08f, 0.18f, 1.00f);
        colors[ImGuiCol_TitleBgActive]        = ImVec4(0.25f, 0.12f, 0.38f, 1.00f);
        colors[ImGuiCol_CheckMark]            = ImVec4(0.85f, 0.45f, 1.00f, 1.00f);
        colors[ImGuiCol_SliderGrab]           = ImVec4(0.65f, 0.32f, 0.90f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.82f, 0.45f, 1.00f, 1.00f);
        colors[ImGuiCol_Tab]                  = ImVec4(0.20f, 0.12f, 0.30f, 0.80f);
        colors[ImGuiCol_TabHovered]           = ImVec4(0.42f, 0.22f, 0.65f, 0.85f);
        colors[ImGuiCol_TabActive]            = ImVec4(0.34f, 0.18f, 0.55f, 1.00f);
        colors[ImGuiCol_PlotLines]            = ImVec4(0.85f, 0.45f, 1.00f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]     = ImVec4(1.00f, 0.65f, 1.00f, 1.00f);
        style.WindowRounding = 6.0f;
        style.FrameRounding  = 4.0f;
        style.GrabRounding   = 4.0f;
        break;
    }
    case 3: // Dark
        ImGui::StyleColorsDark();
        break;
    case 4: // Light
        ImGui::StyleColorsLight();
        break;
    case 5: // Classic
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

        AudioStats aStats = {};
        Audio_GetStats(&aStats);
        s_AudioPeakHistory[s_HistoryOffset]  = (aStats.peakDb > -60.0f) ? aStats.peakDb : -60.0f;

        s_HistoryOffset = (s_HistoryOffset + 1) % HISTORY_SIZE;
        s_LastHistoryTick = now;
    }
}

static void CheckAutoStopTimer()
{
    RecorderStats rec = {};
    Recorder_GetStats(&rec);
    if (rec.isRecording && !rec.isPaused && s_AutoStopMinutes > 0)
    {
        uint64_t limitMs = static_cast<uint64_t>(s_AutoStopMinutes) * 60ULL * 1000ULL;
        if (rec.durationMs >= limitMs)
        {
            Recorder_Stop();
            Log("[rec] Auto-stop timer reached (%d min); stopped recording.", s_AutoStopMinutes);
        }
    }
}

static void RenderRecordingBeacon()
{
    if (!s_ShowRecIndicator) return;
    RecorderStats rec = {};
    Recorder_GetStats(&rec);
    if (!rec.isRecording && !rec.isStarting) return;

    ImGuiIO& io = ImGui::GetIO();
    float radius = 7.0f;
    float posX = io.DisplaySize.x - 24.0f;
    float posY = 24.0f;

    DWORD tick = GetTickCount();
    float pulse = rec.isPaused ? 0.4f : (0.65f + 0.35f * sinf(static_cast<float>(tick) * 0.007f));
    ImU32 colRed = (rec.isPaused || rec.isStarting) ? IM_COL32(230, 180, 20, 220) : IM_COL32(255, 30, 30, static_cast<int>(pulse * 255.0f));
    ImU32 colGlow = (rec.isPaused || rec.isStarting) ? IM_COL32(230, 180, 20, 70) : IM_COL32(255, 30, 30, static_cast<int>(pulse * 100.0f));

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddCircleFilled(ImVec2(posX, posY), radius + 4.0f, colGlow);
    dl->AddCircleFilled(ImVec2(posX, posY), radius, colRed);
    dl->AddCircle(ImVec2(posX, posY), radius, IM_COL32(255, 255, 255, 200), 16, 1.2f);
}

static void RenderMiniHud(const CaptureStats& stats, int positionPreset)
{
    ImGuiIO& io = ImGui::GetIO();
    float margin = 12.0f;
    ImVec2 windowPos;
    ImVec2 windowPivot;

    switch (positionPreset)
    {
    case 0: windowPos = ImVec2(io.DisplaySize.x - margin, margin); windowPivot = ImVec2(1.0f, 0.0f); break;
    case 1: windowPos = ImVec2(margin, margin); windowPivot = ImVec2(0.0f, 0.0f); break;
    case 2: windowPos = ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin); windowPivot = ImVec2(1.0f, 1.0f); break;
    case 3: windowPos = ImVec2(margin, io.DisplaySize.y - margin); windowPivot = ImVec2(0.0f, 1.0f); break;
    default: windowPos = ImVec2(io.DisplaySize.x - margin, margin); windowPivot = ImVec2(1.0f, 0.0f); break;
    }

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPivot);
    ImGui::SetNextWindowBgAlpha(s_MiniHudAlpha);

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
        if (recStats.isStarting)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[INIT...]");
            hasItem = true;
        }
        else if (recStats.isRecording)
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

        if (s_MiniHudFps)
        {
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%.1f FPS", stats.presentFps);
            hasItem = true;
        }

        if (s_MiniHudLatency && stats.readbackMs > 0.0f)
        {
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%.1f ms", stats.readbackMs);
            hasItem = true;
        }

        if (s_MiniHudRes && stats.width > 0)
        {
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%ux%u", stats.width, stats.height);
            hasItem = true;
        }

        if (s_MiniHudAudio)
        {
            AudioStats aStats = {};
            Audio_GetStats(&aStats);
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            if (aStats.peakDb <= -60.0f)
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[Aud: -inf]");
            else
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "[Aud: %.0fdB]", aStats.peakDb);
            hasItem = true;
        }

        if (s_MiniHudRam)
        {
            size_t ramMb = 0;
            GetMemoryStats(&ramMb, nullptr);
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%zu MB RAM", ramMb);
            hasItem = true;
        }

        if (s_MiniHudClock)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (hasItem) { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); }
            ImGui::Text("%02u:%02u", st.wHour, st.wMinute);
            hasItem = true;
        }
    }
    ImGui::End();
}

static void RenderControlCenter(const CaptureStats& stats)
{
    if (!g_ShowMenu.load()) return;

    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(s_WindowAlpha);
    bool menuOpen = g_ShowMenu.load();
    if (ImGui::Begin("d3dcapture - Control Center", &menuOpen, ImGuiWindowFlags_NoCollapse))
    {
        static const char* s_KeyNamesMenu[] = { "INSERT", "HOME", "END", "F11", "TILDE (~)" };
        static const char* s_KeyNamesRec[]  = { "F9", "F10", "F11", "F12", "F8", "ScrollLock", "Pause" };

        const char* curMenuKey = (s_MenuHotkeyIdx >= 0 && s_MenuHotkeyIdx < 5) ? s_KeyNamesMenu[s_MenuHotkeyIdx] : "INSERT";
        const char* curRecKey  = (s_RecordHotkeyIdx >= 0 && s_RecordHotkeyIdx < 7) ? s_KeyNamesRec[s_RecordHotkeyIdx] : "F9";

        if (!stats.captureEnabled)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "[CAPTURE PAUSED]");
            ImGui::SameLine();
            ImGui::TextDisabled("| Toggle Menu: %s | Record: %s", curMenuKey, curRecKey);
        }
        else
        {
            ImGui::TextDisabled("Toggle Menu: %s | Start/Stop Record: %s", curMenuKey, curRecKey);
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("ControlTabs", ImGuiTabBarFlags_None))
        {
            // ─────────────────────────────────────────────────────────────────
            // TAB 1: DASHBOARD
            // ─────────────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Dashboard"))
            {
                ImGui::Spacing();

                // Upper Metrics Table
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

                // System Overview Row
                size_t wsMb = 0, peakWsMb = 0;
                GetMemoryStats(&wsMb, &peakWsMb);
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "GPU: %s  |  Process RAM: %zu MB (Peak: %zu MB)",
                    s_GpuName, wsMb, peakWsMb);

                ImGui::Separator();
                ImGui::Spacing();

                // Quick Video Recorder Control on Dashboard
                RecorderStats recStats = {};
                Recorder_GetStats(&recStats);

                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "Video Recorder Status:");
                ImGui::SameLine();
                if (recStats.isStarting)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[INITIALIZING ENCODER...]");
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) Recorder_Stop();
                }
                else if (recStats.isRecording)
                {
                    unsigned totalSec = static_cast<unsigned>(recStats.durationMs / 1000);
                    unsigned hrs = totalSec / 3600;
                    unsigned mins = (totalSec % 3600) / 60;
                    unsigned secs = totalSec % 60;
                    if (recStats.isPaused)
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[PAUSED] %02u:%02u:%02u (%llu frames)", hrs, mins, secs, static_cast<unsigned long long>(recStats.recordedFrames));
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "[RECORDING] %02u:%02u:%02u (%llu frames)", hrs, mins, secs, static_cast<unsigned long long>(recStats.recordedFrames));

                    ImGui::SameLine();
                    if (ImGui::Button("Stop (F9)")) Recorder_Stop();
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Idle");
                    ImGui::SameLine();
                    if (ImGui::Button("Start Recording (F9)")) TriggerRecordingToggle();
                }

                ImGui::Separator();
                ImGui::Spacing();

                // Performance Graphs
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
                ImGui::PlotLines("##FpsPlot", s_PresentFpsHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, 0.0f, plotFpsMax, ImVec2(-1, 55.0f));

                ImGui::Spacing();
                ImGui::Text("Readback Latency History (Current: %.2f ms | Avg: %.2f ms):", stats.readbackMs, avgLat);
                ImGui::PlotLines("##LatPlot", s_LatencyHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, 0.0f, plotLatMax, ImVec2(-1, 55.0f));

                ImGui::Spacing();
                AudioStats aStats = {};
                Audio_GetStats(&aStats);
                ImGui::Text("Audio Peak History (Level: %.1f dBFS | Ring: %u%%):", aStats.peakDb, aStats.ringFillPercent);
                ImGui::PlotLines("##AudPlot", s_AudioPeakHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, -60.0f, 0.0f, ImVec2(-1, 45.0f));

                ImGui::EndTabItem();
            }

            // ─────────────────────────────────────────────────────────────────
            // TAB 2: VIDEO RECORDING
            // ─────────────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Video Recording"))
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "Hardware-Accelerated H.264 MP4 Video Recorder:");

                RecorderStats recStats = {};
                Recorder_GetStats(&recStats);

                if (recStats.isStarting)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
                    ImGui::Button("[...] Initializing", ImVec2(180, 36));
                    ImGui::PopStyleColor(3);

                    ImGui::SameLine();
                    if (ImGui::Button("[X] Cancel", ImVec2(90, 36)))
                    {
                        Recorder_Stop();
                    }
                }
                else if (!recStats.isRecording)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.58f, 0.30f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.72f, 0.38f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.85f, 0.45f, 1.0f));
                    if (ImGui::Button("[REC] Start Recording", ImVec2(210, 36)))
                    {
                        TriggerRecordingToggle();
                    }
                    ImGui::PopStyleColor(3);
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.16f, 0.16f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.22f, 0.22f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.10f, 0.10f, 1.0f));
                    if (ImGui::Button("[STOP] Stop Recording", ImVec2(180, 36)))
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
                        if (ImGui::Button("[>] Resume", ImVec2(120, 36)))
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
                        if (ImGui::Button("[||] Pause", ImVec2(120, 36)))
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
                if (recStats.isStarting)
                {
                    bool blink = ((GetTickCount() / 300) % 2) == 0;
                    ImGui::TextColored(blink ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.7f, 0.6f, 0.1f, 1.0f),
                        "[STARTING] Initializing hardware encoder & audio streams...");
                    ImGui::TextDisabled("Output: %s", recStats.outputPath);
                }
                else if (recStats.isRecording)
                {
                    unsigned totalSec = static_cast<unsigned>(recStats.durationMs / 1000);
                    unsigned hrs = totalSec / 3600;
                    unsigned mins = (totalSec % 3600) / 60;
                    unsigned secs = totalSec % 60;

                    bool blink = ((GetTickCount() / 500) % 2) == 0;
                    if (recStats.isPaused)
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[PAUSED]");
                    else
                        ImGui::TextColored(blink ? ImVec4(1.0f, 0.25f, 0.25f, 1.0f) : ImVec4(0.6f, 0.1f, 0.1f, 1.0f), "[REC]");

                    ImGui::SameLine();
                    ImGui::Text("Duration: %02u:%02u:%02u  |  Frames: %llu (Dropped: %llu)  |  Res: %ux%u",
                        hrs, mins, secs, static_cast<unsigned long long>(recStats.recordedFrames),
                        static_cast<unsigned long long>(recStats.droppedFrames),
                        recStats.width, recStats.height);

                    ImGui::TextDisabled("Output: %s", recStats.outputPath);
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Status: Ready to record");
                    if (recStats.outputPath[0] != '\0')
                    {
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Last File: %s", recStats.outputPath);
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Recording Parameters
                ImGui::Text("Encoding Settings & Output Configuration:");

                ImGui::SetNextItemWidth(200);
                ImGui::InputText("Filename Prefix", s_CustomPrefix, sizeof(s_CustomPrefix));
                ImGui::SameLine();
                ImGui::TextDisabled("(e.g. gta5_clip)");

                const char* fpsItems[] = { "24 FPS", "30 FPS", "48 FPS", "60 FPS", "90 FPS", "120 FPS" };
                ImGui::SetNextItemWidth(140);
                ImGui::Combo("Video FPS", &s_RecFpsIdx, fpsItems, IM_ARRAYSIZE(fpsItems));

                ImGui::SameLine();
                const char* bitrateItems[] = { "4 Mbps", "8 Mbps (Standard)", "12 Mbps (High)", "16 Mbps (Crisp)", "24 Mbps (Master)", "35 Mbps (Ultra)", "50 Mbps (Max)" };
                ImGui::SetNextItemWidth(190);
                ImGui::Combo("Video Bitrate", &s_RecBitrateIdx, bitrateItems, IM_ARRAYSIZE(bitrateItems));

                const char* aBitrateItems[] = { "128 kbps", "192 kbps (Default)", "256 kbps (High Quality)", "320 kbps (Studio)" };
                ImGui::SetNextItemWidth(180);
                if (ImGui::Combo("AAC Audio Bitrate", &s_RecAudioBitrateIdx, aBitrateItems, IM_ARRAYSIZE(aBitrateItems)))
                {
                    static const uint32_t aBitrates[] = { 128, 192, 256, 320 };
                    Recorder_SetAudioBitrate(aBitrates[s_RecAudioBitrateIdx]);
                }

                ImGui::SameLine();
                if (ImGui::Checkbox("Hardware Encoding (NVENC/AMF/QSV)", &s_HardwareEncoding))
                {
                    Recorder_SetHardwareAccel(s_HardwareEncoding);
                }

                const char* timerItems[] = { "Disabled (Manual Stop)", "1 Minute", "5 Minutes", "10 Minutes", "15 Minutes", "30 Minutes", "60 Minutes" };
                const int timerVals[] = { 0, 1, 5, 10, 15, 30, 60 };
                int curTimerSel = 0;
                for (int i = 0; i < 7; ++i) { if (timerVals[i] == s_AutoStopMinutes) { curTimerSel = i; break; } }
                ImGui::SetNextItemWidth(180);
                if (ImGui::Combo("Auto-Stop Timer", &curTimerSel, timerItems, IM_ARRAYSIZE(timerItems)))
                {
                    s_AutoStopMinutes = timerVals[curTimerSel];
                }

                ImGui::SameLine();
                bool autoSaveOnClose = Recorder_GetAutoSaveOnExit();
                if (ImGui::Checkbox("Auto-Save on Game Exit", &autoSaveOnClose))
                {
                    Recorder_SetAutoSaveOnExit(autoSaveOnClose);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Ensures active recordings are safely flushed and finalized to disk before the game or window closes.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Hotkey Customization
                ImGui::Text("Hotkey Customization:");
                const char* recKeyItems[] = { "F9", "F10", "F11", "F12", "F8", "Scroll Lock", "Pause / Break" };
                ImGui::SetNextItemWidth(140);
                ImGui::Combo("Record Hotkey", &s_RecordHotkeyIdx, recKeyItems, IM_ARRAYSIZE(recKeyItems));

                ImGui::SameLine();
                const char* menuKeyItems[] = { "INSERT", "HOME", "END", "F11", "TILDE (~)" };
                ImGui::SetNextItemWidth(140);
                ImGui::Combo("Menu Hotkey", &s_MenuHotkeyIdx, menuKeyItems, IM_ARRAYSIZE(menuKeyItems));

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // GPU Capture Throttling
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "GPU Capture Engine:");
                bool capEnabled = stats.captureEnabled;
                if (ImGui::Checkbox("Enable GPU Frame Capture (Master Switch)", &capEnabled))
                {
                    Capture_SetEnabled(capEnabled);
                }

                int curTarget = stats.targetFps;
                const char* targetItems[] = { "Uncapped (Match Game FPS)", "120 FPS", "60 FPS", "30 FPS", "24 FPS", "15 FPS" };
                int targetValues[] = { 0, 120, 60, 30, 24, 15 };
                int curSel = 0;
                for (int i = 0; i < 6; ++i) { if (targetValues[i] == curTarget) { curSel = i; break; } }
                ImGui::SetNextItemWidth(220);
                if (ImGui::Combo("Framerate Limiter (Throttle)", &curSel, targetItems, IM_ARRAYSIZE(targetItems)))
                {
                    Capture_SetTargetFps(targetValues[curSel]);
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Snapshot & Diagnostic Dumps
                ImGui::Text("Screenshots & Diagnostics:");
                if (ImGui::Button("Take High-Res Screenshot (BMP)", ImVec2(220, 28)))
                {
                    Capture_TriggerSnapshot();
                }
                ImGui::SameLine();
                if (ImGui::Button("Open Screenshots Folder", ImVec2(180, 28)))
                {
                    OpenDirectoryInExplorer("C:\\d3d9capture\\screenshots");
                }
                if (stats.lastScreenshotPath[0] != '\0')
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Saved: %s", stats.lastScreenshotPath);
                }

                static int s_DumpCount = 30;
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Frames to Dump", &s_DumpCount);
                if (s_DumpCount < 1) s_DumpCount = 1;
                ImGui::SameLine();
                if (stats.dumpQuotaRemaining > 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Dumping (%d left)...", stats.dumpQuotaRemaining);
                }
                else
                {
                    if (ImGui::Button("Start Frame Dump", ImVec2(140, 26))) Capture_SetDumpQuota(s_DumpCount);
                    ImGui::SameLine();
                    if (ImGui::Button("Open Dumps Folder", ImVec2(150, 26))) OpenDirectoryInExplorer("C:\\d3d9capture\\dumps");
                }

                ImGui::EndTabItem();
            }

            // ─────────────────────────────────────────────────────────────────
            // TAB 3: AUDIO & SOUND (DEDICATED TAB)
            // ─────────────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Audio & Sound"))
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "Low-Latency WASAPI Audio Capture Engine:");

                AudioStats audioStats = {};
                Audio_GetStats(&audioStats);

                bool audioEnabled = audioStats.enabled;
                if (ImGui::Checkbox("Enable In-Process Audio Capture", &audioEnabled))
                {
                    Audio_SetEnabled(audioEnabled);
                }
                ImGui::SameLine();
                if (audioStats.hooksInstalled)
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[WASAPI Hooks Active]");
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[Waiting for Audio Device]");

                ImGui::Spacing();

                // Capture Mode Selector
                const char* modeItems[] = { "Auto (Direct Hook + Smart Fallback)", "Direct WASAPI Hook Only", "Desktop Loopback Only" };
                int curMode = Audio_GetCaptureMode();
                ImGui::SetNextItemWidth(320);
                if (ImGui::Combo("Capture Mode", &curMode, modeItems, IM_ARRAYSIZE(modeItems)))
                {
                    Audio_SetCaptureMode(curMode);
                }

                ImGui::Spacing();

                // Active Audio Stream Card
                const char* sourceStr = "None (Idle)";
                ImVec4 sourceColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                if (audioStats.source == AudioSource_Hook)
                {
                    sourceStr = "Direct WASAPI Hook (In-Process Low Latency)";
                    sourceColor = ImVec4(0.2f, 1.0f, 0.4f, 1.0f);
                }
                else if (audioStats.source == AudioSource_Loopback)
                {
                    sourceStr = "Desktop Loopback (Active)";
                    sourceColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
                }
                ImGui::Text("Active Audio Source: ");
                ImGui::SameLine();
                ImGui::TextColored(sourceColor, "%s", sourceStr);

                ImGui::Text("Format: %u Hz | %u Channels | %u-bit %s | Period: %.2f ms | Detected Streams: %u",
                    audioStats.sampleRate, audioStats.channels, audioStats.bitsPerSample,
                    audioStats.isFloat ? "Float" : "PCM", audioStats.devicePeriodMs, audioStats.activeStreams);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Software Volume Gain Slider
                float curGain = Audio_GetVolume();
                float dbGain = (curGain > 0.001f) ? (20.0f * log10f(curGain)) : -96.0f;
                char gainInfo[64];
                if (curGain <= 0.001f)
                    strcpy_s(gainInfo, "Muted (0.00x, -inf dB)");
                else
                    _snprintf_s(gainInfo, sizeof(gainInfo), _TRUNCATE, "%.2fx (%+.1f dB)", curGain, dbGain);

                ImGui::Text("Software Volume Gain: %s", gainInfo);

                s_AudioVolSlider = curGain;
                ImGui::SetNextItemWidth(260);
                if (ImGui::SliderFloat("##AudioGain", &s_AudioVolSlider, 0.0f, 3.0f, "%.2fx"))
                {
                    Audio_SetVolume(s_AudioVolSlider);
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset (1.0x)"))
                {
                    s_AudioVolSlider = 1.0f;
                    Audio_SetVolume(1.0f);
                }
                ImGui::SameLine();
                if (curGain > 0.001f)
                {
                    if (ImGui::Button("Mute"))
                    {
                        s_AudioVolSlider = 0.0f;
                        Audio_SetVolume(0.0f);
                    }
                }
                else
                {
                    if (ImGui::Button("Unmute"))
                    {
                        s_AudioVolSlider = 1.0f;
                        Audio_SetVolume(1.0f);
                    }
                }
                ImGui::TextDisabled("Boosts or attenuates game audio before encoding to AAC (0.0x - 3.0x, default: 1.0x).");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Peak Level dBFS Progress Bar
                float peakDb = audioStats.peakDb;
                if (peakDb < -60.0f) peakDb = -60.0f;
                else if (peakDb > 0.0f) peakDb = 0.0f;
                float levelNorm = (peakDb + 60.0f) / 60.0f;

                char levelLabel[32];
                if (audioStats.peakDb <= -90.0f)
                    _snprintf_s(levelLabel, sizeof(levelLabel), _TRUNCATE, "-inf dBFS");
                else
                    _snprintf_s(levelLabel, sizeof(levelLabel), _TRUNCATE, "%.1f dBFS", audioStats.peakDb);

                ImGui::Text("Audio Peak Meter: ");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, (audioStats.peakDb > -3.0f) ? ImVec4(0.9f, 0.2f, 0.2f, 1.0f) : ((audioStats.peakDb > -12.0f) ? ImVec4(0.9f, 0.75f, 0.2f, 1.0f) : ImVec4(0.2f, 0.85f, 0.4f, 1.0f)));
                ImGui::ProgressBar(levelNorm, ImVec2(240, 18), levelLabel);
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Text("Real-Time Audio Waveform Meter:");
                ImGui::PlotLines("##AudWave", s_AudioPeakHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, -60.0f, 0.0f, ImVec2(-1, 55.0f));

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Testing & Buffer Health
                ImGui::Text("Audio Testing & Buffer Health:");
                if (ImGui::Button("Inject 440 Hz Test Tone (500 ms)", ImVec2(230, 28)))
                {
                    Audio_InjectTestTone(440, 500);
                    s_TestToneTriggerTick = GetTickCount();
                }
                ImGui::SameLine();
                if (s_TestToneTriggerTick > 0 && (GetTickCount() - s_TestToneTriggerTick) < 1500)
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[Tone Injected!]");
                }
                ImGui::SameLine();
                if (ImGui::Button("Flush Audio Buffer", ImVec2(160, 28)))
                {
                    Audio_Flush();
                }

                ImGui::Spacing();
                ImGui::Text("Ring Buffer Usage: %u%%  |  Total Captured: %llu frames",
                    audioStats.ringFillPercent, static_cast<unsigned long long>(audioStats.capturedFrames));
                ImGui::Text("Dropped Frames: %llu  |  Overrun Events: %llu",
                    static_cast<unsigned long long>(audioStats.droppedFrames), static_cast<unsigned long long>(audioStats.overruns));

                if (audioStats.lastError[0] != '\0')
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Last Error: %s", audioStats.lastError);
                }

                ImGui::EndTabItem();
            }

            // ─────────────────────────────────────────────────────────────────
            // TAB 4: LIVE LOGS & DIAGNOSTICS (NEW TAB)
            // ─────────────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Live Logs"))
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "In-Game Live Diagnostic Console (debug.log):");

                // Filter Buttons Row
                const char* filterNames[] = { "All", "[rec]", "[audio]", "[d3d]", "[capture]", "Errors" };
                for (int i = 0; i < 6; ++i)
                {
                    if (i > 0) ImGui::SameLine();
                    if (s_LogCategoryFilter == i)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.65f, 0.95f, 1.0f));
                        ImGui::Button(filterNames[i]);
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        if (ImGui::Button(filterNames[i]))
                            s_LogCategoryFilter = i;
                    }
                }

                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                ImGui::InputTextWithHint("##LogSearch", "Search...", s_LogSearch, sizeof(s_LogSearch));

                ImGui::SameLine();
                ImGui::Checkbox("Auto-Scroll", &s_LogAutoScroll);

                // Action Buttons Row
                if (ImGui::Button("Open debug.log in Notepad"))
                {
                    OpenFileInNotepad("C:\\d3d9capture\\debug.log");
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear In-Memory Log"))
                {
                    Log_ClearRecentEntries();
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear debug.log File"))
                {
                    Log_ClearLogFile();
                }
                ImGui::SameLine();
                if (ImGui::Button("Copy Visible Logs"))
                {
                    static LogEntry s_TempLogs[256];
                    size_t count = Log_GetRecentEntries(s_TempLogs, 256);
                    std::string allText;
                    for (size_t i = 0; i < count; ++i)
                    {
                        allText += s_TempLogs[i].text;
                        allText += "\n";
                    }
                    ImGui::SetClipboardText(allText.c_str());
                }

                ImGui::Spacing();
                ImGui::Separator();

                // Scrolling Log Console Window
                static LogEntry s_Entries[512];
                size_t numEntries = Log_GetRecentEntries(s_Entries, 512);

                ImGui::BeginChild("LogConsoleArea", ImVec2(0, -1), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = 0; i < numEntries; ++i)
                {
                    const char* text = s_Entries[i].text;

                    // Category filter check
                    if (s_LogCategoryFilter == 1 && strstr(text, "[rec]") == nullptr) continue;
                    if (s_LogCategoryFilter == 2 && strstr(text, "[audio]") == nullptr) continue;
                    if (s_LogCategoryFilter == 3 && strstr(text, "[d3d") == nullptr && strstr(text, "[overlay]") == nullptr) continue;
                    if (s_LogCategoryFilter == 4 && strstr(text, "[capture]") == nullptr) continue;
                    if (s_LogCategoryFilter == 5 && strstr(text, "failed") == nullptr && strstr(text, "Failed") == nullptr && strstr(text, "error") == nullptr && strstr(text, "Error") == nullptr) continue;

                    // Search text check
                    if (s_LogSearch[0] != '\0' && strstr(text, s_LogSearch) == nullptr) continue;

                    // Syntax / category highlighting
                    ImVec4 lineCol = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
                    if (strstr(text, "[audio]"))
                        lineCol = ImVec4(0.3f, 0.85f, 0.95f, 1.0f);
                    else if (strstr(text, "[rec]"))
                        lineCol = ImVec4(0.4f, 0.95f, 0.4f, 1.0f);
                    else if (strstr(text, "[overlay]"))
                        lineCol = ImVec4(0.85f, 0.55f, 0.95f, 1.0f);
                    else if (strstr(text, "failed") || strstr(text, "Failed") || strstr(text, "error") || strstr(text, "Error"))
                        lineCol = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);

                    ImGui::PushStyleColor(ImGuiCol_Text, lineCol);
                    ImGui::TextUnformatted(text);
                    ImGui::PopStyleColor();
                }

                if (s_LogAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.0f);

                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // ─────────────────────────────────────────────────────────────────
            // TAB 5: APPEARANCE & HUD
            // ─────────────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Appearance & HUD"))
            {
                ImGui::Spacing();
                ImGui::Text("Color Theme:");
                ImGui::SetNextItemWidth(260);
                if (ImGui::Combo("##ThemeCombo", &s_SelectedTheme, "Emerald (GTA Style)\0Cyberpunk (Cyan)\0Midnight Purple\0Dark (Default)\0Light\0Classic\0\0"))
                {
                    ApplyTheme(s_SelectedTheme);
                }

                ImGui::Spacing();
                ImGui::Text("Window Background Opacity:");
                ImGui::SetNextItemWidth(260);
                ImGui::SliderFloat("##WindowAlpha", &s_WindowAlpha, 0.20f, 1.00f, "%.2f");
                ImGui::TextDisabled("Controls transparency of overlay control panels.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Mini-HUD Configuration (Pinned to Screen):");
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

                    ImGui::Checkbox("Show Audio Meter", &s_MiniHudAudio);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Clock", &s_MiniHudClock);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show Process RAM", &s_MiniHudRam);

                    ImGui::SetNextItemWidth(200);
                    ImGui::SliderFloat("Mini-HUD Alpha", &s_MiniHudAlpha, 0.10f, 1.00f, "%.2f");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Screen Recording Beacon:");
                ImGui::Checkbox("Show Pulsing Red Dot when Recording", &s_ShowRecIndicator);
                ImGui::TextDisabled("Displays a subtle pulsing circular recording beacon in the screen corner.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("ImGui Debug Tools:");
                ImGui::Checkbox("Show ImGui Demo Window", &s_ShowDemoWindow);
                ImGui::SameLine();
                ImGui::Checkbox("Show ImGui Metrics", &s_ShowMetricsWindow);

                ImGui::EndTabItem();
            }

            // ─────────────────────────────────────────────────────────────────
            // TAB 6: SYSTEM & GPU INFO (NEW TAB)
            // ─────────────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("System & GPU"))
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "Hardware & Graphics Pipeline Details:");

                ImGui::BulletText("GPU Adapter:        %s", s_GpuName);
                if (s_GpuVramMb > 0)
                    ImGui::BulletText("Dedicated VRAM:     %llu MB", static_cast<unsigned long long>(s_GpuVramMb));
                ImGui::BulletText("Renderer Pipeline:  %s", (g_ActiveRenderer.load() == OverlayRenderer::D3D11) ? "Direct3D 11 (DXGI SwapChain)" : "Direct3D 9 (Native)");
                ImGui::BulletText("Backbuffer Format:  %s", GetFormatName(stats.format));
                ImGui::BulletText("Active Resolution:  %u x %u", stats.width, stats.height);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "Host Process & Memory:");
                size_t wsMb = 0, peakWsMb = 0;
                GetMemoryStats(&wsMb, &peakWsMb);
                ImGui::BulletText("Process ID (PID):   %lu", GetCurrentProcessId());
                ImGui::BulletText("Thread ID (TID):    %lu", GetCurrentThreadId());
                ImGui::BulletText("RAM Working Set:    %zu MB", wsMb);
                ImGui::BulletText("Peak RAM Set:       %zu MB", peakWsMb);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.00f, 1.00f), "Shared Memory IPC State:");
                ImGui::BulletText("File Mapping:       Local\\D3D9CaptureShm (Capacity: 126 MB)");
                ImGui::BulletText("Frame Ready Event:  Local\\D3D9CaptureReady");
                ImGui::BulletText("Frame Done Event:   Local\\D3D9CaptureDone");
                ImGui::BulletText("Consumer Connected: %s", stats.isConsumerActive ? "YES (shm_reader active)" : "NO (idle)");

                ImGui::EndTabItem();
            }

            // ─────────────────────────────────────────────────────────────────
            // TAB 7: INFO & HELP
            // ─────────────────────────────────────────────────────────────────
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
                ImGui::Text("Press %s to close this menu.", curMenuKey);

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
    if (FAILED(pDev->GetCreationParameters(&cp)) || !cp.hFocusWindow)
    {
        Log("[overlay] Failed to get device creation params / hFocusWindow");
        return;
    }

    g_OverlayHwnd = cp.hFocusWindow;

    IDirect3D9* pD3D9 = nullptr;
    if (SUCCEEDED(pDev->GetDirect3D(&pD3D9)) && pD3D9)
    {
        D3DADAPTER_IDENTIFIER9 ident = {};
        if (SUCCEEDED(pD3D9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident)))
        {
            strncpy_s(s_GpuName, sizeof(s_GpuName), ident.Description, _TRUNCATE);
            Log("[overlay] Detected D3D9 GPU: %s", s_GpuName);
        }
        pD3D9->Release();
    }

    Log("[overlay] Calling CreateContext");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDebugIgnoreFocusLoss = true;

    ApplyTheme(s_SelectedTheme);

    Log("[overlay] Calling ImGui_ImplWin32_Init with hwnd=%p", g_OverlayHwnd);
    if (!ImGui_ImplWin32_Init(g_OverlayHwnd))
    {
        Log("[overlay] ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return;
    }

    Log("[overlay] Calling ImGui_ImplDX9_Init");
    if (!ImGui_ImplDX9_Init(pDev))
    {
        Log("[overlay] ImGui_ImplDX9_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    g_OriginalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(g_OverlayHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    Log("[overlay] WndProc hooked: original=%p", g_OriginalWndProc);

    g_ActiveRenderer.store(OverlayRenderer::D3D9);
    g_OverlayInitialized.store(true);
    Log("[overlay] Init complete");
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

    IDXGIDevice* pDXGIDev = nullptr;
    if (SUCCEEDED(pDev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&pDXGIDev))) && pDXGIDev)
    {
        IDXGIAdapter* pAdapter = nullptr;
        if (SUCCEEDED(pDXGIDev->GetAdapter(&pAdapter)) && pAdapter)
        {
            DXGI_ADAPTER_DESC aDesc = {};
            if (SUCCEEDED(pAdapter->GetDesc(&aDesc)))
            {
                WideCharToMultiByte(CP_ACP, 0, aDesc.Description, -1, s_GpuName, sizeof(s_GpuName), nullptr, nullptr);
                s_GpuVramMb = aDesc.DedicatedVideoMemory / (1024 * 1024);
                Log("[overlay] Detected DXGI GPU: %s (%llu MB VRAM)", s_GpuName, static_cast<unsigned long long>(s_GpuVramMb));
            }
            pAdapter->Release();
        }
        pDXGIDev->Release();
    }

    Log("[overlay] Calling CreateContext (DXGI)");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDebugIgnoreFocusLoss = true;

    ApplyTheme(s_SelectedTheme);

    Log("[overlay] Calling ImGui_ImplWin32_Init with hwnd=%p", g_OverlayHwnd);
    if (!ImGui_ImplWin32_Init(g_OverlayHwnd))
    {
        Log("[overlay] ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        pCtx->Release();
        pDev->Release();
        return;
    }

    Log("[overlay] Calling ImGui_ImplDX11_Init");
    if (!ImGui_ImplDX11_Init(pDev, pCtx))
    {
        Log("[overlay] ImGui_ImplDX11_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        pCtx->Release();
        pDev->Release();
        return;
    }

    g_OriginalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(g_OverlayHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    Log("[overlay] WndProc hooked (DXGI): original=%p", g_OriginalWndProc);

    g_ActiveRenderer.store(OverlayRenderer::D3D11);
    g_OverlayInitialized.store(true);
    Log("[overlay] Init complete (DXGI)");
}

void Overlay_Shutdown()
{
    Log("[overlay] Shutdown requested");
    if (!g_OverlayInitialized.load()) return;

    if (s_MouseHook)
    {
        UnhookWindowsHookEx(s_MouseHook);
        s_MouseHook = nullptr;
    }

    if (g_OverlayHwnd && g_OriginalWndProc)
    {
        SetWindowLongPtrA(g_OverlayHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OriginalWndProc));
        g_OriginalWndProc = nullptr;
    }

    OverlayRenderer r = g_ActiveRenderer.load();
    if (r == OverlayRenderer::D3D9)
    {
        ImGui_ImplDX9_Shutdown();
    }
    else if (r == OverlayRenderer::D3D11)
    {
        ImGui_ImplDX11_Shutdown();
        if (g_pD3D11RTV) { g_pD3D11RTV->Release(); g_pD3D11RTV = nullptr; }
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

void Overlay_OnPostReset(IDirect3DDevice9* pDev)
{
    (void)pDev;
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D9) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

void Overlay_OnPreResetDXGI()
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D11) return;
    if (g_pD3D11RTV) { g_pD3D11RTV->Release(); g_pD3D11RTV = nullptr; }
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

void Overlay_OnPostResetDXGI(IDXGISwapChain* pSwapChain)
{
    (void)pSwapChain;
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D11) return;
    ImGui_ImplDX11_CreateDeviceObjects();
}

void Overlay_OnPresent(IDirect3DDevice9* pDev)
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D9)
    {
        if (pDev && !g_OverlayInitialized.load())
            Overlay_Init(pDev);
        return;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(pDev->GetCreationParameters(&cp)) && cp.hFocusWindow)
    {
        RECT rc;
        if (GetClientRect(cp.hFocusWindow, &rc))
        {
            io.DisplaySize = ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
        }
    }

    if (g_ShowMenu.load())
    {
        io.MouseDrawCursor = true;
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(g_OverlayHwnd, &pt))
        {
            io.AddMousePosEvent((float)pt.x, (float)pt.y);
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
    CheckAutoStopTimer();

    if (s_ShowMiniHud)
    {
        RenderMiniHud(stats, s_MiniHudPos);
    }

    RenderRecordingBeacon();
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

static bool CreateRenderTargetViewDXGI(IDXGISwapChain* pSwapChain)
{
    if (g_pD3D11RTV) return true;
    if (!g_pD3D11Device) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
    if (FAILED(hr) || !pBackBuffer) return false;

    hr = g_pD3D11Device->CreateRenderTargetView(pBackBuffer, nullptr, &g_pD3D11RTV);
    pBackBuffer->Release();
    return SUCCEEDED(hr) && (g_pD3D11RTV != nullptr);
}

void Overlay_OnPresentDXGI(IDXGISwapChain* pSwapChain)
{
    if (!g_OverlayInitialized.load() || g_ActiveRenderer.load() != OverlayRenderer::D3D11)
    {
        if (pSwapChain && !g_OverlayInitialized.load())
            Overlay_InitDXGI(pSwapChain);
        return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(pSwapChain->GetDesc(&desc)) && desc.OutputWindow)
    {
        RECT rc;
        if (GetClientRect(desc.OutputWindow, &rc))
        {
            io.DisplaySize = ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
        }
    }

    if (g_ShowMenu.load())
    {
        io.MouseDrawCursor = true;
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(g_OverlayHwnd, &pt))
        {
            io.AddMousePosEvent((float)pt.x, (float)pt.y);
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
    CheckAutoStopTimer();

    if (s_ShowMiniHud)
    {
        RenderMiniHud(stats, s_MiniHudPos);
    }

    RenderRecordingBeacon();
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
