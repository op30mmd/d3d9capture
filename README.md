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
| `dllmain.cpp` | DLL entry point; patches D3D9 factory, D3D11 device, and DXGI swap chain imports (`Present` / `Reset` / `ResizeBuffers`), plus the fallback scan for an existing device / swap chain |
| `capture.h/cpp` | Double-buffered GPU readback via `GetRenderTargetData` (D3D9) and `CopyResource` (D3D11/DXGI) |
| `consumer_backend.cpp` | Writes frames to named shared memory; screenshots and BMP debug dumps |
| `overlay.h/cpp` | In-game ImGui control centre drawn inside the `Present` hook (D3D9 & D3D11 backends) |
| `recorder.h/cpp` | In-process H.264/MP4 encoder: one long-lived worker draining a bounded queue of frames and session markers into a Media Foundation `IMFSinkWriter` |
| `imgui_impl_dx9_patched.cpp` | Local fork of ImGui's DX9 backend compiled **instead of** the submodule's copy |
| `inject_tool.cpp` | `CreateRemoteThread` injector; accepts a PID, a process name, `--launch`, or `--wait-for` (attach once a Direct3D runtime is loaded) |
| `shm_reader.cpp` | Out-of-process frame consumer; records H.264 MP4 via Media Foundation |
| `imgui/` | Dear ImGui, as a git submodule |
| `build.bat` | MSVC build script for 32-bit and 64-bit targets |
| `tools/d3d9_testapp.cpp` | Minimal D3D9 app used as a verification target |
| `tools/d3d11_testapp.cpp` | Minimal D3D11/DXGI verification target; can emulate MSAA and `DXGI_PRESENT_TEST` occlusion polling |
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
:: For 32-bit Direct3D 9 games (e.g. GTA IV):
inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe C:\path\to\d3d9capture.dll

:: `--wait` blocks until the game exits and reports its exit code:
inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe C:\path\to\d3d9capture.dll --wait

:: For games whose .exe is a launcher stub that re-executes the real renderer
:: (GTA V does this), --launch cannot work — see "Launcher-stub games" below.
:: Start the game normally, then attach to whichever process actually renders:
inject_tool.exe --wait-for GTA5.exe C:\path\to\d3d9capture.dll --timeout 300
```

### Launcher-stub games (GTA V)

`--launch` injects into the process it creates. GTA V's `GTA5.exe` is a stub that
spawns `PlayGTAV.exe`, which spawns the *real* `GTA5.exe`:

```
inject_tool --launch GTA5.exe  ->  GTA5.exe (stub)
                                     └─ PlayGTAV.exe
                                          └─ GTA5.exe   <- the actual renderer
```

The DLL lands in the stub, which re-executes and exits immediately, so the
remote `LoadLibraryA` returns NULL and the renderer is never hooked. `--launch`
now detects a target that dies within 3 seconds and says so instead of
reporting success.

Use `--wait-for` instead. Because the stub and the renderer share the *same
image name*, matching on the name alone is not enough — `--wait-for` waits for a
process of that name which has actually loaded `d3d9.dll` / `d3d11.dll` /
`dxgi.dll`, which is both the proof that it is the renderer and the point at
which the swap-chain scan can succeed.

Both the injector **and** `shm_reader.exe` must run elevated if the game does
(GTA V requires admin): a medium-integrity reader cannot signal the frame events
owned by an elevated game.

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

Import patching only works if the game actually *imports* those functions.
Many titles resolve Direct3D dynamically and import none of them — **GTA V is
one: `GTA5.exe` imports `d3d9.dll` and `dinput8.dll` but neither `d3d11.dll` nor
`dxgi.dll`**, so the D3D11/DXGI import hooks patch zero slots there. The log
line to check is:

```
[hook] Factory import scan complete: render import descriptors=N patched slots=0
```

When nothing is patched, the DLL falls back to scanning loaded modules' `.data`
sections for a live `IDirect3DDevice9` / `IDXGISwapChain` and hooks the vtable
it finds. **This fallback, not the import hook, is what makes GTA V work** — see
the known issue in *Limitations & Notes*.

### 1b. Which `Present` calls count as frames
`Present` is also called with `DXGI_PRESENT_TEST`, which presents nothing and
merely reports occlusion. Games poll it in a tight loop while their window is
minimised or occluded — GTA V does so at roughly 10,000 calls/second. The hook
ignores those calls entirely: counting them inflated the reported FPS by ~51x,
and capturing them handed the consumer a back buffer that was never presented
(the recorder saw duplicate frames, and a minimised game appeared "frozen").

### 2. D3D11 Staging Readback
Inside `IDXGISwapChain::Present`, the backbuffer texture is retrieved. If multisampled, it is resolved via `ResolveSubresource`. Double-buffered staging textures (`D3D11_USAGE_STAGING`, `D3D11_CPU_ACCESS_READ`) perform GPU->CPU DMA readbacks via `CopyResource` and map memory for zero-copy delivery.

### 3. Recorder Threading Model
One worker thread lives for as long as the recorder is initialised. `Recorder_Start`
and `Recorder_Stop` never create or join threads — they push `BeginSession` /
`EndSession` markers into the same queue the frames travel through, and return
immediately. Because a single thread drains that queue in order:

- The render thread never blocks on encoding **or** on `IMFSinkWriter::Finalize`
  (finalising a large recording can take a long time; stopping is what used to
  freeze the game).
- A stop followed immediately by a start is handled strictly sequentially, so
  two encoding sessions can never share the sink writer. Previously a fast
  F9-off/F9-on would start a second worker while the first was still finalising;
  both then used the same global `IMFSinkWriter`, producing
  `WriteSample failed: 0xC00D3E85` and a use-after-free waiting to happen.

All sink-writer state lives in a worker-local `EncodeSession`, so nothing the
encoder touches is a shared global. When the queue is full the **oldest frame**
is dropped; session markers are never dropped, since losing an `EndSession`
would leave a recording unfinalised.

---

## Verifying a change

The two test apps let the whole chain be exercised without a real game. Build
them for the architecture you are testing, then inject as usual.

```bat
cl /nologo /W3 /O2 /MD /Fe:d3d11_testapp.exe ..\tools\d3d11_testapp.cpp ^
   /link d3d11.lib dxgi.lib user32.lib

:: 1. Capture + orientation. The marker must come back TOP-LEFT and the
::    background red channel must advance by exactly 3 per frame.
shm_reader.exe --save 6 out\
inject_tool.exe --launch d3d11_testapp.exe d3d9capture.dll --wait 600

:: 2. MSAA resolve path. Confirm the app reports SampleDesc.Count=4; a swap
::    chain silently created single-sampled would "pass" without testing it.
inject_tool.exe --launch d3d11_testapp.exe d3d9capture.dll --wait 300 4

:: 3. DXGI_PRESENT_TEST filtering: 900 real frames, 45,000 occlusion polls.
::    The DLL log must report presents=900, not 45,900.
inject_tool.exe --launch d3d11_testapp.exe d3d9capture.dll --wait 900 1 50

:: 4. Recorder stop-then-restart: frame 76 stops and restarts in one frame.
::    The log must show one worker thread, no "WriteSample failed", a per-
::    session frame count, and the process must exit with code 0.
inject_tool.exe --launch d3d11_testapp.exe d3d9capture.dll --wait 200 1 0

:: 5. Recorded output really decodes, right way up:
mp4_frame.exe C:\d3d9capture\recordings\<file>.mp4 frame_ 3 5
```

The D3D9 equivalent (`tools/d3d9_testapp.cpp`, built x86) covers the same ground
for the D3D9 path including a device `Reset()`.

---

## Limitations & Notes

- **Bitness must match**: compile as 64-bit for 64-bit games like GTA V (`GTA5.exe`), and 32-bit for 32-bit games like GTA IV (`GTAIV.exe`).
- **Anti-cheat**: `CreateRemoteThread` injection is detectable. Protected games (EAC, BattlEye, VAC) will likely terminate. Use only on games you own and for legitimate purposes.
- **Format**: Supports BGRA and RGBA formats across D3D9 (`D3DFMT_X8R8G8B8`, `D3DFMT_A8R8G8B8`) and D3D11 (`DXGI_FORMAT_R8G8B8A8_UNORM`, `DXGI_FORMAT_B8G8R8A8_UNORM`).
- **Launcher stubs**: `--launch` cannot reach a renderer that its own `.exe`
  re-executes (GTA V). Use `--wait-for`; see *Launcher-stub games* above.
- **Elevation**: if the game runs elevated, run both `inject_tool.exe` and
  `shm_reader.exe` elevated too, or the reader cannot signal the frame events.
- **Minimised / occluded windows**: capture is skipped for `DXGI_PRESENT_TEST`
  calls, but the DLL has no occlusion check of its own, so a minimised game that
  still issues real `Present` calls is captured (and read back) as usual.

### Known issue: the swap-chain fallback scan can corrupt the target's heap

When import patching finds nothing, the DLL scans module `.data` sections for a
live device / swap chain, dereferencing candidate values as COM objects and
calling `QueryInterface` on those whose vtable belongs to a D3D/DXGI module.
That vtable check does **not** rule out a *stale pointer to an already-released
object*, and the resulting `QueryInterface` / `Release` can corrupt the target's
heap: injecting into a process where the scan does not quickly find a real swap
chain has been observed to kill it with `STATUS_HEAP_CORRUPTION (0xC0000374)`.

It survives when a real swap chain is found fast (GTA V, where the pointer sits
in `GTA5.exe`'s `.data`) and gets dangerous when it keeps searching. This
predates the D3D11 work and is unfixed. Note the awkward consequence: on GTA V
this scan is the *only* mechanism that hooks anything, so the one path that
makes GTA V work is also the unsafe one. Fixing it properly needs an
export-level trampoline on `CreateDXGIFactory` / `D3D11CreateDeviceAndSwapChain`
rather than memory scanning.
