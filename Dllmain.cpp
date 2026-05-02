/**
 * d3d9capture - DLL Injector for Direct3D 9 Frame Capture
 *
 * Strategy:
 *  1. On DLL attach, spin up a worker thread.
 *  2. Create a hidden D3D9 device on a throw-away window just to read the
 *     virtual-method table (vtable) — no permanent device is kept alive.
 *  3. VTable-patch IDirect3DDevice9::Present (slot 17) and ::Reset (slot 16)
 *     so we intercept every frame the HOST application draws.
 *  4. In the hooked Present we pull the back-buffer off the GPU using
 *     GetRenderTargetData into a D3DPOOL_SYSTEMMEM surface, then lock it and
 *     hand the raw pixels to the capture backend.
 *  5. Reset hook restores the capture surface when the swap-chain is rebuilt.
 *
 * Build (MSVC, 32-bit — match the target game's bitness):
 *   cl /nologo /W3 /O2 /MD /LD /Fe:d3d9capture.dll dllmain.cpp capture.cpp
 *      /link d3d9.lib user32.lib gdi32.lib
 *
 * Inject with any standard injector (e.g. RemoteDLL, Process Hacker, or the
 * bundled inject_tool.cpp).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <atomic>
#include <mutex>

#include "capture.h"

// ── vtable slot indices for IDirect3DDevice9 ─────────────────────────────────
// Determined from the D3D9 SDK COM vtable layout.
static constexpr int VT_RESET   = 16;
static constexpr int VT_PRESENT = 17;

// ── hook state ────────────────────────────────────────────────────────────────
typedef HRESULT(WINAPI *PFN_Present)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

typedef HRESULT(WINAPI *PFN_Reset)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static PFN_Present  g_OrigPresent = nullptr;
static PFN_Reset    g_OrigReset   = nullptr;
static std::mutex   g_HookMtx;
static std::atomic<bool> g_Hooked{ false };

// ── forward declarations ───────────────────────────────────────────────────────
static bool  InstallHooks(IDirect3DDevice9* pDev);
static bool  PatchVTable(void** ppSlot, void* pNewFn, void** ppOldFn);

// ── hooked Present ─────────────────────────────────────────────────────────────
static HRESULT WINAPI Hooked_Present(
    IDirect3DDevice9* pDev,
    const RECT*       pSrcRect,
    const RECT*       pDstRect,
    HWND              hWnd,
    const RGNDATA*    pDirtyRegion)
{
    // Capture BEFORE calling original so we read the frame the game just drew.
    Capture_OnPresent(pDev);

    return g_OrigPresent(pDev, pSrcRect, pDstRect, hWnd, pDirtyRegion);
}

// ── hooked Reset ──────────────────────────────────────────────────────────────
// The swap-chain (and all DEFAULT-pool resources) are destroyed on Reset.
// We must release our capture surface before the call, or D3D will refuse it.
static HRESULT WINAPI Hooked_Reset(
    IDirect3DDevice9*   pDev,
    D3DPRESENT_PARAMETERS* pPP)
{
    Capture_OnPreReset();

    HRESULT hr = g_OrigReset(pDev, pPP);

    if (SUCCEEDED(hr))
        Capture_OnPostReset(pDev);

    return hr;
}

// ── VTable patching ────────────────────────────────────────────────────────────
/**
 * Overwrites a single vtable slot with our hook pointer using VirtualProtect
 * to bypass the read-only page protection the runtime places on vtables.
 *
 * @param ppSlot   Pointer to the vtable slot (pointer-to-function-pointer).
 * @param pNewFn   Our replacement function.
 * @param ppOldFn  Receives the original function pointer (acts as trampoline).
 * @return true on success.
 */
static bool PatchVTable(void** ppSlot, void* pNewFn, void** ppOldFn)
{
    DWORD oldProt = 0;
    if (!VirtualProtect(ppSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
        return false;

    *ppOldFn = *ppSlot;       // save original (this IS the trampoline — no copy
    *ppSlot  = pNewFn;        // needed because vtable dispatch jumps here direct)

    VirtualProtect(ppSlot, sizeof(void*), oldProt, &oldProt);
    return true;
}

static bool InstallHooks(IDirect3DDevice9* pDev)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (g_Hooked.load()) return true;

    // The COM object's first member is a pointer to its vtable.
    void** vtbl = *reinterpret_cast<void***>(pDev);

    bool ok = true;
    ok &= PatchVTable(&vtbl[VT_PRESENT], (void*)Hooked_Present,
                      (void**)&g_OrigPresent);
    ok &= PatchVTable(&vtbl[VT_RESET],   (void*)Hooked_Reset,
                      (void**)&g_OrigReset);

    if (ok)
        g_Hooked.store(true);

    return ok;
}

// ── temporary device creation for vtable sniffing ─────────────────────────────
/**
 * Creates a minimal, invisible D3D9 device solely to learn the vtable address.
 * We then immediately hook the live game device when Present is first called.
 *
 * Actually — a simpler and more reliable approach:
 *   We create a real device here and hook ITS vtable.  Because COM vtables are
 *   per-class (shared across all instances of the same driver implementation),
 *   patching this dummy device's vtable also patches every other D3D9 device in
 *   the process — including the game's device.
 */
static void SetupHooksViaTemporaryDevice()
{
    // Create a tiny invisible window just for D3D init.
    WNDCLASSEXA wc   = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "D3D9CaptureWnd";
    RegisterClassExA(&wc);

    HWND hWnd = CreateWindowExA(
        0, "D3D9CaptureWnd", "",
        WS_OVERLAPPED, 0, 0, 1, 1,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hWnd) return;

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) { DestroyWindow(hWnd); return; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed             = TRUE;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat     = D3DFMT_UNKNOWN;
    pp.BackBufferWidth      = 1;
    pp.BackBufferHeight     = 1;
    pp.hDeviceWindow        = hWnd;

    IDirect3DDevice9* pDev = nullptr;
    HRESULT hr = pD3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pDev);

    if (SUCCEEDED(hr) && pDev)
    {
        InstallHooks(pDev);   // patches the shared vtable
        pDev->Release();
    }

    pD3D->Release();
    DestroyWindow(hWnd);
    UnregisterClassA("D3D9CaptureWnd", wc.hInstance);
}

// ── worker thread ─────────────────────────────────────────────────────────────
static DWORD WINAPI WorkerThread(LPVOID)
{
    // Small delay so the host process finishes its own D3D init first.
    Sleep(500);

    Capture_Init();
    SetupHooksViaTemporaryDevice();

    // Stay alive; capture state is managed inside capture.cpp.
    while (g_Hooked.load())
        Sleep(100);

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
