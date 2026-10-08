# run_param_test.py 03/10/2026. Builds and runs tests/param_test.cpp (token PARAM_BLOCK_OK); no lift, no game. Uses the committed src/GekkoHook/GekkoParamBlock.h; regenerates it first only when GEKKO_PARAM_JSON is set.
import os
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tests as rt
rt.OUT.mkdir(parents=True, exist_ok=True)
G = rt.ROOT / "src" / "GekkoHook"
if os.environ.get("GEKKO_PARAM_JSON"): subprocess.run([sys.executable, str(rt.ROOT / "tools" / "gen_gekko_params.py")], check=True)
rt.sh('cl /nologo /O1 /std:c++17 /I"%s" "%s" "%s" /Fe:param_test.exe' % (G, rt.HERE / "param_test.cpp", G / "GekkoParam.cpp"), rt.OUT)
r = subprocess.run([str(rt.OUT / "param_test.exe")], capture_output=True, text=True)
print(r.stdout, r.stderr); sys.exit(r.returncode)
