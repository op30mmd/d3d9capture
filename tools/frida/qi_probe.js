/*
 * qi_probe.js — settle "is this candidate really an IDirect3DDevice9?" using
 * COM's own answer instead of vtable-shape guesswork.
 *
 * Structural scanning can only narrow the field: D3D9 vtables sit back-to-back
 * in .rdata, so "119 consecutive code pointers" also matches a texture vtable
 * followed by its neighbours. QueryInterface is definitive, cheap, and safe on
 * anything that really is a COM object — a texture simply answers E_NOINTERFACE.
 *
 * This validates the identification strategy before it is committed to C++.
 */

'use strict';

const PSZ = Process.pointerSize;

const IID_IDirect3DDevice9 = '96 3b 22 d0 7a bf fd 43 92 bd a4 3b 0d 82 b9 eb';
const IID_IDirect3DDevice9Ex = 'ce 10 8b b1 49 26 5a 40 87 0f 95 f7 77 d4 31 3a';

function hex(x) { return '0x' + x.toString(16); }

function describe(p) {
    if (p === null || p.isNull()) return null;
    const m = Process.findModuleByAddress(p);
    return m ? m.name + '(' + m.base + ')!' + hex(p.sub(m.base).toInt32()) : p.toString();
}

function iidBuf(spec) {
    const bytes = spec.split(' ').map(function (b) { return parseInt(b, 16); });
    const buf = Memory.alloc(16);
    buf.writeByteArray(bytes);
    return buf;
}

// ── interval sets (numeric binary search) ────────────────────────────────────

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

// ── modules that implement D3D9 (runtime or proxy) ───────────────────────────

function d3d9ishModules() {
    return Process.enumerateModules().filter(function (m) {
        try {
            return m.findExportByName('Direct3DCreate9') !== null ||
                m.findExportByName('Direct3DCreate9Ex') !== null;
        } catch (e) { return false; }
    });
}

function execRangesOfModule(m) {
    return Process.enumerateRanges('r-x').filter(function (r) {
        return r.base.compare(m.base) >= 0 && r.base.compare(m.base.add(m.size)) < 0;
    });
}

function writableSections(mod) {
    const base = mod.base, out = [];
    try {
        if (base.readU16() !== 0x5a4d) return out;
        const nt = base.add(base.add(0x3c).readU32());
        if (nt.readU32() !== 0x00004550) return out;
        const numSections = nt.add(6).readU16();
        const optSize = nt.add(20).readU16();
        const sec = nt.add(24).add(optSize);
        for (let i = 0; i < numSections; i++) {
            const sh = sec.add(i * 40);
            if (sh.add(36).readU32() & 0x80000000) {
                out.push({ base: base.add(sh.add(12).readU32()), size: sh.add(8).readU32() });
            }
        }
    } catch (e) { /* ignore */ }
    return out;
}

// ── main ─────────────────────────────────────────────────────────────────────

/*
 * `where`: 'sections' sweeps only module writable PE sections (what the DLL
 * currently does); 'heap' additionally sweeps every committed read/write
 * region, which is where a RAGE-style engine keeps its dynamically allocated
 * graphics context.
 */
function run(where) {
    const mods = d3d9ishModules();
    const mainMod = Process.enumerateModules()[0];
    const readable = new IntervalSet(Process.enumerateRanges('r--'));

    // Per-module code sets: a genuine vtable's slots all live in ONE module.
    const perModuleCode = mods.map(function (m) {
        return { mod: m, code: new IntervalSet(execRangesOfModule(m)) };
    });

    const candidates = {};   // vtable -> { instance, module }

    let regions = [];
    [mainMod].concat(mods).forEach(function (m) {
        writableSections(m).forEach(function (sec) {
            regions.push({ base: sec.base, size: sec.size, label: m.name });
        });
    });
    if (where === 'heap') {
        Process.enumerateRanges('rw-').forEach(function (r) {
            if (r.size > 0x2000000) return;       // skip huge reservations
            regions.push({ base: r.base, size: r.size, label: 'rw' });
        });
    }
    send({ tag: 'progress', regions: regions.length,
           bytes: regions.reduce(function (a, r) { return a + r.size; }, 0) });

    [{ dummy: true }].forEach(function () {
        regions.forEach(function (sec) {
            const m = { name: sec.label };
            let buf;
            try { buf = sec.base.readByteArray(sec.size); } catch (e) { return; }
            if (buf === null) return;
            const view = new DataView(buf);
            const n = Math.floor(sec.size / PSZ);
            for (let i = 0; i < n; i++) {
                const raw = view.getUint32(i * PSZ, true);
                if (raw === 0 || !readable.hasNum(raw)) continue;
                const obj = ptr(raw);
                let vt;
                try { vt = obj.readPointer(); } catch (e) { continue; }
                if (vt.isNull() || !readable.hasNum(vt.toUInt32())) continue;

                // Strict pre-filter: the first three slots (QueryInterface,
                // AddRef, Release — present on EVERY COM object) must all be
                // code inside one and the same d3d9-implementing module.
                let owner = null;
                for (let k = 0; k < perModuleCode.length; k++) {
                    const pm = perModuleCode[k];
                    let ok = true;
                    for (let s = 0; s < 3; s++) {
                        let v;
                        try { v = vt.add(s * PSZ).readPointer(); } catch (e) { ok = false; break; }
                        if (v.isNull() || !pm.code.hasNum(v.toUInt32())) { ok = false; break; }
                    }
                    if (ok) { owner = pm.mod; break; }
                }
                if (!owner) continue;

                const key = vt.toString();
                if (!(key in candidates)) {
                    candidates[key] = {
                        vtable: key, instance: obj.toString(),
                        module: owner.name, modulePath: owner.path,
                        moduleBase: owner.base.toString(),
                        vtableRva: hex(vt.sub(owner.base).toInt32()),
                        foundIn: m.name, count: 0
                    };
                }
                candidates[key].count++;
            }
        });
    });

    const list = Object.keys(candidates).map(function (k) { return candidates[k]; });
    send({ tag: 'candidates', count: list.length, list: list });

    // Now ask each candidate what it is.
    const devIID = iidBuf(IID_IDirect3DDevice9);
    const devExIID = iidBuf(IID_IDirect3DDevice9Ex);
    const outPtr = Memory.alloc(PSZ);

    list.forEach(function (c) {
        const obj = ptr(c.instance);
        const vt = ptr(c.vtable);
        let qi, release;
        try {
            // COM interface methods on x86 Windows are STDMETHODCALLTYPE
            // (__stdcall) with `this` as the first *stack* argument — not
            // __thiscall. Getting this wrong shifts every argument.
            qi = new NativeFunction(vt.add(0 * PSZ).readPointer(), 'int32',
                ['pointer', 'pointer', 'pointer'], 'stdcall');
            release = new NativeFunction(vt.add(2 * PSZ).readPointer(), 'uint32',
                ['pointer'], 'stdcall');
        } catch (e) { c.qiError = e.message; return; }

        function ask(iid) {
            try {
                outPtr.writePointer(NULL);
                const hr = qi(obj, iid, outPtr);
                const got = outPtr.readPointer();
                if (hr === 0 && !got.isNull()) { release(got); }
                return { hr: hr >>> 0, got: got.toString() };
            } catch (e) { return { error: e.message }; }
        }

        c.asDevice9 = ask(devIID);
        c.asDevice9Ex = ask(devExIID);
        c.isDevice = (c.asDevice9 && c.asDevice9.hr === 0) || (c.asDevice9Ex && c.asDevice9Ex.hr === 0);
        c.isEx = !!(c.asDevice9Ex && c.asDevice9Ex.hr === 0);
    });

    send({ tag: 'qi', devices: list.filter(function (c) { return c.isDevice; }), all: list });
}

rpc.exports = { run: run };
