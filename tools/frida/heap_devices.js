/*
 * heap_devices.js — find every live D3D9 device in the process, wherever it
 * lives, and confirm each one with QueryInterface.
 *
 * A RAGE-engine title keeps its graphics context on the heap, not in the
 * executable's .data, so sweeping module sections (what d3d9capture currently
 * does) finds nothing. This sweeps all committed memory.
 *
 * The trick that makes a full sweep cheap: every COM object begins with its
 * vtable pointer, and a D3D9 vtable lives in the read-only data of a module
 * that implements D3D9. So we look for *words that are themselves vtable
 * pointers* — the object is at that word's own address. That is one numeric
 * range test per word and zero extra memory reads, instead of dereferencing
 * every candidate.
 *
 * Each distinct vtable is then asked, once, what it is. QueryInterface is the
 * only authoritative answer: vtables sit back-to-back in .rdata, so no amount
 * of "N consecutive code pointers" shape-matching can separate a device from a
 * texture followed by its neighbours.
 */

'use strict';

const PSZ = Process.pointerSize;
const IID_IDirect3DDevice9 = '96 3b 22 d0 7a bf fd 43 92 bd a4 3b 0d 82 b9 eb';
const IID_IDirect3DDevice9Ex = 'ce 10 8b b1 49 26 5a 40 87 0f 95 f7 77 d4 31 3a';
const VT_DEVICE_RESET = 16;
const VT_DEVICE_PRESENT = 17;

function hex(x) { return '0x' + x.toString(16); }

function describe(p) {
    if (p === null || p.isNull()) return null;
    const m = Process.findModuleByAddress(p);
    return m ? m.name + '(' + m.base + ')!' + hex(p.sub(m.base).toInt32()) : p.toString();
}

function iidBuf(spec) {
    const buf = Memory.alloc(16);
    buf.writeByteArray(spec.split(' ').map(function (b) { return parseInt(b, 16); }));
    return buf;
}

function IntervalSet(ranges) {
    const items = ranges.map(function (r) {
        return { lo: r.base.toUInt32(), hi: r.base.add(r.size).toUInt32() };
    }).sort(function (a, b) { return a.lo - b.lo; });
    const lo = [], hi = [];
    items.forEach(function (it) {
        const last = lo.length - 1;
        if (last >= 0 && it.lo <= hi[last]) { if (it.hi > hi[last]) hi[last] = it.hi; }
        else { lo.push(it.lo); hi.push(it.hi); }
    });
    this.lo = new Uint32Array(lo); this.hi = new Uint32Array(hi); this.n = lo.length;
}
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

function d3d9ishModules() {
    return Process.enumerateModules().filter(function (m) {
        try {
            return m.findExportByName('Direct3DCreate9') !== null ||
                m.findExportByName('Direct3DCreate9Ex') !== null;
        } catch (e) { return false; }
    });
}

function rangesInModule(mod, prot) {
    return Process.enumerateRanges(prot).filter(function (r) {
        return r.base.compare(mod.base) >= 0 && r.base.compare(mod.base.add(mod.size)) < 0;
    });
}

function run() {
    const mods = d3d9ishModules();

    // Where D3D9 vtables can live: the read-only data of a d3d9-implementing
    // module. Where their slots must point: that same module's code.
    let rdata = [];
    const perModuleCode = mods.map(function (m) {
        rdata = rdata.concat(rangesInModule(m, 'r--'));
        return { mod: m, code: new IntervalSet(rangesInModule(m, 'r-x')) };
    });
    const vtableSpace = new IntervalSet(rdata);

    const byVTable = {};
    let scanned = 0, hits = 0;

    Process.enumerateRanges('rw-').forEach(function (r) {
        if (r.size > 0x4000000) return;
        let buf;
        try { buf = r.base.readByteArray(r.size); } catch (e) { return; }
        if (buf === null) return;
        const view = new DataView(buf);
        const n = Math.floor(r.size / PSZ);
        scanned += r.size;

        for (let i = 0; i < n; i++) {
            const w = view.getUint32(i * PSZ, true);
            if (!vtableSpace.hasNum(w)) continue;   // not a vtable pointer
            hits++;
            const key = '0x' + w.toString(16);
            if (key in byVTable) { byVTable[key].count++; continue; }

            // First sighting of this vtable: validate and record.
            const vt = ptr(w);
            let owner = null;
            for (let k = 0; k < perModuleCode.length; k++) {
                const pm = perModuleCode[k];
                let ok = true;
                for (let s = 0; s < 3; s++) {   // QueryInterface / AddRef / Release
                    let v;
                    try { v = vt.add(s * PSZ).readPointer(); } catch (e) { ok = false; break; }
                    if (v.isNull() || !pm.code.hasNum(v.toUInt32())) { ok = false; break; }
                }
                if (ok) { owner = pm.mod; break; }
            }
            if (!owner) continue;

            byVTable[key] = {
                vtable: key, instance: r.base.add(i * PSZ).toString(),
                module: owner.name, modulePath: owner.path,
                vtableRva: hex(vt.sub(owner.base).toInt32()),
                count: 1
            };
        }
    });

    send({ tag: 'progress', scannedMB: +(scanned / 1e6).toFixed(1), vtablePointerHits: hits,
           distinctVTables: Object.keys(byVTable).length });

    // Ask each distinct vtable what it is, once.
    const devIID = iidBuf(IID_IDirect3DDevice9);
    const devExIID = iidBuf(IID_IDirect3DDevice9Ex);
    const outPtr = Memory.alloc(PSZ);
    const list = Object.keys(byVTable).map(function (k) { return byVTable[k]; });

    list.forEach(function (c) {
        const obj = ptr(c.instance);
        const vt = ptr(c.vtable);
        let qi, release;
        try {
            // COM methods on x86 are STDMETHODCALLTYPE (__stdcall) with `this`
            // as the first stack argument — not __thiscall.
            qi = new NativeFunction(vt.readPointer(), 'int32',
                ['pointer', 'pointer', 'pointer'], 'stdcall');
            release = new NativeFunction(vt.add(2 * PSZ).readPointer(), 'uint32',
                ['pointer'], 'stdcall');
        } catch (e) { c.error = e.message; return; }

        function ask(iid) {
            try {
                outPtr.writePointer(NULL);
                const hr = qi(obj, iid, outPtr);
                const got = outPtr.readPointer();
                if (hr === 0 && !got.isNull()) release(got);
                return { hr: hr >>> 0, got: got.toString() };
            } catch (e) { return { error: e.message }; }
        }

        c.dev9 = ask(devIID);
        c.dev9ex = ask(devExIID);
        c.isDevice = (c.dev9.hr === 0) || (c.dev9ex.hr === 0);
        c.isEx = c.dev9ex.hr === 0;
        if (c.isDevice) {
            try {
                c.presentSlot = describe(vt.add(VT_DEVICE_PRESENT * PSZ).readPointer());
                c.resetSlot = describe(vt.add(VT_DEVICE_RESET * PSZ).readPointer());
            } catch (e) { /* ignore */ }
        }
    });

    send({ tag: 'result', devices: list.filter(function (c) { return c.isDevice; }), all: list });
}

rpc.exports = { run: run };
