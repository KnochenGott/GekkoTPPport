# mgv_index.py 03/10/2026. One-time offline index of mgv.exe into research/mgv_index/*.tsv. Usage: python tools/mgv_index.py [--limit N]
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys, os, re, time, struct, hashlib, bisect, collections
from multiprocessing import Pool
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_REG_RIP, X86_REG_RSP, X86_REG_RBP, X86_OP_IMM, X86_OP_MEM

EXE = str(paths.MGV_EXE)
SHA = "fa34d6c1f0a294f725084157f38fb240e9bea22607ec939282cb9caac4347404"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "research", "mgv_index")
B = 0x140000000
IMMOPS = {"mov", "movabs", "cmp", "and", "or", "test", "xor", "add", "sub", "imul", "push"}
img = md = None


def init():
    global img, md
    pe = pefile.PE(EXE, fast_load=True)
    img = pe.get_memory_mapped_image()
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    md.skipdata = True


def work(job):
    calls, dref, imm, field, vst = [], [], [], [], []
    for fs, fe in job:
        f = "0x%x" % fs
        fld = set()
        pend = {}
        n = 0
        for i in md.disasm(img[fs - B:fe - B], fs):
            n += 1
            m = i.mnemonic
            if m == ".byte":
                continue
            ops = i.operands
            site = "0x%x" % i.address
            if m in ("call", "jmp") and ops:
                op = ops[0]
                if op.type == X86_OP_IMM:
                    t = op.imm
                    if m == "call" or not (fs <= t < fe):
                        calls.append("%s\t%s\t%s\t0x%x\t" % (f, site, "c" if m == "call" else "j", t))
                    continue
                if op.type == X86_OP_MEM:
                    k = "i" + ("c" if m == "call" else "j")
                    mm = op.mem
                    if mm.base == X86_REG_RIP:
                        calls.append("%s\t%s\t%sr\t0x%x\t" % (f, site, k, i.address + i.size + mm.disp))
                    elif mm.base and mm.segment == 0:
                        calls.append("%s\t%s\t%s\t\t0x%x" % (f, site, k, mm.disp & 0xffffffff))
            for idx, op in enumerate(ops):
                if op.type == X86_OP_MEM:
                    mm = op.mem
                    if mm.base == X86_REG_RIP:
                        ta = i.address + i.size + mm.disp
                        dref.append("0x%x\t%s\t%s\t%s" % (ta, f, site, m))
                        if m == "lea" and idx == 1 and ops[0].type == 1:
                            pend[ops[0].reg] = (ta, n)
                    elif mm.segment == 0 and mm.base not in (0, X86_REG_RSP, X86_REG_RBP) and 0x10 <= mm.disp < 0x100000:
                        a = op.access
                        c = "a" if m == "lea" else ("m" if a == 3 else "w" if a == 2 else "r")
                        fld.add((mm.disp, "%s%d" % (c, op.size)))
                elif op.type == X86_OP_IMM and m in IMMOPS:
                    sz = op.size or 4
                    v = op.imm & ((1 << (8 * sz)) - 1)
                    if v < 0x1000 or v >= (1 << (8 * sz)) - 0x1000:
                        continue
                    if m in ("add", "sub") and v < 0x10000:
                        continue
                    imm.append("0x%016x\t%s\t%s\t%s" % (v, f, site, m))
            if m == "mov" and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[1].type == 1 and ops[1].reg in pend:
                ta, p = pend[ops[1].reg]
                if n - p <= 12 and ops[0].mem.base != X86_REG_RIP:
                    vst.append("0x%x\t%s\t%s\t0x%x" % (ta, f, site, ops[0].mem.disp & 0xffffffff))
            if pend and ops and ops[0].type == 1 and ops[0].reg in pend and m != "lea":
                del pend[ops[0].reg]
        for d, c in fld:
            field.append("0x%06x\t%s\t%s" % (d, f, c))
    return calls, dref, imm, field, vst


def sortwrite(path, lines, key=None):
    lines.sort(key=key)
    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))
        fh.write("\n")


def main():
    t0 = time.time()
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 0
    h = hashlib.sha256(open(EXE, "rb").read()).hexdigest()
    assert h == SHA, "mgv.exe hash mismatch " + h
    os.makedirs(OUT, exist_ok=True)
    prog = open(os.path.join(OUT, "progress.txt"), "w")

    def log(s):
        prog.write("%6.0fs %s\n" % (time.time() - t0, s)); prog.flush()

    pe = pefile.PE(EXE, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
    pd = sorted((B + e.struct.BeginAddress, B + e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
    img = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    tlo, thi = B + text.VirtualAddress, B + text.VirtualAddress + text.Misc_VirtualSize
    pd = [(a, b) for a, b in pd if tlo <= a < thi]
    # gap pseudo-functions: code in .text with no unwind info (leaf functions), split at int3 runs
    gaps, cur = [], tlo
    for a, b in pd + [(thi, thi)]:
        if a > cur:
            for m_ in re.finditer(rb"[^\xcc]+", img[cur - B:a - B]):
                gaps.append((cur + m_.start(), cur + m_.end()))
        cur = max(cur, b)
    log("pdata %d, gap pseudo-functions %d" % (len(pd), len(gaps)))
    allf = sorted(pd + gaps)
    if limit:
        allf = allf[:limit]
    jobs, cur_j, sz = [], [], 0  # about 40 KB of code per job
    for a, b in allf:
        cur_j.append((a, b)); sz += b - a
        if sz > 40000:
            jobs.append(cur_j); cur_j, sz = [], 0
    if cur_j:
        jobs.append(cur_j)
    R = {k: [] for k in "calls dref imm field vst".split()}
    with Pool(os.cpu_count(), initializer=init) as pool:
        for n, r in enumerate(pool.imap_unordered(work, jobs, chunksize=1)):
            for k, v in zip(R, r):
                R[k].extend(v)
            if n % 50 == 0:
                log("job %d/%d  calls %d dref %d imm %d field %d" % (n, len(jobs), len(R["calls"]), len(R["dref"]), len(R["imm"]), len(R["field"])))
    log("disassembly done")

    # function starts: pdata + gaps + call targets / vtable slots that fall in gaps
    pset, gset = {a for a, b in pd}, {a for a, b in gaps}
    pdend = dict(pd)
    pdstarts = sorted(pset)

    def in_pd(t):
        i = bisect.bisect_right(pdstarts, t) - 1
        return i >= 0 and pdend[pdstarts[i]] > t

    flags = collections.defaultdict(str)
    for l in R["calls"]:
        p = l.split("\t")
        if p[2] == "c" and tlo <= int(p[3], 16) < thi:
            flags[int(p[3], 16)] += "c"
    # vtables: runs of >=3 consecutive qwords that point into .text
    # adjacent vtables have no separator, so a run is also split at every address code takes with lea
    leas = {int(l.split("	", 1)[0], 16) for l in R["dref"] if l.endswith("	lea")}
    vts = []
    for s in pe.sections:
        if s.Name.rstrip(b"\0") not in (b".rdata", b".data"):
            continue
        lo = s.VirtualAddress
        qs = struct.unpack_from("<%dQ" % (s.Misc_VirtualSize // 8), img, lo)
        run = 0
        for k, q in enumerate(qs + (0,)):
            if run and B + lo + k * 8 in leas:
                if run >= 3:
                    vts.append((B + lo + (k - run) * 8, list(qs[k - run:k])))
                run = 0
            if tlo <= q < thi:
                run += 1
            else:
                if run >= 3:
                    vts.append((B + lo + (k - run) * 8, list(qs[k - run:k])))
                run = 0
    for st, sl in vts:
        for q in sl:
            flags[q] += "v"
    ss = sorted(pset | gset | {t for t in flags if not in_pd(t)})
    rows = []
    for k, a in enumerate(ss):
        e = pdend[a] if a in pset else (ss[k + 1] if k + 1 < len(ss) else thi)
        src = ("p" if a in pset else "g" if a in gset else "") + "".join(sorted(set(flags.get(a, ""))))
        rows.append("0x%x\t0x%x\t%s" % (a, e, src))
    sortwrite(os.path.join(OUT, "functions.tsv"), rows)

    # vtables + constructors from lea-vtable / mov [reg+disp],reg pairs
    vsorted = [st for st, sl in vts]
    stores = collections.defaultdict(list)
    for l in R["vst"]:
        ta, f, site, disp = l.split("\t")
        ta = int(ta, 16)
        i = bisect.bisect_right(vsorted, ta) - 1
        if i >= 0 and ta < vts[i][0] + 8 * len(vts[i][1]):
            stores[vts[i][0]].append((f, site, disp, ta))
    vrows = []
    for st, sl in vts:
        c = stores.get(st, [])
        ct = ",".join("%s@%s/%s%s" % (f, s, d, "" if t == st else "+" + hex(t - st)) for f, s, d, t in sorted(set(c))[:40])
        flag = "ctor" if any(d == "0x0" and t == st for f, s, d, t in c) else ("ctor?" if c else "")
        vrows.append("0x%x\t%d\t%s\t%s\t%s" % (st, len(sl), flag, ct, ",".join("0x%x" % q for q in sl[:1024])))  # long runs are tables, nslots keeps the true count
    sortwrite(os.path.join(OUT, "vtables.tsv"), vrows)
    log("functions %d vtables %d" % (len(rows), len(vrows)))

    # calls.tsv sorted by caller; calls_rev.tsv sorted by target (indirect-by-register keyed "d0x<disp>")
    sortwrite(os.path.join(OUT, "calls.tsv"), R["calls"])
    rev = []
    for l in R["calls"]:
        f, s, k, t, d = l.split("\t")
        rev.append("%s\t%s\t%s\t%s" % (t or "d" + d, f, s, k))
    sortwrite(os.path.join(OUT, "calls_rev.tsv"), rev)
    del rev, R["calls"]
    sortwrite(os.path.join(OUT, "dataref.tsv"), R["dref"]); del R["dref"]
    sortwrite(os.path.join(OUT, "imm.tsv"), R["imm"]); del R["imm"]
    sortwrite(os.path.join(OUT, "fieldoff.tsv"), R["field"]); del R["field"]
    log("tsv written")

    strs = []
    for s in pe.sections:
        if s.Name.rstrip(b"\0") not in (b".rdata", b".data", b"_RDATA"):
            continue
        d = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
        base = B + s.VirtualAddress
        for m_ in re.finditer(rb"[\x20-\x7e]{5,}", d):
            strs.append((base + m_.start(), "a", m_.group().decode()))
        for m_ in re.finditer(rb"(?:[\x20-\x7e]\x00){5,}", d):
            strs.append((base + m_.start(), "w", m_.group().decode("utf-16-le")))
    strs.sort()
    sst = [s[0] for s in strs]
    slen = [len(s[2]) * (2 if s[1] == "w" else 1) + 2 for s in strs]
    refs = collections.defaultdict(list)
    with open(os.path.join(OUT, "dataref.tsv")) as fh:
        for l in fh:
            ta, f, site, m = l.rstrip("\n").split("\t")
            t = int(ta, 16)
            i = bisect.bisect_right(sst, t) - 1
            if i >= 0 and t < sst[i] + slen[i] and len(refs[i]) < 8 and f not in refs[i]:
                refs[i].append(f)
    sw = ["0x%x\t%s\t%d\t%s\t%s" % (a, e, len(t), t[:300].replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n"), ",".join(refs.get(i, [])))
          for i, (a, e, t) in enumerate(strs)]
    sortwrite(os.path.join(OUT, "strings.tsv"), sw)
    log("strings %d; ALL DONE" % len(sw))


if __name__ == "__main__":
    main()
