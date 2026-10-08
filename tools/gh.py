# gh.py 01/10/2026. Decompile or list xrefs in the running Ghidra (GhidraMCP HTTP server, port 8086) for DISK addresses of the installed mgv.exe: the Ghidra project is an older build, so each address
# is translated with tools/mgv_map.py first (masked prologue pairing). The MCP tool wrapper is fixed to port 8080, so this talks to the server directly.
#   python tools/gh.py dec 0x140fab250          decompile the function at that disk address
#   python tools/gh.py xref 0x140fab250         references to the Ghidra function (as Ghidra addresses)
import re, subprocess, sys, urllib.parse, urllib.request
from pathlib import Path

PORT = 8086
ROOT = Path(__file__).resolve().parent.parent


def to_ghidra(disk):
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "mgv_map.py"), "d2g", disk], capture_output=True, text=True).stdout
    m = re.search(r"-> (?:disk )?ghidra (?:0x)?([0-9a-f]+) \+0x([0-9a-f]+)", out)
    if not m:
        raise SystemExit("no Ghidra counterpart for %s: %s" % (disk, out.strip()[-200:]))
    return hex(int(m.group(1), 16)), out.strip().splitlines()[-1]   # function start in the Ghidra build


def get(path, **q):
    url = "http://127.0.0.1:%d/%s?%s" % (PORT, path, urllib.parse.urlencode(q))
    return urllib.request.urlopen(url, timeout=120).read().decode("utf-8", "replace")


if __name__ == "__main__":
    cmd, disk = sys.argv[1], sys.argv[2]
    g, note = to_ghidra(disk)
    print("//", note)
    if cmd == "dec":
        print(get("decompile_function", address=g))
    elif cmd == "xref":
        print(get("xrefs_to", address=g, limit=60))
