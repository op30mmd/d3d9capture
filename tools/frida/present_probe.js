/*
 * present_probe.js — behavioural verification.
 *
 * The structural scan can only produce *candidates* for the device vtable,
 * because vtables sit back-to-back in .rdata and a run of code pointers does
 * not tell you where one ends. This script settles it by experiment: it hooks
 * the candidates' slot-16 (Reset) and slot-17 (Present) code addresses and
 * reports which ones are actually called, how often, and with which `this`.
 *
 * A real device's Present fires at the frame rate. Everything else is noise.
 *
 * It also reports the game's Direct3DCreate9 import slot, which tells us
 * whether d3d9capture's IAT patch has anything to bite on.
 */

'use strict';

const PSZ = Process.pointerSize;
const VT_DEVICE_RESET = 16;
const VT_DEVICE_PRESENT = 17;

function hex(x) { return '0x' + x.toString(16); }

function describe(p) {
    if (p === null || p.isNull()) return null;
    const m = Process.findModuleByAddress(p);
    return m ? m.name + '(' + m.base + ')!' + hex(p.sub(m.base).toInt32()) : p.toString();
}

function d3d9ishModules() {
    return Process.enumerateModules().filter(function (m) {
        try {
            return m.findExportByName('Direct3DCreate9') !== null ||
                m.findExportByName('Direct3DCreate9Ex') !== null;
        } catch (e) { return /^d3d9(\.dll)?$/i.test(m.name); }
    });
}

// ── import-slot report ───────────────────────────────────────────────────────

function reportImports() {
    const mainMod = Process.enumerateModules()[0];
    const out = [];
    try {
        mainMod.enumerateImports().forEach(function (imp) {
            if (!/Direct3DCreate9/i.test(imp.name)) return;
            const rec = {
                name: imp.name,
                fromModule: imp.module,
                slot: imp.slot ? imp.slot.toString() : null,
                slotRva: imp.slot ? hex(imp.slot.sub(mainMod.base).toInt32()) : null
            };
            if (imp.slot) {
                try {
                    const cur = imp.slot.readPointer();
                    rec.currentValue = cur.toString();
                    rec.resolvesTo = describe(cur);
                } catch (e) { rec.currentValue = '<unreadable>'; }
            }
            out.push(rec);
        });
    } catch (e) {
        return { error: e.message };
    }
    return out;
}

// ── probes ───────────────────────────────────────────────────────────────────

const counters = {};

function probe(label, addr) {
    const key = label + '@' + addr;
    counters[key] = { calls: 0, thisPtrs: {}, addr: addr.toString(), where: describe(addr) };
    try {
        Interceptor.attach(addr, {
            onEnter: function (args) {
                const c = counters[key];
                c.calls++;
                // __thiscall on x86: `this` is in ECX. Sample a few distinct
                // values rather than logging every frame.
                let self;
                try {
                    self = (PSZ === 4) ? this.context.ecx : this.context.rcx;
                } catch (e) { return; }
                const s = self.toString();
                if (Object.keys(c.thisPtrs).length < 6) {
                    c.thisPtrs[s] = (c.thisPtrs[s] || 0) + 1;
                } else if (s in c.thisPtrs) {
                    c.thisPtrs[s]++;
                }
            }
        });
        return true;
    } catch (e) {
        counters[key].error = e.message;
        return false;
    }
}

/*
 * Candidate vtables are passed in from the caller (they come out of the
 * structural scan). For each we probe Present and Reset.
 */
function install(vtables) {
    const results = [];
    vtables.forEach(function (vstr) {
        const vt = ptr(vstr);
        [['present', VT_DEVICE_PRESENT], ['reset', VT_DEVICE_RESET]].forEach(function (pair) {
            let fn;
            try { fn = vt.add(pair[1] * PSZ).readPointer(); } catch (e) { return; }
            if (fn.isNull()) return;
            const mod = Process.findModuleByAddress(fn);
            if (!mod) return; // not code we can safely trampoline
            results.push({ vtable: vstr, slot: pair[0], target: describe(fn), ok: probe(vstr + ':' + pair[0], fn) });
        });
    });
    return results;
}

rpc.exports = {
    imports: reportImports,
    install: install,
    results: function () {
        return counters;
    }
};

send({ tag: 'ready', imports: reportImports(), d3d9: d3d9ishModules().map(function (m) {
    return { name: m.name, base: m.base.toString(), path: m.path };
}) });
