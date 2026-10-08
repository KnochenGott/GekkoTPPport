# run_leg_ik_test.py 07/10/2026. Builds and runs tests/leg_ik_test.cpp (token LEG_IK_OK); no lift, no game.
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tests as rt
rt.OUT.mkdir(parents=True, exist_ok=True)
G = rt.ROOT / "src" / "GekkoHook"
rt.sh('cl /nologo /O1 /std:c++17 /EHsc /I"%s" "%s" /Fe:leg_ik_test.exe' % (G, rt.HERE / "leg_ik_test.cpp"), rt.OUT)
r = subprocess.run([str(rt.OUT / "leg_ik_test.exe")], capture_output=True, text=True)
print(r.stdout, r.stderr); sys.exit(r.returncode)
