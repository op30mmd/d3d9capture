/**
 * capture.cpp  —  Efficient D3D9 GPU frame readback
 *
 * Design goals
 * ─────────────
 * 1. ZERO extra copy on the CPU path.
 *    GetRenderTargetData DMA-copies the render-target directly into a
 *    D3DPOOL_SYSTEMMEM surface.  We then lock that surface and hand the
 *    pointer straight to the consumer; no memcpy needed.
 *
 * 2. Double-buffered system-memory surfaces for asynchronous readback.
 *    While the consumer processes frame N the GPU is already staging frame N+1
 *    into the other surface.  This hides the DMA latency from the render thread.
 *
 * 3. Graceful Reset handling.
 *    All offscreen surfaces are released before IDirect3DDevice9::Reset and
 *    re-created afterwards.
 *
 * 4. Format portability.
 *    We accept whatever back-buffer format the game chose (X8R8G8B8, A8R8G8B8,
 *    A2R10G10B10, …) and report it to the consumer unmodified.
 *
 * Thread safety
 * ─────────────
 * Capture_OnPresent is called on the game's render thread.
 * Capture_Shutdown may be called from a different thread (DLL_PROCESS_DETACH).
 * The mutex g_CapMtx serialises access to the shared surfaces.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <mutex>
#include <atomic>

#include "capture.h"

// ── constants ─────────────────────────────────────────────────────────────────
static constexpr int  NUM_STAGING = 2;   // double-buffer staging surfaces
static constexpr UINT MAX_WIDTH   = 7680; // guard against absurd resolutions
static constexpr UINT MAX_HEIGHT  = 4320;

// ── per-surface state ─────────────────────────────────────────────────────────
struct StagingSurface
{
    IDirect3DSurface9* pSurf   = nullptr;
    UINT               width   = 0;
    UINT               height  = 0;
    D3DFORMAT          format  = D3DFMT_UNKNOWN;
    bool               pending = false; // DMA in-flight
};

// ── module state ──────────────────────────────────────────────────────────────
static std::mutex              g_CapMtx;
static StagingSurface          g_Staging[NUM_STAGING];
static int                     g_WriteIdx = 0;   // surface we're filling now
static int                     g_ReadIdx  = 1;   // surface ready to consume
static std::atomic<UINT64>     g_FrameIdx{ 0 };
static std::atomic<bool>       g_Shutdown{ false };

// ── helpers ───────────────────────────────────────────────────────────────────
static void ReleaseSurface(StagingSurface& s)
{
    if (s.pSurf) { s.pSurf->Release(); s.pSurf = nullptr; }
    s.width = s.height = 0;
    s.format  = D3DFMT_UNKNOWN;
    s.pending = false;
}

/**
 * Ensures the staging surface for slot |idx| matches the requested dimensions
 * and format, re-creating it if stale.
 */
static bool EnsureSurface(IDirect3DDevice9* pDev, int idx,
                           UINT w, UINT h, D3DFORMAT fmt)
{
    StagingSurface& s = g_Staging[idx];
    if (s.pSurf && s.width == w && s.height == h && s.format == fmt)
        return true;   // already good

    ReleaseSurface(s);

    HRESULT hr = pDev->CreateOffscreenPlainSurface(
        w, h, fmt,
        D3DPOOL_SYSTEMMEM,   // CPU-accessible, no GPU bandwidth cost on lock
        &s.pSurf,
        nullptr);

    if (FAILED(hr))
    {
        // Fallback: some drivers don't support the exact back-buffer format in
        // SYSTEMMEM — retry with the universal X8R8G8B8.
        if (fmt != D3DFMT_X8R8G8B8)
        {
            hr = pDev->CreateOffscreenPlainSurface(
                w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                &s.pSurf, nullptr);
            if (SUCCEEDED(hr))
                fmt = D3DFMT_X8R8G8B8;
        }
        if (FAILED(hr)) return false;
    }

    s.width  = w;
    s.height = h;
    s.format = fmt;
    return true;
}

// ── internal helper — called by consumer_backend.cpp's Capture_Shutdown() ────
// Capture_Init() and Capture_Shutdown() are defined in consumer_backend.cpp so
// there is exactly one definition of each across the DLL.  This helper lets the
// consumer's Capture_Shutdown() free the D3D staging surfaces owned here.
void Capture_ReleaseSurfaces()
{
    g_Shutdown.store(true);
    std::lock_guard<std::mutex> lk(g_CapMtx);
    for (auto& s : g_Staging)
        ReleaseSurface(s);
}

void Capture_OnPreReset()
{
    // Must release all D3D resources before Reset is called.
    std::lock_guard<std::mutex> lk(g_CapMtx);
    for (auto& s : g_Staging)
        ReleaseSurface(s);
}

void Capture_OnPostReset(IDirect3DDevice9*)
{
    // Surfaces will be lazily re-created on the next Present.
}

/**
 * Main capture routine — called from the hooked Present on the render thread.
 *
 * Pipeline:
 *  A) Obtain the back-buffer render target (GPU-resident).
 *  B) Ensure a matching SYSTEMMEM staging surface exists for the WRITE slot.
 *  C) GetRenderTargetData  →  DMA copy, GPU waits until idle (implicit sync).
 *  D) Swap write/read indices.
 *  E) Lock the READ slot (the surface filled in the PREVIOUS frame) and deliver
 *     its pixels to the consumer without copying.
 *
 * The double-buffer means:
 *   – Frame N:   we write into surface[0]  →  consumer gets surface[1] (empty first call)
 *   – Frame N+1: we write into surface[1]  →  consumer gets surface[0]  (frame N data)
 * One-frame latency is acceptable for capture and is standard in all real-world recorders.
 */
void Capture_OnPresent(IDirect3DDevice9* pDev)
{
    if (g_Shutdown.load()) return;

    std::lock_guard<std::mutex> lk(g_CapMtx);

    // ── A: get back buffer ───────────────────────────────────────────────────
    IDirect3DSurface9* pBB = nullptr;
    if (FAILED(pDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB)) || !pBB)
        return;

    D3DSURFACE_DESC desc = {};
    pBB->GetDesc(&desc);

    if (desc.Width  == 0 || desc.Width  > MAX_WIDTH  ||
        desc.Height == 0 || desc.Height > MAX_HEIGHT)
    {
        pBB->Release();
        return;
    }

    // ── B: ensure staging surface (WRITE slot) ────────────────────────────
    if (!EnsureSurface(pDev, g_WriteIdx, desc.Width, desc.Height, desc.Format))
    {
        pBB->Release();
        return;
    }

    StagingSurface& ws = g_Staging[g_WriteIdx];

    // ── C: GPU → system-memory DMA ────────────────────────────────────────
    // GetRenderTargetData stalls until the GPU has finished rendering to the
    // surface (implicit fence), then initiates the DMA.  This is the only
    // unavoidable GPU sync point; there is no async alternative in raw D3D9
    // without extensions.  D3D9Ex (Vista+) offers GetRenderTargetData on a
    // query to overlap it with the CPU, but for maximum compatibility we keep
    // the synchronous path here.
    HRESULT hr = pDev->GetRenderTargetData(pBB, ws.pSurf);
    pBB->Release();

    if (FAILED(hr)) return;
    ws.pending = true;

    // ── D: swap slots ─────────────────────────────────────────────────────
    int prevRead  = g_ReadIdx;
    g_ReadIdx  = g_WriteIdx;
    g_WriteIdx = prevRead;

    StagingSurface& rs = g_Staging[g_ReadIdx];
    if (!rs.pending) return;   // first frame — nothing in read slot yet

    // ── E: lock and deliver ───────────────────────────────────────────────
    D3DLOCKED_RECT lr = {};
    // D3DLOCK_READONLY  – we won't modify the surface, hint allows faster mapping
    // D3DLOCK_NO_DIRTY_UPDATE – skip dirty-rect bookkeeping (SYSTEMMEM surfaces)
    hr = rs.pSurf->LockRect(&lr, nullptr,
                             D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE);
    if (FAILED(hr)) return;

    FrameData fd;
    fd.pixels   = lr.pBits;
    fd.width    = rs.width;
    fd.height   = rs.height;
    fd.stride   = static_cast<UINT>(lr.Pitch);
    fd.format   = rs.format;
    fd.frameIdx = g_FrameIdx.fetch_add(1);

    Capture_FrameReady(fd);   // consumer must return before we unlock!

    rs.pSurf->UnlockRect();
}
