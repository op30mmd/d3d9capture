/*
 * find_device.js — locate the live IDirect3DDevice9 the game actually renders
 * with, and every memory slot that points at it.
 *
 * This is the ground truth that d3d9capture's ScanForExistingDevice() has to
 * reproduce from inside the injected DLL, so the checks here are deliberately
 * the same ones that are cheap to implement in C++:
 *
 *   a COM object is a pointer whose [0] is a vtable, and a D3D9 device vtable
 *   is >= 119 consecutive function pointers into the executable pages of a
 *   module that implements D3D9 (the system runtime *or* a proxy such as
 *   ReShade / ENB / DXVK, which is what the game really talks to).
 *
 * Note the object itself lives on the heap, NOT inside the d3d9 image.
 */

'use strict';

const PSZ = Process.pointerSize;
const VT_DEVICE_COUNT = 119;   // IDirect3DDevice9   (last slot: CreateQuery)
const VT_DEVICEEX_COUNT = 133; // IDirect3DDevice9Ex (last slot: ResetEx)
const VT_D3D9_COUNT = 17;      // IDirect3D9         (last slot: CreateDevice)
const VT_D3D9EX_COUNT = 21;    // IDirect3D9Ex       (last slot: CreateDeviceEx)
const VT_DEVICE_RESET = 16;
const VT_DEVICE_PRESENT = 17;

function hex(x) { return '0x' + x.toString(16); }

// ── interval sets ────────────────────────────────────────────────────────────

/*
 * A sorted, numeric interval set. The sweep below tests millions of candidate
 * words, so membership has to be a binary search over plain numbers rather
 * than a linear walk of NativePointer comparisons.
 */
function IntervalSet(ranges) {
    const items = ranges.map(function (r) {
        return { lo: r.base.toUInt32(), hi: r.base.add(r.size).toUInt32() };
    }).sort(function (a, b) { return a.lo - b.lo; });

    // Merge touching/overlapping ranges so the search stays shallow.
    const lo = [], hi = [];
    items.forEach(function (it) {
        const last = lo.length - 1;
        if (last >= 0 && it.lo <= hi[last]) {
            if (it.hi > hi[last]) hi[last] = it.hi;
        } else {
            lo.push(it.lo); hi.push(it.hi);
        }
    });
    this.lo = new Uint32Array(lo);
    this.hi = new Uint32Array(hi);
    this.n = lo.length;
}
/* Numeric membership test — `v` is a plain unsigned 32-bit value. */
IntervalSet.prototype.hasNum = function (v) {
    let a = 0, b = this.n - 1;
    while (a <= b) {
        const mid = (a + b) >>> 1;
        if (v < this.lo[mid]) b = mid - 1;
        else if (v >= this.hi[mid]) a = mid + 1;
        else return true;
    }
    return false;
};
IntervalSet.prototype.has = function (p) { return this.hasNum(p.toUInt32()); };

// ── module classification ────────────────────────────────────────────────────

/*
 * Every module that implements D3D9: the system runtime plus any proxy DLL the
 * game loads in its place. A proxy is detected by the export, not by the name,
 * so renamed wrappers are still caught.
 */
function d3d9ishModules() {
    const out = [];
    Process.enumerateModules().forEach(function (m) {
        let exportsCreate = false;
        try {
            exportsCreate = m.findExportByName('Direct3DCreate9') !== null ||
                m.findExportByName('Direct3DCreate9Ex') !== null;
        } catch (e) { /* no export directory */ }
        if (exportsCreate || /^d3d9(\.dll)?$/i.test(m.name)) {
            out.push(m);
        }
    });
    return out;
}

function execRangesOf(mods) {
    const out = [];
    Process.enumerateRanges('r-x').forEach(function (r) {
        for (let i = 0; i < mods.length; i++) {
            const m = mods[i];
            if (r.base.compare(m.base) >= 0 && r.base.compare(m.base.add(m.size)) < 0) {
                out.push(r);
                return;
            }
        }
    });
    return out;
}

// ── PE section walk ──────────────────────────────────────────────────────────

function writableSections(mod) {
    const base = mod.base;
    const out = [];
    try {
        if (base.readU16() !== 0x5a4d) return out;
        const nt = base.add(base.add(0x3c).readU32());
        if (nt.readU32() !== 0x00004550) return out;
        const numSections = nt.add(6).readU16();
        const optSize = nt.add(20).readU16();
        const sec = nt.add(24).add(optSize);
        for (let i = 0; i < numSections; i++) {
            const sh = sec.add(i * 40);
            const name = sh.readCString(8);
            const vsize = sh.add(8).readU32();
            const vaddr = sh.add(12).readU32();
            const chars = sh.add(36).readU32();
            if (chars & 0x80000000 /* IMAGE_SCN_MEM_WRITE */) {
                out.push({ name: name, base: base.add(vaddr), size: vsize });
            }
        }
    } catch (e) { /* unreadable header */ }
    return out;
}

// ── vtable measurement ───────────────────────────────────────────────────────

/*
 * How many consecutive function pointers into d3d9 code start at `vtable`?
 * Capped, since we only need to tell 17 / 21 / 119 / 133 apart.
 */
function vtableLength(vtable, code, cap) {
    let n = 0;
    for (; n < cap; n++) {
        let v;
        try { v = vtable.add(n * PSZ).readPointer(); } catch (e) { break; }
        if (v.isNull() || !code.has(v)) break;
    }
    return n;
}

function classify(len) {
    if (len >= VT_DEVICEEX_COUNT) return 'IDirect3DDevice9Ex';
    if (len >= VT_DEVICE_COUNT) return 'IDirect3DDevice9';
    if (len >= VT_D3D9EX_COUNT) return 'IDirect3D9Ex?';
    if (len >= VT_D3D9_COUNT) return 'IDirect3D9';
    return 'other(' + len + ')';
}

// ── main scan ────────────────────────────────────────────────────────────────

function scan() {
    const mods = d3d9ishModules();
    const mainMod = Process.enumerateModules()[0];
    const code = new IntervalSet(execRangesOf(mods));
    const readable = new IntervalSet(Process.enumerateRanges('r--'));

    const report = {
        mainModule: { name: mainMod.name, base: mainMod.base.toString(), path: mainMod.path },
        d3d9Modules: mods.map(function (m) {
            return {
                name: m.name, base: m.base.toString(), size: m.size, path: m.path,
                exportsDirect3DCreate9: (function () {
                    try { const e = m.findExportByName('Direct3DCreate9'); return e ? e.toString() : null; }
                    catch (err) { return null; }
                })(),
                exportsDirect3DCreate9Ex: (function () {
                    try { const e = m.findExportByName('Direct3DCreate9Ex'); return e ? e.toString() : null; }
                    catch (err) { return null; }
                })()
            };
        }),
        objects: []
    };

    // Which images do we sweep for a stored device pointer? The game itself,
    // plus every d3d9-ish module (a proxy keeps its own device in .data).
    const sweepModules = [mainMod].concat(mods);

    const seen = {};   // vtable -> aggregated record

    sweepModules.forEach(function (m) {
        writableSections(m).forEach(function (sec) {
            let buf;
            try { buf = sec.base.readByteArray(sec.size); } catch (e) { return; }
            if (buf === null) return;
            const view = new DataView(buf);
            const n = Math.floor(sec.size / PSZ);

            for (let i = 0; i < n; i++) {
                let raw;
                try { raw = view.getUint32(i * PSZ, true); } catch (e) { break; }
                if (raw === 0) continue;
                if (!readable.hasNum(raw)) continue;
                const cand = ptr(raw);

                let vt;
                try { vt = cand.readPointer(); } catch (e) { continue; }
                if (vt.isNull() || !readable.hasNum(vt.toUInt32())) continue;

                // The first slot must be a d3d9 code pointer for this to be a
                // D3D9 COM object at all.
                let slot0;
                try { slot0 = vt.readPointer(); } catch (e) { continue; }
                if (slot0.isNull() || !code.has(slot0)) continue;

                const key = vt.toString();
                if (!(key in seen)) {
                    const len = vtableLength(vt, code, 160);
                    const owner = Process.findModuleByAddress(vt);
                    seen[key] = {
                        vtable: key,
                        vtableModule: owner ? owner.name : null,
                        vtableModulePath: owner ? owner.path : null,
                        vtableRva: owner ? hex(vt.sub(owner.base).toInt32()) : null,
                        vtableSlots: len,
                        iface: classify(len),
                        instances: [],
                        referencedFrom: []
                    };
                }
                const rec = seen[key];
                const objStr = cand.toString();
                if (rec.instances.length < 4 && rec.instances.indexOf(objStr) < 0) rec.instances.push(objStr);
                rec.instanceCount = (rec.instanceCount || 0) + 1;
                if (rec.referencedFrom.length < 6) {
                    rec.referencedFrom.push({
                        module: m.name,
                        section: sec.name,
                        at: sec.base.add(i * PSZ).toString(),
                        rva: hex(sec.base.add(i * PSZ).sub(m.base).toInt32())
                    });
                }
            }
        });
    });

    Object.keys(seen).forEach(function (k) {
        const rec = seen[k];
        // Present/Reset slots — what the DLL would end up hooking.
        try {
            const vt = ptr(rec.vtable);
            rec.slots = {};
            [0, 1, 2, VT_DEVICE_RESET, VT_DEVICE_PRESENT, 118, 119, 121, 132, 133].forEach(function (i) {
                try {
                    const v = vt.add(i * PSZ).readPointer();
                    const m = Process.findModuleByAddress(v);
                    rec.slots[i] = m ? m.name + '(' + m.base + ')!' + hex(v.sub(m.base).toInt32()) : v.toString();
                } catch (e) { rec.slots[i] = '<unreadable>'; }
            });
        } catch (e) { /* short vtable */ }
        report.objects.push(rec);
    });

    // Most interesting first: real devices before factories.
    report.objects.sort(function (a, b) { return b.vtableSlots - a.vtableSlots; });

    // Does the game import Direct3DCreate9, or resolve it dynamically?
    report.imports = [];
    try {
        mainMod.enumerateImports().forEach(function (imp) {
            if (/Direct3DCreate9/i.test(imp.name)) {
                report.imports.push({
                    name: imp.name, module: imp.module,
                    slot: imp.slot ? imp.slot.toString() : null,
                    address: imp.address ? imp.address.toString() : null
                });
            }
        });
    } catch (e) { report.importsError = e.message; }

    // The hard-coded RAGE context the DLL relies on.
    const GTAIV_CONTEXT_RVA = 0x01295888 - 0x00400000;
    if (/gtaiv\.exe$/i.test(mainMod.path)) {
        const ctxAddr = mainMod.base.add(GTAIV_CONTEXT_RVA);
        const out = { contextAddr: ctxAddr.toString(), contextRva: hex(GTAIV_CONTEXT_RVA) };
        try {
            const ctx = ctxAddr.readPointer();
            out.contextValue = ctx.toString();
            const vt = ctx.readPointer();
            out.contextVTable = vt.toString();
            out.contextVTableIsD3D9Code = (function () {
                try { return code.has(vt.readPointer()); } catch (e) { return false; }
            })();
            try {
                const dev = ctx.add(0x160).readPointer();
                out.plus0x160 = dev.toString();
                out.plus0x160IsDevice = (function () {
                    try { return code.has(dev.readPointer().readPointer()); } catch (e) { return false; }
                })();
            } catch (e) { out.plus0x160 = '<unreadable>'; }
        } catch (e) { out.error = e.message; }
        report.gtaivContext = out;
    }

    send({ tag: 'find_device', report: report });
}

rpc.exports = { run: scan };

// Run off the load path: the sweep takes seconds and script.load() must not
// block long enough for the client transport to time out.
setTimeout(function () {
    try { scan(); }
    catch (e) { send({ tag: 'error', message: e.message, stack: e.stack }); }
    send({ tag: 'done' });
}, 0);
