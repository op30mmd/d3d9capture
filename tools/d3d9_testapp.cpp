/*
 * d3d9_testapp.cpp — minimal Direct3D 9 application used as a verification
 * target for d3d9capture.
 *
 * It does exactly what a game does at startup and nothing else: resolve
 * Direct3DCreate9 through its own import table, create a device, then clear and
 * Present in a loop. That makes it a safe stand-in for testing the hook chain
 * (import slot -> CreateDevice -> Present/Reset) without needing to attach to a
 * real game.
 *
 * Build (x86, to match most D3D9 titles):
 *   cl /nologo /W3 /O2 /MD /Fe:d3d9_testapp.exe d3d9_testapp.cpp
 *      /link d3d9.lib user32.lib
 *
 * Usage:
 *   d3d9_testapp.exe [frames]     default 600 frames, then exits.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int main(int argc, char* argv[])
{
    const int totalFrames = (argc > 1) ? atoi(argv[1]) : 600;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d9capture_testapp";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "d3d9capture test app",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("[testapp] CreateWindow failed: %lu\n", GetLastError()); return 1; }

    // Called through the IAT, exactly like a game — this is the slot
    // d3d9capture patches.
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("[testapp] Direct3DCreate9 failed\n"); return 1; }
    printf("[testapp] factory=%p\n", (void*)d3d);

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev)
    {
        printf("[testapp] CreateDevice failed hr=0x%08lX\n", hr);
        d3d->Release();
        return 1;
    }
    printf("[testapp] device=%p\n", (void*)dev);
    fflush(stdout);

    MSG msg = {};
    for (int frame = 0; frame < totalFrames; ++frame)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { frame = totalFrames; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // A changing colour makes captured frames easy to tell apart.
        const D3DCOLOR colour = D3DCOLOR_XRGB((frame * 3) & 0xFF, 64, 128);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, colour, 1.0f, 0);

        // An asymmetric marker in the TOP-LEFT corner. A flat colour cannot
        // reveal a vertical flip, which is the classic way a capture or encode
        // path goes wrong while still looking plausible.
        const D3DRECT marker = { 0, 0, 80, 40 };
        dev->Clear(1, &marker, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 1.0f, 0);
        dev->BeginScene();
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        Sleep(16);
    }

    printf("[testapp] done, %d frames presented\n", totalFrames);
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
