/**
 * d3d9capture - DLL Injector for Direct3D 9 Frame Capture
 *
 * Hook chain (no D3D object is constructed by this DLL):
 *
 *   application's Direct3DCreate9/Ex import slot ← patched in the PE IAT
 *        │                                          and calls the real export
 *        ▼
 *   IDirect3D9::CreateDevice (slot 16)          ← patched on that factory so
 *        │                                          we intercept the game's call.
 *        ▼
 *   IDirect3DDevice9::Present (slot 17) ← patched on the device vtable.
 *   IDirect3DDevice9::Reset   (slot 16) ← patched on the device vtable.
 *   IDirect3DSwapChain9::Present (slot 3) ← patched on the implicit swap chain.
 *        Required, not optional: engines such as GTA IV present through the
 *        swap chain and never call IDirect3DDevice9::Present at all.
 *
 * Why this fixes GTA IV freezes
 * ──────────────────────────────
 * The original approach called CreateDevice on a worker thread, which
 * contended with the game's own CreateDevice call on the render thread.
 * D3D9's internal critical section deadlocked, freezing the process.
 *
 * Here we do not call into D3D9 at all from the injection worker.  We patch the
 * game's Direct3DCreate9/Ex import slot, then wait for the game to create its
 * own factory and device on its render thread.
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
#include <tlhelp32.h>

#include "capture.h"

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

    // The directory may not exist in a freshly injected process.  Creating it
    // here makes the on-disk trace useful even when no frame is captured.
    CreateDirectoryA("C:\\d3d9capture", nullptr);
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\d3d9capture\\debug.log", "a") == 0 && f)
    {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

// ── vtable slot indices ───────────────────────────────────────────────────────
static constexpr int VT_D3D9_CREATEDEVICE    = 16; // IDirect3D9::CreateDevice
static constexpr int VT_D3D9_CREATEDEVICEEX  = 20; // IDirect3D9Ex::CreateDeviceEx
static constexpr int VT_DEVICE_RESET         = 16; // IDirect3DDevice9::Reset
static constexpr int VT_DEVICE_PRESENT       = 17; // IDirect3DDevice9::Present
static constexpr int VT_DEVICE_GETSWAPCHAIN  = 14; // IDirect3DDevice9::GetSwapChain
static constexpr int VT_SWAPCHAIN_PRESENT    = 3;  // IDirect3DSwapChain9::Present
static constexpr int VT_DEVICE_PRESENT_EX    = 121; // IDirect3DDevice9Ex::PresentEx
static constexpr int VT_DEVICE_RESET_EX      = 132; // IDirect3DDevice9Ex::ResetEx

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

// ── saved originals (trampolines) ─────────────────────────────────────────────
static PFN_Direct3DCreate9   g_OrigDirect3DCreate9   = nullptr;
static PFN_Direct3DCreate9Ex g_OrigDirect3DCreate9Ex = nullptr;
static PFN_CreateDevice    g_OrigCreateDevice   = nullptr;
static PFN_CreateDeviceEx  g_OrigCreateDeviceEx = nullptr;
static PFN_Present         g_OrigPresent        = nullptr;
static PFN_PresentEx       g_OrigPresentEx      = nullptr;
static PFN_Reset           g_OrigReset          = nullptr;
static PFN_SwapChainPresent g_OrigSwapChainPresent = nullptr;
// The device whose frames we capture. Needed because a swap chain's Present
// does not receive the device as an argument.
static IDirect3DDevice9*   g_CaptureDevice      = nullptr;
static PFN_ResetEx         g_OrigResetEx        = nullptr;

static std::mutex        g_HookMtx;
static std::atomic<bool> g_DeviceHooked{ false };
static std::atomic<unsigned long> g_FactoryImportsPatched{ 0 };
static std::atomic<unsigned long> g_FactoryImportsSeen{ 0 };
// Set once our patched import slot is actually called: proof that the game is
// still ahead of its own device creation and will hand us the device itself.
static std::atomic<bool> g_FactoryIntercepted{ false };
// Non-zero while the game is inside the real CreateDevice/CreateDeviceEx. The
// D3D9 runtime holds internal locks across that call, so the worker thread must
// not touch any D3D9 object while it is set.
static std::atomic<int> g_D3D9CallsInFlight{ 0 };
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

    // An aligned pointer exchange is atomic on Windows.  This prevents another
    // thread from observing a partially-written import/vtable entry.
    void* old = InterlockedExchangePointer(ppSlot, pNew);
    *ppOld = old;

    DWORD ignored = 0;
    if (!VirtualProtect(ppSlot, sizeof(void*), oldProt, &ignored))
        Log("[dll] VirtualProtect restore failed: %lu", GetLastError());
    FlushInstructionCache(GetCurrentProcess(), ppSlot, sizeof(void*));
    return true;
}

static void HookSwapChainPresent(IDirect3DDevice9* pDev);

// ── device-level hooks ────────────────────────────────────────────────────────
static HRESULT WINAPI Hooked_Present(
    IDirect3DDevice9* pDev,
    const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirty)
{
    static bool logged = false;
    if (!logged) { Log("[dll] Hooked_Present called (first time)"); logged = true; }
    Capture_OnPresent(pDev);
    return g_OrigPresent(pDev, pSrc, pDst, hWnd, pDirty);
}

// Many engines never call IDirect3DDevice9::Present at all: they present
// through the swap chain instead. GTA IV is one of them — with only the device
// hooked, its Present fires zero times per frame while the swap chain's fires
// at the frame rate. Hooking just the device silently captures nothing on such
// a title, which is exactly the failure this DLL was reported to have.
static HRESULT WINAPI Hooked_SwapChainPresent(
    IDirect3DSwapChain9* pChain,
    const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirty, DWORD Flags)
{
    static bool logged = false;
    if (!logged) { Log("[dll] Hooked_SwapChainPresent called (first time)"); logged = true; }
    if (g_CaptureDevice) Capture_OnPresent(g_CaptureDevice);
    return g_OrigSwapChainPresent(pChain, pSrc, pDst, hWnd, pDirty, Flags);
}

static HRESULT WINAPI Hooked_Reset(
    IDirect3DDevice9* pDev, D3DPRESENT_PARAMETERS* pPP)
{
    Log("[dll] Hooked_Reset called");
    Capture_OnPreReset();
    HRESULT hr = g_OrigReset(pDev, pPP);
    if (SUCCEEDED(hr))
    {
        Capture_OnPostReset(pDev);
        HookSwapChainPresent(pDev);
    }
    return hr;
}

static HRESULT WINAPI Hooked_PresentEx(
    IDirect3DDevice9Ex* pDev,
    const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirty, DWORD Flags)
{
    static bool logged = false;
    if (!logged) { Log("[dll] Hooked_PresentEx called (first time)"); logged = true; }
    Capture_OnPresent(pDev);
    return g_OrigPresentEx(pDev, pSrc, pDst, hWnd, pDirty, Flags);
}

static HRESULT WINAPI Hooked_ResetEx(
    IDirect3DDevice9Ex* pDev, D3DPRESENT_PARAMETERS* pPP, D3DDISPLAYMODEEX* pMode)
{
    Log("[dll] Hooked_ResetEx called");
    Capture_OnPreReset();
    HRESULT hr = g_OrigResetEx(pDev, pPP, pMode);
    if (SUCCEEDED(hr))
    {
        Capture_OnPostReset(pDev);
        HookSwapChainPresent(pDev);
    }
    return hr;
}

static constexpr size_t VT_D3D9_COUNT = 17;       // final base slot: CreateDevice
static constexpr size_t VT_D3D9EX_COUNT = 21;     // final Ex slot: CreateDeviceEx
static constexpr size_t VT_DEVICE_COUNT = 119;    // final base slot: CreateQuery
static constexpr size_t VT_DEVICEEX_COUNT = 133;  // final Ex slot: ResetEx

// Both factory and device vtables are patched IN PLACE, never relocated to a
// private copy. That is the opposite of what it looks like it should be, so the
// reasoning is worth recording — both halves were established by experiment.
//
//  * A DEVICE's vtable is per-instance. The Windows D3D9 runtime embeds it in
//    the device's own allocation (observed at device+0x2F5C). Handing such a
//    device a relocated copy crashes the process on the first call through it,
//    before any hook of ours is even reached. Such a table is already private
//    to the object, so patching it in place affects nothing else.
//
//  * A FACTORY's vtable is shared, and copying it looks like the polite thing
//    to do. It is not. Other hook libraries are already in that table — ReShade,
//    loaded as the game's d3d9.dll, patches IDirect3D9::CreateDevice in the
//    shared system vtable in place — and they recover their own trampoline from
//    the object's vtable POINTER. Point the object at a copy and that lookup
//    fails against an address they have never seen. GTA IV died inside
//    CreateDevice this way, while the same clone worked in a test app with no
//    other overlay present.
//
// Patching in place is what every co-resident overlay does, so hooks chain in
// the order they were installed and each library still finds its own original.
static bool IsPerInstanceVTable(const void* object, const void* vtable)
{
    MEMORY_BASIC_INFORMATION vt = {};
    if (VirtualQuery(vtable, &vt, sizeof(vt)) == 0) return false;
    if (vt.Type == MEM_IMAGE) return false;      // part of a module: shared

    MEMORY_BASIC_INFORMATION obj = {};
    if (VirtualQuery(object, &obj, sizeof(obj)) == 0) return false;
    return obj.AllocationBase == vt.AllocationBase;
}

// Replace one vtable slot, saving the previous entry as the trampoline.
static bool HookSlot(void** vtbl, int slot, void* replacement, void** original)
{
    *original = vtbl[slot];
    return PatchVTable(&vtbl[slot], replacement, original);
}

// Hook the implicit swap chain's Present. Called on the game's own thread right
// after CreateDevice, and again after a Reset (a Reset recreates the implicit
// swap chain; when its vtable is per-instance the new object needs re-hooking).
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

// ── factory hooks ─────────────────────────────────────────────────────────────
// Hook the application's import slots rather than constructing a D3D object on
// our worker thread.  Constructing/releasing a factory while GTA IV is bringing
// up its renderer can contend on D3D9's loader/driver locks.  More importantly,
// import hooks also work with proxy D3D9 implementations whose vtables are not
// shared with a factory created by this DLL.
static void InstallFactoryHooks(IDirect3D9* pD3D, bool isEx)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (!pD3D) return;

    void** vtbl = *reinterpret_cast<void***>(pD3D);
    if (!vtbl) return;

    // Re-hooking the same factory would store Hooked_CreateDevice as its own
    // "original" trampoline and recurse forever on the next call. The polling
    // path can legitimately see the same factory more than once.
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

static bool IsD3D9Import(const char* moduleName)
{
    return moduleName && (_stricmp(moduleName, "d3d9.dll") == 0 ||
                          _stricmp(moduleName, "d3d9") == 0);
}

// Patch a module's normal PE import table.  We deliberately do not modify D3D9
// code or create a dummy device: this is safe to run after DLL_PROCESS_ATTACH
// and avoids GTA IV's initialization deadlock.
static void PatchModuleImports(HMODULE module)
{
    if (!module || module == g_ThisModule) return;

    // d3d9.dll has imports used by its own implementation. Hooking those can
    // re-enter the runtime while it is still establishing its internal state.
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(module, modulePath, MAX_PATH);
    const char* moduleName = strrchr(modulePath, '\\');
    moduleName = moduleName ? moduleName + 1 : modulePath;
    if (_stricmp(moduleName, "d3d9.dll") == 0)
    {
        Log("[hook] Skipping D3D9 runtime's own import table: %p", module);
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
        if (!IsD3D9Import(reinterpret_cast<const char*>(base + desc->Name))) continue;
        g_FactoryImportsSeen.fetch_add(1);

        auto firstThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base +
            (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++firstThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
            auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
            void* replacement = nullptr;
            void** original = nullptr;
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
            if (replacement)
            {
                if (PatchVTable(reinterpret_cast<void**>(&firstThunk->u1.Function), replacement, original))
                {
                    g_FactoryImportsPatched.fetch_add(1);
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

// ── locating an already-created device (late attach) ─────────────────────────
//
// When the DLL is injected into a process that has *already* created its
// device, there is no CreateDevice call left to intercept, so the device has
// to be found in memory.
//
// Two things make this harder than it looks, both confirmed against a live
// GTA IV process with the Frida scripts in tools/frida:
//
//  * The game may not talk to the system D3D9 runtime at all. A proxy DLL in
//    the game directory (ReShade, ENB, DXVK) is loaded *as* d3d9.dll and hands
//    the game its own wrapper objects, whose vtables live in the proxy image.
//    So "is this vtable in d3d9.dll?" must be asked by export, not by name.
//
//  * A vtable cannot be identified by shape. D3D9 vtables sit back-to-back in
//    .rdata, so "119 consecutive code pointers" matches a texture vtable
//    followed by its neighbours just as well as a device. QueryInterface is
//    the only authoritative answer.
//
// The sweep is deliberately limited to the writable sections of loaded modules.
// Sweeping the whole heap turns up freed and recycled allocations that merely
// look COM-shaped, and calling QueryInterface on one of those crashes the host
// process — observed doing exactly that during development.

static bool ModuleImplementsD3D9(HMODULE module)
{
    // A proxy wrapper is usually called d3d9.dll, but so is the real runtime,
    // and some wrappers use another name entirely. The export is what matters.
    return module &&
           (GetProcAddress(module, "Direct3DCreate9") != nullptr ||
            GetProcAddress(module, "Direct3DCreate9Ex") != nullptr);
}

// Is `address` executable code belonging to `module`?
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

// Which loaded D3D9 implementation owns this vtable, if any?  A genuine COM
// vtable has QueryInterface/AddRef/Release in slots 0-2, and all three must be
// code in one and the same module.
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

// Ask the object what it is. Safe on anything that really is a COM object: a
// texture simply answers E_NOINTERFACE.
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

// Walk one module's writable sections looking for a stored device pointer.
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

        // Use the section's own virtual size. VirtualQuery's region size stops
        // at the first protection change, which would truncate the sweep to a
        // single page.
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

// Sweep the executable and every loaded D3D9 implementation.
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

// Wait for a device, then — only if the passive route cannot work — go looking
// for one.
//
// The distinction matters enormously. Probing memory is read-only and safe, but
// confirming a candidate means calling QueryInterface on it, and that is a call
// INTO the D3D9 runtime from this worker thread. Doing that while the game's
// render thread is inside CreateDevice deadlocks on the runtime's internal
// locks — the original GTA IV freeze, reintroduced from a new direction.
// (Observed: the test app never returned from CreateDevice when the scan ran
// concurrently with it.)
//
// So: if our patched import slot has been called, the game has not created its
// device yet and Hooked_CreateDevice is guaranteed to deliver it. In that case
// we wait and touch nothing. Scanning is reserved for a late attach, where the
// factory call happened before we existed and there is nothing left to catch.
//
// Even then the scan is best-effort. It cannot find a device the game keeps
// only on the heap, which is what a RAGE-engine title such as GTA IV does, and
// widening it to sweep the heap is not an option: freed allocations still look
// COM-shaped, and calling QueryInterface on one crashes the host process.
// --launch remains the supported way in.
static void PollForExistingDevice(unsigned timeoutMs)
{
    const unsigned kGracePeriodMs = 5000;

    Log("[poll] Waiting for a device (timeout %u ms)", timeoutMs);

    bool announcedPassive = false;
    for (unsigned elapsed = 0; elapsed < timeoutMs && !g_DeviceHooked.load(); elapsed += 50)
    {
        Sleep(50);
        if (g_DeviceHooked.load()) break;

        // The game is mid-startup and will call our CreateDevice hook. Stay out
        // of the runtime's way.
        if (g_FactoryIntercepted.load())
        {
            if (!announcedPassive)
            {
                Log("[poll] Our import hook was called; waiting for the game's own "
                    "CreateDevice instead of probing D3D9");
                announcedPassive = true;
            }
            continue;
        }

        // Give a normal startup a moment to reach Direct3DCreate9 before
        // concluding that this is a late attach.
        if (elapsed < kGracePeriodMs) continue;

        // Never call into D3D9 while the game is inside CreateDevice.
        if (g_D3D9CallsInFlight.load() > 0) continue;

        bool isEx = false;
        if (IDirect3DDevice9* scanned = ScanForExistingDevice(&isEx))
        {
            Log("[poll] Found an existing device by scan after %u ms (isEx=%d)",
                elapsed, isEx ? 1 : 0);
            InstallDeviceHooks(scanned, isEx);
            if (g_DeviceHooked.load()) return;
        }
    }

    if (g_DeviceHooked.load())
        Log("[poll] Device hooks are live.");
    else
        Log("[poll] No device found. If the game was already running, inject with "
            "--launch so the import hook is in place before D3D9 starts.");
}

static void HookDirect3D9Factory()
{
    // Patch the executable's import table FIRST, unconditionally.
    //
    // This used to be skipped entirely for GTAIV.exe, on the assumption that
    // the game resolves Direct3DCreate9 through GetProcAddress and so has no
    // import slot to patch. That assumption is wrong: GTAIV.exe carries a
    // normal Direct3DCreate9 import (verified in a live process — the slot sits
    // at RVA 0xa73554 in the retail x86 build and resolves to whichever d3d9
    // implementation is loaded, including a ReShade/ENB proxy). Skipping it
    // threw away the one reliable hook in favour of a hard-coded address.
    //
    // The scan stays restricted to the executable: a process can contain
    // overlays and helper DLLs that also import Direct3DCreate9 while still
    // establishing their own loader state, and hooking those can re-enter their
    // initialization path and crash the title before its main thread starts.
    HMODULE executable = GetModuleHandleA(nullptr);
    char executablePath[MAX_PATH] = {};
    GetModuleFileNameA(executable, executablePath, MAX_PATH);
    Log("[hook] Scanning executable import table: %s (%p)", executablePath, executable);
    PatchModuleImports(executable);

    const unsigned long seen = g_FactoryImportsSeen.load();
    const unsigned long patched = g_FactoryImportsPatched.load();
    Log("[hook] Factory import scan complete: d3d9 import descriptors=%lu patched slots=%lu", seen, patched);
    if (!patched)
        Log("[hook] No Direct3DCreate9 import was patched. This title may use GetProcAddress, "
            "a proxy DLL, or a delay-load import.");

    // Either way, keep looking for a device that already exists. With the
    // import hooked this loop normally exits on the first pass once the game
    // creates its device; on a late attach it is the only path that can work.
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
    // DllMain returns while the loader lock is held. Do not run CRT/file/shared
    // memory code until it has been released; the primary game thread remains
    // suspended in --launch mode, so this short delay cannot lose D3D startup.
    Sleep(100);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Log("[dll] WorkerThread started (loader lock delay complete)");
    Capture_Init();

    // The suspended-launch injector waits for this acknowledgement before it
    // resumes the game's primary thread. That removes the race between DLL
    // loading and scheduling this worker.
    SignalInjectorReady();
    HookDirect3D9Factory();

    return 0;
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_ThisModule = hInst;
        DisableThreadLibraryCalls(hInst);
        // DllMain must remain loader-lock safe: no logging, file I/O, D3D, or
        // capture teardown belongs here. The worker performs initialization
        // after a loader-lock delay.
        CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        // Process teardown may hold the loader lock. Avoid releasing D3D/IPC
        // resources here; Windows reclaims process resources on termination.
        break;
    }
    return TRUE;
}
