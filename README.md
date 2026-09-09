# d3d9capture — Direct3D 9 Frame Capture via DLL Injection

Efficient, production-quality GPU frame capture for D3D9 games.
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
|   D3D9 Device          Swap Chain                         |
|   +-----------+        +-----------+                      |
|   | vtable    |        | vtable    |                      |
|   | slot 17   |        | slot 3    |                      |
|   +-----+-----+        +-----+-----+                      |
|         | Present()          | Present()                  |
|         +---------+----------+                            |
|                   v                                       |
|        +----------------------+                           |
|        | Hooked_Present  /    |                           |
|        | Hooked_SwapChain     |                           |
|        |        Present       |                           |
|        |                      |                           |
|        | Capture_WantsFrame() |   GPU -> SYSTEMMEM DMA    |
|        |   ...if nobody is    | ------------------------> |
|        |   consuming, stop    |   (double-buffered)       |
|        |          |           |                           |
|        | GetRenderTargetData  |                           |
|        | LockRect             |                           |
|        | FrameReady()         |                           |
|        +----------+-----------+                           |
|                   |                                       |
|           +-------+---------------+                       |
|           |                       |                       |
|     +-----v-----------+     +-----v------------------+    |
|     | Shared memory   |     | recorder.cpp           |    |
|     | ShmHeader +     |     | bounded ring queue     |    |
|     | pixel data      |     | -> MF worker thread    |    |
|     | (+ QPC stamp)   |     | -> H.264 MP4           |    |
|     +-----+-----------+     +------------------------+    |
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
| `dllmain.cpp` | DLL entry point; patches the D3D9 factory import, then `CreateDevice`, device `Present`/`Reset`, and swap chain `Present` |
| `capture.h/cpp` | Double-buffered GPU readback via `GetRenderTargetData` |
| `consumer_backend.cpp` | Writes frames to named shared memory; screenshots and BMP debug dumps |
| `overlay.h/cpp` | In-game ImGui control centre drawn inside the `Present` hook |
| `recorder.h/cpp` | In-process H.264/MP4 encoder: bounded ring queue feeding a Media Foundation `IMFSinkWriter` on a worker thread |
| `imgui_impl_dx9_patched.cpp` | Local fork of ImGui's DX9 backend, compiled **instead of** the submodule's copy — see [ImGui backend patches](#imgui-backend-patches) |
| `inject_tool.cpp` | `CreateRemoteThread` injector; accepts PID or process name |
| `shm_reader.cpp` | Out-of-process frame consumer; records H.264 MP4 via Media Foundation |
| `imgui/` | Dear ImGui, as a git submodule |
| `build.bat` | MSVC build script |
| `tools/d3d9_testapp.cpp` | Minimal D3D9 app used as a verification target |
| `tools/mp4_frame.cpp` | Decodes frames from a recorded MP4 back to BMP, to verify output |
| `tools/frida/` | Scripts that verify the hooks inside a live process |

---

## Build Instructions

### Prerequisites
- Visual Studio 2017 or later (Community edition is fine)
- Windows SDK 8.1+ (for `d3d9.h`, `d3d9.lib`, and the Media Foundation libraries)
- **32-bit toolchain** for 32-bit games (`vcvars32.bat`), 64-bit for 64-bit games
- The Dear ImGui submodule, which the overlay needs:

```bat
git clone --recursive https://github.com/op30mmd/d3d9capture
:: ...or, in an existing clone:
git submodule update --init --recursive
```

### Steps
```bat
:: Open a Developer Command Prompt, then:
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
cd d3d9capture\src
build.bat
```

Outputs land in `d3d9capture\bin\`.

The verification tools under `tools\` are built separately, since they are not
part of the shipped product:

```bat
cl /nologo /W3 /O2 /MD /Fe:..\bin\d3d9_testapp.exe ..\tools\d3d9_testapp.cpp ^
   /link d3d9.lib user32.lib
cl /nologo /W3 /O2 /MT /Fe:..\bin\mp4_frame.exe ..\tools\mp4_frame.cpp
```

#### ImGui backend patches

The overlay needs changes to ImGui's DX9 backend that upstream does not carry:
leaving the game's backbuffer alpha untouched, resetting sRGB, dither and
vertex-blend state, and clearing any vertex declaration the game left bound
before `SetFVF`. Because the submodule points at upstream `ocornut/imgui`, which
this project cannot push to, those changes live in
`src/imgui_impl_dx9_patched.cpp`, and both `build.bat` and CI compile it **in
place of** the submodule's `backends/imgui_impl_dx9.cpp`.

Edit that file rather than the submodule, and do not add the submodule's copy
back to either build command — compiling both is a duplicate-symbol link error.
Its header comment records the upstream base commit and every divergence, so the
patches can be re-applied when the submodule is bumped.

---

## Usage

```bat
:: Terminal 1 — start the frame reader first
shm_reader.exe

:: ...or record straight to an H.264 MP4
shm_reader.exe --record out.mp4 --fps 30 --bitrate 12000

:: Terminal 2 — recommended: launch suspended, inject, then resume automatically
inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe C:\path\to\d3d9capture.dll
:: Optional game arguments follow `--`:
inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe C:\path\to\d3d9capture.dll -- -windowed
:: `--wait` blocks until the game exits and reports its exit code:
inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe C:\path\to\d3d9capture.dll --wait

:: Existing process support (only works if D3D9 has not initialized yet):
inject_tool.exe game.exe C:\path\to\d3d9capture.dll
:: or by PID:
inject_tool.exe 1234 C:\path\to\d3d9capture.dll
```

The first `DUMP_FRAMES` (default: 10) frames are saved as BMP files to
`C:\d3d9capture\` for verification.

### In-game overlay

Press **Insert** to open the control centre. It is drawn by ImGui inside the
same `Present` hook that captures frames, so it needs no reader attached and no
separate process.

| Key | Action |
|---|---|
| `Insert` | Show / hide the control centre |
| `F9` | Start / stop recording, menu open or closed |

While the menu is open the DLL blocks the game's own input so clicks and mouse
movement drive the menu rather than the player: it hooks `DirectInput8Create`
and, on each device it hands out, `GetDeviceState` and `GetDeviceData`, zeroing
the state the game reads back. Wheel movement is forwarded to the overlay for
scrolling instead of being dropped. Only titles that reach input through
`dinput8.dll` are covered.

The menu has four tabs:

| Tab | Contents |
|---|---|
| Dashboard | Live present/capture FPS, resolution, back-buffer format, readback cost, and history graphs |
| Capture & Recording | Start/stop/pause recording, video FPS (24/30/60) and bitrate (4–24 Mbps), the capture master switch, target capture FPS, single screenshots, and burst BMP dumps |
| Appearance & HUD | Five themes, and the mini-HUD's position and contents (FPS, latency, resolution) |
| Info & Help | The shared-memory and event names, how the DirectInput hook behaves, and companion-tool usage |

The mini-HUD is a small readout that stays on screen while the menu is closed.
It is off by default; enable it and choose its corner and contents under
Appearance & HUD. A recording indicator appears on its own whenever the encoder
is running.

### Recording video

Recording works two ways, and they are independent: **F9** or the Capture &
Recording tab encodes inside the game process, while `shm_reader --record`
encodes out of process. Both produce H.264/MP4 through Media Foundation.

#### In-process (F9)

`recorder.cpp` hands captured frames to a Media Foundation `IMFSinkWriter`
through a bounded ring queue drained by a worker thread, so the render thread
never blocks on the encoder — if encoding falls behind, the queue drops frames
rather than stuttering the game. Files are written to
`C:\d3d9capture\recordings\` as `recording_YYYYMMDD_HHMMSS.mp4`; **Open
Folder** in the menu opens that directory.

#### Out of process

`shm_reader --record` encodes delivered frames to H.264/MP4 with Media
Foundation, which ships with Windows: no third-party dependency, no bundled
encoder, and a hardware encoder is used when the GPU offers one.

| Flag | Meaning |
|---|---|
| `--record <file.mp4>` | Encode delivered frames to H.264/MP4 |
| `--fps N` | Frames per second to request and encode (default 30) |
| `--bitrate KBPS` | Target bitrate in kbit/s (default 8000) |
| `--save N [dir]` | Also write the first N delivered frames as BMPs |
| `--seconds S` | Stop after S seconds |

Because the DLL only captures while a reader is waiting for a frame, `--fps`
throttles the *game's* readback cost, not just the reader's work: asking for 30
fps means the game pays the readback 30 times a second rather than on every
presented frame.

Each frame carries the `QueryPerformanceCounter` value from the moment it was
captured, and those timestamps drive the encoded sample times. This matters
because `frameIdx` counts captured frames rather than presented ones, so
consecutive frames can be arbitrarily far apart in real time; encoding them at a
fixed cadence would play back at the wrong speed.

### Where output goes

Output lands in several different places, which is easy to trip over:

| Output | Location |
|---|---|
| In-game recordings (F9) | `C:\d3d9capture\recordings\recording_<timestamp>.mp4` |
| In-game screenshots | `C:\d3d9capture\screenshots\shot_<timestamp>.bmp` |
| Reader video (`--record <path>`) | Exactly the path given, relative to `shm_reader`'s working directory |
| Reader BMPs (`--save N [dir]`) | `.\` by default, or the optional directory argument |
| DLL debug BMPs, burst dumps, and `debug.log` | Hardcoded `C:\d3d9capture\` |

So a reader recording does **not** go to `C:\d3d9capture\` — pass an absolute
path to put one somewhere specific, and keep the `.mp4` extension, since the
container is chosen from it. In-game recordings and screenshots are the
exception: those always land under `C:\d3d9capture\`, in their own
subdirectories.

To check a recording is actually correct — a vertical flip or a red/blue swap
still produces a file that plays fine — decode a frame back out:

```bat
mp4_frame.exe out.mp4 frame 3 90    :: 3 frames, skipping the first 90
```

> **Injection timing:** inject before the game creates its D3D9 factory (for
> example, immediately after launch). The hook intentionally does not create a
> probe D3D object to attach to an already-created device, because doing that
> during GTA IV initialization can deadlock the game.

### Runtime diagnostics

The DLL writes a timestamped process/thread trace to both the debugger and
`C:\d3d9capture\debug.log`. It records import discovery and patch addresses,
factory/device creation arguments and HRESULTs, first back-buffer properties,
capture failures, reset events, and a capture heartbeat every 300 presents.
Attach DebugView or inspect this file when a title fails to hook. A diagnostic
saying no normal factory imports were found generally means the title uses
`GetProcAddress`, delay loading, or D3D9 had already been initialized before
injection.

---

## How the Capture Works

### 1. Factory Import and VTable Patching
The DLL replaces the game executable's imports of `Direct3DCreate9` and
`Direct3DCreate9Ex` with lightweight forwarding hooks. Limiting the early
startup patch to the executable avoids re-entering overlay or compatibility
DLL initialization. The forwarding hook calls the game's original import, then
patches only the returned factory object's `CreateDevice`/`CreateDeviceEx`
slots. When the game creates its device, those hooks patch `Present` and
`Reset` on that device, plus `Present` on its implicit swap chain.

This never creates or releases a D3D factory/device from the injection worker,
which is what avoids the D3D9 initialization-lock deadlock seen in GTA IV. The
same reasoning applies to searching memory for an existing device: confirming a
candidate means calling `QueryInterface` on it, and doing that while the render
thread is inside `CreateDevice` deadlocks on the runtime's internal locks. So
once the import hook has been called, the DLL waits rather than probing.

`GTAIV.exe` does carry an ordinary `Direct3DCreate9` import — the slot sits at
RVA `0xa73554` in the retail x86 build. (Earlier revisions of this project
claimed the game resolved it through `GetProcAddress` and skipped the import
patch in favour of a hard-coded RAGE context address; that was wrong, and the
address does not hold a D3D9 factory on a ReShade-proxied install.)

Each modified slot is made writable with `VirtualProtect`, exchanged atomically,
and its original function retained as the forwarding target. Slots are patched
**in place** rather than by giving an object a private copy of its vtable.
Copying breaks two ways: a device's vtable is per-instance (the runtime embeds
it in the device's own allocation, so relocating it crashes the process), and a
shared factory vtable is where co-resident overlays already live — ReShade
recovers its own trampoline from the object's vtable pointer, so pointing the
factory at a copy makes that lookup fail.

### 2. Present Hooks (device slot 17 and swap chain slot 3)
Both are hooked, and hooking both is necessary rather than belt-and-braces:
many engines never call `IDirect3DDevice9::Present` at all and present through
the swap chain instead. GTA IV is one of them — measured at 99 swap-chain
Presents in 3 seconds against 0 on the device. Hooking only the device installs
cleanly and then captures nothing.

Inside the hook:

```
GetBackBuffer(0, 0, MONO, &pBackBuffer)
    → D3DPOOL_SYSTEMMEM surface (pre-allocated, matching size/format)
        GetRenderTargetData(pBackBuffer, pStagingSurf)
            → implicit GPU fence + DMA copy
                LockRect(D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)
                    → consumer callback
                UnlockRect
g_OrigPresent(...)   ← call original to flip to screen
```

### 3. Capture Only on Demand
The readback below is a synchronous GPU sync point: measured at **6.8 ms per
frame** at 1280x720 in GTA IV. It used to run on every frame whether or not
anything was consuming the result, so a game with no reader attached paid that
cost for frames that were then discarded.

`Capture_WantsFrame()` now gates it. A frame is captured only when something
actually wants one — a pending screenshot, an unused burst-dump or debug-dump
quota, an active in-game recording, or a reader waiting on the shared-memory
"done" event — which brought idle overhead down to **0.03 ms per frame**. With
`shm_reader` attached the readback costs about 15.6 ms per frame, so a consumer
that does not need every frame should pace itself: the "done" event is what asks
for the next frame, so the consumer sets the capture rate and no separate
throttle is needed.

Two overlay controls sit on top of this. The master switch
(`Capture_SetEnabled`) turns the readback off entirely, and the target-FPS
setting (`Capture_SetTargetFps`) caps how often it runs, skipping presents that
arrive sooner than the interval. Both are overridden by a pending screenshot or
burst dump, so an explicit request from the menu is honoured even with capture
switched off.

Note the consequence: with `DUMP_FRAMES = 0`, no recording, and no reader
attached, nothing is captured, by design.

### 4. Double-Buffering
Two staging surfaces alternate between "write" (GPU→CPU DMA in progress) and
"read" (available to consumer). This hides the DMA latency from the render
thread.  One frame of latency is introduced — standard for all capture tools.

### 5. Reset Hook (slot 16)
When the game calls `Reset` (resolution change, alt-tab, fullscreen toggle)
all `D3DPOOL_DEFAULT` resources are invalidated. Our `D3DPOOL_SYSTEMMEM`
surfaces are unaffected, but we release them preemptively and re-create on
the next `Present` to match any new resolution.

The overlay is not so lucky: ImGui's DX9 backend does keep `D3DPOOL_DEFAULT`
objects, so `Overlay_OnPreReset()` runs before the original `Reset` and
`Overlay_OnPostReset()` after a successful one. Skipping either leaves the
device unresettable, which the game sees as a failed `Reset`.

---

## Extending the Capture

Recording to H.264/MP4 is built in — see [Recording video](#recording-video).
The sections below are for going beyond it.

### NVENC / QuickSync Encoding
Media Foundation already uses a hardware encoder where one is available, so
reach for a vendor SDK only if you need something it will not give you (finer
rate control, or encoding without the readback). To feed NVENC directly,
replace the `memcpy` in `consumer_backend.cpp` with a map into an NV12/BGRA
encoder input buffer:
```cpp
// NVENC example sketch
NV_ENC_LOCK_INPUT_BUFFER lockParams = {};
nvEncLockInputBuffer(encoder, inputBuffer, &lockParams);
ConvertBGRAtoNV12(f.pixels, lockParams.bufferDataPtr, f.width, f.height);
nvEncUnlockInputBuffer(encoder, inputBuffer);
nvEncEncodePicture(encoder, &picParams);
```

### Network Streaming
Replace the BMP dump with a socket send to feed e.g. an RTMP or WebRTC
pipeline.

### Adding to the in-game overlay
The overlay in `overlay.cpp` renders inside the hooked `Present` before the
original is called, at zero readback cost, and reads capture state through
`Capture_GetStats()`. Adding a tab or a control means adding to that file; new
state the overlay needs to show or change belongs behind an accessor in
`capture.h`, so the render thread stays the only thread touching D3D9 objects.

---

## Limitations & Notes

- **Bitness must match**: a 32-bit DLL cannot be injected into a 64-bit process
  and vice versa. Most D3D9 games are 32-bit.
- **Anti-cheat**: `CreateRemoteThread` injection is detectable. Protected games
  (EAC, BattlEye, VAC) will likely terminate. Use only on games you own and for
  legitimate purposes (recording, streaming, accessibility tools).
- **D3D9Ex**: games that use `Direct3DCreate9Ex` have identical vtable layouts;
  the hooks work unchanged.
- **GPU sync point**: `GetRenderTargetData` forces a GPU pipeline flush, and it
  is not cheap. Measured in GTA IV at 1280x720 by timing the hook against the
  `Present` it wraps: **6.8 ms per frame**, against 0.7 ms for the game's own
  `Present`. (An earlier version of this file estimated 0.1–0.5 ms; that was an
  order of magnitude optimistic.) This is why capture is gated on consumer
  demand — idle overhead is 0.03 ms per frame — and why a consumer that does not
  need every frame should pace itself. To overlap the copy instead of skipping
  it, consider D3D9Ex's `IDirect3DQuery9` approach.
- **Late attach does not work on every title**: injecting into an
  already-running game only succeeds if a device pointer is reachable from a
  module's writable data. A RAGE-engine title such as GTA IV keeps its device on
  the heap, where it cannot be found safely — sweeping the heap turns up freed
  allocations that still look COM-shaped, and calling `QueryInterface` on one
  crashes the host. Use `--launch` so the import hook is in place before D3D9
  starts.
- **Format**: Most games use `D3DFMT_X8R8G8B8` (BGRA byte order). Check
  `FrameData::format` and convert if your downstream expects RGBA.
- **Overlay input capture is DirectInput-only**: the menu takes over input by
  hooking `dinput8.dll`'s `DirectInput8Create` and the device `GetDeviceState` /
  `GetDeviceData` calls. A title that reads raw input or Win32 messages directly
  will keep receiving input while the menu is open. `F9` is additionally polled
  with `GetAsyncKeyState` so recording can be toggled even where the window
  procedure hook does not see the key.
- **In-game recording drops frames rather than stuttering**: the encoder queue
  is bounded at 24 frames, about 0.4 s at 60 fps. When it is full the *oldest*
  queued frame is discarded to make room, so the render thread never blocks on
  the encoder and a recording made while the encoder is behind loses frames
  instead of dropping the game's frame rate.
