# mgv_map.py 29/09/2026. Pairs functions of the installed mgv.exe (retail, .pdata starts) with the outdated Ghidra
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
# Survive project (research/mgv/ghidra_prints.tsv from tools/ghidra/ExportFunctionPrints.java). The two builds differ by
# a non-constant shift (0x140f8de80 Ghidra = 0x140f8d3f0 disk, MEASURED), so pairing is by masked prologue bytes:
# rip displacements, branch targets and address-like immediates are blanked with the same capstone rules on both sides.
# Unique matches are anchors; ambiguous ones take the candidate whose shift is closest to the nearest anchors' shift.
#   python tools/mgv_map.py build           -> research/mgv/mgv_map.tsv
#   python tools/mgv_map.py d2g 0x140f831f0 -> Ghidra function containing/for a disk address
#   python tools/mgv_map.py g2d 0x140f8de80 -> disk function for a Ghidra address
#   python tools/mgv_map.py test            -> self-check on the measured pair
import bisect, sys
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

ROOT = Path(__file__).resolve().parent.parent
EXE = str(paths.MGV_EXE)
PRINTS = ROOT / "research" / "mgv" / "ghidra_prints.tsv"
OUT = ROOT / "research" / "mgv" / "mgv_map.tsv"
K = 48          # prologue bytes compared
MIN_BYTES = 12  # shorter prints are too generic to pair
cs = Cs(CS_ARCH_X86, CS_MODE_64)
cs.detail = True


def mask(code, va):
    """Masked prologue: whole instructions within K bytes, variable operands zeroed."""
    out = bytearray()
    for i in cs.disasm(code, va):
        if len(out) + i.size > K:
            break
        b = bytearray(i.bytes)
        enc = i.disp_offset, i.disp_size, i.imm_offset, i.imm_size
        if enc[1] and "rip" in i.op_str:
            b[enc[0]:enc[0] + enc[1]] = bytes(enc[1])
        if enc[3]:
            imm = int.from_bytes(b[enc[2]:enc[2] + enc[3]], "little")
            if i.group(1) or i.group(2) or i.group(7) or 0x140000000 <= imm < 0x150000000:  # jump, call, relative
                b[enc[2]:enc[2] + enc[3]] = bytes(enc[3])
        out += b
        if i.mnemonic in ("ret", "int3", "jmp"):
            break
    return bytes(out)


def disk_prints():
    pe = pefile.PE(EXE, fast_load=True)
    pe.parse_data_directories([3])
    img, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    fs = sorted({(base + e.struct.BeginAddress, e.struct.EndAddress - e.struct.BeginAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION})
    return [(va, sz, mask(img[va - base:va - base + min(sz, 64)], va)) for va, sz in fs]


def ghidra_prints():
    rows = []
    for line in PRINTS.read_text(encoding="utf-8").splitlines()[1:]:
        entry, size, name, hexb = line.split("\t")
        va = int(entry, 16)
        rows.append((va, int(size), name, mask(bytes.fromhex(hexb), va)))
    return rows


def pair(disk, ghid):
    """disk [(va,size,key)], ghid [(va,size,name,key)] -> {disk_va: (ghidra_va, name, method)}"""
    dk, gk = {}, {}
    for va, _, k in disk:
        if len(k) >= MIN_BYTES: dk.setdefault(k, []).append(va)
    for va, _, name, k in ghid:
        if len(k) >= MIN_BYTES: gk.setdefault(k, []).append((va, name))
    res = {}
    for k, ds in dk.items():
        gs = gk.get(k)
        if gs and len(ds) == 1 and len(gs) == 1:
            res[ds[0]] = (gs[0][0], gs[0][1], "unique")
    anchors = sorted((d, g[0] - d) for d, g in res.items())
    avas = [a[0] for a in anchors]

    def shift_near(va):
        i = bisect.bisect_left(avas, va)
        near = [anchors[j][1] for j in (i - 1, i) if 0 <= j < len(anchors)]
        return near
    for k, ds in dk.items():
        gs = gk.get(k)
        if not gs or (len(ds) == 1 and len(gs) == 1):
            continue
        for d in ds:
            near = shift_near(d)
            if not near: continue
            want = near[0] if len(near) == 1 else (near[0] + near[1]) // 2
            g = min(gs, key=lambda x: abs((x[0] - d) - want))
            if abs((g[0] - d) - want) <= 0x400 and d not in res:
                res[d] = (g[0], g[1], "shift")
    return res


def load_map():
    rows = [l.split("\t") for l in OUT.read_text(encoding="utf-8").splitlines()[1:]]
    return sorted((int(d, 16), int(g, 16), n, m) for d, g, n, m in rows)


def lookup(va, by_disk):
    rows = load_map()
    if not by_disk:
        rows = sorted((g, d, n, m) for d, g, n, m in rows)
    keys = [r[0] for r in rows]
    i = bisect.bisect_right(keys, va) - 1
    if i < 0: return None
    a, b, n, m = rows[i]
    return a, b, n, m, va - a   # function start on the asked side, its pair, name, method, offset into the function


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "build"
    if cmd == "build":
        disk, ghid = disk_prints(), ghidra_prints()
        res = pair(disk, ghid)
        with OUT.open("w", encoding="utf-8") as f:
            f.write("disk\tghidra\tname\tmethod\n")
            for d in sorted(res):
                g, n, m = res[d]
                f.write("%x\t%x\t%s\t%s\n" % (d, g, n, m))
        u = sum(1 for v in res.values() if v[2] == "unique")
        print("disk %d, ghidra %d, paired %d (unique %d, by shift %d) -> %s" % (len(disk), len(ghid), len(res), u, len(res) - u, OUT))
    elif cmd in ("d2g", "g2d"):
        for a in sys.argv[2:]:
            r = lookup(int(a, 16), cmd == "d2g")
            if not r: print(a, "no mapping"); continue
            start, other, name, m, off = r
            side = ("disk", "ghidra") if cmd == "d2g" else ("ghidra", "disk")
            print("%s %s = fn %x +0x%x -> %s %x +0x%x  %s (%s)" % (side[0], a, start, off, side[1], other, off, name, m))
    elif cmd == "test":
        # measured pair 29/09/2026: Ghidra 0x140f8de80 (Ai update) = disk 0x140f8d3f0, byte-identical prologue
        r = lookup(0x140f8d3f0, True)
        assert r and r[1] == 0x140f8de80, r
        print("ok")


if __name__ == "__main__":
    main()
