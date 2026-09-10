/**
 * d3d9capture / d3d11capture - DLL Injector for Direct3D 9 & Direct3D 11 / DXGI Frame Capture
 *
 * Hook chain (no D3D/DXGI object is constructed by this DLL):
 *
 *   application's Direct3DCreate9/Ex, D3D11CreateDeviceAndSwapChain, or CreateDXGIFactory import slots
 *        │                                          patched in the PE IAT
 *        ▼
 *   IDirect3D9::CreateDevice (slot 16), IDXGIFactory::CreateSwapChain (slot 10)
 *        │                                          patched on that factory
 *        ▼
 *   IDirect3DDevice9::Present (slot 17) / Reset (slot 16)
 *   IDirect3DSwapChain9::Present (slot 3)
 *   IDXGISwapChain::Present (slot 8) / ResizeBuffers (slot 13)
 *
 * Build (MSVC, match game bitness — x86 for D3D9, x64 for D3D11 titles like GTA V):
 *   cl /nologo /W3 /O2 /MD /LD /I imgui /I imgui\backends /Fe:d3d9capture.dll dllmain.cpp capture.cpp
 *      consumer_backend.cpp recorder.cpp overlay.cpp ... /link d3d9.lib d3d11.lib dxgi.lib user32.lib ...
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <atomic>
#include <mutex>
#include <tlhelp32.h>

#include "capture.h"
#include "overlay.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

// ── logging ──────────────────────────────────────────────────────────────────
void Log(const char* fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    char line[1200];
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%02u:%02u:%02u.%03u pid=%lu tid=%lu] %s",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(), GetCurrentThreadId(), message);
    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    CreateDirectoryA("C:\\d3d9capture", nullptr);
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\d3d9capture\\debug.log", "a") == 0 && f)
    {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

// ── vtable slot indices ───────────────────────────────────────────────────────
static constexpr int VT_D3D9_CREATEDEVICE       = 16;  // IDirect3D9::CreateDevice
static constexpr int VT_D3D9_CREATEDEVICEEX     = 20;  // IDirect3D9Ex::CreateDeviceEx
static constexpr int VT_DEVICE_RESET            = 16;  // IDirect3DDevice9::Reset
static constexpr int VT_DEVICE_PRESENT          = 17;  // IDirect3DDevice9::Present
static constexpr int VT_DEVICE_GETSWAPCHAIN     = 14;  // IDirect3DDevice9::GetSwapChain
static constexpr int VT_SWAPCHAIN_PRESENT       = 3;   // IDirect3DSwapChain9::Present
static constexpr int VT_DEVICE_PRESENT_EX       = 121; // IDirect3DDevice9Ex::PresentEx
static constexpr int VT_DEVICE_RESET_EX         = 132; // IDirect3DDevice9Ex::ResetEx

static constexpr int VT_DXGI_SWAPCHAIN_PRESENT       = 8;  // IDXGISwapChain::Present
static constexpr int VT_DXGI_SWAPCHAIN_RESIZEBUFFERS = 13; // IDXGISwapChain::ResizeBuffers
static constexpr int VT_DXGI_FACTORY_CREATESWAPCHAIN = 10; // IDXGIFactory::CreateSwapChain

// ── hook typedefs ─────────────────────────────────────────────────────────────
typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);
typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);

typedef HRESULT (WINAPI *PFN_CreateDevice)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

typedef HRESULT (WINAPI *PFN_Present)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

typedef HRESULT (WINAPI *PFN_Reset)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

typedef HRESULT (WINAPI *PFN_SwapChainPresent)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

typedef HRESULT (WINAPI *PFN_PresentEx)(
    IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

typedef HRESULT (WINAPI *PFN_ResetEx)(
    IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

typedef HRESULT (WINAPI *PFN_CreateDeviceEx)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9**);

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

typedef HRESULT (WINAPI *PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);

typedef HRESULT (WINAPI *PFN_DXGISwapChainPresent)(
    IDXGISwapChain*, UINT, UINT);

typedef HRESULT (WINAPI *PFN_DXGISwapChainResizeBuffers)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

typedef HRESULT (WINAPI *PFN_DXGIFactoryCreateSwapChain)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

// ── saved originals (trampolines) ─────────────────────────────────────────────
static PFN_Direct3DCreate9   g_OrigDirect3DCreate9   = nullptr;
static PFN_Direct3DCreate9Ex g_OrigDirect3DCreate9Ex = nullptr;
static PFN_CreateDevice      g_OrigCreateDevice      = nullptr;
static PFN_CreateDeviceEx    g_OrigCreateDeviceEx    = nullptr;
static PFN_Present           g_OrigPresent           = nullptr;
static PFN_PresentEx         g_OrigPresentEx         = nullptr;
static PFN_Reset             g_OrigReset             = nullptr;
static PFN_SwapChainPresent  g_OrigSwapChainPresent  = nullptr;
static IDirect3DDevice9*     g_CaptureDevice         = nullptr;
static PFN_ResetEx           g_OrigResetEx           = nullptr;

static PFN_D3D11CreateDevice             g_OrigD3D11CreateDevice             = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain g_OrigD3D11CreateDeviceAndSwapChain = nullptr;
static PFN_CreateDXGIFactory             g_OrigCreateDXGIFactory             = nullptr;
static PFN_CreateDXGIFactory1            g_OrigCreateDXGIFactory1            = nullptr;
static PFN_CreateDXGIFactory2            g_OrigCreateDXGIFactory2            = nullptr;
static PFN_DXGISwapChainPresent          g_OrigDXGISwapChainPresent          = nullptr;
static PFN_DXGISwapChainResizeBuffers    g_OrigDXGISwapChainResizeBuffers    = nullptr;
static PFN_DXGIFactoryCreateSwapChain    g_OrigDXGIFactoryCreateSwapChain    = nullptr;

static std::mutex        g_HookMtx;
static std::atomic<bool> g_DeviceHooked{ false };
static std::atomic<unsigned long> g_FactoryImportsPatched{ 0 };
static std::atomic<unsigned long> g_FactoryImportsSeen{ 0 };
static std::atomic<bool> g_FactoryIntercepted{ false };
static std::atomic<int>  g_D3D9CallsInFlight{ 0 };
static HINSTANCE g_ThisModule = nullptr;

// ── vtable patcher ────────────────────────────────────────────────────────────
static bool PatchVTable(void** ppSlot, void* pNew, void** ppOld)
{
    if (!ppSlot || !pNew || !ppOld) return false;
    if (*ppSlot == pNew) return true;

    DWORD oldProt = 0;
    if (!VirtualProtect(ppSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
    {
        Log("[hook] VirtualProtect(%p) failed: %lu", ppSlot, GetLastError());
        return false;
    }

    void* old = InterlockedExchangePointer(ppSlot, pNew);
    *ppOld = old;

    DWORD ignored = 0;
    if (!VirtualProtect(ppSlot, sizeof(void*), oldProt, &ignored))
        Log("[dll] VirtualProtect restore failed: %lu", GetLastError());
    FlushInstructionCache(GetCurrentProcess(), ppSlot, sizeof(void*));
    return true;
}

static bool HookSlot(void** vtbl, int slot, void* replacement, void** original)
{
    *original = vtbl[slot];
    return PatchVTable(&vtbl[slot], replacement, original);
}

static void HookSwapChainPresent(IDirect3DDevice9* pDev);

// ── device-level hooks (D3D9 & DXGI) ──────────────────────────────────────────
static thread_local bool g_InsidePresent = false;
static thread_local bool s_DeviceFrameRendered = false;
static thread_local bool s_SwapChainFrameRendered = false;
static thread_local bool s_DeviceExFrameRendered = false;

static HRESULT WINAPI Hooked_Present(
    IDirect3DDevice9* pDev,
    const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirty)
{
    if (g_InsidePresent)
        return g_OrigPresent(pDev, pSrc, pDst, hWnd, pDirty);

    g_InsidePresent = true;
    static bool logged = false;
    if (!logged) { Log("[dll] Hooked_Present called (first time)"); logged = true; }

    if (!s_DeviceFrameRendered && pDev)
    {
        Capture_OnPresent(pDev);
        Overlay_OnPresent(pDev);
        s_DeviceFrameRendered = true;
    }

    HRESULT hr = g_OrigPresent(pDev, pSrc, pDst, hWnd, pDirty);
    if (hr != D3DERR_WASSTILLDRAWING)
    {
        s_DeviceFrameRendered = false;
    }

    g_InsidePresent = false;
    return hr;
}

static HRESULT WINAPI Hooked_SwapChainPresent(
    IDirect3DSwapChain9* pChain,
    const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirty, DWORD Flags)
{
    if (g_InsidePresent)
        return g_OrigSwapChainPresent(pChain, pSrc, pDst, hWnd, pDirty, Flags);

    g_InsidePresent = true;
    static bool logged = false;
    if (!logged) { Log("[dll] Hooked_SwapChainPresent called (first time)"); logged = true; }

    IDirect3DDevice9* pDev = g_CaptureDevice;
    if (!pDev && pChain)
    {
        if (SUCCEEDED(pChain->GetDevice(&pDev)) && pDev)
        {
            g_CaptureDevice = pDev;
            pDev->Release();
        }
    }

    if (!s_SwapChainFrameRendered && g_CaptureDevice)
    {
        Capture_OnPresent(g_CaptureDevice);
        Overlay_OnPresent(g_CaptureDevice);
        s_SwapChainFrameRendered = true;
    }

    HRESULT hr = g_OrigSwapChainPresent(pChain, pSrc, pDst, hWnd, pDirty, Flags);
    if (hr != D3DERR_WASSTILLDRAWING)
    {
        s_SwapChainFrameRendered = false;
    }

    g_InsidePresent = false;
    return hr;
}

static HRESULT WINAPI Hooked_Reset(
    IDirect3DDevice9* pDev, D3DPRESENT_PARAMETERS* pPP)
{
    Log("[dll] Hooked_Reset called");
    s_DeviceFrameRendered = false;
    s_SwapChainFrameRendered = false;
    s_DeviceExFrameRendered = false;
    Capture_OnPreReset();
    Overlay_OnPreReset();
    HRESULT hr = g_OrigReset(pDev, pPP);
    if (SUCCEEDED(hr))
    {
        Capture_OnPostReset(pDev);
        Overlay_OnPostReset(pDev);
        HookSwapChainPresent(pDev);
    }
    return hr;
}

static HRESULT WINAPI Hooked_PresentEx(
    IDirect3DDevice9Ex* pDev,
    const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirty, DWORD Flags)
{
    if (g_InsidePresent)
        return g_OrigPresentEx(pDev, pSrc, pDst, hWnd, pDirty, Flags);

    g_InsidePresent = true;
    static bool logged = false;
    if (!logged) { Log("[dll] Hooked_PresentEx called (first time)"); logged = true; }

    if (!s_DeviceExFrameRendered && pDev)
    {
        Capture_OnPresent(pDev);
        Overlay_OnPresent(pDev);
        s_DeviceExFrameRendered = true;
    }

    HRESULT hr = g_OrigPresentEx(pDev, pSrc, pDst, hWnd, pDirty, Flags);
    if (hr != D3DERR_WASSTILLDRAWING)
    {
        s_DeviceExFrameRendered = false;
    }

    g_InsidePresent = false;
    return hr;
}

static HRESULT WINAPI Hooked_ResetEx(
    IDirect3DDevice9Ex* pDev, D3DPRESENT_PARAMETERS* pPP, D3DDISPLAYMODEEX* pMode)
{
    Log("[dll] Hooked_ResetEx called");
    s_DeviceFrameRendered = false;
    s_SwapChainFrameRendered = false;
    s_DeviceExFrameRendered = false;
    Capture_OnPreReset();
    Overlay_OnPreReset();
    HRESULT hr = g_OrigResetEx(pDev, pPP, pMode);
    if (SUCCEEDED(hr))
    {
        Capture_OnPostReset(pDev);
        Overlay_OnPostReset(pDev);
        HookSwapChainPresent(pDev);
    }
    return hr;
}

// ── DXGI Hooks (D3D11 / GTA V) ───────────────────────────────────────────────
static HRESULT WINAPI Hooked_DXGISwapChainPresent(
    IDXGISwapChain* pChain, UINT SyncInterval, UINT Flags)
{
    if (g_InsidePresent)
        return g_OrigDXGISwapChainPresent(pChain, SyncInterval, Flags);

    g_InsidePresent = true;
    static bool logged = false;
    if (!logged) { Log("[dll] Hooked_DXGISwapChainPresent called (first time)"); logged = true; }

    if (pChain)
    {
        Capture_OnPresentDXGI(pChain);
        Overlay_OnPresentDXGI(pChain);
    }

    HRESULT hr = g_OrigDXGISwapChainPresent(pChain, SyncInterval, Flags);

    g_InsidePresent = false;
    return hr;
}

static HRESULT WINAPI Hooked_DXGISwapChainResizeBuffers(
    IDXGISwapChain* pChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    Log("[dll] Hooked_DXGISwapChainResizeBuffers called (%ux%u format=%u)", Width, Height, static_cast<unsigned>(NewFormat));
    s_DeviceFrameRendered = false;
    s_SwapChainFrameRendered = false;
    s_DeviceExFrameRendered = false;
    Capture_OnPreReset();
    Overlay_OnPreResetDXGI();

    HRESULT hr = g_OrigDXGISwapChainResizeBuffers(pChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(hr))
    {
        Capture_OnPostResetDXGI(pChain);
        Overlay_OnPostResetDXGI(pChain);
    }
    return hr;
}

static void InstallDXGISwapChainHooks(IDXGISwapChain* pSwapChain)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (g_DeviceHooked.load() || !pSwapChain) return;

    void** vtbl = *reinterpret_cast<void***>(pSwapChain);
    if (!vtbl) return;

    if (vtbl[VT_DXGI_SWAPCHAIN_PRESENT] == reinterpret_cast<void*>(Hooked_DXGISwapChainPresent))
    {
        Log("[hook] DXGI SwapChain %p is already hooked", pSwapChain);
        return;
    }

    Log("[hook] Installing DXGI SwapChain hooks pSwapChain=%p vtable=%p ...", pSwapChain, vtbl);

    HookSlot(vtbl, VT_DXGI_SWAPCHAIN_PRESENT, reinterpret_cast<void*>(Hooked_DXGISwapChainPresent),
             reinterpret_cast<void**>(&g_OrigDXGISwapChainPresent));
    HookSlot(vtbl, VT_DXGI_SWAPCHAIN_RESIZEBUFFERS, reinterpret_cast<void*>(Hooked_DXGISwapChainResizeBuffers),
             reinterpret_cast<void**>(&g_OrigDXGISwapChainResizeBuffers));

    Log("[hook] DXGI SwapChain hooks installed Present orig=%p ResizeBuffers orig=%p",
        reinterpret_cast<void*>(g_OrigDXGISwapChainPresent), reinterpret_cast<void*>(g_OrigDXGISwapChainResizeBuffers));

    Overlay_InitDXGI(pSwapChain);
    g_DeviceHooked.store(true);
}

static bool IsPerInstanceVTable(const void* object, const void* vtable)
{
    MEMORY_BASIC_INFORMATION vt = {};
    if (VirtualQuery(vtable, &vt, sizeof(vt)) == 0) return false;
    if (vt.Type == MEM_IMAGE) return false;

    MEMORY_BASIC_INFORMATION obj = {};
    if (VirtualQuery(object, &obj, sizeof(obj)) == 0) return false;
    return obj.AllocationBase == vt.AllocationBase;
}

static void HookSwapChainPresent(IDirect3DDevice9* pDev)
{
    if (!pDev) return;

    IDirect3DSwapChain9* chain = nullptr;
    if (FAILED(pDev->GetSwapChain(0, &chain)) || !chain)
    {
        Log("[hook] GetSwapChain(0) failed; swap-chain Present will not be hooked");
        return;
    }

    void** vtbl = *reinterpret_cast<void***>(chain);
    if (vtbl && vtbl[VT_SWAPCHAIN_PRESENT] != reinterpret_cast<void*>(Hooked_SwapChainPresent))
    {
        HookSlot(vtbl, VT_SWAPCHAIN_PRESENT, reinterpret_cast<void*>(Hooked_SwapChainPresent),
                 reinterpret_cast<void**>(&g_OrigSwapChainPresent));
        Log("[hook] Swap chain Present hooked chain=%p vtable=%p orig=%p",
            chain, vtbl, reinterpret_cast<void*>(g_OrigSwapChainPresent));
    }
    chain->Release();
}

static void InstallDeviceHooks(IDirect3DDevice9* pDev, bool bIsEx)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (g_DeviceHooked.load() || !pDev) return;

    void** original = *reinterpret_cast<void***>(pDev);
    if (!original) return;

    const bool perInstance = IsPerInstanceVTable(pDev, original);
    Log("[hook] Installing device hooks device=%p vtable=%p isEx=%d perInstance=%d ...",
        pDev, original, bIsEx ? 1 : 0, perInstance ? 1 : 0);

    void** vtbl = original;

    HookSlot(vtbl, VT_DEVICE_PRESENT, reinterpret_cast<void*>(Hooked_Present),
             reinterpret_cast<void**>(&g_OrigPresent));
    HookSlot(vtbl, VT_DEVICE_RESET, reinterpret_cast<void*>(Hooked_Reset),
             reinterpret_cast<void**>(&g_OrigReset));
    if (bIsEx)
    {
        HookSlot(vtbl, VT_DEVICE_PRESENT_EX, reinterpret_cast<void*>(Hooked_PresentEx),
                 reinterpret_cast<void**>(&g_OrigPresentEx));
        HookSlot(vtbl, VT_DEVICE_RESET_EX, reinterpret_cast<void*>(Hooked_ResetEx),
                 reinterpret_cast<void**>(&g_OrigResetEx));
    }

    Log("[hook] Device hooks installed in place (vtable=%p, %s) Present orig=%p Reset orig=%p",
        vtbl, perInstance ? "per-instance table" : "shared table",
        reinterpret_cast<void*>(g_OrigPresent), reinterpret_cast<void*>(g_OrigReset));

    g_CaptureDevice = pDev;
    HookSwapChainPresent(pDev);
    Overlay_Init(pDev);
    g_DeviceHooked.store(true);
}

// ── D3D9 / D3D11 Factory & Device hooks ───────────────────────────────────────
static HRESULT WINAPI Hooked_CreateDevice(
    IDirect3D9*             pD3D,
    UINT                    Adapter,
    D3DDEVTYPE              DeviceType,
    HWND                    hFocusWindow,
    DWORD                   BehaviorFlags,
    D3DPRESENT_PARAMETERS*  pPP,
    IDirect3DDevice9**      ppDevice)
{
    Log("[hook] CreateDevice factory=%p adapter=%u type=%u hwnd=%p flags=0x%08lX pp=%p",
        pD3D, Adapter, static_cast<unsigned>(DeviceType), hFocusWindow, BehaviorFlags, pPP);
    if (!g_OrigCreateDevice)
    {
        Log("[hook] CreateDevice has no original trampoline");
        return D3DERR_INVALIDCALL;
    }
    g_D3D9CallsInFlight.fetch_add(1);
    HRESULT hr = g_OrigCreateDevice(
        pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppDevice);
    g_D3D9CallsInFlight.fetch_sub(1);
    Log("[hook] CreateDevice returned hr=0x%08lX device=%p", hr,
        (ppDevice ? *ppDevice : nullptr));

    if (SUCCEEDED(hr) && ppDevice && *ppDevice)
        InstallDeviceHooks(*ppDevice, false);

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
    Log("[hook] CreateDeviceEx factory=%p adapter=%u type=%u hwnd=%p flags=0x%08lX pp=%p mode=%p",
        pD3D, Adapter, static_cast<unsigned>(DeviceType), hFocusWindow, BehaviorFlags, pPP, pOutMode);
    if (!g_OrigCreateDeviceEx)
    {
        Log("[hook] CreateDeviceEx has no original trampoline");
        return D3DERR_INVALIDCALL;
    }
    g_D3D9CallsInFlight.fetch_add(1);
    HRESULT hr = g_OrigCreateDeviceEx(
        pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, pOutMode, ppDevice);
    g_D3D9CallsInFlight.fetch_sub(1);
    Log("[hook] CreateDeviceEx returned hr=0x%08lX device=%p", hr,
        (ppDevice ? *ppDevice : nullptr));

    if (SUCCEEDED(hr) && ppDevice && *ppDevice)
        InstallDeviceHooks(*ppDevice, true);

    return hr;
}

static void InstallFactoryHooks(IDirect3D9* pD3D, bool isEx)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (!pD3D) return;

    void** vtbl = *reinterpret_cast<void***>(pD3D);
    if (!vtbl) return;

    if (vtbl[VT_D3D9_CREATEDEVICE] == reinterpret_cast<void*>(Hooked_CreateDevice))
    {
        Log("[hook] Factory %p is already hooked; leaving it alone", pD3D);
        return;
    }

    Log("[hook] Installing factory hooks factory=%p vtable=%p isEx=%d perInstance=%d ...",
        pD3D, vtbl, isEx ? 1 : 0, IsPerInstanceVTable(pD3D, vtbl) ? 1 : 0);

    HookSlot(vtbl, VT_D3D9_CREATEDEVICE, reinterpret_cast<void*>(Hooked_CreateDevice),
             reinterpret_cast<void**>(&g_OrigCreateDevice));
    if (isEx)
    {
        HookSlot(vtbl, VT_D3D9_CREATEDEVICEEX, reinterpret_cast<void*>(Hooked_CreateDeviceEx),
                 reinterpret_cast<void**>(&g_OrigCreateDeviceEx));
    }
    Log("[hook] Factory hooks installed in place (vtable=%p) CreateDevice orig=%p",
        vtbl, reinterpret_cast<void*>(g_OrigCreateDevice));
}

static IDirect3D9* WINAPI Hooked_Direct3DCreate9(UINT sdkVersion)
{
    PFN_Direct3DCreate9 original = g_OrigDirect3DCreate9;
    g_FactoryIntercepted.store(true);
    Log("[hook] Direct3DCreate9 intercepted sdk=%u original=%p", sdkVersion, original);
    if (!original) return nullptr;

    IDirect3D9* d3d = original(sdkVersion);
    Log("[hook] Direct3DCreate9 returned factory=%p", d3d);
    InstallFactoryHooks(d3d, false);
    return d3d;
}

static HRESULT WINAPI Hooked_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D)
{
    PFN_Direct3DCreate9Ex original = g_OrigDirect3DCreate9Ex;
    g_FactoryIntercepted.store(true);
    Log("[hook] Direct3DCreate9Ex intercepted sdk=%u original=%p out=%p", sdkVersion, original, ppD3D);
    if (!original) return E_FAIL;

    HRESULT hr = original(sdkVersion, ppD3D);
    Log("[hook] Direct3DCreate9Ex returned hr=0x%08lX factory=%p", hr,
        (ppD3D ? *ppD3D : nullptr));
    if (SUCCEEDED(hr) && ppD3D)
        InstallFactoryHooks(*ppD3D, true);
    return hr;
}

static HRESULT WINAPI Hooked_DXGIFactoryCreateSwapChain(
    IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    Log("[hook] DXGIFactory::CreateSwapChain factory=%p device=%p desc=%p", pFactory, pDevice, pDesc);
    if (!g_OrigDXGIFactoryCreateSwapChain) return E_FAIL;

    g_D3D9CallsInFlight.fetch_add(1);
    HRESULT hr = g_OrigDXGIFactoryCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    g_D3D9CallsInFlight.fetch_sub(1);

    Log("[hook] DXGIFactory::CreateSwapChain returned hr=0x%08lX swapChain=%p",
        hr, (ppSwapChain ? *ppSwapChain : nullptr));

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        InstallDXGISwapChainHooks(*ppSwapChain);
    }
    return hr;
}

static void InstallDXGIFactoryHooks(void* pFactory)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (!pFactory) return;

    void** vtbl = *reinterpret_cast<void***>(pFactory);
    if (!vtbl) return;

    if (vtbl[VT_DXGI_FACTORY_CREATESWAPCHAIN] == reinterpret_cast<void*>(Hooked_DXGIFactoryCreateSwapChain))
        return;

    HookSlot(vtbl, VT_DXGI_FACTORY_CREATESWAPCHAIN, reinterpret_cast<void*>(Hooked_DXGIFactoryCreateSwapChain),
             reinterpret_cast<void**>(&g_OrigDXGIFactoryCreateSwapChain));
    Log("[hook] DXGI Factory CreateSwapChain hooked factory=%p vtable=%p orig=%p",
        pFactory, vtbl, reinterpret_cast<void*>(g_OrigDXGIFactoryCreateSwapChain));
}

static HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    g_FactoryIntercepted.store(true);
    Log("[hook] CreateDXGIFactory intercepted");
    if (!g_OrigCreateDXGIFactory) return E_FAIL;

    HRESULT hr = g_OrigCreateDXGIFactory(riid, ppFactory);
    Log("[hook] CreateDXGIFactory returned hr=0x%08lX factory=%p", hr, (ppFactory ? *ppFactory : nullptr));
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        InstallDXGIFactoryHooks(*ppFactory);
    return hr;
}

static HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    g_FactoryIntercepted.store(true);
    Log("[hook] CreateDXGIFactory1 intercepted");
    if (!g_OrigCreateDXGIFactory1) return E_FAIL;

    HRESULT hr = g_OrigCreateDXGIFactory1(riid, ppFactory);
    Log("[hook] CreateDXGIFactory1 returned hr=0x%08lX factory=%p", hr, (ppFactory ? *ppFactory : nullptr));
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        InstallDXGIFactoryHooks(*ppFactory);
    return hr;
}

static HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    g_FactoryIntercepted.store(true);
    Log("[hook] CreateDXGIFactory2 intercepted");
    if (!g_OrigCreateDXGIFactory2) return E_FAIL;

    HRESULT hr = g_OrigCreateDXGIFactory2(flags, riid, ppFactory);
    Log("[hook] CreateDXGIFactory2 returned hr=0x%08lX factory=%p", hr, (ppFactory ? *ppFactory : nullptr));
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        InstallDXGIFactoryHooks(*ppFactory);
    return hr;
}

static HRESULT WINAPI Hooked_D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    g_FactoryIntercepted.store(true);
    Log("[hook] D3D11CreateDevice intercepted sdk=%u", SDKVersion);
    if (!g_OrigD3D11CreateDevice) return E_FAIL;

    g_D3D9CallsInFlight.fetch_add(1);
    HRESULT hr = g_OrigD3D11CreateDevice(
        pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
        ppDevice, pFeatureLevel, ppImmediateContext);
    g_D3D9CallsInFlight.fetch_sub(1);

    Log("[hook] D3D11CreateDevice returned hr=0x%08lX device=%p", hr, (ppDevice ? *ppDevice : nullptr));
    return hr;
}

static HRESULT WINAPI Hooked_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    g_FactoryIntercepted.store(true);
    Log("[hook] D3D11CreateDeviceAndSwapChain intercepted sdk=%u", SDKVersion);
    if (!g_OrigD3D11CreateDeviceAndSwapChain) return E_FAIL;

    g_D3D9CallsInFlight.fetch_add(1);
    HRESULT hr = g_OrigD3D11CreateDeviceAndSwapChain(
        pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
        pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    g_D3D9CallsInFlight.fetch_sub(1);

    Log("[hook] D3D11CreateDeviceAndSwapChain returned hr=0x%08lX swapChain=%p device=%p",
        hr, (ppSwapChain ? *ppSwapChain : nullptr), (ppDevice ? *ppDevice : nullptr));

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain)
    {
        InstallDXGISwapChainHooks(*ppSwapChain);
    }
    return hr;
}

// ── DirectInput8 hooks ────────────────────────────────────────────────────────
static std::mutex g_DIMtx;

typedef HRESULT (WINAPI *DirectInput8Create_t)(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID *ppvOut,
    LPUNKNOWN punkOuter
);
static DirectInput8Create_t g_OrigDirectInput8Create = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *DICreateDevice_t)(
    IDirectInput8A* pDI,
    REFGUID rguid,
    LPDIRECTINPUTDEVICE8A *lplpDirectInputDevice,
    LPUNKNOWN pUnkOuter
);
static DICreateDevice_t g_OrigDICreateDevice = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *DIGetDeviceState_t)(
    IDirectInputDevice8A* pDev,
    DWORD cbData,
    LPVOID lpvData
);

typedef HRESULT (STDMETHODCALLTYPE *DIGetDeviceData_t)(
    IDirectInputDevice8A* pDev,
    DWORD cbObjectData,
    LPDIDEVICEOBJECTDATA rgdod,
    LPDWORD pdwInOut,
    DWORD dwFlags
);

struct HookedDIDevice
{
    void** vtbl;
    DIGetDeviceState_t origState;
    DIGetDeviceData_t  origData;
    bool isMouse;
};
static HookedDIDevice g_HookedDevices[8] = {};
static std::atomic<int> g_HookedDevCount{ 0 };

static HRESULT STDMETHODCALLTYPE Hooked_DIGetDeviceState(
    IDirectInputDevice8A* pDev,
    DWORD cbData,
    LPVOID lpvData)
{
    void** devVtbl = *reinterpret_cast<void***>(pDev);
    DIGetDeviceState_t orig = nullptr;
    bool isMouse = false;
    int count = g_HookedDevCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
    {
        if (g_HookedDevices[i].vtbl == devVtbl)
        {
            orig = g_HookedDevices[i].origState;
            isMouse = g_HookedDevices[i].isMouse;
            break;
        }
    }
    if (!orig) return DIERR_NOTINITIALIZED;

    HRESULT hr = orig(pDev, cbData, lpvData);
    if (SUCCEEDED(hr) && Overlay_IsMenuOpen())
    {
        if (lpvData && cbData > 0)
        {
            if (isMouse && cbData >= sizeof(LONG) * 3)
            {
                LONG lZ = *reinterpret_cast<const LONG*>(reinterpret_cast<const BYTE*>(lpvData) + 8);
                if (lZ != 0)
                {
                    Overlay_AddMouseWheel(0.0f, static_cast<float>(lZ) / 120.0f);
                }
            }
            ZeroMemory(lpvData, cbData);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_DIGetDeviceData(
    IDirectInputDevice8A* pDev,
    DWORD cbObjectData,
    LPDIDEVICEOBJECTDATA rgdod,
    LPDWORD pdwInOut,
    DWORD dwFlags)
{
    void** devVtbl = *reinterpret_cast<void***>(pDev);
    DIGetDeviceData_t orig = nullptr;
    bool isMouse = false;
    int count = g_HookedDevCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
    {
        if (g_HookedDevices[i].vtbl == devVtbl)
        {
            orig = g_HookedDevices[i].origData;
            isMouse = g_HookedDevices[i].isMouse;
            break;
        }
    }
    if (!orig) return DIERR_NOTINITIALIZED;

    HRESULT hr = orig(pDev, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (SUCCEEDED(hr) && Overlay_IsMenuOpen())
    {
        if (pdwInOut && *pdwInOut > 0 && rgdod)
        {
            if (isMouse)
            {
                for (DWORD i = 0; i < *pdwInOut; ++i)
                {
                    if (rgdod[i].dwOfs == 8 && rgdod[i].dwData != 0)
                    {
                        LONG lZ = static_cast<LONG>(rgdod[i].dwData);
                        Overlay_AddMouseWheel(0.0f, static_cast<float>(lZ) / 120.0f);
                    }
                }
            }
            *pdwInOut = 0;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_DICreateDevice(
    IDirectInput8A* pDI,
    REFGUID rguid,
    LPDIRECTINPUTDEVICE8A *lplpDirectInputDevice,
    LPUNKNOWN pUnkOuter)
{
    HRESULT hr = g_OrigDICreateDevice(pDI, rguid, lplpDirectInputDevice, pUnkOuter);
    if (SUCCEEDED(hr) && lplpDirectInputDevice && *lplpDirectInputDevice)
    {
        IDirectInputDevice8A* pDev = *lplpDirectInputDevice;
        void** devVtbl = *reinterpret_cast<void***>(pDev);
        if (devVtbl)
        {
            std::lock_guard<std::mutex> lk(g_DIMtx);
            int count = g_HookedDevCount.load();
            bool alreadyHooked = false;
            for (int i = 0; i < count; ++i)
            {
                if (g_HookedDevices[i].vtbl == devVtbl)
                {
                    alreadyHooked = true;
                    break;
                }
            }

            if (!alreadyHooked && count < 8)
            {
                bool isMouse = false;
                DIDEVCAPS caps = {};
                caps.dwSize = sizeof(DIDEVCAPS);
                if (SUCCEEDED(pDev->GetCapabilities(&caps)))
                {
                    if (GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE)
                        isMouse = true;
                }
                else if (IsEqualGUID(rguid, GUID_SysMouse) || IsEqualGUID(rguid, GUID_SysMouseEm) || IsEqualGUID(rguid, GUID_SysMouseEm2))
                {
                    isMouse = true;
                }

                void* origState = nullptr;
                void* origData = nullptr;
                HookSlot(devVtbl, 9, reinterpret_cast<void*>(Hooked_DIGetDeviceState), &origState);
                HookSlot(devVtbl, 10, reinterpret_cast<void*>(Hooked_DIGetDeviceData), &origData);

                g_HookedDevices[count].vtbl = devVtbl;
                g_HookedDevices[count].origState = reinterpret_cast<DIGetDeviceState_t>(origState);
                g_HookedDevices[count].origData = reinterpret_cast<DIGetDeviceData_t>(origData);
                g_HookedDevices[count].isMouse = isMouse;
                g_HookedDevCount.store(count + 1, std::memory_order_release);

                Log("[hook] DirectInput device hooked dev=%p vtable=%p isMouse=%d origState=%p origData=%p",
                    pDev, devVtbl, isMouse ? 1 : 0, origState, origData);
            }
        }
    }
    return hr;
}

static void InstallDirectInputHooks(void* pDI)
{
    if (!pDI) return;
    std::lock_guard<std::mutex> lk(g_DIMtx);
    void** vtbl = *reinterpret_cast<void***>(pDI);
    if (!vtbl) return;

    if (vtbl[3] != reinterpret_cast<void*>(Hooked_DICreateDevice))
    {
        HookSlot(vtbl, 3, reinterpret_cast<void*>(Hooked_DICreateDevice),
                 reinterpret_cast<void**>(&g_OrigDICreateDevice));
        Log("[hook] DirectInput8 CreateDevice hooked pDI=%p vtable=%p orig=%p",
            pDI, vtbl, reinterpret_cast<void*>(g_OrigDICreateDevice));
    }
}

static HRESULT WINAPI Hooked_DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID *ppvOut,
    LPUNKNOWN punkOuter)
{
    Log("[hook] DirectInput8Create intercepted version=0x%04lx", dwVersion);
    if (!g_OrigDirectInput8Create)
    {
        Log("[hook] DirectInput8Create has no original trampoline");
        return DIERR_NOTINITIALIZED;
    }
    HRESULT hr = g_OrigDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    Log("[hook] DirectInput8Create returned hr=0x%08lX out=%p", hr,
        (ppvOut ? *ppvOut : nullptr));
    if (SUCCEEDED(hr) && ppvOut && *ppvOut)
    {
        InstallDirectInputHooks(*ppvOut);
    }
    return hr;
}

static bool IsD3D9Import(const char* moduleName)
{
    return moduleName && (_stricmp(moduleName, "d3d9.dll") == 0 ||
                          _stricmp(moduleName, "d3d9") == 0);
}

static bool IsD3D11Import(const char* moduleName)
{
    return moduleName && (_stricmp(moduleName, "d3d11.dll") == 0 ||
                          _stricmp(moduleName, "d3d11") == 0);
}

static bool IsDXGIImport(const char* moduleName)
{
    return moduleName && (_stricmp(moduleName, "dxgi.dll") == 0 ||
                          _stricmp(moduleName, "dxgi") == 0);
}

static bool IsDInput8Import(const char* moduleName)
{
    return moduleName && (_stricmp(moduleName, "dinput8.dll") == 0 ||
                          _stricmp(moduleName, "dinput8") == 0);
}

static void PatchModuleImports(HMODULE module)
{
    if (!module || module == g_ThisModule) return;

    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(module, modulePath, MAX_PATH);
    const char* moduleName = strrchr(modulePath, '\\');
    moduleName = moduleName ? moduleName + 1 : modulePath;
    if (_stricmp(moduleName, "d3d9.dll") == 0 || _stricmp(moduleName, "d3d11.dll") == 0 ||
        _stricmp(moduleName, "dxgi.dll") == 0 || _stricmp(moduleName, "dinput8.dll") == 0)
    {
        Log("[hook] Skipping runtime's own import table: %p (%s)", module, moduleName);
        return;
    }

    auto base = reinterpret_cast<unsigned char*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size) return;

    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; desc->Name; ++desc)
    {
        const char* descName = reinterpret_cast<const char*>(base + desc->Name);
        const bool isD3D9 = IsD3D9Import(descName);
        const bool isD3D11 = IsD3D11Import(descName);
        const bool isDXGI = IsDXGIImport(descName);
        const bool isDInput8 = IsDInput8Import(descName);
        if (!isD3D9 && !isD3D11 && !isDXGI && !isDInput8) continue;

        if (isD3D9 || isD3D11 || isDXGI) g_FactoryImportsSeen.fetch_add(1);

        auto firstThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base +
            (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++firstThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
            auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
            void* replacement = nullptr;
            void** original = nullptr;
            if (isD3D9)
            {
                if (strcmp(reinterpret_cast<const char*>(import->Name), "Direct3DCreate9") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_Direct3DCreate9);
                    original = reinterpret_cast<void**>(&g_OrigDirect3DCreate9);
                }
                else if (strcmp(reinterpret_cast<const char*>(import->Name), "Direct3DCreate9Ex") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_Direct3DCreate9Ex);
                    original = reinterpret_cast<void**>(&g_OrigDirect3DCreate9Ex);
                }
            }
            else if (isD3D11)
            {
                if (strcmp(reinterpret_cast<const char*>(import->Name), "D3D11CreateDeviceAndSwapChain") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_D3D11CreateDeviceAndSwapChain);
                    original = reinterpret_cast<void**>(&g_OrigD3D11CreateDeviceAndSwapChain);
                }
                else if (strcmp(reinterpret_cast<const char*>(import->Name), "D3D11CreateDevice") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_D3D11CreateDevice);
                    original = reinterpret_cast<void**>(&g_OrigD3D11CreateDevice);
                }
            }
            else if (isDXGI)
            {
                if (strcmp(reinterpret_cast<const char*>(import->Name), "CreateDXGIFactory") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_CreateDXGIFactory);
                    original = reinterpret_cast<void**>(&g_OrigCreateDXGIFactory);
                }
                else if (strcmp(reinterpret_cast<const char*>(import->Name), "CreateDXGIFactory1") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_CreateDXGIFactory1);
                    original = reinterpret_cast<void**>(&g_OrigCreateDXGIFactory1);
                }
                else if (strcmp(reinterpret_cast<const char*>(import->Name), "CreateDXGIFactory2") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_CreateDXGIFactory2);
                    original = reinterpret_cast<void**>(&g_OrigCreateDXGIFactory2);
                }
            }
            else if (isDInput8)
            {
                if (strcmp(reinterpret_cast<const char*>(import->Name), "DirectInput8Create") == 0)
                {
                    replacement = reinterpret_cast<void*>(Hooked_DirectInput8Create);
                    original = reinterpret_cast<void**>(&g_OrigDirectInput8Create);
                }
            }
            if (replacement)
            {
                if (PatchVTable(reinterpret_cast<void**>(&firstThunk->u1.Function), replacement, original))
                {
                    if (isD3D9 || isD3D11 || isDXGI) g_FactoryImportsPatched.fetch_add(1);
                    Log("[hook] Patched %s import in module=%p slot=%p original=%p",
                        import->Name, module, &firstThunk->u1.Function, *original);
                }
                else
                {
                    Log("[hook] Failed to patch %s import in module=%p", import->Name, module);
                }
            }
        }
    }
}

static bool ReadTargetPointer(const void* address, void** value)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, value, sizeof(*value), &read) &&
           read == sizeof(*value);
}

// ── memory sweep scanning for existing devices/swapchains ────────────────────
static bool ModuleImplementsD3D9(HMODULE module)
{
    return module &&
           (GetProcAddress(module, "Direct3DCreate9") != nullptr ||
            GetProcAddress(module, "Direct3DCreate9Ex") != nullptr);
}

static bool ModuleImplementsDXGI(HMODULE module)
{
    return module &&
           (GetProcAddress(module, "CreateDXGIFactory") != nullptr ||
            GetProcAddress(module, "CreateDXGIFactory1") != nullptr ||
            GetProcAddress(module, "CreateDXGIFactory2") != nullptr);
}

static bool IsCodeInModule(const void* address, HMODULE module)
{
    if (!address || !module) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.AllocationBase != module) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

static HMODULE OwnerOfD3D9VTable(void* const* vtable)
{
    if (!vtable) return nullptr;

    void* slot[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        if (!ReadTargetPointer(vtable + i, &slot[i]) || !slot[i]) return nullptr;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(slot[0], &mbi, sizeof(mbi)) == 0) return nullptr;
    const auto module = static_cast<HMODULE>(mbi.AllocationBase);
    if (!ModuleImplementsD3D9(module)) return nullptr;

    for (int i = 0; i < 3; ++i)
    {
        if (!IsCodeInModule(slot[i], module)) return nullptr;
    }
    return module;
}

static HMODULE OwnerOfDXGIVTable(void* const* vtable)
{
    if (!vtable) return nullptr;

    void* slot[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        if (!ReadTargetPointer(vtable + i, &slot[i]) || !slot[i]) return nullptr;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(slot[0], &mbi, sizeof(mbi)) == 0) return nullptr;
    const auto module = static_cast<HMODULE>(mbi.AllocationBase);
    if (!ModuleImplementsDXGI(module)) return nullptr;

    for (int i = 0; i < 3; ++i)
    {
        if (!IsCodeInModule(slot[i], module)) return nullptr;
    }
    return module;
}

static bool QueryIsDevice(IUnknown* candidate, bool* isEx)
{
    if (isEx) *isEx = false;
    if (!candidate) return false;

    bool found = false;
    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(candidate->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                                            reinterpret_cast<void**>(&ex))) && ex)
    {
        ex->Release();
        if (isEx) *isEx = true;
        found = true;
    }

    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(candidate->QueryInterface(__uuidof(IDirect3DDevice9),
                                            reinterpret_cast<void**>(&dev))) && dev)
    {
        dev->Release();
        found = true;
    }
    return found;
}

static bool QueryIsSwapChain(IUnknown* candidate)
{
    if (!candidate) return false;

    bool found = false;
    IDXGISwapChain* sc = nullptr;
    if (SUCCEEDED(candidate->QueryInterface(__uuidof(IDXGISwapChain),
                                            reinterpret_cast<void**>(&sc))) && sc)
    {
        sc->Release();
        found = true;
    }
    return found;
}

static IDirect3DDevice9* ScanModuleForDevice(HMODULE module, bool* isEx)
{
    if (!module) return nullptr;

    const auto image = reinterpret_cast<const unsigned char*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const IMAGE_SECTION_HEADER& sh = sections[i];
        if (!(sh.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        const auto begin = reinterpret_cast<void* const*>(image + sh.VirtualAddress);
        const size_t count = sh.Misc.VirtualSize / sizeof(void*);

        for (size_t j = 0; j < count; ++j)
        {
            void* candidate = nullptr;
            if (!ReadTargetPointer(begin + j, &candidate) || !candidate) continue;

            void* vtable = nullptr;
            if (!ReadTargetPointer(candidate, &vtable) || !vtable) continue;

            const HMODULE owner = OwnerOfD3D9VTable(static_cast<void* const*>(vtable));
            if (!owner) continue;

            bool candidateIsEx = false;
            if (!QueryIsDevice(static_cast<IUnknown*>(candidate), &candidateIsEx)) continue;

            Log("[scan] Confirmed device=%p vtable=%p owner=%p isEx=%d "
                "(found in module=%p section='%.8s' at +0x%zx)",
                candidate, vtable, owner, candidateIsEx ? 1 : 0,
                module, sh.Name, static_cast<size_t>(sh.VirtualAddress + j * sizeof(void*)));
            if (isEx) *isEx = candidateIsEx;
            return static_cast<IDirect3DDevice9*>(candidate);
        }
    }
    return nullptr;
}

static IDXGISwapChain* ScanModuleForSwapChain(HMODULE module)
{
    if (!module) return nullptr;

    const auto image = reinterpret_cast<const unsigned char*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const IMAGE_SECTION_HEADER& sh = sections[i];
        if (!(sh.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        const auto begin = reinterpret_cast<void* const*>(image + sh.VirtualAddress);
        const size_t count = sh.Misc.VirtualSize / sizeof(void*);

        for (size_t j = 0; j < count; ++j)
        {
            void* candidate = nullptr;
            if (!ReadTargetPointer(begin + j, &candidate) || !candidate) continue;

            void* vtable = nullptr;
            if (!ReadTargetPointer(candidate, &vtable) || !vtable) continue;

            const HMODULE owner = OwnerOfDXGIVTable(static_cast<void* const*>(vtable));
            if (!owner) continue;

            if (!QueryIsSwapChain(static_cast<IUnknown*>(candidate))) continue;

            Log("[scan] Confirmed IDXGISwapChain=%p vtable=%p owner=%p "
                "(found in module=%p section='%.8s' at +0x%zx)",
                candidate, vtable, owner,
                module, sh.Name, static_cast<size_t>(sh.VirtualAddress + j * sizeof(void*)));
            return static_cast<IDXGISwapChain*>(candidate);
        }
    }
    return nullptr;
}

static IDirect3DDevice9* ScanForExistingDevice(bool* isEx)
{
    if (IDirect3DDevice9* dev = ScanModuleForDevice(GetModuleHandleA(nullptr), isEx))
        return dev;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        Log("[scan] CreateToolhelp32Snapshot failed: %lu", GetLastError());
        return nullptr;
    }

    IDirect3DDevice9* found = nullptr;
    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry))
    {
        do
        {
            HMODULE module = entry.hModule;
            if (module == g_ThisModule || !ModuleImplementsD3D9(module)) continue;
            found = ScanModuleForDevice(module, isEx);
        } while (!found && Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static IDXGISwapChain* ScanForExistingSwapChain()
{
    if (IDXGISwapChain* sc = ScanModuleForSwapChain(GetModuleHandleA(nullptr)))
        return sc;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;

    IDXGISwapChain* found = nullptr;
    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry))
    {
        do
        {
            HMODULE module = entry.hModule;
            if (module == g_ThisModule || !ModuleImplementsDXGI(module)) continue;
            found = ScanModuleForSwapChain(module);
        } while (!found && Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static void PollForExistingDevice(unsigned timeoutMs)
{
    const unsigned kGracePeriodMs = 5000;

    Log("[poll] Waiting for a device / swap chain (timeout %u ms)", timeoutMs);

    bool announcedPassive = false;
    for (unsigned elapsed = 0; elapsed < timeoutMs && !g_DeviceHooked.load(); elapsed += 50)
    {
        Sleep(50);
        if (g_DeviceHooked.load()) break;

        if (g_FactoryIntercepted.load())
        {
            if (!announcedPassive)
            {
                Log("[poll] Our import hook was called; waiting for the game's own "
                    "device/swapchain creation instead of probing memory");
                announcedPassive = true;
            }
            continue;
        }

        if (elapsed < kGracePeriodMs) continue;
        if (g_D3D9CallsInFlight.load() > 0) continue;

        bool isEx = false;
        if (IDirect3DDevice9* scanned = ScanForExistingDevice(&isEx))
        {
            Log("[poll] Found an existing D3D9 device by scan after %u ms (isEx=%d)",
                elapsed, isEx ? 1 : 0);
            InstallDeviceHooks(scanned, isEx);
            if (g_DeviceHooked.load()) return;
        }

        if (IDXGISwapChain* scannedSC = ScanForExistingSwapChain())
        {
            Log("[poll] Found an existing DXGI swap chain by scan after %u ms", elapsed);
            InstallDXGISwapChainHooks(scannedSC);
            if (g_DeviceHooked.load()) return;
        }
    }

    if (g_DeviceHooked.load())
        Log("[poll] Device / Swap chain hooks are live.");
    else
        Log("[poll] No device/swapchain found. If the game was already running, inject with "
            "--launch so the import hook is in place before renderer starts.");
}

static void HookDirect3DFactory()
{
    HMODULE executable = GetModuleHandleA(nullptr);
    char executablePath[MAX_PATH] = {};
    GetModuleFileNameA(executable, executablePath, MAX_PATH);
    Log("[hook] Scanning executable import table: %s (%p)", executablePath, executable);
    PatchModuleImports(executable);

    const unsigned long seen = g_FactoryImportsSeen.load();
    const unsigned long patched = g_FactoryImportsPatched.load();
    Log("[hook] Factory import scan complete: render import descriptors=%lu patched slots=%lu", seen, patched);

    PollForExistingDevice(60000);
}

// ── worker thread ─────────────────────────────────────────────────────────────
static void SignalInjectorReady()
{
    char name[96] = {};
    _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\d3d9capture-ready-%lu", GetCurrentProcessId());
    HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (event)
    {
        SetEvent(event);
        CloseHandle(event);
        Log("[dll] Signalled injector readiness event");
    }
}

static DWORD WINAPI WorkerThread(LPVOID)
{
    Sleep(100);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Log("[dll] WorkerThread started (loader lock delay complete)");
    Capture_Init();

    SignalInjectorReady();
    HookDirect3DFactory();

    return 0;
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_ThisModule = hInst;
        DisableThreadLibraryCalls(hInst);
        CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr)
        {
            Overlay_Shutdown();
        }
        break;
    }
    return TRUE;
}
