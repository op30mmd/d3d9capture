/*
 * verify_hooks.js — check that d3d9capture's hooks are actually installed in a
 * live process, and report exactly which slots it took over.
 *
 * Run it against a process that has d3d9capture.dll injected:
 *
 *     python tools/frida/run.py tools/frida/verify_hooks.js --name d3d9_testapp.exe
 *     python tools/frida/run.py tools/frida/verify_hooks.js --name GTAIV.exe
 *
 * Checks, in order:
 *   1. d3d9capture.dll is loaded.
 *   2. The executable's Direct3DCreate9 import slot points into it.
 *   3. Some D3D9 vtable slot points into it — this is the real proof, because
 *      it finds the hook wherever it ended up, whether the DLL patched a vtable
 *      in place or handed the object a private copy.
 *
 * Each hooked slot is reported with its index, so a wrong-slot hook (the classic
 * way a D3D hook silently does nothing) shows up as an index that is not 16/17
 * for a device or 16/20 for a factory.
 */

'use strict';

const PSZ = Process.pointerSize;

// Indices d3d9capture is expected to occupy. Slot 3 belongs to
// IDirect3DSwapChain9::Present — the path engines like GTA IV actually use.
const SLOT_NAMES = {
    3: 'SwapChain::Present',
    16: 'Reset / CreateDevice',
    17: 'Present',
    20: 'CreateDeviceEx',
    121: 'PresentEx',
    132: 'ResetEx'
};
const EXPECTED_SLOTS = [3, 16, 17, 20, 121, 132];

function hex(x) { return '0x' + x.toString(16); }

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

function rangesInModule(mod, prot) {
    return Process.enumerateRanges(prot).filter(function (r) {
        return r.base.compare(mod.base) >= 0 && r.base.compare(mod.base.add(mod.size)) < 0;
    });
}

function findCaptureModule() {
    const mods = Process.enumerateModules().filter(function (m) {
        return /d3d9capture\.dll$/i.test(m.name);
    });
    return mods.length ? mods[0] : null;
}

function d3d9ishModules() {
    return Process.enumerateModules().filter(function (m) {
        try {
            return m.findExportByName('Direct3DCreate9') !== null ||
                m.findExportByName('Direct3DCreate9Ex') !== null;
        } catch (e) { return false; }
    });
}

function run() {
    const report = { checks: [], hookedSlots: [] };
    function check(name, ok, detail) {
        report.checks.push({ check: name, pass: !!ok, detail: detail });
    }

    const cap = findCaptureModule();
    check('d3d9capture.dll is loaded', cap !== null,
        cap ? cap.base + ' size=' + cap.size : 'module not present — was injection successful?');
    if (!cap) { send({ tag: 'verify', report: report }); return; }

    const capCode = new IntervalSet(rangesInModule(cap, 'r-x'));
    const capAll = new IntervalSet([{ base: cap.base, size: cap.size }]);

    // ── 2. import slot ───────────────────────────────────────────────────────
    const mainMod = Process.enumerateModules()[0];
    let importPatched = false, importDetail = 'no Direct3DCreate9 import in the executable';
    try {
        mainMod.enumerateImports().forEach(function (imp) {
            if (!/Direct3DCreate9/i.test(imp.name) || !imp.slot) return;
            const v = imp.slot.readPointer();
            const inCap = capAll.hasNum(v.toUInt32());
            if (inCap) importPatched = true;
            importDetail = imp.name + ' slot ' + imp.slot + ' (rva ' +
                hex(imp.slot.sub(mainMod.base).toInt32()) + ') -> ' + v +
                (inCap ? ' [d3d9capture]' : ' [NOT hooked]');
        });
    } catch (e) { importDetail = 'enumerateImports failed: ' + e.message; }
    check('executable import slot redirected to d3d9capture', importPatched, importDetail);

    // ── 3. vtable slots ──────────────────────────────────────────────────────
    // Find every word in writable memory that points into the capture DLL and
    // sits in something vtable-shaped: its neighbouring slots must point into a
    // module that implements D3D9.
    const d3d9Code = new IntervalSet(
        d3d9ishModules().reduce(function (acc, m) {
            return acc.concat(rangesInModule(m, 'r-x'));
        }, []));

    // Where can a hooked vtable slot live?
    //
    //  * Read-only data of a d3d9-implementing module. d3d9capture patches
    //    shared vtables IN PLACE and restores the original page protection
    //    afterwards, so the hook lands in .rdata — searching only writable
    //    memory reports a false negative.
    //  * Writable memory, for a device whose vtable is per-instance (embedded
    //    in the device's own allocation, as the Windows D3D9 runtime does).
    //
    // Module data is searched first because it is small and, on a real game
    // with a proxy overlay, is where the hooks actually end up.
    const BUDGET = 512 * 1024 * 1024;

    function moduleDataRegions() {
        return d3d9ishModules().reduce(function (acc, m) {
            return acc.concat(rangesInModule(m, 'r--'));
        }, []);
    }
    function heapRegions() {
        return Process.enumerateRanges('rw-').filter(function (r) {
            return r.size <= 0x4000000;
        });
    }

    function sweep(regions, visit) {
        let used = 0, truncated = false;
        for (let k = 0; k < regions.length; k++) {
            const r = regions[k];
            if (used >= BUDGET) { truncated = true; break; }
            let buf;
            try { buf = r.base.readByteArray(r.size); } catch (e) { continue; }
            if (buf === null) continue;
            used += r.size;
            const view = new DataView(buf);
            const n = Math.floor(r.size / PSZ);
            for (let i = 0; i < n; i++) visit(view.getUint32(i * PSZ, true), r.base, i, view, n);
        }
        return truncated;
    }

    // Exact 4-byte little-endian pattern. Frida rejects a match pattern that
    // begins with a wildcard ("invalid match pattern"), so masked searches for
    // "any pointer into this module" are not available — only exact values.
    function exactPattern(value) {
        const b = [value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff];
        return b.map(function (x) { return x.toString(16).padStart(2, '0'); }).join(' ');
    }

    function isValueReferenced(regions, value) {
        const pattern = exactPattern(value);
        for (let i = 0; i < regions.length; i++) {
            let hits;
            try { hits = Memory.scanSync(regions[i].base, regions[i].size, pattern); }
            catch (e) { continue; }
            for (let j = 0; j < hits.length; j++) {
                if (hits[j].address.toUInt32() % PSZ === 0) return true;
            }
        }
        return false;
    }

    // Pass 1 — locate slots that now point into d3d9capture.
    const hooks = [];
    report.scanTruncated = sweep(moduleDataRegions().concat(heapRegions()),
        function (w, base, i, view, n) {
            if (!capCode.hasNum(w)) return;
            // Neighbouring slots must look like D3D9 code for this to be a vtable.
            let neighbours = 0;
            for (let d = -2; d <= 2; d++) {
                const j = i + d;
                if (d === 0 || j < 0 || j >= n) continue;
                if (d3d9Code.hasNum(view.getUint32(j * PSZ, true))) neighbours++;
            }
            if (neighbours < 2) return;
            hooks.push({ slotNum: base.add(i * PSZ).toUInt32(), target: w });
        });

    // Pass 2 — establish each hook's slot index.
    //
    // Walking backwards to "the first entry that is not code" guesses wrong: a
    // code pointer immediately preceding the table shifts every index by one,
    // turning a correct Present hook into an apparent off-by-one bug. A vtable's
    // start is not a matter of shape — it is whichever address the objects
    // themselves point at. So for each index d3d9capture is supposed to occupy,
    // ask whether anything in memory actually references the vtable start that
    // index implies.
    const heap = heapRegions();
    hooks.forEach(function (h) {
        let slotIndex = null, startNum = null;
        for (let i = 0; i < EXPECTED_SLOTS.length; i++) {
            const k = EXPECTED_SLOTS[i];
            const candidate = h.slotNum - k * PSZ;
            if (isValueReferenced(heap, candidate)) { slotIndex = k; startNum = candidate; break; }
        }
        const startPtr = startNum === null ? null : ptr(startNum);
        report.hookedSlots.push({
            slotAddress: ptr(h.slotNum).toString(),
            pointsTo: ptr(h.target).toString(),
            offsetInCaptureDll: hex(ptr(h.target).sub(cap.base).toInt32()),
            vtableStart: startPtr ? startPtr.toString() : '<no referencing object found>',
            slotIndex: slotIndex,
            slotName: (slotIndex !== null && SLOT_NAMES[slotIndex]) || 'not at an expected index',
            vtableInModule: (function () {
                if (!startPtr) return null;
                const m = Process.findModuleByAddress(startPtr);
                return m ? m.name : '<private: per-instance vtable>';
            })()
        });
    });

    check('at least one D3D9 vtable slot points into d3d9capture',
        report.hookedSlots.length > 0,
        report.hookedSlots.length + ' hooked slot(s) found');

    const expected = report.hookedSlots.filter(function (h) {
        return EXPECTED_SLOTS.indexOf(h.slotIndex) >= 0;
    });
    check('hooked slots are at the expected D3D9 indices',
        report.hookedSlots.length > 0 && expected.length === report.hookedSlots.length,
        expected.length + ' of ' + report.hookedSlots.length + ' at a known index');

    report.pass = report.checks.every(function (c) { return c.pass; });
    send({ tag: 'verify', report: report });
}

rpc.exports = { run: run };
setTimeout(function () {
    try { run(); } catch (e) { send({ tag: 'error', message: e.message, stack: e.stack }); }
}, 0);
