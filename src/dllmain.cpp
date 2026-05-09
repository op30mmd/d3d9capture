/**
 * d3d9capture - DLL Injector for Direct3D 9 Frame Capture
 *
 * Hook chain (no dummy device created):
 *
 *   Direct3DCreate9()                  ← called by us once, briefly, to get
 *        │                               the IDirect3D9 vtable address only.
 *        ▼                               The object is released immediately.
 *   IDirect3D9::CreateDevice (slot 16) ← patched on the shared vtable so we
 *        │                               intercept the GAME'S CreateDevice call.
 *        ▼
 *   IDirect3DDevice9::Present (slot 17) ← patched on the device vtable.
 *   IDirect3DDevice9::Reset   (slot 16) ← patched on the device vtable.
 *
 * Why this fixes GTA IV freezes
 * ──────────────────────────────
 * The original approach called CreateDevice on a worker thread, which
 * contended with the game's own CreateDevice call on the render thread.
 * D3D9's internal critical section deadlocked, freezing the process.
 *
 * Here we only call Direct3DCreate9 (cheap — no GPU resources allocated) to
 * read the vtable, then release it immediately.  CreateDevice is never called
 * by us; we wait for the game to call it and intercept that call instead.
 *
 * Build (MSVC, match game bitness — most D3D9 titles are 32-bit):
 *   cl /nologo /W3 /O2 /MD /LD /Fe:d3d9capture.dll dllmain.cpp capture.cpp
 *      consumer_backend.cpp /link d3d9.lib user32.lib gdi32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <mutex>

#include "capture.h"

// ── vtable slot indices ───────────────────────────────────────────────────────
static constexpr int VT_D3D9_CREATEDEVICE    = 16; // IDirect3D9::CreateDevice
static constexpr int VT_D3D9_CREATEDEVICEEX  = 20; // IDirect3D9Ex::CreateDeviceEx
static constexpr int VT_DEVICE_RESET         = 16; // IDirect3DDevice9::Reset
static constexpr int VT_DEVICE_PRESENT       = 17; // IDirect3DDevice9::Present

// ── hook typedefs ─────────────────────────────────────────────────────────────
typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);

typedef HRESULT (WINAPI *PFN_CreateDevice)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

typedef HRESULT (WINAPI *PFN_Present)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

typedef HRESULT (WINAPI *PFN_Reset)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

typedef HRESULT (WINAPI *PFN_CreateDeviceEx)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9**);

// ── saved originals (trampolines) ─────────────────────────────────────────────
static PFN_CreateDevice    g_OrigCreateDevice   = nullptr;
static PFN_CreateDeviceEx  g_OrigCreateDeviceEx = nullptr;
static PFN_Present         g_OrigPresent        = nullptr;
static PFN_Reset           g_OrigReset          = nullptr;

static std::mutex        g_HookMtx;
static std::atomic<bool> g_DeviceHooked{ false };

// ── vtable patcher ────────────────────────────────────────────────────────────
static bool PatchVTable(void** ppSlot, void* pNew, void** ppOld)
{
    if (*ppSlot == pNew) return true;
    DWORD oldProt = 0;
    if (!VirtualProtect(ppSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    *ppOld  = *ppSlot;
    *ppSlot = pNew;
    VirtualProtect(ppSlot, sizeof(void*), oldProt, &oldProt);
    return true;
}

// ── device-level hooks ────────────────────────────────────────────────────────
static HRESULT WINAPI Hooked_Present(
    IDirect3DDevice9* pDev,
    const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirty)
{
    Capture_OnPresent(pDev);
    return g_OrigPresent(pDev, pSrc, pDst, hWnd, pDirty);
}

static HRESULT WINAPI Hooked_Reset(
    IDirect3DDevice9* pDev, D3DPRESENT_PARAMETERS* pPP)
{
    Capture_OnPreReset();
    HRESULT hr = g_OrigReset(pDev, pPP);
    if (SUCCEEDED(hr))
        Capture_OnPostReset(pDev);
    return hr;
}

static void InstallDeviceHooks(IDirect3DDevice9* pDev)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (g_DeviceHooked.load()) return;

    void** vtbl = *reinterpret_cast<void***>(pDev);
    bool ok = true;
    ok &= PatchVTable(&vtbl[VT_DEVICE_PRESENT], (void*)Hooked_Present,
                      (void**)&g_OrigPresent);
    ok &= PatchVTable(&vtbl[VT_DEVICE_RESET],   (void*)Hooked_Reset,
                      (void**)&g_OrigReset);
    if (ok)
        g_DeviceHooked.store(true);
}

// ── IDirect3D9::CreateDevice hook ─────────────────────────────────────────────
// Called on the GAME'S thread when the game creates its device — no race.
static HRESULT WINAPI Hooked_CreateDevice(
    IDirect3D9*             pD3D,
    UINT                    Adapter,
    D3DDEVTYPE              DeviceType,
    HWND                    hFocusWindow,
    DWORD                   BehaviorFlags,
    D3DPRESENT_PARAMETERS*  pPP,
    IDirect3DDevice9**      ppDevice)
{
    HRESULT hr = g_OrigCreateDevice(
        pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice)
        InstallDeviceHooks(*ppDevice);

    return hr;
}

static HRESULT WINAPI Hooked_CreateDeviceEx(
    IDirect3D9Ex*           pD3D,
    UINT                    Adapter,
    D3DDEVTYPE              DeviceType,
    HWND                    hFocusWindow,
    DWORD                   BehaviorFlags,
    D3DPRESENT_PARAMETERS*  pPP,
    D3DDISPLAYMODEEX*       pOutMode,
    IDirect3DDevice9**      ppDevice)
{
    HRESULT hr = g_OrigCreateDeviceEx(
        pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, pOutMode, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice)
        InstallDeviceHooks(*ppDevice);

    return hr;
}

// ── IDirect3D9 vtable hook setup ──────────────────────────────────────────────
// We call Direct3DCreate9 once to obtain an IDirect3D9 pointer, read its
// vtable, patch CreateDevice, then immediately release the object.
// No device is ever created by us; no GPU resources are touched.
static void HookDirect3D9Factory()
{
    typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);

    // Load d3d9.dll if not already loaded (it always is in a D3D9 game).
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) return;

    auto pfnCreateEx = reinterpret_cast<PFN_Direct3DCreate9Ex>(
        GetProcAddress(hD3D9, "Direct3DCreate9Ex"));

    if (pfnCreateEx)
    {
        IDirect3D9Ex* pD3DEx = nullptr;
        if (SUCCEEDED(pfnCreateEx(D3D_SDK_VERSION, &pD3DEx)) && pD3DEx)
        {
            void** vtbl = *reinterpret_cast<void***>(pD3DEx);
            PatchVTable(&vtbl[VT_D3D9_CREATEDEVICE],   (void*)Hooked_CreateDevice,
                        (void**)&g_OrigCreateDevice);
            PatchVTable(&vtbl[VT_D3D9_CREATEDEVICEEX], (void*)Hooked_CreateDeviceEx,
                        (void**)&g_OrigCreateDeviceEx);

            // NOTE: We intentionally do NOT call pD3DEx->Release() here.
            // In some games (GTA IV), releasing the factory on a background
            // thread during early initialization can trigger a deadlock.
            return;
        }
    }

    auto pfnCreate = reinterpret_cast<PFN_Direct3DCreate9>(
        GetProcAddress(hD3D9, "Direct3DCreate9"));
    if (!pfnCreate) return;

    IDirect3D9* pD3D = pfnCreate(D3D_SDK_VERSION);
    if (!pD3D) return;

    // Patch CreateDevice on the shared IDirect3D9 vtable.
    void** vtbl = *reinterpret_cast<void***>(pD3D);
    PatchVTable(&vtbl[VT_D3D9_CREATEDEVICE], (void*)Hooked_CreateDevice,
                (void**)&g_OrigCreateDevice);

    // NOTE: pD3D->Release() removed to avoid destruction-related deadlocks.
}

// ── worker thread ─────────────────────────────────────────────────────────────
static DWORD WINAPI WorkerThread(LPVOID)
{
    // Brief delay so d3d9.dll is mapped before we query it.
    // We don't need to wait for the game's device — our CreateDevice hook
    // will fire on the game's own thread at the right moment.
    Sleep(2000);

    Capture_Init();
    HookDirect3D9Factory();

    return 0;
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        Capture_Shutdown();
        break;
    }
    return TRUE;
}
