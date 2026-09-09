# Frida verification scripts

Tools for checking what d3d9capture's hooks actually do inside a live process,
rather than what the code assumes they do. Every finding recorded in the DLL's
comments was produced with these.

## Running

```
pip install frida
python tools/frida/run.py <script.js> --name <process.exe> [--seconds N]
```

`run.py` attaches, prints each `send()` payload as JSON, and detaches. Use
`--seconds` for scripts that install hooks and report afterwards.

## Scripts

| Script | Purpose |
| --- | --- |
| `recon.js` | First look at a process: which d3d9 modules are loaded (system runtime vs. a game-directory proxy), where the interface vtables are, and whether the hard-coded GTA IV RAGE context assumption holds. |
| `find_device.js` | Sweeps module writable sections for stored D3D9 object pointers and reports each one's vtable, slots and referencing addresses. |
| `qi_probe.js` | Narrows candidates structurally, then asks each one `QueryInterface(IID_IDirect3DDevice9)`. Scope is selectable (`run('sections')` / `run('heap')`). |
| `heap_devices.js` | Whole-process variant of the above. **See the warning below.** |
| `present_probe.js` | Behavioural check: hooks candidate slot-16/17 addresses and counts calls, so a real device's Present shows up at the frame rate. |
| `verify_hooks.js` | The one to run after injecting. Confirms d3d9capture.dll is loaded, the import slot is redirected into it, and which vtable slots it took over — reporting each slot's *index*, so a wrong-slot hook is visible. |

## What these established

* **The game may not use the system D3D9 runtime.** The test machine's GTA IV
  loads ReShade as `d3d9.dll` from the game directory (it exports
  `ReShadeUpdateAndPresentEffectRuntime`), and `DINPUT8.dll` there also exports
  `Direct3DCreate9`. A d3d9 implementation must be detected by *export*, not by
  module name.

* **GTAIV.exe does have a `Direct3DCreate9` import.** The slot sits at RVA
  `0xa73554` and resolves to whichever d3d9 implementation is loaded. The DLL
  previously skipped the import patch for GTAIV.exe on the belief that the game
  resolves the entry point via `GetProcAddress`; that belief was wrong, and it
  cost the project its only reliable hook.

* **The hard-coded RAGE context address does not generalise.** At
  `base + 0xe95888` this install holds an unrelated heap pointer whose first
  field is not a vtable.

* **A vtable cannot be identified by shape.** D3D9 vtables sit back-to-back in
  `.rdata`, so "N consecutive code pointers" runs together across neighbours —
  every candidate measured as 160+ slots. `QueryInterface` is the only
  authoritative test.

* **COM methods on x86 are `__stdcall`, not `__thiscall`** — `this` is the first
  *stack* argument. Calling them as `thiscall` shifts every argument and faults.

* **GTA IV never calls `IDirect3DDevice9::Present`.** It presents through
  `IDirect3DSwapChain9::Present` (slot 3) — measured at 99 calls in 3 seconds
  against 0 on the device. A capture DLL that hooks only the device installs
  its hook perfectly and then captures nothing, which is exactly how this
  project failed on GTA IV. `InstallDeviceHooks` now also hooks the implicit
  swap chain, and re-hooks it after a Reset.

* **Patch vtables in place; do not hand an object a relocated copy.** Cloning
  looks safer but breaks two different ways. A co-resident overlay recovers its
  own trampoline from the object's vtable *pointer* — ReShade patches
  `IDirect3D9::CreateDevice` in the shared system vtable in place — so pointing
  the object at a copy fails a lookup it has never seen, and GTA IV died inside
  CreateDevice. See also the per-instance case below.

* **The device's vtable is per-instance.** The Windows D3D9 runtime embeds it in
  the device's own allocation (observed at `device+0x2F5C`). Handing such a
  device a relocated *copy* of its vtable crashes the process on the first call
  through it, before any hook is reached. Together with the ReShade case above,
  this is why the DLL now patches every vtable in place and clones none.

* **Frida rejects match patterns that begin with a wildcard.** `"?? ?? ff 6f"`
  raises *invalid match pattern*, so there is no masked "any pointer into this
  module" search — only exact values. `verify_hooks.js` therefore sweeps in
  JavaScript to find hooks and uses exact-value `Memory.scan` to resolve each
  hook's slot index.

* **Probing must not run while the game is inside CreateDevice.** Calling into
  D3D9 from a worker thread during device creation deadlocks on the runtime's
  internal locks — the original GTA IV freeze, reached from a new direction. The
  DLL now waits passively whenever its import hook has fired.

## Warning

`heap_devices.js` sweeps all committed memory and calls `QueryInterface` on
anything COM-shaped. Freed and recycled allocations pass the structural filter,
and calling into one **crashed a running game** during development. Prefer
`qi_probe.js` with section scope. The same hazard is why the DLL's own scan is
restricted to module writable sections.

Note also that probing is only safe while the target is not inside
`CreateDevice`: calling into D3D9 from a worker thread during device creation
deadlocks on the runtime's internal locks.

## End-to-end test target

`tools/d3d9_testapp.cpp` is a minimal D3D9 app (window, device, clear+present
loop) that colours frame *n* as `XRGB(n*3, 64, 128)`, so captured frames can be
checked against known pixel values without involving a game.

```
cl /nologo /W3 /O2 /MD /Fe:d3d9_testapp.exe tools\d3d9_testapp.cpp /link d3d9.lib user32.lib
inject_tool.exe --launch d3d9_testapp.exe d3d9capture.dll 60
python tools/frida/run.py tools/frida/verify_hooks.js --name d3d9_testapp.exe
```
