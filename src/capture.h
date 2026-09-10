#pragma once
/**
 * capture.h  —  Frame capture interface
 *
 * Ownership model
 * ───────────────
 * Capture_Init / Capture_Shutdown are defined ONCE in consumer_backend.cpp.
 * They own top-level setup/teardown, and must call Capture_ReleaseSurfaces()
 * during shutdown to free the GPU staging surfaces owned by capture.cpp.
 *
 * Lifecycle (called from dllmain.cpp):
 *   Capture_Init()            – consumer one-time setup (consumer_backend.cpp)
 *   Capture_OnPresent()       – called every frame inside hooked Present (D3D9)
 *   Capture_OnPresentDXGI()   – called every frame inside hooked Present (D3D11/DXGI)
 *   Capture_OnPreReset()      – releases staging surfaces before Reset / ResizeBuffers
 *   Capture_OnPostReset()     – surfaces re-created lazily on next Present
 *   Capture_Shutdown()        – consumer teardown; must call Capture_ReleaseSurfaces()
 *
 * Internal helper (capture.cpp → called by consumer_backend.cpp):
 *   Capture_ReleaseSurfaces() – releases the D3D staging surfaces
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

// ── logging helper (defined in dllmain.cpp) ──────────────────────────────────
void Log(const char* fmt, ...);

// ── public API (Capture_Init / Capture_Shutdown defined in consumer_backend.cpp)
void Capture_Init();
void Capture_OnPresent(IDirect3DDevice9* pDev);
void Capture_OnPresentDXGI(IDXGISwapChain* pSwapChain);
void Capture_OnPreReset();
void Capture_OnPostReset(IDirect3DDevice9* pDev);
void Capture_OnPostResetDXGI(IDXGISwapChain* pSwapChain);
void Capture_Shutdown();

// ── internal helper implemented in capture.cpp, called by Capture_Shutdown() ──
void Capture_ReleaseSurfaces();

/**
 * Will anything actually use the next frame?  (consumer_backend.cpp)
 *
 * Capturing a frame means a synchronous GPU->CPU readback, which costs several
 * milliseconds and stalls the pipeline. capture.cpp asks this before paying
 * that cost, so a game with no consumer attached runs at full speed, and a game
 * with a slow consumer is throttled to the consumer's pace instead of the
 * render thread's.
 */
bool Capture_WantsFrame();
bool Capture_IsConsumerActive();

struct FrameData
{
    const void* pixels;   // top-left origin, tightly-packed rows
    UINT        width;
    UINT        height;
    UINT        stride;   // bytes per row (may be > width*4 due to GPU alignment)
    UINT32      format;   // D3DFORMAT or DXGI_FORMAT value
    UINT64      frameIdx; // monotonically increasing counter
};

struct CaptureStats
{
    UINT64    presentCalls;
    UINT64    capturedFrames;
    UINT      width;
    UINT      height;
    UINT32    format;
    float     presentFps;
    float     captureFps;
    float     readbackMs;
    bool      isConsumerActive;
    bool      captureEnabled;
    int       targetFps;
    int       dumpQuotaRemaining;
    char      lastScreenshotPath[MAX_PATH];
};

void Capture_GetStats(CaptureStats* pStats);
void Capture_SetEnabled(bool enabled);
bool Capture_IsEnabled();
void Capture_SetTargetFps(int targetFps);
int  Capture_GetTargetFps();
void Capture_TriggerSnapshot();
bool Capture_IsSnapshotPending();
bool Capture_TakeSnapshotPending();
void Capture_SetLastScreenshotPath(const char* path);
void Capture_GetLastScreenshotPath(char* dst, size_t maxLen);
void Capture_SetDumpQuota(int count);
int  Capture_GetDumpQuota();

/**
 * Consumer callback — implement or replace this in consumer_backend.cpp.
 * Called on the render thread; return quickly!  Copy pixels elsewhere and
 * signal a worker thread if you need to encode/write to disk.
 */
void Capture_FrameReady(const FrameData& frame);

// ── In-Memory Log Ring Buffer for In-Game Console ────────────────────────────
struct LogEntry
{
    char text[512];
};

size_t Log_GetRecentEntries(LogEntry* outEntries, size_t maxCount);
void   Log_ClearRecentEntries();
void   Log_ClearLogFile();
