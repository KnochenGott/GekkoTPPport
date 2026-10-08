# lift_gekko.py 30/09/2026. The one place that says how the Survive gekko module is lifted (used by build_gekko.bat and the tests).
#   python tools/lift/lift_gekko.py OUT_STEM        -> OUT_STEM.cpp
# Roots: the table initialisers (action table, AI state table, second layer) and the AI state 0 enter; everything in the gekko module range
# (0x140f60000-0x140fb0000 of the installed mgv.exe) is lifted. --env replaces a Survive function by a shim (src/GekkoHook/lift_env.h).
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import paths

MGV = str(paths.MGV_EXE)
LIFT = Path(__file__).resolve().parent / "lift.py"
ROOTS = ["0x14107f6b0", "0x141f7e070", "0x141f7dbd0", "0x141f7de50", "0x140f8d8d0", "0x1402ba870",   # table initialisers, AI state 0 enter, the shared empty stub the tables point at
         "0x141073500", "0x141073570", "0x141073610", "0x140f72e40",   # factories: SsdNpc (0x428 bytes) and its siblings, gekko Observer
         "0x141988fa0", "0x141989050", "0x141989090",   # scope singleton and its current-context getter (the Observer setup calls the getter)
         "0x141092020", "0x141091f70", "0x141f7d660",   # component base class tick (runs a component's state machine: enter, update, exit through slot 6), its move-step wrapper 0x141091f70 (slot 20, mode 7 of the state); locomotion state table initialiser (0x143086190: handlers 0x140f898c0, 0x140f89af0, 0x140f89ec0, 0x140f89c10)
         "0x140f7f7f0", "0x1410700d0",   # NPC state-bit query the locomotion calls through the tracker slot (+0x68), reached only through a vtable; countdown component creator
         "0x141408ca0", "0x141408a60", "0x14137e370", "0x141bf1e80",
         "0x141040790", "0x141087040", "0x140f51ef0",
         "0x140e786d0", "0x140f768c0", "0x140f82ab0", "0x141af7530",
         "0x141072e00", "0x1411ba1f0",   # SsdNpc service slot 123; Setup of container child 1: creates the container children through the creation context
         "0x140c0fea0", "0x140c10900",   # the NPC manager accessors the NPC creator 0x140b6c370 puts in the SsdNpc descriptor +0x40, +0x48 (docs 5.16); +0x38 is an allocator object, answered by the host
         "0x141962ea0", "0x14196e700", "0x1400fc5e0", "0x140339a90", "0x141bcd410", "0x1400fc230",   # ctx struct getter and the setters Survive's boot calls for ctx +0x18, +0x40, +0x48, +0x78 (docs 5.16)
         "0x141fc1f20", "0x141f2fad0", "0x141f2faa0", "0x141f2e370", "0x141f40740", "0x141f40770", "0x141f407a0",   # static initialisers of the objects ctx +0x18, +0x40, +0x48 point at
         "0x14032d880", "0x1403283a0", "0x140326d00", "0x140326230"]   # clock system: time source object, rows, tables (the SsdNpc component Setups read the clock singleton through ctx +0x78)
ENV = ["0x141924274=nop",            # CRT atexit registration every static initialiser tail-calls (Survive's exit list is not run)
       "0x141924340=nop",            # stack cookie check (Survive's cookie would terminate TPP)
       "0x14006bd20=env_alloc",
       "0x141ad6c10=env_free",
       "0x14006bd90=env_free",
       "0x141989100=env_alloc",
       "0x1400167f0=env_alloc16",   # alloc(size rcx, kind edx) with fixed 16 alignment (0x140004b20), the clock time source allocates its tables with it
       "0x140016830=env_free",   # free(ptr rcx) of the same allocator family
       "0x140035250=nop",
       "0x1400167d0=env_alloc16",   # aligned alloc(size rcx, align 16, flags): the name registry creator 0x141af7530 allocates its 0x48 byte object with it (the TPP twin of its callee 0x140004b20 is refused)
       "0x140004a30=env_alloc16",   # alloc(size rcx) with fixed 16 alignment (the scope singleton allocates its 0x4018 block with it)
       "0x141924790=env_memset", "0x141924360=env_memcpy", "0x1419247fe=env_memcmp"]   # MSVCR110 import thunks (the IAT slots of the image are not resolved in the embedded data)
# 0x141bf1e80 Survive Path2Impl constructor, run by GekkoNavProxy to build the Survive-layout copies of TPP paths.
# 0x141408ca0 0x141408a60 0x14137e370 Survive's NavigationController quark (docs/DOCUMENTATION.md), its creator and two leaf helpers without unwind info (ChunkAllocator array placement).
# Survive-only code just outside the module that it calls (found by tools/scratch/reach_survive_only.py): NPC service group and the neighbours after the module.
# Classes whose virtual functions are all roots (a vtable is data, so direct calls never reach its entries). Each is read from mgv.exe until a non-code qword.
#   0x1423d8cb0 the shared-object pool (13 slots; matches TPP's pool vtable 0x1424f39f0 at score 1.00 but its factories build Survive classes,)
#   0x1423ca168, 0x1423ca180, 0x1423ca1a0 the class its slot 0x20 builds (ctor 0x141abb610; 0x1423ca1a0 matches TPP TargetObject 0x1424e5160 in vtable_maps.tsv)
#   0x1423cfe08 the class its slot 0x30 builds (ctor 0x141ad5e80)
#   0x1422d2900, 0x1422d2040 the SsdNpc base class every NPC type derives from (factory 0x141073500, 0x428 bytes; QI 0x141075b60 answers id 0xb567a90fe2c5; slot 2 = 0x14107fa80 wires ~30 components)
#   0x1422ad070, 0x1422ace60, 0x1422ace28 the gekko Observer (factory 0x140f72e40, 0xe0 bytes; QI 0x140f738c0 answers 0xdc11dba59fbb with this+0x38): timers and flags the movement brain queries
VTABLES = ["0x14224e748", "0x14224e818", "0x1423d8cb0", "0x1423ca168", "0x1423ca180", "0x1423ca1a0", "0x1423cfe08", "0x1422d2900", "0x1422d2040", "0x1422ad070", "0x1422ace60", "0x1422ace28",
           "0x1422d14f0", "0x1422d1530",
           "0x14233c3d0", "0x14233c400", "0x14233c4f8", "0x14233c530", "0x1423fb7f8",
           "0x1422c8c80", "0x1422c8cb0", "0x1422d4c00", "0x1422d4c30", "0x1422a5740", "0x1422a5770",
           "0x1422afe38", "0x1422afe70", "0x1422afdc0", "0x1422af448", "0x1422af480", "0x1422af4e8",
           "0x14228aa60", "0x14228aaa0", "0x1422ad3d0", "0x1422e1db0", "0x1422adcf8", "0x1422add28"]   # ActionController (primary, interface), gekko ActionController component (primary, interface), second component 0xa62b87fc4a6a (primary, interface)   # life class, generic damage handler, damage calculator; LifeController and typed DamageHandler (leaf virtuals such as the QI have no unwind info, so --all-in misses them)   # NavigationController quark and interface vtables, and the path object class (Path2Impl) the nav service hands out (docs/DOCUMENTATION.md)
FOLLOW = [("0x140c0fd00", "0x140c0ff20"), ("0x140c10000", "0x140c10a40"),   # the two NPC manager tables behind the accessors
          ("0x141924060", "0x141924170"),   # MSVC vector constructor / destructor iterators (plain code; the automatic twin is a CRT import thunk)
          ("0x140320000", "0x14033a000"),   # clock system (singleton 0x1403269d0, tables 0x140326d00)
          ("0x140e7c000", "0x140e83000"), ("0x140e77000", "0x140e7b400"), ("0x140e96000", "0x140e98100"),   # ActionController class and its request mailbox 0x140e96b60
           ("0x140f5f000", "0x140f60000"), ("0x140fb0000", "0x140fb6000"),
          ("0x141b18f00", "0x141b19400"),   # the pool's slot functions
          ("0x141091f00", "0x141092600"),   # the component base class state machine (tick 0x141092020, RequestState 0x141092360)
          ("0x1400e4dd0", "0x1400e4e50"),   # angle wrap (twin 0x1400d93f0 is identical, 23 instructions; lifted so the headless test can run it)
          ("0x14062e480", "0x14062e540"),   # GeoCheckControl constructor (N=1; same layout as TPP's 0x14045beb0, the capacity is the high word of +0x68)
          ("0x140635010", "0x140635400"),   # hit record -> normal (math on the result the TPP line check writes)
          ("0x141abb000", "0x141abd000"), ("0x141ad5e00", "0x141ad7000"),
          # Engine helpers that operate on Survive-owned objects are lifted, not twinned: TPP's twin walks TPP globals (twin 0x141bd0f20 of 0x141af72f0 faulted in TPP 0x14003a148 with a Survive object)
          ("0x141af7000", "0x141af7800"), ("0x14196c0e0", "0x14196c110"),   # mutex constructor (three zero dwords) the name registry creator calls
           ("0x141070000", "0x141082000"), ("0x141af1a00", "0x141af1b00"), ("0x140034000", "0x14003b000"),
          ("0x141408000", "0x14140b000"), ("0x141bf1000", "0x141bfc000"),
          ("0x141040000", "0x141043000"), ("0x141086000", "0x141089800"), ("0x140eaf000", "0x140eb1000"), ("0x140f50000", "0x140f53800"), ("0x141015000", "0x141016000"),   # life class, generic damage handler, shared damage code, damage calculator, response handler base
          ("0x1400be690", "0x1400be6e3"), ("0x1400169d0", "0x1400169d7")]   # Intrusive pointer reset (Path2Impl ctor call at 0x141bf1efb); the TPP twin 0x1400b31f0 is identical in shape but the lifted Survive body needs no TPP exe


def vtable_roots():
    import pefile, struct
    pe = pefile.PE(MGV, fast_load=True)
    img, base = pe.get_memory_mapped_image(), 0x140000000
    text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
    lo, hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
    roots = []
    for v in VTABLES:
        a = int(v, 16)
        while lo <= struct.unpack_from("<Q", img, a - base)[0] < hi:
            roots.append("0x%x" % struct.unpack_from("<Q", img, a - base)[0])
            a += 8
    return sorted(set(roots))


def class_roots():
    """Roots, vtables and function ranges of the classes listed in classes.txt (addresses from class_scan.py)."""
    import class_scan
    keys = []
    for line in open(Path(__file__).resolve().parent / "classes.txt", encoding="utf-8"):
        line = line.split("#")[0].strip()
        if line:
            keys.append(int(line, 16))
    inits = class_scan.scan_inits()
    roots, vtables, follow, inits_used = [], [], [], []
    for k in keys:
        d = inits.get(k)
        if not d:
            sys.stderr.write("class %x: no static initialiser found\n" % k)
            continue
        roots.append("0x%x" % d["init"])
        inits_used.append(d["init"])
        vtables.append(d["descVt"])
        slots = class_scan.vtable_slots(d["descVt"])
        creator = slots[1] if len(slots) > 1 else None
        fns = [creator] if creator else []
        if creator:
            for t in class_scan.lea_targets(creator):
                if class_scan.IB <= t < class_scan.IB + len(class_scan.IMG) - 8 and class_scan.is_code(class_scan.q(t)):
                    vtables.append(t)
                    fns += class_scan.vtable_slots(t)
        for f in fns:
            lo, hi = class_scan.func_of(f)
            follow.append(("0x%x" % lo, "0x%x" % hi))
    for vt in vtables:
        roots += ["0x%x" % f for f in class_scan.vtable_slots(vt)]
    return sorted(set(roots)), [], follow, inits_used


def main():
    out = sys.argv[1]
    c_roots, c_vtables, c_follow, c_inits = class_roots()
    hdr = Path(__file__).resolve().parents[2] / "src" / "GekkoHook" / "GekkoClassInits.h"
    hdr.write_text("// GekkoClassInits.h generated by tools/lift/lift_gekko.py from tools/lift/classes.txt, do not edit." + chr(10) + "#pragma once" + chr(10) + "constexpr unsigned long long kClassInits[] = { " + ", ".join("0x%xull" % a for a in c_inits) + " };" + chr(10), encoding="utf-8")
    cmd = [sys.executable, str(LIFT), MGV] + ROOTS + c_roots + vtable_roots() + c_vtables + ["--all-in", "0x140f60000", "0x140fb0000", "--out", out, "--resolver", "port_kit",
                                                        "--env-header", "lift_env.h", "--pc"]
    for lo, hi in FOLLOW + c_follow:
        cmd += ["--follow", lo, hi]
    for e in ENV:
        cmd += ["--env", e]
    cmd += ["--lift-twins", str(Path(__file__).resolve().parent / "lift_twins.txt")]
    argfile = Path(out).with_suffix(".args")
    argfile.write_text(chr(10).join(cmd[2:]) + chr(10), encoding="utf-8")
    cmd = cmd[:2] + ["@" + str(argfile)]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    sys.stdout.write(r.stdout[:3000])
    sys.stderr.write(r.stderr[-1500:])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
