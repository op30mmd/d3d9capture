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
#include <thread>
#include <condition_variable>

#include "capture.h"
#include "recorder.h"

// ── constants ─────────────────────────────────────────────────────────────────
static constexpr int  NUM_STAGING = 4;    // quad-buffer staging surfaces (3 frames of GPU DMA slack)
static constexpr UINT MAX_WIDTH   = 7680; // guard against absurd resolutions
static constexpr UINT MAX_HEIGHT  = 4320;

// ── D3D9 staging state ────────────────────────────────────────────────────────
// One slot is a three-stage pipeline:
//   back buffer --StretchRect--> pRT (default pool, GPU-side copy)
//               --GetRenderTargetData--> pSurf (system memory, the GPU->CPU DMA)
//               --LockRect, once pQuery says the DMA is done--> consumer
//
// Reading the back buffer with GetRenderTargetData directly, as this used to,
// stalls the render thread until the GPU has finished the whole frame: on GTA
// SA at 1920x1080 that was 3-17 ms per frame and cut the game from 60 to
// 37 fps while recording. The intermediate render target lets the copy and the
// DMA run behind the game's own work, and the event query lets a slot be
// locked only when its DMA has finished, so the lock never waits either.
enum class Stage9 { Free, Copied, Dma };  // see Capture_OnPresent

struct StagingSurface9
{
    IDirect3DSurface9* pRT     = nullptr;   // D3DPOOL_DEFAULT render target
    IDirect3DSurface9* pSurf   = nullptr;   // D3DPOOL_SYSTEMMEM offscreen plain
    IDirect3DQuery9*   pQuery  = nullptr;   // D3DQUERYTYPE_EVENT, issued after the DMA (may be null)
    UINT               width   = 0;
    UINT               height  = 0;
    D3DFORMAT          format  = D3DFMT_UNKNOWN;
    std::atomic<Stage9> stage{ Stage9::Free };
    UINT64             issuedAt = 0;        // present number the copy / DMA was issued on
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
static int                     g_WriteIdx9 = 0;   // next slot to capture into
static int                     g_ReadIdx9  = 0;   // oldest slot not yet delivered

// ── D3D9 readback worker ─────────────────────────────────────────────────────
// When the device was created with D3DCREATE_MULTITHREADED (the CreateDevice
// hook adds it), everything after the StretchRect runs on this thread: the
// DMA, the lock and the copy to the consumer. The render thread then pays
// ~0.01 ms per captured frame instead of ~4.5 ms, which is what the D3D11
// path gets for free from its GPU-writable staging textures.
//
// g_Worker9Mtx is held by the worker for as long as it owns a slot, and by
// reset / shutdown while they release the slots; the render thread never takes
// it (it only ever touches Free slots, which the worker never touches).
static std::thread              g_Worker9;
static std::atomic<bool>        g_Worker9Run{ false };
static std::mutex               g_Worker9Mtx;
static std::mutex               g_Worker9CvMtx;
static std::condition_variable  g_Worker9Cv;
static IDirect3DDevice9*        g_Worker9Device = nullptr;   // AddRef'd while the worker may use it
static std::atomic<bool>        g_Worker9Active{ false };    // the off-thread path is in use
static std::mutex               g_T9StatsMtx;                // worker-side stage timings vs. the heartbeat

static StagingTexture11        g_Staging11[NUM_STAGING];
static int                     g_WriteIdx11 = 0;

static std::atomic<UINT64>     g_FrameIdx{ 0 };
static std::atomic<UINT64>     g_PresentCalls{ 0 };
static std::atomic<bool>       g_Shutdown{ false };

static std::atomic<bool>       g_CaptureEnabled{ true };
static std::atomic<int>        g_TargetFps{ 0 }; // 0 = unlimited
static std::atomic<UINT>       g_LastWidth{ 0 };
static std::atomic<UINT>       g_LastHeight{ 0 };
static std::atomic<UINT32>     g_LastFormat{ 0 };
static std::atomic<float>      g_LastReadbackMs{ 0.0f };
static std::atomic<float>      g_LastMapMs{ 0.0f };      // CPU time blocked in Map()
static std::atomic<UINT64>     g_MapSkips{ 0 };          // frames skipped rather than stalling
static std::atomic<float>      g_LastHookMs{ 0.0f };     // total time stolen from the render thread
static std::atomic<float>      g_PresentFps{ 0.0f };
static std::atomic<float>      g_CaptureFps{ 0.0f };

// ── D3D9 render-thread cost accounting ───────────────────────────────────────
// Everything the hook does on the game's render thread, per stage, accumulated
// between heartbeats.  Only the render thread touches it.
struct StageTiming
{
    double sumMs = 0.0;
    double maxMs = 0.0;
    UINT64 count = 0;
    void Add(double ms) { sumMs += ms; if (ms > maxMs) maxMs = ms; ++count; }
    double Avg() const { return count ? sumMs / static_cast<double>(count) : 0.0; }
    void Reset() { sumMs = 0.0; maxMs = 0.0; count = 0; }
};
static StageTiming             g_T9Copy;       // StretchRect: back buffer -> default-pool copy
static StageTiming             g_T9Readback;   // GetRenderTargetData: the GPU->CPU DMA, one present later
static StageTiming             g_T9Lock;       // LockRect on the system-memory surface
static StageTiming             g_T9Deliver;    // Capture_FrameReady: shm publish + recorder queue
static StageTiming             g_T9Capture;    // Capture_OnPresent total (incl. the idle early-outs)
static StageTiming             g_T9Overlay;    // Overlay_OnPresent total
static StageTiming             g_T9Present;    // the game's own Present, for scale
static LARGE_INTEGER           g_T9LastHeartbeat = {};
static std::atomic<UINT64>     g_T9NotReady{ 0 };  // deliveries deferred because the DMA was still running
static std::atomic<UINT64>     g_T9Forced{ 0 };    // slots locked blind / DMA issued without the copy confirmed
static std::atomic<UINT64>     g_T9Full{ 0 };      // captures skipped because every slot was still in flight

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
    if (s.pQuery) { s.pQuery->Release(); s.pQuery = nullptr; }
    if (s.pSurf)  { s.pSurf->Release();  s.pSurf  = nullptr; }
    if (s.pRT)    { s.pRT->Release();    s.pRT    = nullptr; }
    s.width = s.height = 0;
    s.format  = D3DFMT_UNKNOWN;
    s.stage.store(Stage9::Free);
    s.issuedAt = 0;
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

    hr = pDev->CreateRenderTarget(w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &s.pRT, nullptr);
    if (FAILED(hr))
    {
        Log("[cap9] CreateRenderTarget failed: 0x%08X", hr);
        ReleaseSurface9(s);
        return false;
    }

    // Without an event query the slot is locked blind after NUM_STAGING
    // presents, which is still far better than reading the back buffer
    // synchronously; the driver just loses the chance to tell us earlier.
    if (FAILED(pDev->CreateQuery(D3DQUERYTYPE_EVENT, &s.pQuery)))
    {
        s.pQuery = nullptr;
        static bool warned = false;
        if (!warned) { Log("[cap9] D3DQUERYTYPE_EVENT not available; locking staging surfaces blind"); warned = true; }
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

// Take g_Worker9Mtx with a bound: at process exit the worker thread may already
// be dead while holding it, and blocking forever in DllMain is worse than
// leaking a few surfaces the process is about to drop anyway.
static bool AcquireWorker9(std::unique_lock<std::mutex>& lk, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        if (lk.try_lock()) return true;
        if (GetTickCount() - start >= timeoutMs) return false;
        Sleep(1);
    }
}

static void StopWorker9()
{
    if (!g_Worker9Run.exchange(false)) return;
    g_Worker9Cv.notify_all();
    if (g_Worker9.joinable()) g_Worker9.detach();   // never join from a DLL teardown path
}

void Capture_ReleaseSurfaces()
{
    g_Shutdown.store(true);
    StopWorker9();
    std::lock_guard<std::mutex> lk(g_CapMtx);
    std::unique_lock<std::mutex> wlk(g_Worker9Mtx, std::defer_lock);
    const bool ownWorker = AcquireWorker9(wlk, 500);
    if (ownWorker)
    {
        for (auto& s : g_Staging9)
            ReleaseSurface9(s);
        if (g_Worker9Device) { g_Worker9Device->Release(); g_Worker9Device = nullptr; }
    }
    else
        Log("[cap9] worker did not release its slot within 500 ms; leaving D3D9 surfaces to the process");
    g_WriteIdx9 = g_ReadIdx9 = 0;
    for (auto& s : g_Staging11)
        ReleaseStagingTexture11(s);
}

void Capture_OnPreReset()
{
    Log("[cap] Capture_OnPreReset");
    std::lock_guard<std::mutex> lk(g_CapMtx);
    {
        // The worker finishes the slot it is on (a few ms) and then the
        // default-pool copies can go, as Reset requires.
        std::lock_guard<std::mutex> wlk(g_Worker9Mtx);
        for (auto& s : g_Staging9)
            ReleaseSurface9(s);
        g_WriteIdx9 = g_ReadIdx9 = 0;
    }
    for (auto& s : g_Staging11)
        ReleaseStagingTexture11(s);
}

void Capture_OnPostReset(IDirect3DDevice9*)
{
}

void Capture_OnPostResetDXGI(IDXGISwapChain*)
{
}

// ── D3D9 pipeline helpers ─────────────────────────────────────────────────────
// Three presents, three stages, so the render thread never waits for the GPU:
//   present N   : StretchRect  back buffer -> slot.pRT      (GPU copy, queued)
//   present N+1 : GetRenderTargetData  pRT -> pSurf         (DMA; the copy has retired)
//   present N+2+: LockRect + deliver, once the event query says the DMA is done
// Measured on GTA SA (AMD R7 350): GetRenderTargetData is synchronous on this
// driver (~2.5 ms for 8 MB), so whichever thread calls it pays that. With a
// thread-safe device the worker does; otherwise the render thread does, one
// present after the copy so at least the copy itself has retired.

static void Deliver9(StagingSurface9& rs)
{
    LARGE_INTEGER lockStart = {}, lockEnd = {}, deliverEnd = {};
    QueryPerformanceCounter(&lockStart);
    D3DLOCKED_RECT lr = {};
    HRESULT hr = rs.pSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE);
    QueryPerformanceCounter(&lockEnd);
    const double lockMs = static_cast<double>(lockEnd.QuadPart - lockStart.QuadPart) * 1000.0 / static_cast<double>(g_QpcFreq.QuadPart);
    double deliverMs = 0.0;
    if (SUCCEEDED(hr))
    {
        FrameData fd;
        fd.pixels   = lr.pBits;
        fd.width    = rs.width;
        fd.height   = rs.height;
        fd.stride   = static_cast<UINT>(lr.Pitch);
        fd.format   = static_cast<UINT32>(rs.format);
        fd.frameIdx = g_FrameIdx.fetch_add(1);

        Capture_FrameReady(fd);
        QueryPerformanceCounter(&deliverEnd);
        deliverMs = static_cast<double>(deliverEnd.QuadPart - lockEnd.QuadPart) * 1000.0 / static_cast<double>(g_QpcFreq.QuadPart);

        rs.pSurf->UnlockRect();
    }
    // A failed lock (device lost) drops the frame rather than wedging the ring.
    {
        std::lock_guard<std::mutex> st(g_T9StatsMtx);
        g_T9Lock.Add(lockMs);
        if (SUCCEEDED(hr)) g_T9Deliver.Add(deliverMs);
    }
}

// GetRenderTargetData for a Copied slot; returns false if it failed (slot freed).
static bool Dma9(IDirect3DDevice9* pDev, StagingSurface9& ds, UINT64 presentNumber)
{
    LARGE_INTEGER dmaStart = {}, dmaEnd = {};
    QueryPerformanceCounter(&dmaStart);
    HRESULT hr = pDev->GetRenderTargetData(ds.pRT, ds.pSurf);
    if (SUCCEEDED(hr) && ds.pQuery)
        ds.pQuery->Issue(D3DISSUE_END);
    QueryPerformanceCounter(&dmaEnd);
    const double dmaMs = static_cast<double>(dmaEnd.QuadPart - dmaStart.QuadPart) * 1000.0 / static_cast<double>(g_QpcFreq.QuadPart);
    {
        std::lock_guard<std::mutex> st(g_T9StatsMtx);
        g_T9Readback.Add(dmaMs);
    }
    g_LastReadbackMs.store(static_cast<float>(dmaMs));
    if (FAILED(hr))
    {
        static bool logged = false;
        if (!logged) { Log("[cap9] GetRenderTargetData failed: 0x%08lX", hr); logged = true; }
        ds.stage.store(Stage9::Free);
        return false;
    }
    ds.issuedAt = presentNumber;
    ds.stage.store(Stage9::Dma);
    return true;
}

// StretchRect into the write slot. Common to both paths.
static void Copy9(IDirect3DDevice9* pDev, IDirect3DSurface9* pBB, const D3DSURFACE_DESC& desc, UINT64 presentNumber)
{
    StagingSurface9& ws = g_Staging9[g_WriteIdx9];
    if (ws.stage.load() != Stage9::Free)
    {
        // Every slot is still in flight: the consumer (or the GPU) is further
        // behind than NUM_STAGING frames. Skip this frame rather than wait.
        ++g_T9Full;
        pBB->Release();
        return;
    }

    // Only a Free slot may be (re)created: the worker never touches those.
    if (!EnsureSurface9(pDev, g_WriteIdx9, desc.Width, desc.Height, desc.Format))
    {
        pBB->Release();
        return;
    }

    LARGE_INTEGER cpStart = {}, cpEnd = {};
    QueryPerformanceCounter(&cpStart);
    // GPU-side copy; resolves MSAA if the back buffer has it. Queued behind
    // the game's frame, not waited for. The query lets the worker wait for
    // the copy to retire before it issues the (synchronous) DMA.
    HRESULT hr = pDev->StretchRect(pBB, nullptr, ws.pRT, nullptr, D3DTEXF_NONE);
    if (SUCCEEDED(hr) && ws.pQuery && g_Worker9Active.load())
        ws.pQuery->Issue(D3DISSUE_END);
    QueryPerformanceCounter(&cpEnd);
    pBB->Release();

    if (FAILED(hr))
    {
        static bool logged = false;
        if (!logged) { Log("[cap9] StretchRect failed: 0x%08lX", hr); logged = true; }
        return;
    }
    ws.issuedAt = presentNumber;
    ws.stage.store(Stage9::Copied);
    g_WriteIdx9 = (g_WriteIdx9 + 1) % NUM_STAGING;

    g_T9Copy.Add(static_cast<double>(cpEnd.QuadPart - cpStart.QuadPart) * 1000.0 / static_cast<double>(g_QpcFreq.QuadPart));
    g_LastWidth.store(desc.Width);
    g_LastHeight.store(desc.Height);
    Recorder_SetDefaultResolution(desc.Width, desc.Height);
    g_LastFormat.store(static_cast<UINT32>(desc.Format));
    g_LastCaptureTime = cpEnd;

    if (g_Worker9Active.load())
        g_Worker9Cv.notify_one();
}

// Everything on the render thread (device not thread-safe).
static void Present9_OnThread(IDirect3DDevice9* pDev, IDirect3DSurface9* pBB, const D3DSURFACE_DESC& desc, UINT64 presentNumber)
{
    // 1. Deliver the oldest slot whose DMA has finished. At most one per
    //    present. A slot that has had NUM_STAGING presents is locked blind.
    {
        StagingSurface9& rs = g_Staging9[g_ReadIdx9];
        bool ready = rs.stage.load() == Stage9::Dma && rs.pSurf;
        if (ready && rs.pQuery && rs.pQuery->GetData(nullptr, 0, 0) == S_FALSE)
        {
            if (presentNumber - rs.issuedAt >= static_cast<UINT64>(NUM_STAGING))
                ++g_T9Forced;
            else
            {
                ++g_T9NotReady;
                ready = false;
            }
        }
        if (ready)
        {
            Deliver9(rs);
            rs.stage.store(Stage9::Free);
            g_ReadIdx9 = (g_ReadIdx9 + 1) % NUM_STAGING;
        }
    }

    // 2. DMA for the slot copied on the previous present.
    {
        StagingSurface9& ds = g_Staging9[(g_WriteIdx9 + NUM_STAGING - 1) % NUM_STAGING];
        if (ds.stage.load() == Stage9::Copied)
            Dma9(pDev, ds, presentNumber);
    }

    // 3. Copy this frame.
    Copy9(pDev, pBB, desc, presentNumber);
}

// Render thread half of the off-thread path: just the copy.
static void Present9_OffThread(IDirect3DDevice9* pDev, IDirect3DSurface9* pBB, const D3DSURFACE_DESC& desc, UINT64 presentNumber)
{
    Copy9(pDev, pBB, desc, presentNumber);
}

// Worker half: for each Copied slot in order, wait for the copy to retire
// (polling the event query, so the runtime lock is held only for the poll),
// then DMA, lock and deliver. Holds g_Worker9Mtx for the whole slot.
static void Worker9Loop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Log("[cap9] readback worker started");
    while (g_Worker9Run.load())
    {
        {
            std::unique_lock<std::mutex> cv(g_Worker9CvMtx);
            g_Worker9Cv.wait_for(cv, std::chrono::milliseconds(4));
        }
        if (!g_Worker9Run.load()) break;

        for (;;)
        {
            std::lock_guard<std::mutex> lk(g_Worker9Mtx);
            if (!g_Worker9Run.load() || g_Shutdown.load()) break;
            StagingSurface9& rs = g_Staging9[g_ReadIdx9];
            if (rs.stage.load() != Stage9::Copied || !rs.pSurf || !g_Worker9Device) break;

            // Wait for the StretchRect to retire before the DMA, otherwise
            // GetRenderTargetData waits for it while holding the runtime lock
            // and the render thread's next D3D call waits behind that.
            if (rs.pQuery)
            {
                const DWORD start = GetTickCount();
                while (rs.pQuery->GetData(nullptr, 0, 0) == S_FALSE && g_Worker9Run.load())
                {
                    if (GetTickCount() - start > 250) { ++g_T9Forced; break; }
                    Sleep(1);
                }
            }
            if (!g_Worker9Run.load()) break;

            const UINT64 presentNumber = g_PresentCalls.load();
            if (!Dma9(g_Worker9Device, rs, presentNumber))
            {
                g_ReadIdx9 = (g_ReadIdx9 + 1) % NUM_STAGING;
                continue;
            }
            Deliver9(rs);
            rs.stage.store(Stage9::Free);
            g_ReadIdx9 = (g_ReadIdx9 + 1) % NUM_STAGING;
        }
    }
    Log("[cap9] readback worker stopped");
}

// Start the worker for a thread-safe device; returns whether the off-thread
// path is in use for pDev. Device creation flags are checked once per device.
static bool EnsureWorker9(IDirect3DDevice9* pDev)
{
    if (g_Worker9Active.load() && g_Worker9Device == pDev) return true;
    if (g_Worker9Device && g_Worker9Device != pDev)
    {
        // A different device: the old worker's device is gone. Stop and restart.
        StopWorker9();
        std::lock_guard<std::mutex> wlk(g_Worker9Mtx);
        g_Worker9Device->Release();
        g_Worker9Device = nullptr;
        g_Worker9Active.store(false);
    }
    if (g_Worker9Device == pDev) return false;   // checked before: not thread-safe

    D3DDEVICE_CREATION_PARAMETERS cp = {};
    const bool threadSafe = SUCCEEDED(pDev->GetCreationParameters(&cp)) &&
                            (cp.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    pDev->AddRef();
    g_Worker9Device = pDev;
    if (!threadSafe)
    {
        Log("[cap9] device %p lacks D3DCREATE_MULTITHREADED (flags=0x%08lX); readback stays on the render thread",
            pDev, cp.BehaviorFlags);
        return false;
    }
    g_Worker9Run.store(true);
    g_Worker9Active.store(true);
    g_Worker9 = std::thread(Worker9Loop);
    Log("[cap9] device %p is thread-safe (flags=0x%08lX); readback moved to a worker thread",
        pDev, cp.BehaviorFlags);
    return true;
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
    if (targetFps > 0 && !snapshotPending && !dumpPending && !Capture_IsConsumerActive() && !Recorder_IsRecording())
    {
        double minIntervalSec = 1.0 / static_cast<double>(targetFps);
        double timeSinceLastCap = static_cast<double>(nowQpc.QuadPart - g_LastCaptureTime.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
        if (timeSinceLastCap < (minIntervalSec * 0.70))
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

    if (!EnsureWorker9(pDev))
        Present9_OnThread(pDev, pBB, desc, presentNumber);
    else
        Present9_OffThread(pDev, pBB, desc, presentNumber);
}

void Capture_ReportPresentTiming9(double captureMs, double overlayMs, double presentMs)
{
    if (g_QpcFreq.QuadPart == 0) return;   // Capture_OnPresent has not run yet

    g_T9Capture.Add(captureMs);
    g_T9Overlay.Add(overlayMs);
    g_T9Present.Add(presentMs);

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_T9LastHeartbeat.QuadPart == 0) { g_T9LastHeartbeat = now; return; }
    const double sinceSec = static_cast<double>(now.QuadPart - g_T9LastHeartbeat.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
    if (sinceSec < 5.0) return;
    g_T9LastHeartbeat = now;

    std::lock_guard<std::mutex> st(g_T9StatsMtx);
    // hook = capture + overlay: the time this DLL adds to every Present.
    Log("[cap9] heartbeat(%s): presents=%llu captured=%llu %ux%u fps=%.1f/%.1f | per present avg/max ms: "
        "hook=%.3f/%.3f (capture=%.3f/%.3f overlay=%.3f/%.3f) game-present=%.3f/%.3f | "
        "per captured frame: copy=%.3f/%.3f %sdma=%.3f/%.3f lock=%.3f/%.3f deliver=%.3f/%.3f (n=%llu) "
        "dma-not-ready=%llu forced-locks=%llu slots-full=%llu",
        g_Worker9Active.load() ? "worker" : "on-thread",
        static_cast<unsigned long long>(g_PresentCalls.load()),
        static_cast<unsigned long long>(g_FrameIdx.load()),
        g_LastWidth.load(), g_LastHeight.load(),
        static_cast<double>(g_PresentFps.load()), static_cast<double>(g_CaptureFps.load()),
        g_T9Capture.Avg() + g_T9Overlay.Avg(), g_T9Capture.maxMs + g_T9Overlay.maxMs,
        g_T9Capture.Avg(), g_T9Capture.maxMs,
        g_T9Overlay.Avg(), g_T9Overlay.maxMs,
        g_T9Present.Avg(), g_T9Present.maxMs,
        g_T9Copy.Avg(), g_T9Copy.maxMs,
        g_Worker9Active.load() ? "[worker] " : "",
        g_T9Readback.Avg(), g_T9Readback.maxMs,
        g_T9Lock.Avg(), g_T9Lock.maxMs,
        g_T9Deliver.Avg(), g_T9Deliver.maxMs,
        static_cast<unsigned long long>(g_T9Readback.count),
        static_cast<unsigned long long>(g_T9NotReady.load()),
        static_cast<unsigned long long>(g_T9Forced.load()),
        static_cast<unsigned long long>(g_T9Full.load()));

    g_T9Copy.Reset(); g_T9Readback.Reset(); g_T9Lock.Reset(); g_T9Deliver.Reset();
    g_T9Capture.Reset();  g_T9Overlay.Reset(); g_T9Present.Reset();
    g_T9NotReady.store(0); g_T9Forced.store(0); g_T9Full.store(0);
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
    if (targetFps > 0 && !snapshotPending && !dumpPending && !Capture_IsConsumerActive() && !Recorder_IsRecording())
    {
        double minIntervalSec = 1.0 / static_cast<double>(targetFps);
        double timeSinceLastCap = static_cast<double>(nowQpc.QuadPart - g_LastCaptureTime.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
        if (timeSinceLastCap < (minIntervalSec * 0.70))
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

    LARGE_INTEGER hookStart = {};
    QueryPerformanceCounter(&hookStart);

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
    Recorder_SetDefaultResolution(desc.Width, desc.Height);
    g_LastFormat.store(static_cast<UINT32>(desc.Format));
    g_LastCaptureTime = rbEnd;

    // With NUM_STAGING = 3, read the oldest texture queued 2 frames ago: (g_WriteIdx11 + 1) % NUM_STAGING.
    // This gives a full 2 frames of GPU execution time for CopyResource to finish,
    // completely eliminating DXGI_ERROR_WAS_STILL_DRAWING skips and CPU stalls.
    const int readIdx = (g_WriteIdx11 + 1) % NUM_STAGING;

    StagingTexture11& rs = g_Staging11[readIdx];
    if (rs.pending && rs.pTex)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        LARGE_INTEGER mapStart = {}, mapEnd = {};
        QueryPerformanceCounter(&mapStart);
        hr = pCtx->Map(rs.pTex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            // If still drawing after 3 frames of GPU pipeline slack, wait for completion
            // via blocking Map so we never drop a frame or cause video stutter.
            hr = pCtx->Map(rs.pTex, 0, D3D11_MAP_READ, 0, &mapped);
        }
        QueryPerformanceCounter(&mapEnd);
        g_LastMapMs.store(static_cast<float>(
            static_cast<double>(mapEnd.QuadPart - mapStart.QuadPart) * 1000.0 /
            static_cast<double>(g_QpcFreq.QuadPart)));
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
            rs.pending = false;   // consumed; do not deliver this frame twice

            LARGE_INTEGER hookEnd = {};
            QueryPerformanceCounter(&hookEnd);
            g_LastHookMs.store(static_cast<float>(
                static_cast<double>(hookEnd.QuadPart - hookStart.QuadPart) * 1000.0 /
                static_cast<double>(g_QpcFreq.QuadPart)));

            if ((presentNumber % 300) == 0)
                Log("[cap11] heartbeat: presents=%llu captured=%llu %ux%u stride=%u fmt=%u copy=%.2fms map=%.2fms hook=%.2fms skips=%llu",
                    static_cast<unsigned long long>(presentNumber),
                    static_cast<unsigned long long>(fd.frameIdx + 1), fd.width, fd.height,
                    fd.stride, static_cast<unsigned>(fd.format),
                    static_cast<double>(g_LastReadbackMs.load()),
                    static_cast<double>(g_LastMapMs.load()),
                    static_cast<double>(g_LastHookMs.load()),
                    static_cast<unsigned long long>(g_MapSkips.load()));
        }
        else if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            g_MapSkips.fetch_add(1);
        }
    }

    g_WriteIdx11 = (g_WriteIdx11 + 1) % NUM_STAGING;

    pCtx->Release();
    pDev->Release();
}
