# port_kit.py 29/09/2026. Port kit: turns one Survive (installed mgv.exe) function into a work package for porting it to TPP:
# the matching Ghidra decompile, every callee classified (internal / TPP twin / virtual / Survive-only), resolved constants and
# a C++ skeleton. It does not translate the code by itself; it removes the lookup work. See docs/DOCUMENTATION.md.
#   python tools/port_kit.py 0x140f83c30 [--depth 1] [--range 0x140f60000 0x140fb0000] [--no-ghidra] [--fuzzy]
#   python tools/port_kit.py --selftest
# Needs research/mgv/mgv_map.tsv (python tools/mgv_map.py build) and, for the decompile, the Ghidra HTTP server on :8086.
import argparse, bisect, collections, difflib, pickle, struct, sys, urllib.request
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mgv_map
import pathcode

ROOT = Path(__file__).resolve().parent.parent
MGV = mgv_map.EXE
import paths
TPP = str(paths.TPP_EXE)
OUT = ROOT / "build" / "port"
CACHE = ROOT / "build" / "port_kit_cache"
GHIDRA = "http://127.0.0.1:8086"
VTMAP = ROOT / "research" / "fullctrl_vtable_map.tsv"
FOX_DICTS = paths.MODBLDR.parent   # optional name dictionaries (modbldr-tools folder)
DEFAULT_RANGE = (0x140f60000, 0x140fb0000)   # the gekko module in mgv.exe (installed build)

# Known Survive -> TPP retail twins, each verified by shape and prologue (docs/DOCUMENTATION.md). Extend as ports find more.
KNOWN_TWINS = {
    0x14198d440: (0x141a6bca0, "AnimControl::AddPlugins"),
    0x1419ee790: (0x141ace850, "CcAction::RegistAction"),
    0x1419ee930: (0x141ace9e0, "VelocityControlAction::Create"),
    0x140bc1a20: (0x140af0790, "AnimationController create"),
    0x140cadbf0: (0x140bf5dd0, "CreatePlugin"),
    0x141989050: (0x140bff050, "global manager getter [G+8], body identical in both builds (tools/ctrl_slot_map.py norm, 30/09)"),
    0x140e96b60: (None, "ActionController mailbox (Survive-only: SsdNpc/GameObject framework)"),
}
FLOAT_OPS = {"movss", "mulss", "addss", "subss", "divss", "comiss", "ucomiss", "maxss", "minss", "cvtss2sd", "sqrtss",
             "movaps", "mulps", "addps", "subps", "andps", "xorps", "orps", "movups", "shufps"}

cs = Cs(CS_ARCH_X86, CS_MODE_64)
cs.detail = True


class Exe:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories([3])
        self.img = self.pe.get_memory_mapped_image()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.funcs = sorted({(self.base + e.struct.BeginAddress, self.base + e.struct.EndAddress)
                             for e in self.pe.DIRECTORY_ENTRY_EXCEPTION})
        self.starts = [f[0] for f in self.funcs]

    def chunk(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        return self.funcs[i] if i >= 0 and va < self.funcs[i][1] else None

    def bytes(self, va, n):
        return self.img[va - self.base:va - self.base + n]

    def insns(self, start, end):
        return list(cs.disasm(self.bytes(start, end - start), start))

    def f32(self, va):
        return struct.unpack_from("<f", self.img, va - self.base)[0]


def mnemonics(exe, va, n=40):
    out = []
    for i in cs.disasm(exe.bytes(va, 200), va):
        out.append(i.mnemonic)
        if len(out) >= n or i.mnemonic in ("ret", "int3"):
            break
    return out


def tpp_index(tpp):
    """masked prologue -> [function starts] for every TPP function, cached (about a minute the first time)."""
    f = CACHE / "tpp_index.pkl"
    if f.exists():
        return pickle.loads(f.read_bytes())
    idx = collections.defaultdict(list)
    for n, (a, e) in enumerate(tpp.funcs):
        k = mgv_map.mask(tpp.bytes(a, min(e - a, 64)), a)
        if len(k) >= mgv_map.MIN_BYTES:
            idx[k].append(a)
        if n % 20000 == 0:
            print("  indexing TPP functions %d/%d" % (n, len(tpp.funcs)), file=sys.stderr)
    CACHE.mkdir(parents=True, exist_ok=True)
    f.write_bytes(pickle.dumps(dict(idx)))
    return dict(idx)


def hash_names():
    """48-bit StringId -> name from tools/port_kit_names.txt and the Fox_Parser dictionaries (cached)."""
    f = CACHE / "hash_names.pkl"
    if f.exists():
        return pickle.loads(f.read_bytes())
    names = {}
    for p in [ROOT / "tools" / "port_kit_names.txt"] + sorted(FOX_DICTS.glob("*_dictionary.txt")):
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
            s = line.strip()
            if s and not s.startswith("#"):
                try:
                    names[pathcode.string_id(s) & 0xFFFFFFFFFFFF] = s
                except Exception:
                    pass
    CACHE.mkdir(parents=True, exist_ok=True)
    f.write_bytes(pickle.dumps(names))
    return names


def ghidra_decompile(g):
    try:
        with urllib.request.urlopen("%s/decompile_function?address=0x%x" % (GHIDRA, g), timeout=180) as r:
            return r.read().decode("utf-8", "ignore")
    except Exception as e:
        return "// Ghidra decompile unavailable (%s)\n" % e


def load_vtmap():
    slots = {}
    if VTMAP.exists():
        for l in VTMAP.read_text(encoding="utf-8").splitlines()[1:]:
            f = l.split("\t")
            if len(f) >= 6 and f[5]:
                slots[int(f[1], 16)] = (float(f[3]), int(f[4]) * 8)
    return slots


def classify(exe, tpp, idx, va, rng, fuzzy):
    """One callee: (kind, detail), kind in internal / known / twin / ambiguous / fuzzy / helper / survive-only."""
    if va in KNOWN_TWINS:
        t, why = KNOWN_TWINS[va]
        return ("known", "TPP 0x%x %s" % (t, why)) if t else ("survive-only", why)
    if rng[0] <= va < rng[1]:
        return ("internal", "")
    ch = exe.chunk(va)
    if not ch:
        return ("helper", "no .pdata entry: compiler/CRT leaf helper (memset, chkstk, cookie); use the TPP equivalent")
    key = mgv_map.mask(exe.bytes(va, min(ch[1] - va, 64)), va)
    hits = idx.get(key, []) if len(key) >= mgv_map.MIN_BYTES else []
    if len(hits) == 1:
        return ("twin", "TPP 0x%x exact masked prologue" % hits[0])
    if len(hits) > 1:
        return ("ambiguous", "TPP " + " ".join("0x%x" % h for h in hits[:4]))
    if fuzzy and tpp is not None:
        mine = mnemonics(exe, va)
        best = []
        for a, _ in tpp.funcs:
            m = mnemonics(tpp, a)
            if m[:4] != mine[:4]:
                continue
            r = difflib.SequenceMatcher(None, mine, m).ratio()
            if r > 0.85:
                best.append((r, a))
        best.sort(reverse=True)
        if best:
            return ("fuzzy", "TPP " + " ".join("0x%x (%.2f)" % (a, r) for r, a in best[:3]))
    return ("survive-only", "no exact twin (try --fuzzy)")


def analyse(exe, va):
    """Direct calls, virtual call offsets, float constants and hash immediates of the .pdata chunk holding va."""
    ch = exe.chunk(va)
    calls, virt, floats, hashes = collections.Counter(), collections.Counter(), {}, {}
    for i in exe.insns(ch[0], ch[1]):
        if i.mnemonic == "call":
            o = i.operands[0]
            if o.type == X86_OP_IMM:
                calls[o.imm] += 1
            elif o.type == X86_OP_MEM and o.mem.base and i.reg_name(o.mem.base) != "rip":
                virt[o.mem.disp] += 1
        for o in i.operands:
            if o.type == X86_OP_MEM and o.mem.base and i.reg_name(o.mem.base) == "rip" and i.mnemonic in FLOAT_OPS:
                t = i.address + i.size + o.mem.disp
                try:
                    floats[t] = exe.f32(t)
                except Exception:
                    pass
            if o.type == X86_OP_IMM and i.mnemonic == "movabs" and o.imm >= (1 << 40):
                hashes[o.imm & 0xFFFFFFFFFFFF] = i.address
    return ch, calls, virt, floats, hashes


def report(exe, tpp, idx, va, rng, args, names, vt, done):
    g = mgv_map.lookup(va, True)
    ghid = g[1] if g else None   # the Ghidra function that contains this chunk (a chunk can sit inside a larger function)
    ch, calls, virt, floats, hashes = analyse(exe, va)
    lines = ["# Port kit report: disk 0x%x (chunk 0x%x-0x%x, %d bytes)" % (va, ch[0], ch[1], ch[1] - ch[0]),
             "Ghidra project function: %s" % ("0x%x (%s), disk chunk is +0x%x into disk function 0x%x" % (g[1], g[2], g[4], g[0]) if g else "no mapping (run tools/mgv_map.py build)"), "",
             "## Callees (%d distinct direct targets)" % len(calls)]
    internal, kinds = [], {}
    for t, n in sorted(calls.items()):
        kind, detail = classify(exe, tpp, idx, t, rng, args.fuzzy)
        kinds[t] = (kind, detail)
        if kind == "internal":
            internal.append(t)
        lines.append("- 0x%x x%d  **%s** %s" % (t, n, kind, detail))
    lines += ["", "## Virtual calls (call [reg+disp]); offsets are Survive object layouts"]
    for d, n in sorted(virt.items()):
        note = ""
        if d in vt and vt[d][0] >= 0.8:
            note = "  controller slot maybe TPP +0x%x (map score %.2f; unreliable for tiny getters)" % (vt[d][1], vt[d][0])
        lines.append("- +0x%x x%d%s" % (d, n, note))
    lines += ["", "## Constants"]
    for t, v in sorted(floats.items()):
        if v != v or (v != 0 and abs(v) < 1e-6):
            continue   # NaN and denormals are packed integer or mask data read as float
        deg = "  (= %.1f deg if radians)" % (v * 57.29578) if 0.05 <= abs(v) <= 6.3 else ""
        lines.append("- float @0x%x = %g%s" % (t, v, deg))
    for h, a in sorted(hashes.items(), key=lambda x: x[1]):
        lines.append("- StringId 0x%012x @0x%x  %s" % (h, a, names.get(h, "(unresolved)")))
    d = OUT / ("%x" % va)
    d.mkdir(parents=True, exist_ok=True)
    (d / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if not args.no_ghidra and ghid:
        (d / "decompile.c").write_text(ghidra_decompile(ghid), encoding="utf-8")
    sk = ["// skeleton for disk 0x%x, generated by tools/port_kit.py. Fill the body from decompile.c, keep offsets in TPP form." % va,
          "#include <cstdint>", "namespace Port {", ""]
    sk += ["// 0x%x: %s %s" % (t, k, dt) for t, (k, dt) in sorted(kinds.items())]
    sk += ["", "void Ported_%x(/* args */)" % va, "{", "    // TODO", "}", "", "}"]
    (d / "skeleton.cpp").write_text("\n".join(sk) + "\n", encoding="utf-8")
    print("\n".join(lines[:4 + min(len(calls), 25)]))
    print("wrote %s (report.md, decompile.c, skeleton.cpp)" % d)
    done.add(va)
    if args.depth > 1:
        sub = argparse.Namespace(**{**vars(args), "depth": args.depth - 1})
        for t in internal:
            if t not in done and len(done) < 40:
                report(exe, tpp, idx, t, rng, sub, names, vt, done)


def selftest():
    exe = Exe(MGV)
    g = mgv_map.lookup(0x140f83c30, True)
    assert g and g[1] == 0x140f84520, g
    ch, calls, virt, floats, hashes = analyse(exe, 0x140f83c30)
    assert ch[0] == 0x140f83c30 and len(calls) > 3 and 0xc8 in virt, (calls, virt)
    assert classify(exe, None, {}, 0x14198d440, DEFAULT_RANGE, False)[0] == "known"
    assert classify(exe, None, {}, 0x140f83240, DEFAULT_RANGE, False)[0] == "internal"
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", nargs="?")
    ap.add_argument("--depth", type=int, default=1)
    ap.add_argument("--range", nargs=2, type=lambda x: int(x, 16), default=DEFAULT_RANGE)
    ap.add_argument("--no-ghidra", action="store_true")
    ap.add_argument("--fuzzy", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    exe, tpp = Exe(MGV), Exe(TPP)
    report(exe, tpp, tpp_index(tpp), int(a.addr, 16), tuple(a.range), a, hash_names(), load_vtmap(), set())


if __name__ == "__main__":
    main()