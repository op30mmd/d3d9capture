/**
 * capture.cpp  —  Efficient D3D9 & D3D11/DXGI GPU frame readback
 *
 * Design goals
 * ─────────────
 * 1. ZERO extra copy on the CPU path.
 *    Direct GPU->CPU DMA into a SYSTEMMEM surface (D3D9) or staging texture (D3D11).
 *    We lock/map that surface and hand the pointer straight to the consumer.
 *
 * 2. Double-buffered CPU-accessible surfaces for asynchronous readback.
 *    While the consumer processes frame N the GPU is already staging frame N+1
 *    into the other surface.  This hides the DMA latency from the render thread.
 *
 * 3. Graceful Reset/ResizeBuffers handling.
 *    All offscreen surfaces/textures are released before Reset / ResizeBuffers and
 *    re-created lazily afterwards.
 *
 * 4. Format portability.
 *    We accept whatever back-buffer format the game chose and report it to the consumer unmodified.
 *
 * Thread safety
 * ─────────────
 * Capture_OnPresent / Capture_OnPresentDXGI are called on the game's render thread.
 * Capture_Shutdown may be called from a different thread (DLL_PROCESS_DETACH).
 * The mutex g_CapMtx serialises access to the shared surfaces.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <mutex>
#include <atomic>

#include "capture.h"

// ── constants ─────────────────────────────────────────────────────────────────
static constexpr int  NUM_STAGING = 2;    // double-buffer staging surfaces
static constexpr UINT MAX_WIDTH   = 7680; // guard against absurd resolutions
static constexpr UINT MAX_HEIGHT  = 4320;

// ── D3D9 staging state ────────────────────────────────────────────────────────
struct StagingSurface9
{
    IDirect3DSurface9* pSurf   = nullptr;
    UINT               width   = 0;
    UINT               height  = 0;
    D3DFORMAT          format  = D3DFMT_UNKNOWN;
    bool               pending = false; // DMA in-flight
};

// ── D3D11 staging state ───────────────────────────────────────────────────────
struct StagingTexture11
{
    ID3D11Texture2D* pTex    = nullptr;
    UINT             width   = 0;
    UINT             height  = 0;
    DXGI_FORMAT      format  = DXGI_FORMAT_UNKNOWN;
    bool             pending = false;
};

// ── module state ──────────────────────────────────────────────────────────────
static std::mutex              g_CapMtx;
static StagingSurface9         g_Staging9[NUM_STAGING];
static int                     g_WriteIdx9 = 0;
static int                     g_ReadIdx9  = 1;

static StagingTexture11        g_Staging11[NUM_STAGING];
static int                     g_WriteIdx11 = 0;
static int                     g_ReadIdx11  = 1;

static std::atomic<UINT64>     g_FrameIdx{ 0 };
static std::atomic<UINT64>     g_PresentCalls{ 0 };
static std::atomic<bool>       g_Shutdown{ false };

static std::atomic<bool>       g_CaptureEnabled{ true };
static std::atomic<int>        g_TargetFps{ 0 }; // 0 = unlimited
static std::atomic<UINT>       g_LastWidth{ 0 };
static std::atomic<UINT>       g_LastHeight{ 0 };
static std::atomic<UINT32>     g_LastFormat{ 0 };
static std::atomic<float>      g_LastReadbackMs{ 0.0f };
static std::atomic<float>      g_PresentFps{ 0.0f };
static std::atomic<float>      g_CaptureFps{ 0.0f };

static LARGE_INTEGER           g_QpcFreq = {};
static LARGE_INTEGER           g_LastFpsCalc = {};
static UINT64                  g_LastFpsPresentCount = 0;
static UINT64                  g_LastFpsCaptureCount = 0;
static LARGE_INTEGER           g_LastCaptureTime = {};

void Capture_SetEnabled(bool enabled)
{
    g_CaptureEnabled.store(enabled);
}

bool Capture_IsEnabled()
{
    return g_CaptureEnabled.load();
}

void Capture_SetTargetFps(int targetFps)
{
    g_TargetFps.store(targetFps);
}

int Capture_GetTargetFps()
{
    return g_TargetFps.load();
}

void Capture_GetStats(CaptureStats* pStats)
{
    if (!pStats) return;
    pStats->presentCalls        = g_PresentCalls.load();
    pStats->capturedFrames      = g_FrameIdx.load();
    pStats->width               = g_LastWidth.load();
    pStats->height              = g_LastHeight.load();
    pStats->format              = g_LastFormat.load();
    pStats->presentFps          = g_PresentFps.load();
    pStats->captureFps          = g_CaptureFps.load();
    pStats->readbackMs          = g_LastReadbackMs.load();
    pStats->isConsumerActive    = Capture_IsConsumerActive();
    pStats->captureEnabled      = g_CaptureEnabled.load();
    pStats->targetFps           = g_TargetFps.load();
    pStats->dumpQuotaRemaining  = Capture_GetDumpQuota();
    Capture_GetLastScreenshotPath(pStats->lastScreenshotPath, sizeof(pStats->lastScreenshotPath));
}

// ── helpers ───────────────────────────────────────────────────────────────────
static void ReleaseSurface9(StagingSurface9& s)
{
    if (s.pSurf) { s.pSurf->Release(); s.pSurf = nullptr; }
    s.width = s.height = 0;
    s.format  = D3DFMT_UNKNOWN;
    s.pending = false;
}

static void ReleaseStagingTexture11(StagingTexture11& s)
{
    if (s.pTex) { s.pTex->Release(); s.pTex = nullptr; }
    s.width = s.height = 0;
    s.format = DXGI_FORMAT_UNKNOWN;
    s.pending = false;
}

static bool EnsureSurface9(IDirect3DDevice9* pDev, int idx,
                           UINT w, UINT h, D3DFORMAT fmt)
{
    StagingSurface9& s = g_Staging9[idx];
    if (s.pSurf && s.width == w && s.height == h && s.format == fmt)
        return true;

    ReleaseSurface9(s);

    Log("[cap9] Creating staging surface %dx%d fmt=%u ...", w, h, fmt);
    HRESULT hr = pDev->CreateOffscreenPlainSurface(
        w, h, fmt, D3DPOOL_SYSTEMMEM, &s.pSurf, nullptr);

    if (FAILED(hr))
    {
        Log("[cap9] CreateOffscreenPlainSurface failed: 0x%08X. Trying fallback X8R8G8B8...", hr);
        if (fmt != D3DFMT_X8R8G8B8)
        {
            hr = pDev->CreateOffscreenPlainSurface(
                w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &s.pSurf, nullptr);
            if (SUCCEEDED(hr))
            {
                Log("[cap9] Fallback successful");
                fmt = D3DFMT_X8R8G8B8;
            }
        }
        if (FAILED(hr))
        {
            Log("[cap9] Fallback FAILED: 0x%08X", hr);
            return false;
        }
    }

    s.width  = w;
    s.height = h;
    s.format = fmt;
    return true;
}

static bool EnsureStagingTexture11(ID3D11Device* pDev, int idx, UINT w, UINT h, DXGI_FORMAT fmt)
{
    StagingTexture11& s = g_Staging11[idx];
    if (s.pTex && s.width == w && s.height == h && s.format == fmt)
        return true;

    ReleaseStagingTexture11(s);

    Log("[cap11] Creating D3D11 staging texture %dx%d fmt=%u ...", w, h, static_cast<unsigned>(fmt));
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = pDev->CreateTexture2D(&desc, nullptr, &s.pTex);
    if (FAILED(hr))
    {
        Log("[cap11] CreateTexture2D staging failed: 0x%08X", hr);
        return false;
    }

    s.width = w;
    s.height = h;
    s.format = fmt;
    return true;
}

void Capture_ReleaseSurfaces()
{
    g_Shutdown.store(true);
    std::lock_guard<std::mutex> lk(g_CapMtx);
    for (auto& s : g_Staging9)
        ReleaseSurface9(s);
    for (auto& s : g_Staging11)
        ReleaseStagingTexture11(s);
}

void Capture_OnPreReset()
{
    Log("[cap] Capture_OnPreReset");
    std::lock_guard<std::mutex> lk(g_CapMtx);
    for (auto& s : g_Staging9)
        ReleaseSurface9(s);
    for (auto& s : g_Staging11)
        ReleaseStagingTexture11(s);
}

void Capture_OnPostReset(IDirect3DDevice9*)
{
}

void Capture_OnPostResetDXGI(IDXGISwapChain*)
{
}

// ── D3D9 Capture Routine ──────────────────────────────────────────────────────
void Capture_OnPresent(IDirect3DDevice9* pDev)
{
    if (g_Shutdown.load()) return;

    const UINT64 presentNumber = g_PresentCalls.fetch_add(1) + 1;
    static bool logged = false;
    if (!logged) { Log("[cap9] Capture_OnPresent called (first time), device=%p", pDev); logged = true; }

    if (g_QpcFreq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&g_QpcFreq);
        QueryPerformanceCounter(&g_LastFpsCalc);
        g_LastCaptureTime = g_LastFpsCalc;
    }

    LARGE_INTEGER nowQpc = {};
    QueryPerformanceCounter(&nowQpc);
    double elapsedSec = static_cast<double>(nowQpc.QuadPart - g_LastFpsCalc.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
    if (elapsedSec >= 0.5)
    {
        UINT64 curPresents = presentNumber;
        UINT64 curCaptures = g_FrameIdx.load();
        g_PresentFps.store(static_cast<float>((curPresents - g_LastFpsPresentCount) / elapsedSec));
        g_CaptureFps.store(static_cast<float>((curCaptures - g_LastFpsCaptureCount) / elapsedSec));
        g_LastFpsPresentCount = curPresents;
        g_LastFpsCaptureCount = curCaptures;
        g_LastFpsCalc = nowQpc;
    }

    bool snapshotPending = Capture_IsSnapshotPending();
    bool dumpPending = (Capture_GetDumpQuota() > 0);

    if (!g_CaptureEnabled.load() && !snapshotPending && !dumpPending)
        return;

    int targetFps = g_TargetFps.load();
    if (targetFps > 0 && !snapshotPending && !dumpPending)
    {
        double minIntervalSec = 1.0 / static_cast<double>(targetFps);
        double timeSinceLastCap = static_cast<double>(nowQpc.QuadPart - g_LastCaptureTime.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
        if (timeSinceLastCap < minIntervalSec)
            return;
    }

    if (!Capture_WantsFrame())
    {
        if ((presentNumber % 1800) == 0)
            Log("[cap9] idle: presents=%llu, no consumer attached (readback skipped)",
                static_cast<unsigned long long>(presentNumber));
        return;
    }

    std::lock_guard<std::mutex> lk(g_CapMtx);

    IDirect3DSurface9* pBB = nullptr;
    if (FAILED(pDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB)) || !pBB)
        return;

    D3DSURFACE_DESC desc = {};
    HRESULT descHr = pBB->GetDesc(&desc);
    if (FAILED(descHr))
    {
        pBB->Release();
        return;
    }

    if (desc.Width == 0 || desc.Width > MAX_WIDTH ||
        desc.Height == 0 || desc.Height > MAX_HEIGHT)
    {
        pBB->Release();
        return;
    }

    if (!EnsureSurface9(pDev, g_WriteIdx9, desc.Width, desc.Height, desc.Format))
    {
        pBB->Release();
        return;
    }

    StagingSurface9& ws = g_Staging9[g_WriteIdx9];

    LARGE_INTEGER rbStart = {}, rbEnd = {};
    QueryPerformanceCounter(&rbStart);
    HRESULT hr = pDev->GetRenderTargetData(pBB, ws.pSurf);
    QueryPerformanceCounter(&rbEnd);
    pBB->Release();

    if (FAILED(hr)) return;
    ws.pending = true;

    double rbMs = static_cast<double>(rbEnd.QuadPart - rbStart.QuadPart) * 1000.0 / static_cast<double>(g_QpcFreq.QuadPart);
    g_LastReadbackMs.store(static_cast<float>(rbMs));
    g_LastWidth.store(desc.Width);
    g_LastHeight.store(desc.Height);
    g_LastFormat.store(static_cast<UINT32>(desc.Format));
    g_LastCaptureTime = rbEnd;

    int prevRead = g_ReadIdx9;
    g_ReadIdx9 = g_WriteIdx9;
    g_WriteIdx9 = prevRead;

    StagingSurface9& rs = g_Staging9[g_ReadIdx9];
    if (!rs.pending) return;

    D3DLOCKED_RECT lr = {};
    hr = rs.pSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE);
    if (FAILED(hr)) return;

    FrameData fd;
    fd.pixels   = lr.pBits;
    fd.width    = rs.width;
    fd.height   = rs.height;
    fd.stride   = static_cast<UINT>(lr.Pitch);
    fd.format   = static_cast<UINT32>(rs.format);
    fd.frameIdx = g_FrameIdx.fetch_add(1);

    Capture_FrameReady(fd);

    rs.pSurf->UnlockRect();
}

// ── D3D11 / DXGI Capture Routine (GTA V) ──────────────────────────────────────
void Capture_OnPresentDXGI(IDXGISwapChain* pSwapChain)
{
    if (g_Shutdown.load() || !pSwapChain) return;

    const UINT64 presentNumber = g_PresentCalls.fetch_add(1) + 1;
    static bool logged = false;
    if (!logged) { Log("[cap11] Capture_OnPresentDXGI called (first time), swapChain=%p", pSwapChain); logged = true; }

    if (g_QpcFreq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&g_QpcFreq);
        QueryPerformanceCounter(&g_LastFpsCalc);
        g_LastCaptureTime = g_LastFpsCalc;
    }

    LARGE_INTEGER nowQpc = {};
    QueryPerformanceCounter(&nowQpc);
    double elapsedSec = static_cast<double>(nowQpc.QuadPart - g_LastFpsCalc.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
    if (elapsedSec >= 0.5)
    {
        UINT64 curPresents = presentNumber;
        UINT64 curCaptures = g_FrameIdx.load();
        g_PresentFps.store(static_cast<float>((curPresents - g_LastFpsPresentCount) / elapsedSec));
        g_CaptureFps.store(static_cast<float>((curCaptures - g_LastFpsCaptureCount) / elapsedSec));
        g_LastFpsPresentCount = curPresents;
        g_LastFpsCaptureCount = curCaptures;
        g_LastFpsCalc = nowQpc;
    }

    bool snapshotPending = Capture_IsSnapshotPending();
    bool dumpPending = (Capture_GetDumpQuota() > 0);

    if (!g_CaptureEnabled.load() && !snapshotPending && !dumpPending)
        return;

    int targetFps = g_TargetFps.load();
    if (targetFps > 0 && !snapshotPending && !dumpPending)
    {
        double minIntervalSec = 1.0 / static_cast<double>(targetFps);
        double timeSinceLastCap = static_cast<double>(nowQpc.QuadPart - g_LastCaptureTime.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
        if (timeSinceLastCap < minIntervalSec)
            return;
    }

    if (!Capture_WantsFrame())
    {
        if ((presentNumber % 1800) == 0)
            Log("[cap11] idle: presents=%llu, no consumer attached (readback skipped)",
                static_cast<unsigned long long>(presentNumber));
        return;
    }

    std::lock_guard<std::mutex> lk(g_CapMtx);

    ID3D11Device* pDev = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pDev))) || !pDev)
        return;

    ID3D11DeviceContext* pCtx = nullptr;
    pDev->GetImmediateContext(&pCtx);
    if (!pCtx)
    {
        pDev->Release();
        return;
    }

    ID3D11Texture2D* pBB = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBB));
    if (FAILED(hr) || !pBB)
    {
        pCtx->Release();
        pDev->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    pBB->GetDesc(&desc);

    if (desc.Width == 0 || desc.Width > MAX_WIDTH ||
        desc.Height == 0 || desc.Height > MAX_HEIGHT)
    {
        pBB->Release();
        pCtx->Release();
        pDev->Release();
        return;
    }

    ID3D11Texture2D* pSourceTex = pBB;
    ID3D11Texture2D* pResolveTex = nullptr;

    if (desc.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC resolveDesc = desc;
        resolveDesc.SampleDesc.Count = 1;
        resolveDesc.SampleDesc.Quality = 0;
        resolveDesc.Usage = D3D11_USAGE_DEFAULT;
        resolveDesc.BindFlags = 0;
        resolveDesc.CPUAccessFlags = 0;

        if (SUCCEEDED(pDev->CreateTexture2D(&resolveDesc, nullptr, &pResolveTex)) && pResolveTex)
        {
            pCtx->ResolveSubresource(pResolveTex, 0, pBB, 0, desc.Format);
            pSourceTex = pResolveTex;
        }
    }

    if (!EnsureStagingTexture11(pDev, g_WriteIdx11, desc.Width, desc.Height, desc.Format))
    {
        if (pResolveTex) pResolveTex->Release();
        pBB->Release();
        pCtx->Release();
        pDev->Release();
        return;
    }

    StagingTexture11& ws = g_Staging11[g_WriteIdx11];

    LARGE_INTEGER rbStart = {}, rbEnd = {};
    QueryPerformanceCounter(&rbStart);
    pCtx->CopyResource(ws.pTex, pSourceTex);
    QueryPerformanceCounter(&rbEnd);

    if (pResolveTex) pResolveTex->Release();
    pBB->Release();

    ws.pending = true;

    double rbMs = static_cast<double>(rbEnd.QuadPart - rbStart.QuadPart) * 1000.0 / static_cast<double>(g_QpcFreq.QuadPart);
    g_LastReadbackMs.store(static_cast<float>(rbMs));
    g_LastWidth.store(desc.Width);
    g_LastHeight.store(desc.Height);
    g_LastFormat.store(static_cast<UINT32>(desc.Format));
    g_LastCaptureTime = rbEnd;

    int prevRead = g_ReadIdx11;
    g_ReadIdx11 = g_WriteIdx11;
    g_WriteIdx11 = prevRead;

    StagingTexture11& rs = g_Staging11[g_ReadIdx11];
    if (rs.pending && rs.pTex)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = pCtx->Map(rs.pTex, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            FrameData fd;
            fd.pixels   = mapped.pData;
            fd.width    = rs.width;
            fd.height   = rs.height;
            fd.stride   = mapped.RowPitch;
            fd.format   = static_cast<UINT32>(rs.format);
            fd.frameIdx = g_FrameIdx.fetch_add(1);

            Capture_FrameReady(fd);

            pCtx->Unmap(rs.pTex, 0);

            if ((presentNumber % 300) == 0)
                Log("[cap11] heartbeat: presents=%llu captured=%llu %ux%u stride=%u fmt=%u rb=%.2fms",
                    static_cast<unsigned long long>(presentNumber),
                    static_cast<unsigned long long>(fd.frameIdx + 1), fd.width, fd.height,
                    fd.stride, static_cast<unsigned>(fd.format), static_cast<double>(g_LastReadbackMs.load()));
        }
    }

    pCtx->Release();
    pDev->Release();
}
