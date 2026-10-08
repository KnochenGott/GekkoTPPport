# fault.py 30/09/2026. Runs world_test for one action/event and disassembles around the Survive fault address.
#   python tools/lift/tests/fault.py ACTION [EVENT] [CONTEXT_INSNS]
import re, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import lift, paths
a = sys.argv[1]; ev = sys.argv[2] if len(sys.argv) > 2 else "1"; ctxn = int(sys.argv[3]) if len(sys.argv) > 3 else 12
exe = Path(__file__).resolve().parent.parent.parent.parent / "build" / "lift_test" / "world_test.exe"
out = subprocess.run([str(exe), a, ev], capture_output=True, text=True, errors="replace", timeout=60).stdout
print(out.strip())
m = re.search(r"survive pc 0x([0-9a-f]+)", out)
if m:
    pc = int(m.group(1), 16)
    im = lift.Image(str(paths.MGV_EXE))
    ch = im.chunk(pc)
    ins = list(lift.cs.disasm(im.bytes(ch[0], ch[1] - ch[0]), ch[0]))
    k = next(i for i, x in enumerate(ins) if x.address == pc)
    for x in ins[max(0, k - ctxn):k + 4]:
        print("%s %x: %s %s" % (">>" if x.address == pc else "  ", x.address, x.mnemonic, x.op_str))
