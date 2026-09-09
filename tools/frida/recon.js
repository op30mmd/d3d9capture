/*
 * recon.js — read-only reconnaissance of a live D3D9 process.
 *
 * Answers the questions d3d9capture's hook code has to guess at:
 *   1. Which d3d9 module is actually in use (real runtime vs. a proxy/wrapper)?
 *   2. Where is the IDirect3DDevice9 vtable, and is the device Ex or not?
 *   3. Where does the live device object live in memory, and who points at it?
 *   4. Do the hard-coded GTA IV RAGE context assumptions hold?
 *   5. Is there a Direct3DCreate9 import to patch, or is it GetProcAddress?
 *
 * Nothing is created and nothing is called: we identify D3D9 interfaces by the
 * *shape* of their vtables (a run of N consecutive pointers into d3d9's .text),
 * which is safe to do against a running game.
 */

'use strict';

const IS64 = Process.pointerSize === 8;

// Vtable entry counts for the interfaces we care about. A run of exactly this
// many consecutive code pointers uniquely identifies the interface in .rdata.
const VT_D3D9_COUNT = 17;      // IDirect3D9,       last slot: CreateDevice
const VT_D3D9EX_COUNT = 21;    // IDirect3D9Ex,     last slot: CreateDeviceEx
const VT_DEVICE_COUNT = 119;   // IDirect3DDevice9, last slot: CreateQuery
const VT_DEVICEEX_COUNT = 133; // IDirect3DDevice9Ex, last slot: ResetEx

const VT_DEVICE_RESET = 16;
const VT_DEVICE_PRESENT = 17;

function log(o) { send(o); }

// ── module helpers ───────────────────────────────────────────────────────────

function d3d9Modules() {
    return Process.enumerateModules().filter(function (m) {
        return /^d3d9(\.dll)?$/i.test(m.name);
    });
}

function moduleRanges(mod) {
    // Split a module into its executable and read-only-data ranges.
    const ranges = Process.enumerateRanges('r--').filter(function (r) {
        return r.base.compare(mod.base) >= 0 &&
            r.base.compare(mod.base.add(mod.size)) < 0;
    });
    return ranges;
}

function makeInRange(base, size) {
    const lo = base, hi = base.add(size);
    return function (p) {
        return p !== null && !p.isNull() && p.compare(lo) >= 0 && p.compare(hi) < 0;
    };
}

// ── vtable discovery ─────────────────────────────────────────────────────────

/*
 * Walk a module's non-writable pages looking for runs of consecutive pointers
 * that all land in that module's executable pages. Each such run is a C++
 * vtable. We report runs long enough to matter, keyed by length.
 */
function findVTableRuns(mod, minRun) {
    const execRanges = Process.enumerateRanges('r-x').filter(function (r) {
        return r.base.compare(mod.base) >= 0 &&
            r.base.compare(mod.base.add(mod.size)) < 0;
    });
    if (execRanges.length === 0) return [];

    function isCode(p) {
        for (let i = 0; i < execRanges.length; i++) {
            const r = execRanges[i];
            if (p.compare(r.base) >= 0 && p.compare(r.base.add(r.size)) < 0) return true;
        }
        return false;
    }

    // Candidate vtable storage: readable, non-executable, ideally non-writable.
    const dataRanges = Process.enumerateRanges('r--').filter(function (r) {
        return r.base.compare(mod.base) >= 0 &&
            r.base.compare(mod.base.add(mod.size)) < 0;
    });

    const runs = [];
    const step = Process.pointerSize;

    dataRanges.forEach(function (r) {
        let buf;
        try {
            buf = r.base.readByteArray(r.size);
        } catch (e) {
            return;
        }
        const view = new DataView(buf);
        const n = Math.floor(r.size / step);

        let runStart = -1, runLen = 0;
        for (let i = 0; i < n; i++) {
            let v;
            try {
                v = IS64 ? ptr(view.getBigUint64(i * step, true).toString())
                    : ptr(view.getUint32(i * step, true));
            } catch (e) { v = NULL; }

            if (!v.isNull() && isCode(v)) {
                if (runStart < 0) { runStart = i; runLen = 1; } else { runLen++; }
            } else {
                if (runStart >= 0 && runLen >= minRun) {
                    runs.push({ addr: r.base.add(runStart * step), len: runLen });
                }
                runStart = -1; runLen = 0;
            }
        }
        if (runStart >= 0 && runLen >= minRun) {
            runs.push({ addr: r.base.add(runStart * step), len: runLen });
        }
    });

    return runs;
}

// ── object discovery ─────────────────────────────────────────────────────────

function leBytes(p) {
    // Little-endian byte pattern for a pointer value, for Memory.scan.
    const bytes = [];
    let s = p.toString(16).padStart(Process.pointerSize * 2, '0');
    for (let i = s.length - 2; i >= 0; i -= 2) bytes.push(s.substr(i, 2));
    return bytes.join(' ');
}

/*
 * Find every object in the process whose first field is `vtable` — i.e. every
 * live instance of that interface. Scans committed read/write pages only.
 */
function findInstances(vtable, limit) {
    const pattern = leBytes(vtable);
    const hits = [];
    const ranges = Process.enumerateRanges('rw-');
    for (let i = 0; i < ranges.length && hits.length < limit; i++) {
        const r = ranges[i];
        if (r.size > 0x4000000) continue; // skip absurdly large reservations
        let found;
        try {
            found = Memory.scanSync(r.base, r.size, pattern);
        } catch (e) { continue; }
        for (let j = 0; j < found.length && hits.length < limit; j++) {
            hits.push({ obj: found[j].address, range: r.base, prot: r.protection });
        }
    }
    return hits;
}

/*
 * Find pointers *to* a given object — i.e. the fields the game keeps its device
 * in. This is what a hook DLL has to locate when it attaches late.
 */
function findReferences(target, limit) {
    const pattern = leBytes(target);
    const refs = [];
    const ranges = Process.enumerateRanges('rw-');
    for (let i = 0; i < ranges.length && refs.length < limit; i++) {
        const r = ranges[i];
        if (r.size > 0x4000000) continue;
        let found;
        try {
            found = Memory.scanSync(r.base, r.size, pattern);
        } catch (e) { continue; }
        for (let j = 0; j < found.length && refs.length < limit; j++) {
            const a = found[j].address;
            if (a.equals(target)) continue;
            const mod = Process.findModuleByAddress(a);
            refs.push({
                at: a,
                module: mod ? mod.name : null,
                rva: mod ? a.sub(mod.base) : null
            });
        }
    }
    return refs;
}

// ── import-table check ───────────────────────────────────────────────────────

function importCheck(modName) {
    const out = [];
    try {
        Module.enumerateImports(modName).forEach(function (imp) {
            if (/Direct3DCreate9/i.test(imp.name)) {
                out.push({
                    name: imp.name,
                    module: imp.module,
                    slot: imp.slot ? imp.slot.toString() : null,
                    address: imp.address ? imp.address.toString() : null
                });
            }
        });
    } catch (e) {
        return { error: e.message };
    }
    return out;
}

// ── GTA IV specific assumption check ─────────────────────────────────────────

const GTAIV_CONTEXT_RVA = 0x01295888 - 0x00400000; // 0x00E95888

function safeReadPtr(p) {
    try { return p.readPointer(); } catch (e) { return null; }
}

function describePointer(p) {
    if (p === null || p.isNull()) return null;
    const mod = Process.findModuleByAddress(p);
    const info = { value: p.toString() };
    if (mod) { info.module = mod.name; info.rva = '0x' + p.sub(mod.base).toString(16); }
    const r = Process.findRangeByAddress(p);
    if (r) info.prot = r.protection;
    return info;
}

/* Given an object pointer, guess which D3D9 interface it is from its vtable. */
function classifyObject(p, vtables) {
    const vt = safeReadPtr(p);
    if (vt === null || vt.isNull()) return null;
    const match = vtables.filter(function (v) { return v.addr.equals(vt); })[0];
    return {
        object: p.toString(),
        vtable: describePointer(vt),
        interface: match ? match.iface : 'unknown',
        vtableLen: match ? match.len : null
    };
}

// ── main ─────────────────────────────────────────────────────────────────────

function main() {
    const report = { pid: Process.id, arch: Process.arch };

    const main_ = Process.enumerateModules()[0];
    report.mainModule = { name: main_.name, base: main_.base.toString(), size: main_.size, path: main_.path };

    // 1. Which d3d9 modules are loaded, and are any of them a proxy in the
    //    game directory rather than the system runtime?
    const gameDir = main_.path.substring(0, main_.path.lastIndexOf('\\')).toLowerCase();
    report.d3d9Modules = d3d9Modules().map(function (m) {
        return {
            name: m.name, base: m.base.toString(), size: m.size, path: m.path,
            isProxy: m.path.toLowerCase().indexOf(gameDir) === 0
        };
    });

    // Any module at all that exports Direct3DCreate9 (catches renamed proxies).
    report.d3d9Exporters = [];
    Process.enumerateModules().forEach(function (m) {
        try {
            const e = Module.findExportByName(m.name, 'Direct3DCreate9');
            if (e) report.d3d9Exporters.push({ name: m.name, path: m.path, exp: e.toString() });
        } catch (err) { /* module without exports */ }
    });

    // 2. Locate the interface vtables inside each d3d9 module.
    report.vtables = [];
    const known = [];
    d3d9Modules().forEach(function (m) {
        const runs = findVTableRuns(m, 12);
        runs.forEach(function (run) {
            let iface = null;
            if (run.len === VT_DEVICEEX_COUNT) iface = 'IDirect3DDevice9Ex';
            else if (run.len === VT_DEVICE_COUNT) iface = 'IDirect3DDevice9';
            else if (run.len === VT_D3D9EX_COUNT) iface = 'IDirect3D9Ex';
            else if (run.len === VT_D3D9_COUNT) iface = 'IDirect3D9';
            if (iface) {
                known.push({ addr: run.addr, len: run.len, iface: iface, module: m.name });
                report.vtables.push({
                    module: m.name, iface: iface, len: run.len,
                    addr: run.addr.toString(),
                    rva: '0x' + run.addr.sub(m.base).toString(16)
                });
            }
        });
        // Also report the longest runs, in case the counts above are off.
        runs.sort(function (a, b) { return b.len - a.len; });
        report.longestRuns = (report.longestRuns || []).concat(
            runs.slice(0, 6).map(function (r) {
                return { module: m.name, len: r.len, addr: r.addr.toString(), rva: '0x' + r.addr.sub(m.base).toString(16) };
            }));
    });

    // 3. Find the live device instance(s).
    report.devices = [];
    known.filter(function (v) { return v.iface.indexOf('Device') >= 0; }).forEach(function (v) {
        const hits = findInstances(v.addr, 8);
        hits.forEach(function (h) {
            const present = safeReadPtr(v.addr.add(VT_DEVICE_PRESENT * Process.pointerSize));
            report.devices.push({
                object: h.obj.toString(),
                iface: v.iface,
                vtable: v.addr.toString(),
                inModule: (function () {
                    const m = Process.findModuleByAddress(h.obj);
                    return m ? m.name : '<heap>';
                })(),
                presentSlot: describePointer(present)
            });
        });
    });

    // 4. Find the live factory instance(s).
    report.factories = [];
    known.filter(function (v) { return v.iface.indexOf('Device') < 0; }).forEach(function (v) {
        findInstances(v.addr, 8).forEach(function (h) {
            report.factories.push({
                object: h.obj.toString(), iface: v.iface, vtable: v.addr.toString(),
                inModule: (function () {
                    const m = Process.findModuleByAddress(h.obj);
                    return m ? m.name : '<heap>';
                })()
            });
        });
    });

    // 5. Who points at the device? This is what a late-attaching hook must find.
    report.deviceRefs = [];
    if (report.devices.length > 0) {
        const dev = ptr(report.devices[0].object);
        report.deviceRefs = findReferences(dev, 24).map(function (r) {
            return { at: r.at.toString(), module: r.module, rva: r.rva ? '0x' + r.rva.toString(16) : null };
        });
    }

    // 6. Check the DLL's hard-coded GTA IV RAGE context assumption.
    if (/gtaiv\.exe$/i.test(main_.path)) {
        const ctxAddr = main_.base.add(GTAIV_CONTEXT_RVA);
        const ctx = safeReadPtr(ctxAddr);
        const check = {
            contextAddr: ctxAddr.toString(),
            contextRva: '0x' + GTAIV_CONTEXT_RVA.toString(16),
            contextValue: describePointer(ctx)
        };
        if (ctx !== null && !ctx.isNull()) {
            check.at_plus_0x00 = classifyObject(ctx, known);
            const dev160 = safeReadPtr(ctx.add(0x160));
            check.at_plus_0x160_value = describePointer(dev160);
            if (dev160 !== null && !dev160.isNull()) {
                check.at_plus_0x160 = classifyObject(dev160, known);
            }
        }
        report.gtaivContext = check;
    }

    // 7. Import table — is there anything to patch?
    report.imports = importCheck(main_.name);

    log({ tag: 'report', report: report });
}

rpc.exports = { run: main };
main();
