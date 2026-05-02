# d3d9capture — Direct3D 9 Frame Capture via DLL Injection

Efficient, production-quality GPU frame capture for D3D9 games.
No screen-scraping, no GDI, no Desktop Duplication API — pixels are pulled
directly from the GPU's back-buffer immediately after the game draws them.

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  Game Process                   │
│                                                 │
│  D3D9 Device                                    │
│  ┌──────────┐   Present()                       │
│  │ VTable   │──────────────────────────┐        │
│  └──────────┘                          │        │
│                                        ▼        │
│                              ┌─────────────────┐│
│                              │ Hooked_Present  ││
│                              │                 ││
│                              │ GetBackBuffer   ││  GPU → SYSTEMMEM DMA
│                              │     ↓           ││─────────────────────►
│                              │ GetRenderTarget ││  (double-buffered)
│                              │     Data        ││
│                              │     ↓           ││
│                              │ LockRect        ││
│                              │     ↓           ││
│                              │ FrameReady()    ││
│                              └────────┬────────┘│
│                                       │         │
│         Shared Memory (mmap)          │         │
│  ┌────────────────────────────┐       │         │
│  │  ShmHeader + pixel data    │◄──────┘         │
│  └─────────────┬──────────────┘                 │
└────────────────│────────────────────────────────┘
                 │  SetEvent(EvtReady)
                 ▼
┌────────────────────────────┐
│      shm_reader.exe        │
│  (encoder / streamer / UI) │
│  WaitForSingleObject       │
│  → process frame           │
│  → SetEvent(EvtDone)       │
└────────────────────────────┘
```

---

## Files

| File | Purpose |
|---|---|
| `dllmain.cpp` | DLL entry point; creates temp device to harvest vtable; patches Present + Reset |
| `capture.h/cpp` | Double-buffered GPU readback via `GetRenderTargetData` |
| `consumer_backend.cpp` | Writes frames to named shared memory; optional BMP debug dumps |
| `inject_tool.cpp` | `CreateRemoteThread` injector; accepts PID or process name |
| `shm_reader.cpp` | Out-of-process frame consumer template |
| `build.bat` | MSVC build script |

---

## Build Instructions

### Prerequisites
- Visual Studio 2017 or later (Community edition is fine)
- Windows SDK 8.1+ (for `d3d9.h`, `d3d9.lib`)
- **32-bit toolchain** for 32-bit games (`vcvars32.bat`), 64-bit for 64-bit games

### Steps
```bat
:: Open a Developer Command Prompt, then:
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

:: Terminal 2 — launch the game, then inject
inject_tool.exe  game.exe  C:\path\to\d3d9capture.dll
:: or by PID:
inject_tool.exe  1234     C:\path\to\d3d9capture.dll
```

The first `DUMP_FRAMES` (default: 10) frames are saved as BMP files to
`C:\d3d9capture\` for verification.

---

## How the Capture Works

### 1. VTable Patching
D3D9 devices are COM objects; their virtual-method dispatch table is a plain
array of function pointers in read-only memory. We:

1. Create a tiny invisible device (1×1 pixel, software VP) just to locate
   the vtable in memory.
2. Call `VirtualProtect` to make the two target slots writable.
3. Swap in our hook pointers, saving the originals as trampolines.
4. Restore the page protection.

Because COM vtables are **per-class, not per-instance**, patching the dummy
device's vtable simultaneously patches the game's device.

### 2. Present Hook (slot 17)
`Present` is called exactly once per rendered frame. Inside the hook:

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

### 3. Double-Buffering
Two staging surfaces alternate between "write" (GPU→CPU DMA in progress) and
"read" (available to consumer). This hides the DMA latency from the render
thread.  One frame of latency is introduced — standard for all capture tools.

### 4. Reset Hook (slot 16)
When the game calls `Reset` (resolution change, alt-tab, fullscreen toggle)
all `D3DPOOL_DEFAULT` resources are invalidated. Our `D3DPOOL_SYSTEMMEM`
surfaces are unaffected, but we release them preemptively and re-create on
the next `Present` to match any new resolution.

---

## Extending the Capture

### NVENC / QuickSync Encoding
Replace the `memcpy` in `consumer_backend.cpp` with a direct map into an
NV12/BGRA encoder input buffer:
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

### In-Process Texture Overlay
Instead of pulling pixels back to the CPU you can render an overlay texture
directly inside the hooked `Present` before calling the original — zero
readback cost.

---

## Limitations & Notes

- **Bitness must match**: a 32-bit DLL cannot be injected into a 64-bit process
  and vice versa. Most D3D9 games are 32-bit.
- **Anti-cheat**: `CreateRemoteThread` injection is detectable. Protected games
  (EAC, BattlEye, VAC) will likely terminate. Use only on games you own and for
  legitimate purposes (recording, streaming, accessibility tools).
- **D3D9Ex**: games that use `Direct3DCreate9Ex` have identical vtable layouts;
  the hooks work unchanged.
- **GPU sync point**: `GetRenderTargetData` introduces a GPU pipeline flush
  (~0.1–0.5 ms on modern hardware). For maximum frame-rate transparency
  consider D3D9Ex's `IDirect3DQuery9` approach to overlap the copy.
- **Format**: Most games use `D3DFMT_X8R8G8B8` (BGRA byte order). Check
  `FrameData::format` and convert if your downstream expects RGBA.
