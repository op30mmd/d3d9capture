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

#include "capture.h"

// ── tunables ──────────────────────────────────────────────────────────────────
static constexpr int   DUMP_FRAMES       = 10;                  // 0 = disabled
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
    UINT32  dataOffset; // bytes from start of mapping to first pixel byte
};
#pragma pack(pop)

// ── module state ──────────────────────────────────────────────────────────────
static HANDLE             g_hMapping    = nullptr;
static void*              g_pView       = nullptr;
static HANDLE             g_hEvtReady   = nullptr;  // signalled when frame written
static HANDLE             g_hEvtDone    = nullptr;  // signalled by reader when done
static std::atomic<int>   g_DumpCount   { 0 };
static char               g_DumpDir[MAX_PATH] = "C:\\d3d9capture\\";

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
    g_hMapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, SHM_MAX_BYTES, SHM_NAME);
    if (!g_hMapping) return;

    g_pView = MapViewOfFile(g_hMapping, FILE_MAP_WRITE, 0, 0, 0);

    // Events for producer/consumer synchronisation.
    g_hEvtReady = CreateEventA(nullptr, FALSE, FALSE, EVT_FRAME_READY);
    g_hEvtDone  = CreateEventA(nullptr, FALSE, TRUE,  EVT_FRAME_DONE);
}

static void ShutdownSharedMemory()
{
    if (g_pView)    { UnmapViewOfFile(g_pView);  g_pView    = nullptr; }
    if (g_hMapping) { CloseHandle(g_hMapping);   g_hMapping = nullptr; }
    if (g_hEvtReady){ CloseHandle(g_hEvtReady);  g_hEvtReady= nullptr; }
    if (g_hEvtDone) { CloseHandle(g_hEvtDone);   g_hEvtDone = nullptr; }
}

// ── public API (called from capture.h) ────────────────────────────────────────
void Capture_Init()
{
    CreateDirectoryA(g_DumpDir, nullptr);
    InitSharedMemory();
}

void Capture_Shutdown()
{
    ShutdownSharedMemory();
}

/**
 * Called on the render thread for every captured frame.
 * Keep this fast — the capture mutex is held for the duration.
 */
void Capture_FrameReady(const FrameData& f)
{
    // ── 1. Shared memory delivery ─────────────────────────────────────────
    if (g_pView && g_hEvtDone)
    {
        // Wait briefly for the reader to finish with the previous frame.
        // Timeout = 1 ms; if reader is slow we skip to avoid stalling the game.
        if (WaitForSingleObject(g_hEvtDone, 1) == WAIT_OBJECT_0)
        {
            ShmHeader* hdr = static_cast<ShmHeader*>(g_pView);
            hdr->width      = f.width;
            hdr->height     = f.height;
            hdr->stride     = f.width * 4;  // we write tightly-packed rows below
            hdr->format     = static_cast<UINT32>(f.format);
            hdr->frameIdx   = f.frameIdx;
            hdr->dataOffset = sizeof(ShmHeader);

            BYTE* dst = static_cast<BYTE*>(g_pView) + sizeof(ShmHeader);

            // Copy with stride correction so the reader always sees tight rows.
            const BYTE* src     = static_cast<const BYTE*>(f.pixels);
            DWORD       rowBytes = f.width * 4;
            for (UINT y = 0; y < f.height; ++y, src += f.stride, dst += rowBytes)
                memcpy(dst, src, rowBytes);

            SetEvent(g_hEvtReady);  // wake reader
        }
    }

    // ── 2. Debug BMP dump ─────────────────────────────────────────────────
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
