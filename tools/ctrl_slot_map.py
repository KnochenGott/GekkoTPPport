# ctrl_slot_map.py 30/09/2026. Maps the Survive AnimationController vtable slots (installed mgv.exe) to TPP's AnimationControllerImpl slots (0x1422e6070) by comparing
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
# whole function bodies with rip-relative operands, call targets and immediates that look like addresses masked. Exact = identical instruction stream.
#   python tools/ctrl_slot_map.py            -> research/ctrl_slot_map.tsv (survive_off, survive_fn, tpp_off, tpp_fn, quality)
import struct
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

ROOT = Path(__file__).resolve().parent.parent
MGV = str(paths.MGV_EXE)
TPP = str(paths.TPP_EXE)
cs = Cs(CS_ARCH_X86, CS_MODE_64)
cs.detail = True


def load(p):
    pe = pefile.PE(p, fast_load=True)
    pe.parse_data_directories([3])
    return pe, pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase


def norm(exe, va, depth=2):
    pe, img, base = exe
    end = None
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        if base + e.struct.BeginAddress == va:
            end = base + e.struct.EndAddress
            break
    out = []
    n = 0
    for i in cs.disasm(img[va - base:va - base + (end - va if end else 0x60)], va):
        t = i.op_str
        if "rip" in t:
            t = "rip"
        if i.mnemonic.startswith("j") and i.op_str.startswith("0x"):
            t = "B"   # branch targets are absolute addresses
        if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            t = "T" if i.mnemonic == "call" else "J"
            if depth > 0 and len(out) < 6:   # short stubs (tail jumps) differ only by their target: compare one level down
                try: t += str(hash(tuple(norm(exe, int(i.op_str, 16), depth - 1))))
                except Exception: pass
        out.append(i.mnemonic + " " + t)
        n += 1
        if n > 400 or (end is None and i.mnemonic in ("ret", "jmp")):
            break
    return out


def main():
    mg, tp = load(MGV), load(TPP)
    rows = [l.split("\t") for l in (ROOT / "research" / "fullctrl_vtable_map.tsv").read_text(encoding="utf-8").splitlines()[1:]]
    tvt = 0x1422e6070
    tslots = {}
    for s in range(0, 200):
        f = struct.unpack_from("<Q", tp[1], tvt + 8 * s - tp[2])[0]
        if not (0x140000000 <= f < 0x142000000):
            break
        tslots[s] = (f, norm(tp, f))
    # slow-changing slot shift (survive - tpp) from the unique exact matches, per block of 10 slots
    uniq = {}
    for r in rows:
        sn = norm(mg, int(r[2], 16))
        c = [ts for ts, (tf, tn) in tslots.items() if tn == sn]
        if len(c) == 1:
            uniq.setdefault(int(r[0]) // 10, []).append(int(r[0]) - c[0])
    shift = {k: sorted(v)[len(v) // 2] for k, v in uniq.items()}
    out = ["survive_slot\tsurvive_off\tsurvive_fn\ttpp_slot\ttpp_off\ttpp_fn\tquality"]
    for r in rows:
        s, off, fn = int(r[0]), r[1], int(r[2], 16)
        sn = norm(mg, fn)
        best, bq = None, 0.0
        cands = [ts for ts, (tf, tn) in tslots.items() if tn == sn]
        if cands:
            best, bq = min(cands, key=lambda ts: abs((s - ts) - shift.get(s // 10, 0))), 1.0
        if best is None:
            import difflib
            for ts, (tf, tn) in tslots.items():
                q = difflib.SequenceMatcher(None, sn, tn).ratio() if abs(len(sn) - len(tn)) < 12 else 0
                if q > bq:
                    best, bq = ts, q
        t = tslots.get(best)
        out.append("%d\t%s\t%x\t%s\t%s\t%s\t%.2f" % (s, off, fn, best if best is not None else "", hex(best * 8) if best is not None else "", hex(t[0]) if t else "", bq))
    (ROOT / "research" / "ctrl_slot_map.tsv").write_text("\n".join(out) + "\n", encoding="utf-8")
    ex = sum(1 for l in out[1:] if l.endswith("1.00"))
    print("survive slots %d, exact matches %d -> research/ctrl_slot_map.tsv" % (len(out) - 1, ex))


if __name__ == "__main__":
    main()
