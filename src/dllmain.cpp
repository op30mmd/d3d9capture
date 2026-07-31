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
static PFN_ResetEx         g_OrigResetEx        = nullptr;

static std::mutex        g_HookMtx;
static std::atomic<bool> g_DeviceHooked{ false };
static std::atomic<unsigned long> g_FactoryImportsPatched{ 0 };
static std::atomic<unsigned long> g_FactoryImportsSeen{ 0 };
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

static HRESULT WINAPI Hooked_Reset(
    IDirect3DDevice9* pDev, D3DPRESENT_PARAMETERS* pPP)
{
    Log("[dll] Hooked_Reset called");
    Capture_OnPreReset();
    HRESULT hr = g_OrigReset(pDev, pPP);
    if (SUCCEEDED(hr))
        Capture_OnPostReset(pDev);
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
        Capture_OnPostReset(pDev);
    return hr;
}

static constexpr size_t VT_D3D9_COUNT = 17;       // final base slot: CreateDevice
static constexpr size_t VT_D3D9EX_COUNT = 21;     // final Ex slot: CreateDeviceEx
static constexpr size_t VT_DEVICE_COUNT = 119;    // final base slot: CreateQuery
static constexpr size_t VT_DEVICEEX_COUNT = 133;  // final Ex slot: ResetEx

// Many overlays (including the GTA IV D3D wrapper) share a D3D runtime vtable
// between objects. Never edit that shared read-only table: give only the object
// we received a private, writable vtable copy. This avoids clobbering ReShade,
// the system runtime, and other device users.
static void** CloneObjectVTable(void* object, size_t count)
{
    if (!object || !count) return nullptr;
    void*** objectVTable = reinterpret_cast<void***>(object);
    void** original = *objectVTable;
    if (!original) return nullptr;

    void** clone = static_cast<void**>(VirtualAlloc(nullptr, count * sizeof(void*),
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!clone) return nullptr;
    memcpy(clone, original, count * sizeof(void*));
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(objectVTable), clone);
    return clone;
}

static void InstallDeviceHooks(IDirect3DDevice9* pDev, bool bIsEx)
{
    std::lock_guard<std::mutex> lk(g_HookMtx);
    if (g_DeviceHooked.load() || !pDev) return;

    void** original = *reinterpret_cast<void***>(pDev);
    Log("[hook] Installing private device hooks device=%p vtable=%p isEx=%d ...", pDev, original, bIsEx);
    void** vtbl = CloneObjectVTable(pDev, bIsEx ? VT_DEVICEEX_COUNT : VT_DEVICE_COUNT);
    if (!vtbl)
    {
        Log("[hook] FAILED to clone device vtable: %lu", GetLastError());
        return;
    }

    g_OrigPresent = reinterpret_cast<PFN_Present>(vtbl[VT_DEVICE_PRESENT]);
    g_OrigReset = reinterpret_cast<PFN_Reset>(vtbl[VT_DEVICE_RESET]);
    vtbl[VT_DEVICE_PRESENT] = reinterpret_cast<void*>(Hooked_Present);
    vtbl[VT_DEVICE_RESET] = reinterpret_cast<void*>(Hooked_Reset);
    if (bIsEx)
    {
        g_OrigPresentEx = reinterpret_cast<PFN_PresentEx>(vtbl[VT_DEVICE_PRESENT_EX]);
        g_OrigResetEx = reinterpret_cast<PFN_ResetEx>(vtbl[VT_DEVICE_RESET_EX]);
        vtbl[VT_DEVICE_PRESENT_EX] = reinterpret_cast<void*>(Hooked_PresentEx);
        vtbl[VT_DEVICE_RESET_EX] = reinterpret_cast<void*>(Hooked_ResetEx);
    }

    Log("[hook] Device hooks installed successfully (private vtable=%p)", vtbl);
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
    HRESULT hr = g_OrigCreateDevice(
        pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppDevice);
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
    HRESULT hr = g_OrigCreateDeviceEx(
        pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, pOutMode, ppDevice);
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
    if (!pD3D) return;

    void** original = *reinterpret_cast<void***>(pD3D);
    void** vtbl = CloneObjectVTable(pD3D, isEx ? VT_D3D9EX_COUNT : VT_D3D9_COUNT);
    if (!vtbl)
    {
        Log("[hook] FAILED to clone factory vtable=%p: %lu", original, GetLastError());
        return;
    }

    g_OrigCreateDevice = reinterpret_cast<PFN_CreateDevice>(vtbl[VT_D3D9_CREATEDEVICE]);
    vtbl[VT_D3D9_CREATEDEVICE] = reinterpret_cast<void*>(Hooked_CreateDevice);
    if (isEx)
    {
        g_OrigCreateDeviceEx = reinterpret_cast<PFN_CreateDeviceEx>(vtbl[VT_D3D9_CREATEDEVICEEX]);
        vtbl[VT_D3D9_CREATEDEVICEEX] = reinterpret_cast<void*>(Hooked_CreateDeviceEx);
    }
    Log("[hook] Factory hooks installed with private vtable=%p (original=%p)", vtbl, original);
}

static IDirect3D9* WINAPI Hooked_Direct3DCreate9(UINT sdkVersion)
{
    PFN_Direct3DCreate9 original = g_OrigDirect3DCreate9;
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

// GTA IV resolves Direct3DCreate9 through GetProcAddress, so there is no IAT
// entry to intercept. For the supported x86 GTAIV.exe build, its master RAGE
// graphics context lives at VA 0x01295888 when loaded at image base 0x00400000.
// Context +0 is the wrapper IDirect3D9 interface; the game's d3d9 wrapper
// redirects its CreateDevice vtable slot to its own implementation. Patching
// that existing wrapper catches the real device without calling D3D9 ourselves.
// 0x01295888 is the preferred-image VA reported by analysis. Its RVA is
// 0x00E95888: retain the subtraction so ASLR is handled.
static constexpr uintptr_t GTAIV_CONTEXT_RVA = 0x01295888u - 0x00400000u;

static bool ReadTargetPointer(const void* address, void** value)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, value, sizeof(*value), &read) &&
           read == sizeof(*value);
}

static bool IsGTAIVExecutable()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    return _stricmp(name ? name + 1 : path, "GTAIV.exe") == 0;
}

static void WaitForGTAIVDevice()
{
#if defined(_WIN64)
    Log("[gtaiv] RAGE context resolver is only valid for the x86 GTA IV executable");
#else
    const uintptr_t contextAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + GTAIV_CONTEXT_RVA;
    Log("[gtaiv] Waiting for RAGE IDirect3D9 context at %p", reinterpret_cast<void*>(contextAddress));

    void* lastContext = nullptr;
    for (unsigned elapsed = 0; elapsed < 60000 && !g_DeviceHooked.load(); elapsed += 10)
    {
        void* context = nullptr;
        void* factory = nullptr;
        void* vtable = nullptr;
        void* createDevice = nullptr;
        if (ReadTargetPointer(reinterpret_cast<void*>(contextAddress), &context) && context != lastContext)
        {
            Log("[gtaiv] RAGE context changed: %p at %u ms", context, elapsed);
            lastContext = context;
        }
        if (context &&
            ReadTargetPointer(context, &factory) && factory &&
            ReadTargetPointer(factory, &vtable) && vtable &&
            ReadTargetPointer(static_cast<void**>(vtable) + VT_D3D9_CREATEDEVICE, &createDevice) && createDevice)
        {
            Log("[gtaiv] Found RAGE factory=%p vtable=%p CreateDevice=%p after %u ms",
                factory, vtable, createDevice, elapsed);
            InstallFactoryHooks(static_cast<IDirect3D9*>(factory), false);
            Log("[gtaiv] Hooked wrapper CreateDevice; waiting for the game to create its device");
            return;
        }
        Sleep(10);
    }
    Log("[gtaiv] Timed out waiting for RAGE factory context; no device was hooked");
#endif
}

static void HookDirect3D9Factory()
{
    if (IsGTAIVExecutable())
    {
        WaitForGTAIVDevice();
        return;
    }

    // Restrict the early-startup hook to the executable's import table.  A
    // process can contain overlays, compatibility layers, and D3D helper DLLs
    // which also import Direct3DCreate9 while establishing their own loader
    // state.  Hooking those secondary imports can re-enter their initialization
    // path and crash the title before its main thread starts.  GTA IV imports
    // D3D9 from its executable, which is the stable interception point.
    HMODULE executable = GetModuleHandleA(nullptr);
    char executablePath[MAX_PATH] = {};
    GetModuleFileNameA(executable, executablePath, MAX_PATH);
    Log("[hook] Scanning executable import table only: %s (%p)", executablePath, executable);
    PatchModuleImports(executable);

    const unsigned long seen = g_FactoryImportsSeen.load();
    const unsigned long patched = g_FactoryImportsPatched.load();
    Log("[hook] Factory import scan complete: d3d9 import descriptors=%lu patched slots=%lu", seen, patched);
    if (!patched)
        Log("[hook] No Direct3DCreate9 import in the executable. This title may use GetProcAddress, "
            "a proxy DLL, or a delay-load import; no secondary module is patched during startup.");
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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Log("[dll] WorkerThread started");
    // Install immediately. Waiting here loses games which construct D3D9
    // during startup, and calling Direct3DCreate9 ourselves to catch up is the
    // GTA IV deadlock that this implementation avoids.
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
        Log("[dll] DllMain(DLL_PROCESS_ATTACH), module=%p", hInst);
        DisableThreadLibraryCalls(hInst);
        CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        Log("[dll] DllMain(DLL_PROCESS_DETACH)");
        Capture_Shutdown();
        break;
    }
    return TRUE;
}
