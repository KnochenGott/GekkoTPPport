# verify_ctrl_slots.py 01/10/2026. For every Survive AnimationController slot the lifted gekko code calls, lists the TPP controller slots (vtable 0x1422e6070) whose body has the same masked instruction stream
# (callees compared up to a depth), so a slot is only forwarded when exactly one TPP slot matches. Ambiguous slots (twin overloads) are reported, not guessed.
#   python tools/verify_ctrl_slots.py [depth]   -> research/ctrl_slot_verify.tsv   (input: research/ctrl_slots_used.txt, one hex vtable offset per line)
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctrl_slot_map as C

ROOT = Path(__file__).resolve().parent.parent
depth = int(sys.argv[1]) if len(sys.argv) > 1 else 4
mg, tp = C.load(C.MGV), C.load(C.TPP)
img = mg[1]
# Survive controller vtable: the first occurrence of 0x140bc2fe0 is slot 13 (it is also slot 96); the table is contiguous
j = img.find(struct.pack("<Q", 0x140bc2fe0))
vt_s = mg[2] + j - 13 * 8
tvt = 0x1422e6070
tsl = {}
for s in range(0, 200):
    f = struct.unpack_from("<Q", tp[1], tvt + 8 * s - tp[2])[0]
    if not (0x140000000 <= f < 0x142000000):
        break
    tsl[s] = f
tn = {s: C.norm(tp, f, depth) for s, f in tsl.items()}
used = [int(l.split()[0], 16) // 8 for l in (ROOT / "research" / "ctrl_slots_used.txt").read_text().splitlines() if l.strip()]
out = ["survive_slot\tsurvive_off\tsurvive_fn\ttpp_matches\tverdict\tid_indexed"]
for s in used:
    f = struct.unpack_from("<Q", img, vt_s + 8 * s - mg[2])[0]
    try:
        n = C.norm(mg, f, depth)
    except Exception as e:
        out.append("%d\t0x%x\t%x\t-\terror %s\t-" % (s, s * 8, f, e))
        continue
    m = [t for t, x in tn.items() if x == n]
    idx = "sub edx, dword ptr [rcx + 0xc]" in n[:3]   # instance-indexed method: the forwarder swaps this for the real controller and the id for unit 0
    out.append("%d\t0x%x\t%x\t%s\t%s\t%s" % (s, s * 8, f, ",".join("%d(0x%x)" % (t, t * 8) for t in m) or "-",
                                             "unique" if len(m) == 1 else "AMBIGUOUS" if m else "no exact match", "yes" if idx else "no"))
(ROOT / "research" / "ctrl_slot_verify.tsv").write_text(chr(10).join(out) + chr(10), encoding="utf-8")
print(chr(10).join(out))
