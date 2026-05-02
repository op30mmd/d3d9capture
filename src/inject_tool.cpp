/**
 * inject_tool.cpp  —  Simple DLL injector using CreateRemoteThread
 *
 * Usage:
 *   inject_tool.exe  <pid | process-name>  <full-path-to-dll>
 *
 * Build:
 *   cl /nologo /W3 /O2 /MT /Fe:inject_tool.exe inject_tool.cpp
 *
 * Notes:
 *  – Must be run as administrator (or with SeDebugPrivilege) to open game
 *    processes.
 *  – The injector and the target MUST have the same bitness (both 32-bit or
 *    both 64-bit).  Most D3D9 games are 32-bit; build accordingly.
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
    LPTHREAD_START_ROUTINE pfnLoadLib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(hKernel, "LoadLibraryA");

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

    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    printf("[inject] LoadLibraryA returned module handle: 0x%08lX\n", exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    return exitCode != 0;
}

// ── entry point ───────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        printf("Usage: inject_tool.exe <pid | process.exe>  <C:\\full\\path\\to\\d3d9capture.dll>\n");
        return 1;
    }

    EnableDebugPrivilege();

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
