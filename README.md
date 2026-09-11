# d3d9capture — Direct3D 9 & 11 Frame Capture via DLL Injection

GPU frame and audio capture for Direct3D 9 and Direct3D 11 games, verified on
GTA San Andreas, GTA IV and GTA V. No screen-scraping, no GDI, no Desktop
Duplication API: pixels are pulled from the back buffer inside the game's own
`Present` call, and audio is tapped from the game's own WASAPI render buffer.

Frames can be consumed two ways, and both work at once:

- **In process** — an ImGui control centre (default **Insert**) drives capture
  from inside the game, and a built-in Media Foundation encoder records
  H.264/AAC MP4 (default **F9**) with no second program involved.
- **Out of process** — the DLL publishes frames to shared memory and
  `shm_reader.exe` encodes or saves them.

| Game | Executable | Bitness | API | Injection |
|---|---|---|---|---|
| GTA San Andreas | `gta_sa.exe` | x86 | D3D9 | `--launch` (factory hook via `GetProcAddress`) |
| GTA IV | `GTAIV.exe` | x86 | D3D9 | `--launch` (factory import hook) |
| GTA V | `GTA5.exe` | x64 | D3D11 | `--wait-for` (launcher stub; fallback scan) |

---

## Contents

1. [Quick start](#quick-start)
2. [Architecture](#architecture)
3. [Building](#building)
4. [Usage](#usage)
   - [Injector](#injector)
   - [Out-of-process reader](#out-of-process-reader)
   - [In-game overlay](#in-game-overlay)
   - [Where things end up](#where-things-end-up)
5. [Game notes](#game-notes)
6. [How it works](#how-it-works)
7. [Verifying a change](#verifying-a-change)
8. [Limitations & known issues](#limitations--known-issues)
9. [Project layout](#project-layout)

---

## Quick start

1. Get binaries of the **same bitness as the game** — download
   `d3d9capture-<version>-x86.zip` or `-x64.zip` from the GitHub Releases page
   (built by CI on every `v*` tag), or [build them](#building).
2. Launch the game through the injector:

   ```bat
   :: 32-bit D3D9 games (x86 build of everything)
   inject_tool.exe --launch "C:\Games\GTA San Andreas\gta_sa.exe" d3d9capture.dll
   inject_tool.exe --launch C:\Games\GTAIV\GTAIV.exe d3d9capture.dll

   :: GTA V (x64 build): start the game normally first, then attach
   inject_tool.exe --wait-for GTA5.exe d3d9capture.dll --timeout 300
   ```

3. In game, press **Insert** for the control centre and **F9** to start and
   stop recording. Recordings land in `C:\d3d9capture\recordings\`.

If the injector prints `ERROR: bitness mismatch`, you have the wrong zip for
that game; it names which of the game, the DLL and the injector disagrees.

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
|     +-----+-----------+             +-------^--------+    |
|           |                                 |             |
|           |      audio.cpp: WASAPI hook -> PCM ring       |
|           |                                               |
|  ImGui overlay (Insert) draws inside the same Present     |
|  hook and drives capture, recording and screenshots.      |
+-----------------------------------------------------------+
            |  SetEvent(Local\D3D9CaptureReady)
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

## Building

### Prerequisites

- Visual Studio 2017 or later (Community is fine) with the Windows SDK
  (`d3d9.h`, `d3d11.h`, `dxgi.h`, Media Foundation libraries)
- The Dear ImGui submodule:

  ```bat
  git clone --recursive https://github.com/op30mmd/d3d9capture
  :: ...or, in an existing clone:
  git submodule update --init --recursive
  ```

### Build

`build.bat` builds whatever architecture the active `vcvars` environment
targets and prints it, because a DLL of the wrong bitness injects with nothing
but `LoadLibraryA returned 0`.

```bat
:: 32-bit games (GTA San Andreas, GTA IV)
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
cd d3d9capture\src
build.bat

:: 64-bit games (GTA V)
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd d3d9capture\src
build.bat
```

Outputs (`d3d9capture.dll`, `inject_tool.exe`, `shm_reader.exe`) land in
`d3d9capture\bin\`. Both architectures write to the same folder, so rebuild
when switching between a 32-bit and a 64-bit game, or keep two checkouts.

CI (`.github/workflows/main.yml`) builds both architectures with `/WX` on every
push, also compiles the verification tools so they cannot bitrot, and attaches
`-x86.zip` / `-x64.zip` to a GitHub Release on every `v*` tag.

---

## Usage

### Injector

```
inject_tool.exe <pid | process.exe> <dll>
inject_tool.exe --launch <game.exe> <dll> [--wait] [-- game arguments]
inject_tool.exe --wait-for <process.exe> <dll> [--timeout <sec>]
```

| Mode | What it does |
|---|---|
| `<pid \| name>` | Inject into a running process. The DLL then has to find the device by the fallback scan. |
| `--launch` | Create the game suspended, inject, wait for the DLL's worker to signal readiness, then resume. The factory hook is in place before the game creates its device, which is the reliable path for D3D9 titles. `--wait` blocks until the game exits and reports its exit code; anything after `--` is passed to the game. |
| `--wait-for` | Poll for a process of that name that has actually loaded `d3d9.dll` / `d3d11.dll` / `dxgi.dll`, then inject into it. For games whose exe is a launcher stub that re-executes the real renderer (GTA V). |

Before touching the game, the injector reads the bitness of the game (PE header
for `--launch`, `IsWow64Process` for an attached PID), of the DLL and of itself,
and refuses a mismatch:

```
[inject] ERROR: bitness mismatch.
         target   32-bit (x86)   C:\Games\GTA San Andreas\gta_sa.exe
         dll      64-bit (x64)   C:\path\to\d3d9capture.dll
         injector 64-bit (x64)   (this inject_tool.exe)
```

Run the injector as administrator, or at least with `SeDebugPrivilege`. If the
game runs elevated (GTA V does), `shm_reader.exe` must run elevated too, or it
cannot signal the frame events the game owns.

### Out-of-process reader

Start the reader **before** injecting; the DLL only reads back frames while a
consumer is attached, so an idle injection costs the game nothing.

```
shm_reader.exe [--record out.mp4] [--fps N] [--bitrate KBPS] [--save N [dir]] [--seconds S]
```

| Option | Meaning |
|---|---|
| `--record out.mp4` | Encode delivered frames to H.264 MP4 via Media Foundation |
| `--fps N` | Frames per second to request and encode (default 30). The reader paces; the game is never throttled. |
| `--bitrate KBPS` | Target bitrate in kbit/s (default 8000) |
| `--save N [dir]` | Also write the first N delivered frames as BMPs |
| `--seconds S` | Stop after S seconds |

The channel is three named kernel objects: `Local\D3D9CaptureShm` (header +
pixels + QPC stamp), `Local\D3D9CaptureReady` and `Local\D3D9CaptureDone`.
Any program that speaks that handshake can replace `shm_reader.exe`.

### In-game overlay

Press **Insert** to open the control centre; it is drawn by ImGui inside the
same `Present` hook that captures frames. Both hotkeys are configurable in the
**Video Recording** tab:

| Action | Default | Alternatives |
|---|---|---|
| Show / hide the control centre | `Insert` | `Home`, `End`, `F11`, `` ` `` |
| Start / stop recording (menu open or closed) | `F9` | `F10`, `F11`, `F12`, `F8`, `Scroll Lock`, `Pause` |

| Tab | Contents |
|---|---|
| **Dashboard** | FPS / latency / audio plots, quick record button, capture stats |
| **Video Recording** | Filename prefix, video FPS (24–120) and bitrate (4–50 Mbps), AAC bitrate (128–320 kbps), auto-stop timer, auto-save on exit, hotkeys, framerate limiter, high-res BMP screenshot |
| **Audio & Sound** | Hook status, capture mode, software gain (0–300 %), peak meter, test tone |
| **Live Logs** | The DLL's log with tag filtering and search |
| **Appearance & HUD** | Themes, mini-HUD items and position |
| **System & GPU** | Adapter, backend, process RAM, shared-memory status |

The recording beacon in the mini-HUD is tri-state:

| Colour | Meaning |
|---|---|
| **Amber (pulsing)** | `[STARTING]` — sink writer is pre-arming; no frames are captured yet |
| **Red (solid)** | `[REC]` — recording is live |
| **Off** | Idle |

Audio capture modes:

| Mode | Description |
|---|---|
| **Auto** | Use the in-process WASAPI hook; while a recording wants audio and the hook has seen no render-client activity for 1.5 s, capture desktop loopback instead, and switch back the moment the hook is active again |
| **Direct hook** | In-process WASAPI hook only |
| **Loopback** | Desktop / system loopback only |

The peak meter and the "audible" gate measure through a 2nd-order 30 Hz
high-pass rather than the raw samples — see [Audio capture](#audio-capture).

**Auto-save on exit** — when enabled, an active recording is stopped and
finalised if the game exits or crashes, so the MP4 is always written out.

### Where things end up

| Path | Contents |
|---|---|
| `C:\d3d9capture\debug.log` | The DLL's log (also shown in the **Live Logs** tab). The first place to look when nothing is captured. |
| `C:\d3d9capture\recordings\` | In-process MP4 recordings |
| `C:\d3d9capture\screenshots\` | BMP screenshots from the overlay |
| `Local\d3d9capture-ready-<pid>` | Event the DLL signals when its worker is up; `--launch` waits on it before resuming the game |

---

## Game notes

### GTA San Andreas (x86, D3D9)

Use `--launch` with the x86 build. The widely used HOODLUM `gta_sa.exe` lists
only kernel32/user32/winmm/vorbisfile in its import directory and resolves
Direct3D and DirectInput at runtime, so the import scan reports
`patched slots=0`. That is expected; the factory is caught through the exe's
`GetProcAddress` slot instead, and the log should continue:

```
[hook] Patched GetProcAddress import in module=00400000 slot=... original=...
[hook] GetProcAddress(..., "Direct3DCreate9") -> ... redirected to hook ... (runtime-resolved import)
[hook] Direct3DCreate9 intercepted sdk=31 original=...
[poll] Our import hook was called; waiting for the game's own device/swapchain creation instead of probing memory
```

Idle, the game's DirectSound 3D mixer emits a sub-audible wander on the centre
and rear channels; the level meter filters it out (see
[Audio capture](#audio-capture)).

### GTA IV (x86, D3D9)

Use `--launch` with the x86 build. `GTAIV.exe` imports `Direct3DCreate9`
directly, so the IAT patch is the whole story. The game resolves its data files
relative to the working directory, which is why `--launch` runs it from its own
folder.

### GTA V (x64, D3D11)

`--launch` cannot work: `GTA5.exe` is a stub that spawns `PlayGTAV.exe`, which
spawns the *real* `GTA5.exe`.

```
inject_tool --launch GTA5.exe  ->  GTA5.exe (stub)
                                     └─ PlayGTAV.exe
                                          └─ GTA5.exe   <- the actual renderer
```

The DLL lands in the stub, which re-executes and exits immediately. `--launch`
detects a target that dies within 3 seconds and says so instead of reporting
success. Use `--wait-for`: because the stub and the renderer share the same
image name, it waits for the process of that name which has actually loaded a
Direct3D runtime, which is both the proof that it is the renderer and the point
at which the fallback scan can succeed.

`GTA5.exe` imports `d3d9.dll` and `dinput8.dll` but neither `d3d11.dll` nor
`dxgi.dll`, so no factory import is patched and **the fallback scan is what
hooks GTA V** — see the [known issue](#known-issue-the-fallback-scan-can-corrupt-the-targets-heap).
The game runs elevated, so run the injector and the reader elevated too.

---

## How it works

### Hook chain

```
Direct3DCreate9/Ex, D3D11CreateDevice[AndSwapChain], CreateDXGIFactory[1|2]
   patched in the executable's import table
   ...or returned by the executable's patched GetProcAddress import
   ...or, failing both, a live device / swap chain found by memory scan
        |
        v
IDirect3D9::CreateDevice (slot 16) / IDXGIFactory::CreateSwapChain (slot 10)
   patched on that factory
        |
        v
IDirect3DDevice9::Present (17) / Reset (16), IDirect3DSwapChain9::Present,
IDXGISwapChain::Present (8) / ResizeBuffers (13)
   patched on that device / swap chain
```

All vtable and import-slot patches are done in place with
`InterlockedExchangePointer`, so co-resident overlays (ReShade, ENB) keep
working. `DirectInput8Create` is hooked the same way so that, while the control
centre is open, the game's DirectInput devices report no input and the mouse
wheel goes to the overlay instead.

**Import patching** covers games that import the factory functions.
**`GetProcAddress` patching** covers games that resolve them at runtime: only
the main executable's slot is patched (system DLLs call `GetProcAddress`
constantly and none of them creates the game's device), and when the call names
one of the factory functions above on the matching runtime module the hook is
returned and the real export kept as the trampoline. **The fallback scan**
covers the rest: after a 5-second grace period the worker thread scans loaded
modules' `.data` sections for a live `IDirect3DDevice9` / `IDXGISwapChain` and
hooks the vtable it finds. The log line that says which path is in play is:

```
[hook] Factory import scan complete: render import descriptors=N patched slots=M
```

### Which `Present` calls count as frames

`Present` is also called with `DXGI_PRESENT_TEST`, which presents nothing and
merely reports occlusion. Games poll it in a tight loop while their window is
minimised or occluded — GTA V does so at roughly 10,000 calls/second. The hook
ignores those calls entirely: counting them inflated the reported FPS by ~51x,
and capturing them handed the consumer a back buffer that was never presented
(the recorder saw duplicate frames, and a minimised game appeared "frozen").

### Readback without stalling the render thread

**D3D9**: `GetRenderTargetData` into a system-memory surface, then `LockRect`.

**D3D11 / DXGI**: the back buffer is retrieved inside `IDXGISwapChain::Present`
and resolved with `ResolveSubresource` if multisampled. `CopyResource` into a
staging texture only *queues* the GPU→CPU copy; mapping the texture it was just
issued into makes the CPU wait for the GPU to finish, a full pipeline sync on
the render thread for every captured frame — measured at 2.1 ms per frame and
felt as a stutter in step with the capture rate. So the readback always maps
the staging texture filled on the **previous** captured frame, giving the copy a
whole frame to retire, and maps with `D3D11_MAP_FLAG_DO_NOT_WAIT` so that under
heavy GPU load a frame is skipped rather than the render thread stalled. That
is what the second staging texture is for.

The `[cap11] heartbeat` log line reports `copy=`, `map=` and `hook=`
separately for this reason: `copy` is near zero by design (it is just the
enqueue), `map` is the GPU wait, `hook` is the total time taken from the render
thread, and `skips=` counts frames dropped instead of stalling.

Frames are de-strided into a packed `width * 4` buffer before they are queued
(see [Row pitch](#limitations--known-issues)).

### Recorder threading model

One worker thread lives for as long as the recorder is initialised.
`Recorder_Start` and `Recorder_Stop` never create or join threads — they push
`BeginSession` / `EndSession` markers into the same 128-entry queue the frames
travel through, and return immediately. Because a single thread drains that
queue in order:

- The render thread never blocks on encoding **or** on
  `IMFSinkWriter::Finalize` (finalising a large recording can take a long time;
  stopping is what used to freeze the game).
- A stop followed immediately by a start is handled strictly sequentially, so
  two encoding sessions can never share the sink writer. Previously a fast
  F9-off/F9-on started a second worker while the first was still finalising;
  both then used the same global `IMFSinkWriter`, producing
  `WriteSample failed: 0xC00D3E85` and a use-after-free waiting to happen.

All sink-writer state lives in a worker-local `EncodeSession`. When the queue
is full the **oldest frame** is dropped; session markers are never dropped,
since losing an `EndSession` would leave a recording unfinalised.

**Zero-drop start.** Opening an `IMFSinkWriter` — negotiating the hardware
H.264/AAC MFT, calling `BeginWriting` — takes 1.75–1.95 s on typical hardware.
Queuing frames during that window used to fill the queue and discard 45–85
frames before encoding began, visible as a stutter at the start of every
recording. Now `Recorder_Start` sets `g_IsStarting` and the render thread does
not queue frames while it is set; on the worker, `BeginSession` opens the
session synchronously, then flushes stale audio from the ring, enables audio
capture, zeros the frame counters, and atomically flips `g_IsStarting` →
`g_IsRecording`. Frame 0 and audio sample 0 are both stamped at `t = 0` with no
backlog.

**Auto-save on exit.** The game's `ExitProcess` / `TerminateProcess` are hooked
(import slots in every module plus inline hooks on the kernel32/kernelbase
exports). If a recording is live when the game exits, `Recorder_StopSync` runs
with a 5-second timeout so the sink writer is finalised.

### Audio capture

`audio.cpp` hooks `IAudioRenderClient::GetBuffer` / `ReleaseBuffer` and
`IAudioClient::Initialize` / `GetService`. Since Vista every audio path on
Windows — XAudio2, DirectSound emulation, FMOD, Wwise, a game's own mixer —
ends at an `IAudioRenderClient`, so that one pair of hooks captures every
engine. PCM is copied in the game's audio thread at `ReleaseBuffer`, the last
moment before the audio engine consumes it, downmixed (ITU-R BS.775) to
interleaved 16-bit stereo, and pushed into a mutex-serialised ring with a QPC
stamp; the encoder worker drains the ring between video frames and writes AAC
samples whose timestamps line up with the video's.

A game usually has several render clients (GTA SA: a stereo client for the
intro movies, then an 8-channel one for the world). Formats are tracked per
client from `Initialize`, and one **master** client is elected by channel count
and sample rate, with failover to whichever client is actually audible; only
the master is recorded. Low-rate streams (voice chat) are ignored.

In **Auto** mode a desktop-loopback thread engages while a recording wants audio
and the hook has seen no render-client activity for 1.5 s, and pauses again as
soon as the hook is active. A software gain (0–3×) is applied at the ring
drain, so volume can be changed without touching the mixer.

**Level metering.** The peak meter and the "is this stream audible" gate run
every sample through two cascaded one-pole high-pass stages (12 dB/oct, −3 dB at
30 Hz), one state per channel. GTA San Andreas forced this: with nothing
playing, its DirectSound 3D mixer idles with a ~5 Hz, −38 dBFS wander on the
centre and rear channels (front L/R exactly zero), slowly modulated over ~8 s.
A raw `max(|x|)` read that as a level bouncing between −60 and −38 dBFS, so the
overlay's waveform bounced with it. Filtered, the wander reads −85 dBFS — well
under the −70 dBFS gate — while a 60 Hz tone reads ~2 dB low and 100 Hz under
1 dB.

### Overlay init is reachable from two threads

`Overlay_Init` / `Overlay_InitDXGI` can be entered concurrently: the DLL's
worker thread calls them as soon as it installs hooks (including from the
fallback scan), and the game's render thread reaches the same init from inside
`Present`. Checking the "initialised" flag alone is not enough, because it is
only set at the *end* of init — the window covers `CreateContext`, both ImGui
backend inits and the `WndProc` hook.

Running the `WndProc` hook twice is what kills the process: the second
`SetWindowLongPtr` returns the `HookedWndProc` already installed and stores it
as the "original", so `CallWindowProc` recurses into itself until the stack
goes. Init is therefore serialised with a mutex and re-checks the flag under it.
`Overlay_Shutdown` deliberately does **not** take that mutex — it runs from
`DllMain` under the loader lock, and init can load DLLs, so locking there would
risk a deadlock instead.

---

## Verifying a change

The two test apps let the whole chain be exercised without a real game. Build
them for the architecture you are testing (CI builds both), then inject as
usual.

```bat
cl /nologo /W3 /O2 /MD /Fe:d3d11_testapp.exe ..\tools\d3d11_testapp.cpp ^
   /link d3d11.lib dxgi.lib user32.lib

:: d3d11_testapp.exe [frames] [msaa] [test-polls] [width] [height]

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

:: 3b. PADDED STRIDE. Always test a width that is not a multiple of 16, or a
::     whole class of bug stays invisible: at 640 and 800 the GPU pitch happens
::     to equal width*4, so nothing exercises the de-striding path. 1366 gives
::     a 5504-byte pitch against a 5464-byte row, which is what crashed the
::     recorder on GTA V. Run it WITH recording enabled.
inject_tool.exe --launch d3d11_testapp.exe d3d9capture.dll --wait 400 1 0 1366 768

:: 4. Recorder stop-then-restart: frame 76 stops and restarts in one frame.
::    The log must show one worker thread, no "WriteSample failed", a per-
::    session frame count, and the process must exit with code 0.
inject_tool.exe --launch d3d11_testapp.exe d3d9capture.dll --wait 200 1 0

:: 5. Recorded output really decodes, right way up:
::    mp4_frame.exe <in.mp4> <out-prefix> [count] [skip]
mp4_frame.exe C:\d3d9capture\recordings\<file>.mp4 frame_ 3 5

:: 6. The FALLBACK SCAN path, which is the only thing that hooks GTA V.
::    --launch never reaches it: the import hook succeeds first, and the
::    poll then waits for the game's own creation instead of probing memory.
::    Start the app first and attach late, so the worker thread hooks from
::    the scan while the render thread is already presenting -- which is
::    also the arrangement that exposes the overlay double-init race.
d3d11_testapp.exe 600 1 0 1366 768
inject_tool.exe --wait-for d3d11_testapp.exe d3d9capture.dll --timeout 30
```

The D3D9 equivalent (`tools/d3d9_testapp.cpp`, built x86) covers the same
ground for the D3D9 path, including a device `Reset()` at frame 85. Both test
apps import their factory function statically, so the `GetProcAddress` path is
only exercised by a real runtime-resolving exe such as GTA SA; the lines to
look for are listed under [Game notes](#gta-san-andreas-x86-d3d9).

`tools/frida/` holds Frida scripts that verify the hooks inside a live process.

---

## Limitations & known issues

- **Bitness must match** between the game, the DLL and `inject_tool.exe`. A
  64-bit injector hands `CreateRemoteThread` a 64-bit `LoadLibraryA` address
  that means nothing in a WOW64 process, and a 64-bit DLL cannot be mapped into
  one anyway; the injector checks all three and refuses a mismatch.
- **Anti-cheat**: `CreateRemoteThread` injection is detectable. Protected games
  (EAC, BattlEye, VAC) will likely terminate. Use only on games you own and for
  legitimate purposes.
- **Pixel formats**: frames are passed through in the back buffer's native
  32-bpp layout and tagged with its format code (`D3DFORMAT` / `DXGI_FORMAT`).
  Both built-in encoders declare their input as `MFVideoFormat_RGB32` (BGRA),
  which matches `D3DFMT_X8R8G8B8` / `A8R8G8B8` and `DXGI_FORMAT_B8G8R8A8_UNORM`;
  an `R8G8B8A8` swap chain records with red and blue swapped, and non-32-bpp
  back buffers are not handled.
- **Launcher stubs**: `--launch` cannot reach a renderer that its own `.exe`
  re-executes (GTA V). Use `--wait-for`.
- **Elevation**: if the game runs elevated, run both `inject_tool.exe` and
  `shm_reader.exe` elevated too.
- **Row pitch**: the GPU pitch of a mapped back buffer is not `width * 4`. It
  is padded for alignment whenever the width is not a multiple of 16 (GTA V at
  1366 wide maps at 5504 bytes per row, not 5464). Frames are de-strided into a
  packed buffer before they are queued, so anything downstream must use
  `width * 4` and not the pitch the GPU reported. An H.264 *decoder* aligns the
  same way on the way back out, which is why `tools/mp4_frame.cpp` reads its
  output pitch instead of assuming one.
- **Resolution changes while recording**: a sink writer is fixed at the size it
  was opened with, so frames of a different size are dropped (and logged) rather
  than encoded. Stop and start the recording to capture at the new size.
- **Minimised / occluded windows**: capture is skipped for `DXGI_PRESENT_TEST`
  calls, but the DLL has no occlusion check of its own, so a minimised game that
  still issues real `Present` calls is captured (and read back) as usual.

### Known issue: the fallback scan can corrupt the target's heap

When neither the import patch nor the `GetProcAddress` patch catches a factory,
the DLL scans module `.data` sections for a live device / swap chain,
dereferencing candidate values as COM objects and calling `QueryInterface` on
those whose vtable belongs to a D3D/DXGI module. That vtable check does **not**
rule out a *stale pointer to an already-released object*, and the resulting
`QueryInterface` / `Release` can corrupt the target's heap: injecting into a
process where the scan does not quickly find a real swap chain has been
observed to kill it with `STATUS_HEAP_CORRUPTION (0xC0000374)`.

It survives when a real swap chain is found fast and gets dangerous when it
keeps searching. Both halves have been observed: GTA V, whose pointer sits in
`GTA5.exe`'s `.data`, is hooked immediately and runs fine, and so is
`d3d11_testapp` now that it keeps its swap chain in a global — but injecting
into a build whose swap chain lived only on the stack, where the scan finds
nothing and keeps probing, killed it every time. This predates the D3D11 work
and is unfixed. The awkward consequence: on GTA V this scan is the *only*
mechanism that hooks anything, so the one path that makes GTA V work is also
the unsafe one. Fixing it properly needs an export-level trampoline on
`CreateDXGIFactory` / `D3D11CreateDeviceAndSwapChain` rather than memory
scanning.

---

## Project layout

| File | Purpose |
|---|---|
| `src/dllmain.cpp` | DLL entry point; patches the factory imports and the executable's `GetProcAddress`, installs the device / swap-chain hooks, runs the fallback scan, hooks process exit for auto-save |
| `src/capture.h/cpp` | Double-buffered GPU readback via `GetRenderTargetData` (D3D9) and `CopyResource` (D3D11/DXGI); `DXGI_PRESENT_TEST` filtering; de-striding; calls `Recorder_SetDefaultResolution` on every present |
| `src/consumer_backend.cpp` | Shared-memory publisher (`Local\D3D9CaptureShm` + events); screenshots and BMP debug dumps |
| `src/recorder.h/cpp` | In-process H.264/AAC MP4 encoder: one long-lived worker draining a bounded queue of frames and session markers into an `IMFSinkWriter`; async pre-arm; auto-save on exit |
| `src/audio.h/cpp` | In-process WASAPI capture: `IAudioRenderClient` hooks, per-client format tracking and master election, BS.775 downmix, QPC-stamped ring, loopback fallback, software gain, high-passed level metering |
| `src/overlay.h/cpp` | ImGui control centre drawn inside the `Present` hook (D3D9 & D3D11 backends); tabs, mini-HUD, hotkeys, beacon |
| `src/imgui_impl_dx9_patched.cpp` | Local fork of ImGui's DX9 backend compiled **instead of** the submodule's copy |
| `src/inject_tool.cpp` | `CreateRemoteThread` injector: PID / name, `--launch`, `--wait-for`; bitness checks |
| `src/shm_reader.cpp` | Out-of-process frame consumer; records H.264 MP4 via Media Foundation |
| `src/build.bat` | MSVC build script; builds the architecture of the active `vcvars` environment |
| `src/imgui/` | Dear ImGui, as a git submodule |
| `tools/d3d9_testapp.cpp` | Minimal D3D9 verification target, including a device `Reset()` |
| `tools/d3d11_testapp.cpp` | Minimal D3D11/DXGI verification target; can emulate MSAA, `DXGI_PRESENT_TEST` polling, and non-16-aligned widths |
| `tools/mp4_frame.cpp` | Decodes frames from a recorded MP4 back to BMP to verify output; reads the decoder's real row pitch |
| `tools/frida/` | Frida scripts that verify the hooks inside a live process |
| `.github/workflows/main.yml` | CI: x86 + x64 builds, verification tools, tagged releases |
