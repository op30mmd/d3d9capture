# d3d9capture — Direct3D 9 & Direct3D 11 Frame Capture via DLL Injection

Efficient, production-quality GPU frame capture for D3D9 and D3D11 games (including GTA IV and GTA V).
No screen-scraping, no GDI, no Desktop Duplication API — pixels are pulled
directly from the GPU's back-buffer immediately after the game draws them.

Frames can be consumed two ways, and both work at once:

- **Out of process** — the DLL publishes frames to shared memory and
  `shm_reader.exe` encodes or saves them.
- **In process** — an ImGui overlay (**Insert**) drives capture from inside the
  game, and a built-in Media Foundation encoder records H.264 MP4 (**F9**)
  without a reader attached.

---

## Architecture

```
+-----------------------------------------------------------+
|                  Game Process                             |
|                                                           |
|   D3D9 / D3D11 Device        Swap Chain                   |
|   +-------------------+      +---------------------+      |
|   | CreateDevice      |      | Present()           |      |
|   | CreateSwapChain   |      | ResizeBuffers()     |      |
|   +---------+---------+      +----------+----------+      |
|             |                           |                 |
|             +-------------+-------------+                 |
|                           v                               |
|        +--------------------------------------+           |
|        | Hooked_Present / DXGI Present        |           |
|        |                                      |           |
|        | Capture_WantsFrame()                 | GPU->CPU  |
|        |   ...if nobody is consuming, stop    | --------> |
|        |          |                           | DMA Copy  |
|        | GetRenderTargetData / CopyResource   |           |
|        | LockRect / Map                       |           |
|        | FrameReady()                         |           |
|        +------------------+-------------------+           |
|                           |                               |
|           +---------------+---------------+               |
|           |                               |               |
|     +-----v-----------+             +-----v----------+    |
|     | Shared memory   |             | recorder.cpp   |    |
|     | ShmHeader +     |             | ring queue     |    |
|     | pixel data      |             | -> MF thread   |    |
|     | (+ QPC stamp)   |             | -> H.264 MP4   |    |
|     +-----+-----------+             +----------------+    |
|           |                                               |
|  ImGui overlay (Insert) draws inside the same Present     |
|  hook and drives capture, recording and screenshots.      |
+-----------------------------------------------------------+
            |  SetEvent(EvtReady)
            v
+------------------------------+
|       shm_reader.exe         |
|                              |
|   --record -> H.264 MP4 via  |
|      Media Foundation        |
|   (or your own encoder)      |
|                              |
|   WaitForSingleObject(Ready) |
|   -> encode / save frame     |
|   -> pace to --fps           |
|   -> SetEvent(Done)          |
+------------------------------+

The reader's SetEvent(Done) is what asks for the next frame, so the
consumer -- not the render thread -- sets the capture rate.
```

---

## Files

| File | Purpose |
|---|---|
| `dllmain.cpp` | DLL entry point; patches D3D9 factory, D3D11 device, and DXGI swap chain imports (`Present` / `Reset` / `ResizeBuffers`) |
| `capture.h/cpp` | Double-buffered GPU readback via `GetRenderTargetData` (D3D9) and `CopyResource` (D3D11/DXGI) |
| `consumer_backend.cpp` | Writes frames to named shared memory; screenshots and BMP debug dumps |
| `overlay.h/cpp` | In-game ImGui control centre drawn inside the `Present` hook (D3D9 & D3D11 backends) |
| `recorder.h/cpp` | In-process H.264/MP4 encoder: bounded ring queue feeding a Media Foundation `IMFSinkWriter` on a worker thread |
| `imgui_impl_dx9_patched.cpp` | Local fork of ImGui's DX9 backend compiled **instead of** the submodule's copy |
| `inject_tool.cpp` | `CreateRemoteThread` injector; accepts PID or process name |
| `shm_reader.cpp` | Out-of-process frame consumer; records H.264 MP4 via Media Foundation |
| `imgui/` | Dear ImGui, as a git submodule |
| `build.bat` | MSVC build script for 32-bit and 64-bit targets |
| `tools/d3d9_testapp.cpp` | Minimal D3D9 app used as a verification target |
| `tools/mp4_frame.cpp` | Decodes frames from a recorded MP4 back to BMP, to verify output |
| `tools/frida/` | Scripts that verify the hooks inside a live process |

---

## Build Instructions

### Prerequisites
- Visual Studio 2017 or later (Community edition is fine)
- Windows SDK 8.1+ (for `d3d9.h`, `d3d11.h`, `dxgi.h`, and Media Foundation libraries)
- **32-bit toolchain** (`vcvars32.bat`) for 32-bit games (e.g. GTA IV / D3D9)
- **64-bit toolchain** (`vcvars64.bat`) for 64-bit games (e.g. GTA V / D3D11)
- The Dear ImGui submodule:

```bat
git clone --recursive https://github.com/op30mmd/d3d9capture
:: ...or, in an existing clone:
git submodule update --init --recursive
```

### Steps

For **64-bit games (e.g. GTA V)**:
```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd d3d9capture\src
build.bat
```

For **32-bit games (e.g. GTA IV)**:
```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
cd d3d9capture\src
build.bat
```

Outputs land in `d3d9capture\bin\`.

---

## Usage

```bat
:: Terminal 1 — start the frame reader first
shm_reader.exe

:: ...or record straight to an H.264 MP4
shm_reader.exe --record out.mp4 --fps 30 --bitrate 12000

:: Terminal 2 — recommended: launch suspended, inject, then resume automatically
:: For 64-bit Direct3D 11 games (e.g. GTA V):
inject_tool.exe --launch "C:\Program Files\Rockstar Games\Grand Theft Auto V\GTA5.exe" C:\path\to\d3d9capture.dll

:: For 32-bit Direct3D 9 games (e.g. GTA IV):
inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe C:\path\to\d3d9capture.dll

:: `--wait` blocks until the game exits and reports its exit code:
inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe C:\path\to\d3d9capture.dll --wait
```

### In-game overlay

Press **Insert** to open the control centre. It is drawn by ImGui inside the
same `Present` hook that captures frames.

| Key | Action |
|---|---|
| `Insert` | Show / hide the control centre |
| `F9` | Start / stop recording, menu open or closed |

---

## How the Capture Works

### 1. Import and VTable Patching
The DLL patches executable imports for D3D9 (`Direct3DCreate9`), D3D11 (`D3D11CreateDeviceAndSwapChain`), and DXGI (`CreateDXGIFactory`).
When the game creates its device or swap chain, hooks are installed on:
- `IDirect3DDevice9::Present` / `Reset` (D3D9)
- `IDirect3DSwapChain9::Present` (D3D9)
- `IDXGISwapChain::Present` / `ResizeBuffers` (D3D11 / DXGI)

All vtable modifications are performed **in-place** using atomic pointer exchanges (`InterlockedExchangePointer`), maintaining compatibility with co-resident overlays like ReShade or ENB.

### 2. D3D11 Staging Readback
Inside `IDXGISwapChain::Present`, the backbuffer texture is retrieved. If multisampled, it is resolved via `ResolveSubresource`. Double-buffered staging textures (`D3D11_USAGE_STAGING`, `D3D11_CPU_ACCESS_READ`) perform GPU->CPU DMA readbacks via `CopyResource` and map memory for zero-copy delivery.

---

## Limitations & Notes

- **Bitness must match**: compile as 64-bit for 64-bit games like GTA V (`GTA5.exe`), and 32-bit for 32-bit games like GTA IV (`GTAIV.exe`).
- **Anti-cheat**: `CreateRemoteThread` injection is detectable. Protected games (EAC, BattlEye, VAC) will likely terminate. Use only on games you own and for legitimate purposes.
- **Format**: Supports BGRA and RGBA formats across D3D9 (`D3DFMT_X8R8G8B8`, `D3DFMT_A8R8G8B8`) and D3D11 (`DXGI_FORMAT_R8G8B8A8_UNORM`, `DXGI_FORMAT_B8G8R8A8_UNORM`).
