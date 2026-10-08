# run_brain_test.py 30/09/2026. Builds tests/brain_test.cpp with the world shim and the lifted module (needs run_world_test.py once).
#   python tools/lift/tests/run_brain_test.py [FRAMES [TX TY TZ]]
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tests as rt

def main():
    G = rt.ROOT / "src" / "GekkoHook"
    inc = '/I"%s" /I"%s"' % (G, rt.LIFT)
    rt.sh('cl /nologo /O1 /std:c++17 /EHa %s "%s" "%s" "%s" gekko_module.obj lift_rt.obj lift_native_call.obj ctrl_fwd.obj /Fe:brain_test.exe' %
          (inc, rt.HERE / "brain_test.cpp", G / "GekkoBrain.cpp", G / "GekkoWorld.cpp"), rt.OUT)
    r = subprocess.run([str(rt.OUT / "brain_test.exe")] + sys.argv[1:], capture_output=True, text=True, errors="replace")
    sys.stdout.write(r.stdout[-12000:]); sys.stdout.write(r.stderr[-1500:])

if __name__ == "__main__":
    main()