#pragma once
/**
 * capture.h  —  Frame capture interface
 *
 * Lifecycle called from dllmain.cpp:
 *   Capture_Init()       – one-time setup
 *   Capture_OnPresent()  – called every frame (inside hooked Present)
 *   Capture_OnPreReset() – release GPU resources before Reset
 *   Capture_OnPostReset()– re-create GPU resources after successful Reset
 *   Capture_Shutdown()   – cleanup
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

// ── public API ────────────────────────────────────────────────────────────────
void Capture_Init();
void Capture_OnPresent(IDirect3DDevice9* pDev);
void Capture_OnPreReset();
void Capture_OnPostReset(IDirect3DDevice9* pDev);
void Capture_Shutdown();

// ── frame descriptor handed to the consumer backend ──────────────────────────
struct FrameData
{
    const void* pixels;   // top-left origin, tightly-packed rows
    UINT        width;
    UINT        height;
    UINT        stride;   // bytes per row (may be > width*4 due to GPU alignment)
    D3DFORMAT   format;   // typically D3DFMT_A8R8G8B8 or D3DFMT_X8R8G8B8
    UINT64      frameIdx; // monotonically increasing counter
};

/**
 * Consumer callback — implement or replace this in consumer_backend.cpp.
 * Called on the render thread; return quickly!  Copy pixels elsewhere and
 * signal a worker thread if you need to encode/write to disk.
 */
void Capture_FrameReady(const FrameData& frame);
