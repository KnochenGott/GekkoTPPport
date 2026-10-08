# shape_match.py 29/09/2026. Finds the Survive (mgv.exe) functions whose instruction shape is closest to a TPP
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
# (mgsvtpp.exe) function: mnemonic + operand kinds, registers kept, displacements/immediates/targets blanked, compared
# with difflib over the first N instructions. Candidates: every .pdata function start in mgv.exe.
# Usage: python tools/shape_match.py TPP_VA [N=48] [top=8]
import difflib, re, sys, struct, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
TPP = str(paths.TPP_EXE)
MGV = str(paths.MGV_EXE)
cs = Cs(CS_ARCH_X86, CS_MODE_64)
NUM = re.compile(r"0x[0-9a-f]+|\b\d+\b")

def load(p):
    pe = pefile.PE(p, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
    return pe, pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase

def shape(img, B, va, n):
    out = []
    for i in cs.disasm(img[va - B:va - B + n * 15], va):
        if i.mnemonic == "int3": break
        out.append(i.mnemonic + " " + NUM.sub("#", i.op_str))
        if len(out) >= n or i.mnemonic == "ret": break
    return out

va, n, top = int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 48, int(sys.argv[3]) if len(sys.argv) > 3 else 8
tpe, timg, tB = load(TPP)
want = shape(timg, tB, va, n)
mpe, mimg, mB = load(MGV)
starts = sorted({mB + e.struct.BeginAddress for e in mpe.DIRECTORY_ENTRY_EXCEPTION})
want_m = [w.split()[0] for w in want]
res = []
for s in starts:
    got = shape(mimg, mB, s, n)
    if abs(len(got) - len(want)) > len(want) // 2: continue
    q = difflib.SequenceMatcher(None, want_m, [g.split()[0] for g in got], autojunk=False)
    if q.quick_ratio() < 0.6: continue
    res.append((difflib.SequenceMatcher(None, want, got, autojunk=False).ratio(), s))
res.sort(reverse=True)
print("TPP %x: %d insns, %d mgv candidates scored" % (va, len(want), len(res)))
for r, s in res[:top]: print("  %.3f  mgv %x" % (r, s))
