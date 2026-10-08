# class_scan.py 08/10/2026. Finds Survive's registered component classes and the closure of component keys reachable from a start key (docs/DOCUMENTATION.md 5.16).
# A class registers itself in a static initialiser that stores {descriptor vtable, key, 0} in a global and calls 0x141989090. The descriptor vtable slot 1 is the creator, the creator
# stores the object vtables (lea of data whose first qwords are code pointers). Keys are the 48-bit constants (movabs) in the code of the creator and of every function in the object vtables.
import bisect, json, struct, sys
import capstone, pefile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import paths

EXE = str(paths.MGV_EXE)
IB = 0x140000000
REGISTER = 0x141989090

pe = pefile.PE(EXE, fast_load=True)
pe.parse_data_directories()
IMG = pe.get_memory_mapped_image()
TEXT = [s for s in pe.sections if s.Name.startswith(b".text")][0]
T_LO, T_HI = IB + TEXT.VirtualAddress, IB + TEXT.VirtualAddress + TEXT.Misc_VirtualSize
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = False
FUNCS = sorted((IB + e.struct.BeginAddress, IB + e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
STARTS = [f[0] for f in FUNCS]


def q(a):
    return struct.unpack_from("<Q", IMG, a - IB)[0]


def is_code(v):
    return T_LO <= v < T_HI


def func_of(a):
    i = bisect.bisect_right(STARTS, a) - 1
    return FUNCS[i] if i >= 0 and FUNCS[i][0] <= a < FUNCS[i][1] else (a, a + 0x200)


def keys_in(fn_start):
    lo, hi = func_of(fn_start)
    out = set()
    for i in MD.disasm(IMG[lo - IB:hi - IB], lo):
        if i.mnemonic == "movabs":
            try:
                v = int(i.op_str.split(",")[1].strip(), 16)
            except ValueError:
                continue
            if (1 << 32) < v < (1 << 48):
                out.add(v)
    return out


def vtable_slots(a):
    out = []
    while is_code(q(a)):
        out.append(q(a))
        a += 8
    return out


def lea_targets(fn_start):
    lo, hi = func_of(fn_start)
    out = []
    for i in MD.disasm(IMG[lo - IB:hi - IB], lo):
        if i.mnemonic == "lea" and "rip" in i.op_str:
            try:
                d = int(i.op_str.split("rip + ")[1].rstrip("]"), 16) if "rip + " in i.op_str else -int(i.op_str.split("rip - ")[1].rstrip("]"), 16)
            except (IndexError, ValueError):
                continue
            out.append(i.address + i.size + d)
    return out


def scan_inits(lo=0x141f3c000, hi=0x141fb0000):
    inits = {}
    fstart, key, vt = lo, None, None
    for i in MD.disasm(IMG[lo - IB:hi - IB], lo):
        if i.mnemonic == "lea" and vt is None and "rip + " in i.op_str:
            vt = i.address + i.size + int(i.op_str.split("rip + ")[1].rstrip("]"), 16)
        if i.mnemonic == "movabs":
            try:
                v = int(i.op_str.split(",")[1].strip(), 16)
            except ValueError:
                v = None
            if v and (1 << 32) < v < (1 << 48) and key is None:
                key = v
        if i.mnemonic == "call" and i.op_str == "0x%x" % REGISTER and key is not None:
            inits[key] = {"init": fstart, "descVt": vt}
        if i.mnemonic in ("int3", "ret"):
            fstart, key, vt = i.address + 1, None, None
    return inits


def closure(start_keys, inits):
    seen, order, work = set(), [], list(start_keys)
    info = {}
    while work:
        k = work.pop()
        if k in seen or k not in inits:
            continue
        seen.add(k)
        d = inits[k]
        slots = vtable_slots(d["descVt"]) if d["descVt"] else []
        creator = slots[1] if len(slots) > 1 else None
        objvts, refs = set(), set()
        if creator:
            refs |= keys_in(creator)
            for t in lea_targets(creator):
                if IB <= t < IB + len(IMG) - 8 and is_code(q(t)):
                    objvts.add(t)
        funcs = set()
        for vt in objvts:
            funcs |= set(vtable_slots(vt))
        for f in funcs:
            refs |= keys_in(f)
        info[k] = {"init": d["init"], "descVt": d["descVt"], "creator": creator, "objVts": sorted(objvts), "refs": sorted(refs & set(inits))}
        for r in refs:
            if r in inits:
                work.append(r)
        order.append(k)
    return info


if __name__ == "__main__":
    inits = scan_inits()
    start = [int(x, 16) for x in sys.argv[1:]] or [0xb567a90fe2c5]
    info = closure(start, inits)
    json.dump({hex(k): {kk: (hex(vv) if isinstance(vv, int) else [hex(x) for x in vv] if isinstance(vv, list) and vv and isinstance(vv[0], int) else vv) for kk, vv in v.items()} for k, v in info.items()},
              open(str(Path(__file__).resolve().parents[2] / "build" / "class_closure.json"), "w"), indent=1)
    print("classes registered:", len(inits), " closure:", len(info))
    for k, v in info.items():
        print(hex(k), "init", hex(v["init"]), "creator", hex(v["creator"]) if v["creator"] else None, "objvts", [hex(x) for x in v["objVts"]], "refs", len(v["refs"]))
