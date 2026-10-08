# run_tests.py 29/09/2026. Builds tests/samples.dll with MSVC, lifts its exports (except nat_helper, which stays a native call),
# builds the differential test and runs it. Usage: python tools/lift/tests/run_tests.py
import subprocess, sys
from pathlib import Path

import pefile

HERE = Path(__file__).resolve().parent
LIFT = HERE.parent
ROOT = LIFT.parent.parent
OUT = ROOT / "build" / "lift_test"


def vcvars():
    vw = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    vs = subprocess.check_output([str(vw), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                  "-property", "installationPath"], text=True).strip()
    return Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"


def sh(cmd, cwd):
    full = 'cmd /c ""%s" >nul && %s"' % (vcvars(), cmd)
    r = subprocess.run(full, cwd=cwd, capture_output=True, text=True, errors="replace")
    if r.returncode:
        print(r.stdout[-4000:], r.stderr[-2000:])
        sys.exit("command failed: " + cmd)
    return r.stdout


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    dll = OUT / "samples.dll"
    sh('cl /nologo /O2 /GS- /LD /std:c++17 "%s" /Fe:samples.dll /link /BASE:0x180000000 /DYNAMICBASE:NO' % (HERE / "samples.cpp"), OUT)
    pe = pefile.PE(str(dll))
    exports = {e.name.decode(): pe.OPTIONAL_HEADER.ImageBase + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
    roots = [hex(a) for n, a in exports.items() if n != "nat_helper"]
    print("lifting %d functions" % len(roots))
    r = subprocess.run([sys.executable, str(LIFT / "lift.py"), str(dll), *roots, "--out", str(OUT / "lifted")], capture_output=True, text=True, errors="replace")
    print(r.stdout[-3000:], r.stderr[-2000:])
    if r.returncode:
        sys.exit(1)
    sh('ml64 /nologo /c /Fo lift_native_call.obj "%s"' % (LIFT / "lift_native_call.asm"), OUT)
    sh('cl /nologo /O1 /std:c++17 /EHsc /I"%s" "%s" "%s" lifted.cpp lift_native_call.obj /Fe:test.exe /link /STACK:8000000'
       % (LIFT, HERE / "test_main.cpp", LIFT / "lift_rt.cpp"), OUT)
    t = subprocess.run([str(OUT / "test.exe"), str(dll)], capture_output=True, text=True, errors="replace")
    print(t.stdout, t.stderr)
    sys.exit(t.returncode)


if __name__ == "__main__":
    main()