/**
 * inject_tool.cpp  —  Simple DLL injector using CreateRemoteThread
 *
 * Usage:
 *   inject_tool.exe  <pid | process-name>  <full-path-to-dll>
 *   inject_tool.exe  --launch <game.exe> <full-path-to-dll> [-- game arguments]
 *   inject_tool.exe  --wait-for <process.exe> <full-path-to-dll> [--timeout <sec>]
 *
 * Build:
 *   cl /nologo /W3 /O2 /MT /Fe:inject_tool.exe inject_tool.cpp
 *
 * Notes:
 *  – Must be run as administrator (or with SeDebugPrivilege) to open game
 *    processes.
 *  – The injector, the DLL and the target MUST all have the same bitness
 *    (all 32-bit or all 64-bit).  Most D3D9 games (GTA IV, GTA San Andreas)
 *    are 32-bit; GTA V is 64-bit.  Build accordingly.  The injector checks
 *    all three before it touches the game and names the mismatch, because a
 *    wrong-bitness LoadLibraryA simply returns NULL with no other clue.
 *  – Anticheat software may detect this technique.  For protected games use a
 *    kernel-level or driver-based injector instead.
 */

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")  // OpenProcessToken, LookupPrivilegeValue, AdjustTokenPrivileges

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

// ── helpers ───────────────────────────────────────────────────────────────────
static DWORD FindPidByName(const char* name)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    DWORD pid = 0;

    if (Process32First(hSnap, &pe))
    {
        do {
            if (_stricmp(pe.szExeFile, name) == 0)
            {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return pid;
}

// Collect every PID with this image name, not just the first.  A game whose
// launcher re-executes the real renderer under the SAME name (GTA V ships
// GTA5.exe as a stub that spawns PlayGTAV.exe, which spawns the real GTA5.exe)
// produces several matches, and only one of them is the renderer.
static int FindPidsByName(const char* name, DWORD* out, int maxOut)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    int count = 0;
    if (Process32First(hSnap, &pe))
    {
        do {
            if (_stricmp(pe.szExeFile, name) == 0 && count < maxOut)
                out[count++] = pe.th32ProcessID;
        } while (Process32Next(hSnap, &pe) && count < maxOut);
    }
    CloseHandle(hSnap);
    return count;
}

/**
 * Has this process actually loaded a Direct3D runtime?
 *
 * This is what separates the real renderer from a launcher stub sharing its
 * name: the stub never touches d3d9/d3d11/dxgi.  It is also the readiness
 * signal we want, because the DLL's fallback swap-chain scan needs the graphics
 * runtime to be up before it can find anything.
 */
static bool ProcessHasRenderer(DWORD pid)
{
    // TH32CS_SNAPMODULE32 lets a 64-bit injector see a 32-bit (WOW64) process;
    // without it the snapshot fails with ERROR_PARTIAL_COPY and the game is
    // never recognised as a renderer.  The bitness check in Inject() still
    // refuses such a target with a clear message.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;  // starting up, or no access

    MODULEENTRY32 me = { sizeof(me) };
    bool found = false;
    if (Module32First(snap, &me))
    {
        do {
            if (_stricmp(me.szModule, "d3d11.dll") == 0 ||
                _stricmp(me.szModule, "dxgi.dll")  == 0 ||
                _stricmp(me.szModule, "d3d9.dll")  == 0)
            {
                found = true;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool EnableDebugPrivilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount   = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(nullptr, "SeDebugPrivilege",
                               &tp.Privileges[0].Luid))
    {
        CloseHandle(hToken);
        return false;
    }

    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(hToken);
    return GetLastError() == ERROR_SUCCESS;
}

// ── bitness checks ────────────────────────────────────────────────────────────
// A DLL can only be loaded into a process of the same bitness, and
// CreateRemoteThread with our own LoadLibraryA address only makes sense when
// the target maps the same kernel32 we do.  Injecting a 64-bit build into a
// 32-bit game (GTA San Andreas, GTA IV) or vice versa fails with a bare
// "LoadLibraryA returned 0", so every path resolves the three bitnesses first
// and refuses with a message that says which one is wrong.
enum class Arch { Unknown, X86, X64 };

static const char* ArchName(Arch a)
{
    switch (a)
    {
    case Arch::X86: return "32-bit (x86)";
    case Arch::X64: return "64-bit (x64)";
    default:        return "unknown";
    }
}

static Arch SelfArch()
{
    return sizeof(void*) == 8 ? Arch::X64 : Arch::X86;
}

// Bitness of an EXE or DLL on disk, from IMAGE_FILE_HEADER.Machine.
static Arch PeFileArch(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Arch::Unknown;

    Arch arch = Arch::Unknown;
    IMAGE_DOS_HEADER dos = {};
    DWORD read = 0;
    if (ReadFile(h, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(h, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
    {
        DWORD signature = 0;
        IMAGE_FILE_HEADER fh = {};
        if (ReadFile(h, &signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) &&
            signature == IMAGE_NT_SIGNATURE &&
            ReadFile(h, &fh, sizeof(fh), &read, nullptr) && read == sizeof(fh))
        {
            if (fh.Machine == IMAGE_FILE_MACHINE_I386) arch = Arch::X86;
            else if (fh.Machine == IMAGE_FILE_MACHINE_AMD64) arch = Arch::X64;
        }
    }
    CloseHandle(h);
    return arch;
}

// Bitness of a live process.  Works on a CREATE_SUSPENDED process too, since
// WOW64 status is fixed at creation.
static Arch ProcessArch(HANDLE hProc)
{
    BOOL wow64 = FALSE;
    if (!IsWow64Process(hProc, &wow64)) return Arch::Unknown;
    if (wow64) return Arch::X86;

    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? Arch::X86 : Arch::X64;
}

// Print the mismatch and how to fix it.  `targetLabel` names the game
// (an exe path or "PID 1234").
static void ReportBitnessMismatch(const char* targetLabel, Arch target,
                                  const char* dllPath, Arch dll)
{
    const Arch self = SelfArch();
    const char* want = target == Arch::X86 ? "x86" : "x64";

    printf("[inject] ERROR: bitness mismatch.\n");
    printf("         target   %-14s %s\n", ArchName(target), targetLabel);
    printf("         dll      %-14s %s\n", ArchName(dll), dllPath);
    printf("         injector %-14s (this inject_tool.exe)\n", ArchName(self));
    printf("         A DLL can only be loaded into a process of the same bitness, and the\n"
           "         injector must match the target too.  Use the %s build of both\n"
           "         d3d9capture.dll and inject_tool.exe: run build.bat from a \"%s Native\n"
           "         Tools\" developer prompt, or download the d3d9capture-%s release zip.\n",
           want, want, want);
}

// Returns true when injector, DLL and target all share one bitness.
static bool CheckBitness(const char* targetLabel, Arch target, const char* dllPath)
{
    const Arch dll  = PeFileArch(dllPath);
    const Arch self = SelfArch();

    if (target == Arch::Unknown)
        printf("[inject] WARNING: could not determine the target's bitness; continuing.\n");
    if (dll == Arch::Unknown)
        printf("[inject] WARNING: %s is not a readable x86/x64 PE image; continuing.\n", dllPath);

    const bool dllOk  = dll == Arch::Unknown || target == Arch::Unknown || dll == target;
    const bool selfOk = target == Arch::Unknown || self == target;
    if (dllOk && selfOk) return true;

    ReportBitnessMismatch(targetLabel, target, dllPath, dll);
    return false;
}

// ── injector core ─────────────────────────────────────────────────────────────
/**
 * Classic CreateRemoteThread + LoadLibraryA injection.
 *
 * Steps:
 *  1. Open the target process with PROCESS_ALL_ACCESS.
 *  2. VirtualAllocEx a page of memory in the target for the DLL path string.
 *  3. WriteProcessMemory the path into that page.
 *  4. GetProcAddress(kernel32, "LoadLibraryA") — the address is identical in
 *     all processes on the same OS session because ASLR randomises the base
 *     per-boot, not per-process.
 *  5. CreateRemoteThread(target, LoadLibraryA, remotePathAddr) — the OS
 *     creates a thread in the target that calls LoadLibraryA(path), which maps
 *     our DLL and calls its DllMain.
 */
static bool Inject(DWORD pid, const char* dllPath)
{
    printf("[inject] Opening PID %lu ...\n", pid);

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc)
    {
        printf("[inject] OpenProcess failed: %lu\n", GetLastError());
        return false;
    }

    char pidLabel[32] = {};
    _snprintf_s(pidLabel, sizeof(pidLabel), _TRUNCATE, "PID %lu", pid);
    if (!CheckBitness(pidLabel, ProcessArch(hProc), dllPath))
    {
        CloseHandle(hProc);
        return false;
    }

    // Allocate space for the path string in the remote process.
    size_t pathLen  = strlen(dllPath) + 1;
    LPVOID pRemote  = VirtualAllocEx(hProc, nullptr, pathLen,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemote)
    {
        printf("[inject] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, pRemote, dllPath, pathLen, nullptr))
    {
        printf("[inject] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HMODULE hKernel = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE pfnLoadLib = hKernel
        ? (LPTHREAD_START_ROUTINE)GetProcAddress(hKernel, "LoadLibraryA") : nullptr;
    if (!pfnLoadLib)
    {
        printf("[inject] Could not resolve LoadLibraryA: %lu\n", GetLastError());
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    printf("[inject] Creating remote thread → LoadLibraryA(\"%s\") ...\n", dllPath);

    HANDLE hThread = CreateRemoteThread(
        hProc, nullptr, 0, pfnLoadLib, pRemote, 0, nullptr);

    if (!hThread)
    {
        printf("[inject] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    const DWORD wait = WaitForSingleObject(hThread, 10000);
    if (wait != WAIT_OBJECT_0)
    {
        printf("[inject] LoadLibraryA did not finish within 10 seconds (wait=%lu).\n", wait);
        CloseHandle(hThread);
        // Do not free pRemote: the target thread may still read the DLL path.
        CloseHandle(hProc);
        return false;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeThread(hThread, &exitCode))
    {
        printf("[inject] GetExitCodeThread failed: %lu\n", GetLastError());
        CloseHandle(hThread);
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    printf("[inject] LoadLibraryA returned module handle: 0x%08lX\n", exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    return exitCode != 0;
}

static HANDLE CreateCaptureReadyEvent(DWORD pid)
{
    char name[96] = {};
    _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\d3d9capture-ready-%lu", pid);
    HANDLE event = CreateEventA(nullptr, TRUE, FALSE, name);
    if (!event)
        printf("[inject] CreateEvent(%s) failed: %lu\n", name, GetLastError());
    return event;
}

// ── entry point ───────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    const bool launchMode  = argc >= 4 && strcmp(argv[1], "--launch") == 0;
    const bool waitForMode = argc >= 4 && strcmp(argv[1], "--wait-for") == 0;
    if ((!launchMode && !waitForMode && argc != 3) ||
        ((launchMode || waitForMode) && argc < 4))
    {
        printf("Usage:\n");
        printf("  inject_tool.exe <pid | process.exe> <full-path-to-dll>\n");
        printf("  inject_tool.exe --launch <game.exe> <full-path-to-dll> [-- game arguments]\n");
        printf("  inject_tool.exe --wait-for <process.exe> <full-path-to-dll> [--timeout <sec>]\n");
        printf("\n");
        printf("  --launch   inject before the renderer starts.  Does NOT work for games\n");
        printf("             whose exe is a launcher stub that re-executes the real game\n");
        printf("             (GTA V): the DLL lands in a process that immediately exits.\n");
        printf("  --wait-for start the game normally, then use this: it waits for a process\n");
        printf("             of that name which has actually loaded d3d9/d3d11/dxgi and\n");
        printf("             injects into that one, skipping launcher stubs.\n");
        printf("\n");
        printf("  This inject_tool.exe is %s.  The game, the DLL and the injector must\n", ArchName(SelfArch()));
        printf("  all share one bitness: use the x86 build for 32-bit games (GTA IV, GTA SA)\n");
        printf("  and the x64 build for 64-bit games (GTA V).\n");
        return 1;
    }

    EnableDebugPrivilege();

    // --wait-for is the answer for launcher-stub games: poll until a process of
    // the given name has a Direct3D runtime mapped, which is both the proof that
    // it is the renderer and the point at which the DLL's swap-chain scan can
    // succeed.
    if (waitForMode)
    {
        const char* targetName = argv[2];
        char dllPath[MAX_PATH] = {};
        if (!GetFullPathNameA(argv[3], MAX_PATH, dllPath, nullptr) ||
            GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES)
        {
            printf("[inject] DLL not found: %s\n", argv[3]);
            return 1;
        }

        unsigned timeoutSec = 300;
        for (int i = 4; i + 1 < argc; ++i)
            if (strcmp(argv[i], "--timeout") == 0)
                timeoutSec = (unsigned)atoi(argv[i + 1]);

        printf("[inject] Waiting up to %u s for \"%s\" to load a Direct3D runtime ...\n",
               timeoutSec, targetName);

        DWORD tried[64] = {};
        int   nTried = 0;
        const DWORD deadline = GetTickCount() + timeoutSec * 1000;
        bool announced = false;

        for (;;)
        {
            DWORD pids[64] = {};
            const int n = FindPidsByName(targetName, pids, 64);

            if (n > 0 && !announced)
            {
                printf("[inject] Found %d process(es) named \"%s\"; waiting for the renderer ...\n",
                       n, targetName);
                announced = true;
            }

            for (int i = 0; i < n; ++i)
            {
                bool seen = false;
                for (int t = 0; t < nTried; ++t)
                    if (tried[t] == pids[i]) { seen = true; break; }
                if (seen || !ProcessHasRenderer(pids[i])) continue;

                if (nTried < 64) tried[nTried++] = pids[i];

                printf("[inject] PID %lu has a Direct3D runtime loaded - injecting.\n", pids[i]);
                HANDLE readyEvent = CreateCaptureReadyEvent(pids[i]);
                if (!Inject(pids[i], dllPath))
                {
                    if (readyEvent) CloseHandle(readyEvent);
                    printf("[inject] Injection into PID %lu failed; still watching.\n", pids[i]);
                    continue;
                }

                if (readyEvent)
                {
                    printf("[inject] Waiting for capture worker readiness ...\n");
                    const DWORD w = WaitForSingleObject(readyEvent, 10000);
                    CloseHandle(readyEvent);
                    if (w != WAIT_OBJECT_0)
                        printf("[inject] WARNING: capture worker did not signal readiness (wait=%lu).\n", w);
                }
                printf("[inject] SUCCESS: injected into PID %lu.\n", pids[i]);
                return 0;
            }

            if (GetTickCount() >= deadline)
            {
                printf("[inject] Timed out after %u s; no \"%s\" process loaded a Direct3D runtime.\n",
                       timeoutSec, targetName);
                return 1;
            }
            Sleep(250);
        }
    }

    // --launch creates the game with its primary thread suspended, injects
    // before any game code can create D3D9, then resumes it.  This is the
    // reliable companion to the GTA-IV-safe factory-import hook.
    if (launchMode)
    {
        char dllPath[MAX_PATH] = {};
        if (!GetFullPathNameA(argv[3], MAX_PATH, dllPath, nullptr) ||
            GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES)
        {
            printf("[inject] DLL not found: %s\n", argv[3]);
            return 1;
        }

        char gamePath[MAX_PATH] = {};
        if (!GetFullPathNameA(argv[2], MAX_PATH, gamePath, nullptr))
        {
            printf("[inject] Could not resolve game path.\n");
            return 1;
        }
        if (GetFileAttributesA(gamePath) == INVALID_FILE_ATTRIBUTES)
        {
            printf("[inject] Game executable not found: %s\n", gamePath);
            return 1;
        }

        // Refuse a wrong-bitness build before the game is even created, so
        // the user gets the diagnosis instead of a spawned-then-killed process.
        if (!CheckBitness(gamePath, PeFileArch(gamePath), dllPath))
            return 1;

        bool waitExit = false;
        char commandLine[8192] = {};
        _snprintf_s(commandLine, sizeof(commandLine), _TRUNCATE, "\"%s\"", gamePath);
        for (int i = 4; i < argc; ++i)
        {
            if (strcmp(argv[i], "--wait") == 0)
            {
                waitExit = true;
                continue;
            }
            if (strcmp(argv[i], "--") == 0 && i == 4) continue;
            strncat_s(commandLine, sizeof(commandLine), " ", _TRUNCATE);
            strncat_s(commandLine, sizeof(commandLine), argv[i], _TRUNCATE);
        }

        // Run the game from its own directory. Games resolve their data files
        // relative to the working directory, so inheriting the injector's cwd
        // makes a real title fail to start (GTA IV looks for common\ and pc\
        // beside the executable).
        char workingDir[MAX_PATH] = {};
        strncpy_s(workingDir, sizeof(workingDir), gamePath, _TRUNCATE);
        if (char* lastSlash = strrchr(workingDir, '\\'))
            *lastSlash = '\0';
        else
            workingDir[0] = '\0';

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        printf("[inject] Launching suspended: %s\n", commandLine);
        printf("[inject] Working directory: %s\n", workingDir[0] ? workingDir : "<inherited>");
        if (!CreateProcessA(gamePath, commandLine, nullptr, nullptr, TRUE,
                            CREATE_SUSPENDED, nullptr,
                            workingDir[0] ? workingDir : nullptr, &si, &pi))
        {
            printf("[inject] CreateProcess failed: %lu\n", GetLastError());
            return 1;
        }

        HANDLE readyEvent = CreateCaptureReadyEvent(pi.dwProcessId);
        if (!readyEvent)
        {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 1;
        }

        const bool injected = Inject(pi.dwProcessId, dllPath);
        if (!injected)
        {
            printf("[inject] Injection failed; terminating suspended process.\n");
            printf("[inject] HINT: LoadLibraryA returning 0 usually means the DLL's own\n"
                   "               dependencies are missing in the game (e.g. the Visual C++\n"
                   "               runtime for the DLL's bitness) or its DllMain failed.\n"
                   "               If this game exe is a launcher stub that re-executes the\n"
                   "               real renderer (GTA V does this), --launch cannot work:\n"
                   "               start the game normally, then use:\n"
                   "                 inject_tool.exe --wait-for <renderer.exe> <dll>\n");

            CloseHandle(readyEvent);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 1;
        }

        printf("[inject] Waiting for capture worker readiness ...\n");
        const DWORD readyWait = WaitForSingleObject(readyEvent, 5000);
        CloseHandle(readyEvent);
        if (readyWait != WAIT_OBJECT_0)
        {
            printf("[inject] Capture worker did not become ready (wait=%lu); refusing to resume.\n", readyWait);
            printf("[inject] HINT: if this game exe is a launcher stub that re-executes\n"
                   "               the real renderer (GTA V does this), --launch cannot work.\n"
                   "               Start the game normally, then use:\n"
                   "                 inject_tool.exe --wait-for <renderer.exe> <dll>\n");

            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 1;
        }

        if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
        {
            printf("[inject] ResumeThread failed: %lu\n", GetLastError());
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 1;
        }
        printf("[inject] SUCCESS: resumed PID %lu after injection.\n", pi.dwProcessId);
        CloseHandle(pi.hThread);

        // A launcher stub re-executes the real game and exits almost at once,
        // taking our DLL with it.  Say so plainly rather than leaving the user
        // with a "successful" injection into a process that no longer exists.
        if (!waitExit && WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0)
        {
            DWORD earlyCode = 0;
            GetExitCodeProcess(pi.hProcess, &earlyCode);
            printf("[inject] WARNING: PID %lu exited within 3 s (code %lu).\n",
                   pi.dwProcessId, earlyCode);
            printf("[inject] That usually means it was a launcher stub, so the DLL went with it.\n");
            printf("[inject] Use:  inject_tool.exe --wait-for <renderer.exe> <dll>\n");
        }

        if (waitExit)
        {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            printf("[inject] Target process exited with code %lu\n", code);
        }
        CloseHandle(pi.hProcess);
        return 0;
    }

    // Resolve PID: if the argument is purely digits treat it as a PID directly,
    // otherwise search by process name.
    DWORD pid = 0;
    bool  allDigits = true;
    for (const char* p = argv[1]; *p; ++p)
        if (!isdigit((unsigned char)*p)) { allDigits = false; break; }

    if (allDigits)
        pid = (DWORD)atoi(argv[1]);
    else
        pid = FindPidByName(argv[1]);

    if (!pid)
    {
        printf("[inject] Process \"%s\" not found.\n", argv[1]);
        return 1;
    }

    // Resolve the DLL path to an absolute path so LoadLibraryA finds it
    // from any working directory inside the target process.
    char absPath[MAX_PATH] = {};
    if (!GetFullPathNameA(argv[2], MAX_PATH, absPath, nullptr))
    {
        printf("[inject] Could not resolve DLL path.\n");
        return 1;
    }

    if (GetFileAttributesA(absPath) == INVALID_FILE_ATTRIBUTES)
    {
        printf("[inject] DLL not found: %s\n", absPath);
        return 1;
    }

    bool ok = Inject(pid, absPath);
    printf("[inject] %s\n", ok ? "SUCCESS" : "FAILED");
    return ok ? 0 : 1;
}
