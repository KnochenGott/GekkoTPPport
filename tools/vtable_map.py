# vtable_map.py 30/09/2026. Finds the Survive vtable that corresponds to a TPP vtable and maps slot to slot: candidates are runs of code pointers in mgv.exe .rdata of similar
# length; a slot matches when the function bodies have the same instruction stream (tools/ctrl_slot_map.norm, branch and call targets masked). Output research/vtable_maps.tsv.
#   python tools/vtable_map.py NAME 0x1424c8ee0 [SLOTS]
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctrl_slot_map as C

ROOT = Path(__file__).resolve().parent.parent


def runs(exe, minlen):
    pe, img, base = exe
    out = []
    for sec in pe.sections:
        if sec.Name.rstrip(b"\0") not in (b".rdata", b".data"):
            continue
        a0 = base + sec.VirtualAddress
        a1 = a0 + sec.Misc_VirtualSize
        cur = []
        a = a0
        while a < a1:
            v = struct.unpack_from("<Q", img, a - base)[0]
            if 0x140001000 <= v < 0x142000000:
                cur.append((a, v))
            else:
                if len(cur) >= minlen:
                    out.append(cur)
                cur = []
            a += 8
    return out


def main():
    name, tva = sys.argv[1], int(sys.argv[2], 16)
    mg, tp = C.load(C.MGV), C.load(C.TPP)
    tslots = []
    for s in range(400):
        f = struct.unpack_from("<Q", tp[1], tva + 8 * s - tp[2])[0]
        if not (0x140000000 <= f < 0x142000000):
            break
        tslots.append(f)
    n = len(tslots)
    tnorm = [tuple(C.norm(tp, f, 0)) for f in tslots]
    tset = {}
    for i, t in enumerate(tnorm):
        tset.setdefault(t, []).append(i)
    best = (0.0, None, None)
    for r in runs(mg, max(4, n // 2)):
        if len(r) > 3 * n:
            continue
        hits, mapping = 0, []
        for k, (a, f) in enumerate(r[:n + 8]):
            try:
                nm = tuple(C.norm(mg, f, 0))
            except Exception:
                mapping.append((k, f, None)); continue
            m = tset.get(nm)
            mapping.append((k, f, m))
            if m:
                hits += 1
        score = hits / max(1, min(len(r), n))
        if score > best[0]:
            best = (score, r[0][0], mapping)
    with (ROOT / "research" / "vtable_maps.tsv").open("a", encoding="utf-8") as f:
        f.write("# %s TPP %x (%d slots): best survive vtable %s score %.2f\n" % (name, tva, n, hex(best[1]) if best[1] else "none", best[0]))
        if best[2]:
            for k, fn, m in best[2]:
                f.write("%s\t%d\t%x\t%s\n" % (name, k, fn, ",".join(map(str, m)) if m else ""))
    print(name, "best score %.2f at %s" % (best[0], hex(best[1]) if best[1] else None))


if __name__ == "__main__":
    main()
