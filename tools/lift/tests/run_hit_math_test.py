# run_hit_math_test.py 07/10/2026. Builds and runs tests/hit_math_test.cpp (token HIT_MATH_OK); no lift, no game.
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tests as rt
rt.OUT.mkdir(parents=True, exist_ok=True)
G = rt.ROOT / "src" / "GekkoHook"
rt.sh('cl /nologo /O1 /std:c++17 /EHsc /I"%s" "%s" /Fe:hit_math_test.exe' % (G, rt.HERE / "hit_math_test.cpp"), rt.OUT)
r = subprocess.run([str(rt.OUT / "hit_math_test.exe")], capture_output=True, text=True)
print(r.stdout, r.stderr); sys.exit(r.returncode)
