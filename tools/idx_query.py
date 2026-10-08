# idx_query.py 03/10/2026. Query the mgv.exe index in research/mgv_index. Usage: idx_query.py [-n MAX] callers|callees|imm|field|vtable|str|xref ARG [ARG2]
import sys, os, mmap, bisect

D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "research", "mgv_index")
MAX = 40
_cache = {}


def hx(s):
    return int(s, 16) if s.lower().startswith("0x") else int(s, 16)


def rows(name, key):
    """All lines of a sorted TSV whose first column equals key (bisect over the mmap)."""
    f = open(os.path.join(D, name), "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    key = key.encode()
    lo, hi = 0, len(mm)
    while lo < hi:  # first line start whose column 0 >= key
        mid = (lo + hi) // 2
        s = mm.rfind(b"\n", 0, mid) + 1
        e = mm.find(b"\n", s)
        if mm[s:e].split(b"\t", 1)[0] < key:
            lo = e + 1
        else:
            hi = s
    out = []
    while lo < len(mm):
        e = mm.find(b"\n", lo)
        l = mm[lo:e].decode()
        if l.split("\t", 1)[0].encode() != key:
            break
        out.append(l)
        lo = e + 1
    return out


def funcs():
    if "f" not in _cache:
        r = [l.split("\t") for l in open(os.path.join(D, "functions.tsv")).read().split("\n") if l]
        _cache["f"] = ([int(a, 16) for a, b, c in r], [int(b, 16) for a, b, c in r])
    return _cache["f"]


def owner(a):
    s, e = funcs()
    i = bisect.bisect_right(s, a) - 1
    return s[i] if i >= 0 and a < e[i] else None


def pr(lines):
    for l in lines[:MAX]:
        print(l)
    if len(lines) > MAX:
        print("... %d more (use -n)" % (len(lines) - MAX))
    if not lines:
        print("(none)")


def vtables():
    if "v" not in _cache:
        _cache["v"] = [l.split("\t") for l in open(os.path.join(D, "vtables.tsv")).read().split("\n") if l]
    return _cache["v"]


def main():
    global MAX
    a = sys.argv[1:]
    if a[:1] == ["-n"]:
        MAX = int(a[1]); a = a[2:]
    cmd, arg = a[0], a[1]
    if cmd == "callers":  # direct calls/jumps to ADDR; an offset like 0x408 finds register-indirect calls [reg+0x408]
        v = hx(arg)
        key = "0x%x" % v if v >= 0x140000000 else "d0x%x" % v
        pr(["caller=%s site=%s kind=%s" % (f, s, k) for t, f, s, k in (l.split("\t") for l in rows("calls_rev.tsv", key))])
    elif cmd == "callees":
        v = hx(arg)
        fn = owner(v) or v
        pr(["site=%s kind=%s target=%s disp=%s" % (s, k, t, d) for f, s, k, t, d in (l.split("\t") for l in rows("calls.tsv", "0x%x" % fn))])
    elif cmd == "imm":
        pr(["func=%s site=%s %s" % (f, s, m) for v, f, s, m in (l.split("\t") for l in rows("imm.tsv", "0x%016x" % hx(arg)))])
    elif cmd == "field":
        lo, hi = (0, 1 << 64)
        if len(a) > 2:
            lo, hi = [hx(x) for x in a[2].split("-")]
        pr(["func=%s access=%s" % (f, c) for d, f, c in (l.split("\t") for l in rows("fieldoff.tsv", "0x%06x" % hx(arg))) if lo <= int(f, 16) < hi])
    elif cmd == "vtable":
        v = hx(arg)
        t = vtables()
        i = bisect.bisect_right([int(x[0], 16) for x in t], v) - 1
        r = t[i]
        st, n = int(r[0], 16), int(r[1])
        if i < 0 or v >= st + 8 * n:
            print("(no vtable)"); return
        print("vtable %s nslots=%d flag=%s ctors=%s" % (r[0], n, r[2], r[3]))
        sl = r[4].split(",")
        k0 = (v - st) // 8  # slot indexes are relative to ADDR
        pr(["slot %d (+0x%x) %s" % (k - k0, (k - k0) * 8, s) for k, s in enumerate(sl) if k >= k0])
        if len(a) > 2:  # vtable ADDR SLOTINDEX
            print("slot %s -> %s" % (a[2], sl[k0 + int(a[2])]))
    elif cmd == "str":
        t = arg.lower()
        pr([l.rstrip("\n") for l in open(os.path.join(D, "strings.tsv"), encoding="utf-8") if t in l.lower()])
    elif cmd == "xref":
        v = hx(arg)
        k = "0x%x" % v
        print("-- dataref (target func site mnemonic)")
        pr(rows("dataref.tsv", k))
        print("-- calls/jumps to")
        pr(["caller=%s site=%s kind=%s" % (f, s, kk) for t, f, s, kk in (l.split("\t") for l in rows("calls_rev.tsv", k))])
        print("-- vtable slots holding it")
        pr(["vtable %s slot %d" % (r[0], j) for r in vtables() for j, s in enumerate(r[4].split(",")) if s == k])
    else:
        print(__doc__)


main()
