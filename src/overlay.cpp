#include "overlay.h"
#include "capture.h" // For Log
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include <atomic>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::atomic<bool> g_OverlayInitialized{ false };
static HWND g_OverlayHwnd = nullptr;
static WNDPROC g_OriginalWndProc = nullptr;
static bool g_ShowMenu = false;

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == VK_INSERT)
    {
        g_ShowMenu = !g_ShowMenu;
        return 1;
    }

    if (g_ShowMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    // If we want to block game input while the menu is open, we can return early here
    // for input messages if ImGui wants capture.
    ImGuiIO& io = ImGui::GetIO();
    if (g_ShowMenu && (io.WantCaptureMouse || io.WantCaptureKeyboard))
    {
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
        if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) return 0;
    }

    return CallWindowProc(g_OriginalWndProc, hWnd, msg, wParam, lParam);
}

void Overlay_Init(IDirect3DDevice9* pDev)
{
    if (g_OverlayInitialized.load()) return;

    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (FAILED(pDev->GetCreationParameters(&cp)))
    {
        Log("[overlay] Failed to get creation parameters");
        return;
    }

    g_OverlayHwnd = cp.hFocusWindow;
    if (!g_OverlayHwnd)
    {
        Log("[overlay] cp.hFocusWindow is null");
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(g_OverlayHwnd))
    {
        Log("[overlay] ImGui_ImplWin32_Init failed");
        return;
    }
    if (!ImGui_ImplDX9_Init(pDev))
    {
        Log("[overlay] ImGui_ImplDX9_Init failed");
        return;
    }

    g_OriginalWndProc = (WNDPROC)SetWindowLongPtrA(g_OverlayHwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    Log("[overlay] Initialized successfully. Menu toggled with INSERT.");

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

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_OverlayInitialized.store(false);
    Log("[overlay] Shutdown complete");
}

void Overlay_OnPreReset()
{
    if (!g_OverlayInitialized.load()) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
}

void Overlay_OnPostReset(IDirect3DDevice9* pDev)
{
    if (!g_OverlayInitialized.load()) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

void Overlay_OnPresent(IDirect3DDevice9* pDev)
{
    if (!g_OverlayInitialized.load()) return;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_ShowMenu)
    {
        ImGui::Begin("d3d9capture Overlay", &g_ShowMenu);
        ImGui::Text("Overlay is active.");
        ImGui::Text("Press INSERT to toggle this menu.");
        ImGui::Separator();

        bool isCapturing = Capture_WantsFrame();
        if (isCapturing)
        {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: Capturing");
        }
        else
        {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Status: Idle (no consumers attached)");
        }

        ImGui::End();
    }

    ImGui::EndFrame();

    // D3D9 requires being within a Scene block to render geometry
    pDev->BeginScene();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    pDev->EndScene();
}
