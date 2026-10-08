# verify_twins.py 30/09/2026. Checks every native (TPP twin) call of the lifted gekko module: the Survive callee and the TPP function must have the same instruction stream
# (rip-relative operands and call targets masked, call targets are masked, so it proves the same body shape, not the same callees). Writes research/twin_verify.tsv; only "same" pairs may go into the native gate.
import re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctrl_slot_map as C

ROOT = Path(__file__).resolve().parent.parent
mg, tp = C.load(C.MGV), C.load(C.TPP)
text = (ROOT / "build" / "gekko" / "gekko_module.cpp").read_text(encoding="utf-8", errors="replace").splitlines()
pairs = {}
for n, l in enumerate(text):
    m = re.search(r"lift::call_native\(c, (0x[0-9a-f]+)ull\); /\* twin TPP", l)
    if m:
        s = re.search(r"// [0-9a-f]+: (?:call|jmp) (0x[0-9a-f]+)", " ".join(text[max(0, n - 4):n]))
        # the nearest preceding "// addr: call X" comment names the Survive callee
        cands = [re.search(r"// [0-9a-f]+: (?:call|jmp) (0x[0-9a-f]+)", text[k]) for k in range(n - 1, max(0, n - 6), -1)]
        cands = [c for c in cands if c]
        if cands:
            pairs[int(cands[0].group(1), 16)] = int(m.group(1), 16)
out = ["survive\ttpp\tresult"]
for s, t in sorted(pairs.items()):
    a, b = C.norm(mg, s, 0), C.norm(tp, t, 0)
    out.append("%x\t%x\t%s (%d vs %d instructions)" % (s, t, "same" if a == b else "DIFFERENT", len(a), len(b)))
(ROOT / "research" / "twin_verify.tsv").write_text("\n".join(out) + "\n", encoding="utf-8")
print("\n".join(out))
