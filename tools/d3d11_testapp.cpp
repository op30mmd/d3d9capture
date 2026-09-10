/*
 * d3d11_testapp.cpp — minimal Direct3D 11 / DXGI application used as a
 * verification target for the D3D11 capture path.
 *
 * Mirrors tools/d3d9_testapp.cpp: it resolves D3D11CreateDeviceAndSwapChain
 * through its own import table, creates a swap chain, then clears and Presents
 * in a loop, exercising the hook chain
 *   import slot -> D3D11CreateDeviceAndSwapChain -> IDXGISwapChain::Present
 *   and IDXGISwapChain::ResizeBuffers
 * without needing a real D3D11 title.
 *
 * Build (x64, to match D3D11 titles such as GTA V):
 *   cl /nologo /W3 /O2 /MD /Fe:d3d11_testapp.exe d3d11_testapp.cpp
 *      /link d3d11.lib dxgi.lib user32.lib
 *
 * Usage:
 *   d3d11_testapp.exe [frames] [msaa] [test-polls]
 *
 *   frames      real frames to present, default 600. Values >= 100 also drive a
 *               scripted sequence: overlay on/off, a recorder stop-then-restart
 *               in a single frame, and a ResizeBuffers.
 *   msaa        swap-chain sample count, default 1. Use 4 to exercise the
 *               ResolveSubresource path. The app prints the sample count the
 *               swap chain was ACTUALLY created with, so a silent downgrade
 *               cannot be mistaken for a passing MSAA test.
 *   test-polls  extra Present(0, DXGI_PRESENT_TEST) calls per real frame,
 *               default 0. This reproduces what a game does while occluded or
 *               minimised. Those calls present nothing, so the DLL must ignore
 *               them: with 900 frames and 50 polls the capture layer must
 *               report 900 presents, not 45,900.
 *
 * The frames it renders carry a white marker in the TOP-LEFT corner. A flat
 * colour cannot reveal a vertical flip, so checking that the marker is still
 * top-left in a captured BMP (or in a frame decoded back out of a recorded MP4
 * with tools/mp4_frame.cpp) is what proves the capture and encode orientation.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static ID3D11RenderTargetView* MakeRTV(ID3D11Device* dev, IDXGISwapChain* sc)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
        return nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    return rtv;
}

int main(int argc, char* argv[])
{
    const int totalFrames = (argc > 1) ? atoi(argv[1]) : 600;
    const UINT msaa = (argc > 2) ? (UINT)atoi(argv[2]) : 1;
    const UINT testPolls = (argc > 3) ? (UINT)atoi(argv[3]) : 0;
    unsigned long long realPresents = 0, testPresents = 0;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d11capture_testapp";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "d3d11capture test app",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("[testapp] CreateWindow failed: %lu\n", GetLastError()); return 1; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = msaa;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = {};
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &sd, &sc, &dev, &fl, &ctx);

    if (FAILED(hr))
    {
        printf("[testapp] HARDWARE device failed hr=0x%08lX, retrying WARP\n", hr);
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &sd, &sc, &dev, &fl, &ctx);
    }
    if (FAILED(hr) || !sc || !dev || !ctx)
    {
        printf("[testapp] D3D11CreateDeviceAndSwapChain failed hr=0x%08lX\n", hr);
        return 1;
    }
    printf("[testapp] device=%p swapchain=%p featureLevel=0x%04X\n",
        (void*)dev, (void*)sc, (unsigned)fl);
    fflush(stdout);

    {
        DXGI_SWAP_CHAIN_DESC got = {};
        sc->GetDesc(&got);
        printf("[testapp] ACTUAL swapchain desc: %ux%u fmt=%u SampleDesc.Count=%u Quality=%u\n",
            got.BufferDesc.Width, got.BufferDesc.Height, (unsigned)got.BufferDesc.Format,
            got.SampleDesc.Count, got.SampleDesc.Quality);
        if (msaa > 1 && got.SampleDesc.Count <= 1)
            printf("[testapp] *** WARNING: requested %ux MSAA but swap chain is single-sampled ***\n", msaa);
        fflush(stdout);
    }

    // ClearView (D3D11.1) lets us clear a sub-rect, which is how the D3D9 app
    // draws its orientation marker.
    ID3D11DeviceContext1* ctx1 = nullptr;
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1));
    printf("[testapp] ID3D11DeviceContext1 %s\n", ctx1 ? "available (marker enabled)" : "unavailable");

    ID3D11RenderTargetView* rtv = MakeRTV(dev, sc);
    if (!rtv) { printf("[testapp] CreateRenderTargetView failed\n"); return 1; }

    MSG msg = {};
    for (int frame = 0; frame < totalFrames; ++frame)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { frame = totalFrames; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (totalFrames >= 100)
        {
            if (frame == 15)
            {
                printf("[testapp] Frame 15: Toggling ImGui overlay ON (VK_INSERT)\n");
                SendMessageA(hwnd, WM_KEYDOWN, VK_INSERT, 0);
            }
            else if (frame == 25)
            {
                printf("[testapp] Frame 25: Toggling Video Recording ON (VK_F9)\n");
                SendMessageA(hwnd, WM_KEYDOWN, VK_F9, 0);
            }
            else if (frame == 50)
            {
                printf("[testapp] Frame 50: Simulating mouse interaction on overlay\n");
                SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(150, 100));
                SendMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(150, 100));
                SendMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(150, 100));
            }
            else if (frame == 76)
            {
                // Restart immediately after stopping. The async stop returns
                // before the previous encoder worker has drained/finalised, so
                // this lands inside that window.
                printf("[testapp] Frame 76: STOP then immediate START (two F9 edges, same frame)\n");
                SendMessageA(hwnd, WM_KEYDOWN, VK_F9, 0);
                SendMessageA(hwnd, WM_KEYDOWN, VK_F9, 0);
            }
            else if (frame == 120)
            {
                printf("[testapp] Frame 120: Toggling Video Recording OFF (VK_F9)\n");
                SendMessageA(hwnd, WM_KEYDOWN, VK_F9, 0);
            }
            else if (frame == 85)
            {
                printf("[testapp] Frame 85: Triggering ResizeBuffers(800x600)\n");
                if (rtv) { rtv->Release(); rtv = nullptr; }
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                HRESULT rhr = sc->ResizeBuffers(0, 800, 600, DXGI_FORMAT_UNKNOWN, 0);
                printf("[testapp] sc->ResizeBuffers returned hr=0x%08lX\n", rhr);
                rtv = MakeRTV(dev, sc);
                if (!rtv) { printf("[testapp] RTV re-create FAILED after resize\n"); break; }
            }
            else if (frame == 95)
            {
                printf("[testapp] Frame 95: Toggling ImGui overlay OFF (VK_INSERT)\n");
                SendMessageA(hwnd, WM_KEYDOWN, VK_INSERT, 0);
            }
        }

        // A changing colour makes captured frames easy to tell apart.
        const float bg[4] = { ((frame * 3) & 0xFF) / 255.0f, 64.0f / 255.0f, 128.0f / 255.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv, bg);

        // An asymmetric marker in the TOP-LEFT corner. A flat colour cannot
        // reveal a vertical flip, which is the classic way a capture or encode
        // path goes wrong while still looking plausible.
        if (ctx1)
        {
            const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            const D3D11_RECT marker = { 0, 0, 80, 40 };
            ctx1->ClearView(rtv, white, &marker, 1);
        }

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        sc->Present(0, 0);
        ++realPresents;

        // Emulate what a game does while its window is occluded/minimised:
        // poll Present(DXGI_PRESENT_TEST) to find out when it becomes visible
        // again. These do NOT present and are NOT frames.
        for (UINT t = 0; t < testPolls; ++t)
        {
            sc->Present(0, DXGI_PRESENT_TEST);
            ++testPresents;
        }

        Sleep(16);
    }

    printf("[testapp] done. REAL frames presented = %llu ; DXGI_PRESENT_TEST polls = %llu ; total Present() calls = %llu\n",
           realPresents, testPresents, realPresents + testPresents);
    fflush(stdout);
    if (rtv) rtv->Release();
    if (ctx1) ctx1->Release();
    ctx->Release();
    sc->Release();
    dev->Release();
    DestroyWindow(hwnd);
    return 0;
}
