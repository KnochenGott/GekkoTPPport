# run_world_test.py 29/09/2026. Lifts the gekko module with --pc and builds tests/world_test.cpp against it. Usage:
#   python tools/lift/tests/run_world_test.py [ACTION EVENT]     (add --relift after changing the lifter)
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import paths
import run_tests as rt

MGV = str(paths.MGV_EXE)

def main():
    args = [a for a in sys.argv[1:] if a != "--relift"]
    relift = "--relift" in sys.argv or not (rt.OUT / "gekko_module.cpp").exists()
    rt.OUT.mkdir(parents=True, exist_ok=True)
    G = rt.ROOT / "src" / "GekkoHook"
    inc = '/DLIFT_PC_TRACE /I"%s" /I"%s"' % (G, rt.LIFT)
    if relift:
        r = subprocess.run([sys.executable, str(rt.LIFT / "lift_gekko.py"), str(rt.OUT / "gekko_module")], capture_output=True, text=True, errors="replace")
        print(r.stdout.splitlines()[0] if r.stdout else r.stderr[-500:])
        rt.sh('cl /nologo /c /O1 /std:c++17 /EHa %s "%s" /Fo:gekko_module.obj' % (inc, rt.OUT / "gekko_module.cpp"), rt.OUT)
        rt.sh('ml64 /nologo /c /Fo lift_native_call.obj "%s"' % (rt.LIFT / "lift_native_call.asm"), rt.OUT)
        rt.sh('ml64 /nologo /c /Fo ctrl_fwd.obj "%s"' % (G / "GekkoCtrlFwd.asm"), rt.OUT)
        rt.sh('cl /nologo /c /O1 /std:c++17 /EHa %s "%s" /Fo:lift_rt.obj' % (inc, rt.LIFT / "lift_rt.cpp"), rt.OUT)
    rt.sh('cl /nologo /O1 /std:c++17 /EHa %s "%s" "%s" "%s" "%s" "%s" "%s" "%s" "%s" "%s" gekko_module.obj lift_rt.obj lift_native_call.obj ctrl_fwd.obj /Fe:world_test.exe' % (inc, rt.HERE / "world_test.cpp", G / "GekkoWorld.cpp", G / "GekkoLife.cpp", G / "GekkoNodes.cpp", G / "GekkoParam.cpp", G / "GekkoPush.cpp", G / "GekkoNavProxy.cpp", rt.HERE / "nav_proxy_test.cpp", rt.HERE / "life_test.cpp"), rt.OUT)
    r = subprocess.run([str(rt.OUT / "world_test.exe")] + args, capture_output=True, text=True, errors="replace")
    print(r.stdout[-6000:], r.stderr[-1500:])

if __name__ == "__main__":
    main()