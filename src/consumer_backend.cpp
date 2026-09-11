/**
 * consumer_backend.cpp  —  Reference consumer for captured D3D9 frames
 *
 * Two outputs are provided; comment out whichever you don't need:
 *
 *  1. SHARED MEMORY  (for inter-process delivery, e.g. to an encoder process)
 *     Writes each frame into a named file-mapping object.  A companion reader
 *     process polls a "ready" event and pulls pixels without a socket/pipe copy.
 *
 *  2. BMP DUMP  (debugging aid)
 *     Saves the first N frames as numbered BMP files to C:\d3d9capture\.
 *     Set DUMP_FRAMES = 0 to disable.
 *
 * Because Capture_FrameReady is called on the render thread WITH the capture
 * mutex held you must be fast here.  The shared-memory path is a memcpy +
 * SetEvent — well under one millisecond for 1080p.
 *
 * For a production encoder (NVENC, x264, …) you would:
 *   – copy pixels into a ring-buffer here (fast)
 *   – signal a dedicated encoder thread (fast)
 *   – encode asynchronously (off the render thread)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>

#include "capture.h"
#include "recorder.h"
#include "audio.h"

// ── tunables ──────────────────────────────────────────────────────────────────
static constexpr int   DUMP_FRAMES       = 0;                   // 0 = disabled; >0 writes the first N frames as BMPs on the render thread (8 MB and ~12 ms each at 1080p), debugging only
static constexpr DWORD SHM_MAX_BYTES     = 7680 * 4320 * 4 + 64; // 4K RGBA + header
static constexpr char  SHM_NAME[]        = "Local\\D3D9CaptureShm";
static constexpr char  EVT_FRAME_READY[] = "Local\\D3D9CaptureReady";
static constexpr char  EVT_FRAME_DONE[]  = "Local\\D3D9CaptureDone";

// ── shared-memory layout ──────────────────────────────────────────────────────
#pragma pack(push, 1)
struct ShmHeader
{
    UINT32  width;
    UINT32  height;
    UINT32  stride;
    UINT32  format;    // D3DFORMAT value
    UINT64  frameIdx;
    // QueryPerformanceCounter value at capture time. Required for video:
    // frameIdx counts *captured* frames, not presented ones, so consecutive
    // indices can be arbitrarily far apart in wall-clock time whenever the
    // consumer is not asking for every frame. Encoding those at a constant
    // rate would silently play back at the wrong speed. QPC frequency is
    // system-wide, so the reader can obtain it itself.
    UINT64  timestampQpc;
    UINT32  dataOffset; // bytes from start of mapping to first pixel byte
};
#pragma pack(pop)

// ── module state ──────────────────────────────────────────────────────────────
static HANDLE             g_hMapping    = nullptr;
static void*              g_pView       = nullptr;
static HANDLE             g_hEvtReady   = nullptr;  // signalled when frame written
static HANDLE             g_hEvtDone    = nullptr;  // signalled by reader when done
static std::atomic<int>   g_DumpCount   { 0 };
// Set when Capture_WantsFrame() has consumed the reader's "done" signal, so
// Capture_FrameReady knows a reader is waiting without testing the event twice.
static std::atomic<bool>  g_ReaderArmed { false };
static char               g_DumpDir[MAX_PATH] = "C:\\d3d9capture\\";

static std::atomic<bool>  g_SnapshotPending{ false };
static std::atomic<int>   g_DumpQuota{ 0 };
static char               g_LastScreenshotPath[MAX_PATH] = "";
static std::mutex         g_ScreenshotMtx;

void Capture_TriggerSnapshot()
{
    g_SnapshotPending.store(true);
}

bool Capture_IsSnapshotPending()
{
    return g_SnapshotPending.load();
}

bool Capture_TakeSnapshotPending()
{
    return g_SnapshotPending.exchange(false);
}

void Capture_SetLastScreenshotPath(const char* path)
{
    std::lock_guard<std::mutex> lk(g_ScreenshotMtx);
    strncpy_s(g_LastScreenshotPath, sizeof(g_LastScreenshotPath), path, _TRUNCATE);
}

void Capture_GetLastScreenshotPath(char* dst, size_t maxLen)
{
    std::lock_guard<std::mutex> lk(g_ScreenshotMtx);
    strncpy_s(dst, maxLen, g_LastScreenshotPath, _TRUNCATE);
}

void Capture_SetDumpQuota(int count)
{
    g_DumpQuota.store(count);
}

int Capture_GetDumpQuota()
{
    return g_DumpQuota.load();
}

// ── BMP writer ────────────────────────────────────────────────────────────────
static bool WriteBmp(const char* path, const FrameData& f)
{
    // Only handles 32bpp X8R8G8B8 / A8R8G8B8 formats.
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};

    DWORD pixelBytes = f.width * f.height * 4;
    bfh.bfType      = 0x4D42; // 'BM'
    bfh.bfSize      = sizeof(bfh) + sizeof(bih) + pixelBytes;
    bfh.bfOffBits   = sizeof(bfh) + sizeof(bih);

    bih.biSize        = sizeof(bih);
    bih.biWidth       = (LONG)f.width;
    // BMP rows are bottom-up; negate height to tell readers it's top-down.
    bih.biHeight      = -(LONG)f.height;
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written;
    WriteFile(hFile, &bfh, sizeof(bfh), &written, nullptr);
    WriteFile(hFile, &bih, sizeof(bih), &written, nullptr);

    // Write row by row to handle non-tight stride.
    const BYTE* row = static_cast<const BYTE*>(f.pixels);
    DWORD rowBytes  = f.width * 4;
    for (UINT y = 0; y < f.height; ++y, row += f.stride)
        WriteFile(hFile, row, rowBytes, &written, nullptr);

    CloseHandle(hFile);
    return true;
}

// ── shared-memory init/shutdown ───────────────────────────────────────────────
static void InitSharedMemory()
{
    Log("[shm] InitSharedMemory ...");
    g_hMapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, SHM_MAX_BYTES, SHM_NAME);
    if (!g_hMapping)
    {
        Log("[shm] CreateFileMappingA failed: %lu", GetLastError());
        return;
    }

    g_pView = MapViewOfFile(g_hMapping, FILE_MAP_WRITE, 0, 0, 0);
    if (!g_pView)
    {
        Log("[shm] MapViewOfFile failed: %lu", GetLastError());
        return;
    }

    // Events for producer/consumer synchronisation.
    g_hEvtReady = CreateEventA(nullptr, FALSE, FALSE, EVT_FRAME_READY);
    g_hEvtDone  = CreateEventA(nullptr, FALSE, TRUE,  EVT_FRAME_DONE);
    Log("[shm] Shared memory initialized");
}

static void ShutdownSharedMemory()
{
    if (g_pView)    { UnmapViewOfFile(g_pView);  g_pView    = nullptr; }
    if (g_hMapping) { CloseHandle(g_hMapping);   g_hMapping = nullptr; }
    if (g_hEvtReady){ CloseHandle(g_hEvtReady);  g_hEvtReady= nullptr; }
    if (g_hEvtDone) { CloseHandle(g_hEvtDone);   g_hEvtDone = nullptr; }
}

// ── public API (called from capture.h) ────────────────────────────────────────
// Capture_Init and Capture_Shutdown are defined HERE (consumer_backend.cpp) and
// nowhere else.  capture.cpp intentionally does not define them to avoid the
// LNK2005 "multiply defined symbol" error that occurs when both translation
// units are linked into the same DLL.
void Capture_Init()
{
    Log("[con] Capture_Init");
    CreateDirectoryA(g_DumpDir, nullptr);
    InitSharedMemory();
    Audio_Init();
    Recorder_Init();
    // capture.cpp has no init work; surfaces are created lazily on first Present.
}

void Capture_Shutdown()
{
    Recorder_Shutdown();
    Audio_Shutdown();
    ShutdownSharedMemory();
    Capture_ReleaseSurfaces();  // free the D3D staging surfaces owned by capture.cpp
}

static std::atomic<DWORD> g_LastConsumerActiveTick{ 0 };

bool Capture_IsConsumerActive()
{
    DWORD last = g_LastConsumerActiveTick.load();
    if (last == 0) return false;
    return (GetTickCount() - last) < 1500;
}

/**
 * Called on the render thread before each candidate frame, to decide whether
 * the expensive GPU->CPU readback is worth doing at all.
 */
bool Capture_WantsFrame()
{
    // Snapshot requested
    if (g_SnapshotPending.load()) return true;

    // Burst dump requested
    if (g_DumpQuota.load() > 0) return true;

    // Video Recording active
    if (Recorder_WantsFrame()) return true;

    // The debug dump still wants frames until its quota is used up.
    if (DUMP_FRAMES > 0 && g_DumpCount.load() < DUMP_FRAMES) return true;

    // Otherwise a frame is only worth capturing if a reader is waiting for one.
    // The "done" event is auto-reset and only a reader ever re-signals it, so
    // this doubles as consumer detection and as back-pressure.
    if (g_pView && g_hEvtDone && WaitForSingleObject(g_hEvtDone, 0) == WAIT_OBJECT_0)
    {
        g_ReaderArmed.store(true);
        g_LastConsumerActiveTick.store(GetTickCount());
        return true;
    }
    return false;
}

/**
 * Called on the render thread for every captured frame.
 * Keep this fast — the capture mutex is held for the duration.
 */
void Capture_FrameReady(const FrameData& f)
{
    static bool logged = false;
    if (!logged) { Log("[con] Capture_FrameReady called (first time)"); logged = true; }

    // ── Video Recording ───────────────────────────────────────────────────
    Recorder_OnFrameReady(f.pixels, f.width, f.height, f.stride, f.frameIdx);

    // ── Instant Screenshot ────────────────────────────────────────────────
    if (Capture_TakeSnapshotPending())
    {
        char snapDir[MAX_PATH];
        _snprintf_s(snapDir, sizeof(snapDir), "%sscreenshots\\", g_DumpDir);
        CreateDirectoryA(g_DumpDir, nullptr);
        CreateDirectoryA(snapDir, nullptr);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char snapPath[MAX_PATH];
        _snprintf_s(snapPath, sizeof(snapPath), "%sshot_%04d%02d%02d_%02d%02d%02d_%03d.bmp",
                    snapDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        if (WriteBmp(snapPath, f))
        {
            Log("[con] Screenshot saved to %s", snapPath);
            Capture_SetLastScreenshotPath(snapPath);
        }
        else
        {
            Log("[con] Failed to write screenshot BMP: %s", snapPath);
        }
    }

    // ── Burst Frame Dump ──────────────────────────────────────────────────
    int quota = g_DumpQuota.load();
    if (quota > 0)
    {
        g_DumpQuota.fetch_sub(1);
        char dumpSubDir[MAX_PATH];
        _snprintf_s(dumpSubDir, sizeof(dumpSubDir), "%sdumps\\", g_DumpDir);
        CreateDirectoryA(g_DumpDir, nullptr);
        CreateDirectoryA(dumpSubDir, nullptr);

        char dumpPath[MAX_PATH];
        _snprintf_s(dumpPath, sizeof(dumpPath), "%sdump_%06llu.bmp",
                    dumpSubDir, static_cast<unsigned long long>(f.frameIdx));
        WriteBmp(dumpPath, f);
    }

    // ── 1. Shared memory delivery ─────────────────────────────────────────
    // Capture_WantsFrame() already consumed the reader's "done" signal; do not
    // wait on it a second time here. The previous code used a 1 ms timeout,
    // which cost the render thread that full millisecond on every frame
    // whenever no reader was attached — the normal case.
    if (g_pView && g_ReaderArmed.exchange(false))
    {
        g_LastConsumerActiveTick.store(GetTickCount());
        {
            ShmHeader* hdr = static_cast<ShmHeader*>(g_pView);
            hdr->width      = f.width;
            hdr->height     = f.height;
            hdr->stride     = f.width * 4;  // we write tightly-packed rows below
            hdr->format     = static_cast<UINT32>(f.format);
            hdr->frameIdx   = f.frameIdx;
            hdr->dataOffset = sizeof(ShmHeader);

            LARGE_INTEGER qpc = {};
            QueryPerformanceCounter(&qpc);
            hdr->timestampQpc = static_cast<UINT64>(qpc.QuadPart);

            BYTE* dst = static_cast<BYTE*>(g_pView) + sizeof(ShmHeader);

            // Copy with stride correction so the reader always sees tight rows.
            const BYTE* src     = static_cast<const BYTE*>(f.pixels);
            DWORD       rowBytes = f.width * 4;
            for (UINT y = 0; y < f.height; ++y, src += f.stride, dst += rowBytes)
                memcpy(dst, src, rowBytes);

            SetEvent(g_hEvtReady);  // wake reader
        }
    }

    // ── 2. Initial Debug BMP dump ─────────────────────────────────────────
    if (DUMP_FRAMES > 0)
    {
        int n = g_DumpCount.fetch_add(1);
        if (n < DUMP_FRAMES)
        {
            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), "%sframe_%05llu.bmp",
                        g_DumpDir, static_cast<unsigned long long>(f.frameIdx));
            WriteBmp(path, f);
        }
    }
}
