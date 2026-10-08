# run_melee_test.py 29/09/2026. Builds and runs tests/melee_test.cpp: the lifted melee chooser on the shim objects, no game needed.
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tests as rt

def main():
    rt.OUT.mkdir(parents=True, exist_ok=True)
    G = rt.ROOT / "src" / "GekkoHook"
    rt.sh('ml64 /nologo /c /Fo lift_native_call.obj "%s"' % (rt.LIFT / "lift_native_call.asm"), rt.OUT)
    rt.sh('cl /nologo /O1 /std:c++17 /EHa /I"%s" /I"%s" "%s" "%s" "%s" "%s" lift_native_call.obj /Fe:melee_test.exe'
          % (G, rt.LIFT, rt.HERE / "melee_test.cpp", G / "GekkoLift.cpp", G / "lifted" / "melee_lifted.cpp", rt.LIFT / "lift_rt.cpp"), rt.OUT)
    r = subprocess.run([str(rt.OUT / "melee_test.exe")], capture_output=True, text=True, errors="replace")
    print(r.stdout, r.stderr)
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()