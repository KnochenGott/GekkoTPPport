# gen_gekko_params.py 03/10/2026. Gecko block of the community SsdNpcLevelParameters.json -> src/GekkoHook/GekkoParamBlock.h (committed) + build/gekko/gen/gekko_param_block.tsv.
# Usage: gen_gekko_params.py [json] [outdir]. json defaults to the environment variable GEKKO_PARAM_JSON (alicealys/mgv-server-emulator, see README); outdir (header) defaults to src/GekkoHook.
# Only needed to regenerate the committed header; the build uses the committed one when GEKKO_PARAM_JSON is not set.
# Field table = Survive's loader 0x140b983f0 (READ with tools/scratch/dis_g.py): offsets, types, defaults. Fails when the JSON keys and the loader keys differ.
import json, struct, sys
from pathlib import Path
R = Path(__file__).resolve().parent.parent
import os
_j = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("GEKKO_PARAM_JSON")
if not _j or not Path(_j).is_file(): sys.exit("GEKKO_PARAM_JSON must point to SsdNpcLevelParameters.json of alicealys/mgv-server-emulator (see README.md); got: %r" % _j)
SRC = Path(_j)
OUTDIR = Path(sys.argv[2]) if len(sys.argv) > 2 else R / "src" / "GekkoHook"
TSVDIR = R / "build" / "gekko" / "gen"
OUTDIR.mkdir(parents=True, exist_ok=True); TSVDIR.mkdir(parents=True, exist_ok=True)
g = json.load(open(SRC))["Gecko"]
bp = g["baseParam"][0]            # loader takes the first baseParam element only (Round 4)
F = []                            # (struct offset, kind f32/u32/u16/u8, value, json key)
def f32(o, k): F.append((o, "f32", float(bp.get(k, 0)), k))
# sight reader 0x140c166b0 into struct+8: three single groups, then three sub-blocks (0x140c16a30) of 0x34 bytes
for o, p in ((0, "contactSightForm"), (0xc, "normalSightForm"), (0x18, "farSightForm")):
    for d, s in enumerate(("distance", "verticalAngle", "horizontalAngle")): f32(8 + o + 4 * d, "%s_%s" % (p, s))
for base, p in ((0x24, "baseSight"), (0x58, "dungeonSight"), (0x8c, "noFogSight")):
    if (p + "_discovery_distance") in bp: F.append((8 + base, "u8", 1, p + "_discovery_distance (presence flag)"))
    for go, grp in ((4, "discovery"), (0x10, "indis"), (0x1c, "dim"), (0x28, "far")):
        for d, s in enumerate(("distance", "verticalAngle", "horizontalAngle")): f32(8 + base + go + 4 * d, "%s_%s_%s" % (p, grp, s))
# camouflage reader 0x140c15ff0 into struct+0xc8
for go, grp in ((0, "discovery"), (0xc, "indis"), (0x18, "dim"), (0x24, "far")):
    for d, s in enumerate(("enemy", "character", "object")): f32(0xc8 + go + 4 * d, "sightCamouflage_%s_%s" % (grp, s))
# hearing reader 0x140c15a50 / 0x140c15a90 into struct+0xf8
for o, s in ((0, "zero"), (4, "ss"), (8, "hs"), (0xc, "s"), (0x10, "m"), (0x14, "l"), (0x18, "hll"), (0x1c, "ll"), (0x20, "alert"), (0x24, "special")):
    f32(0xf8 + o, "hearingRange_normal_" + s)
# scalars: 0x140c15ec0 (life, defense dwords), 0x140c15a20 (fulton, 0 -> 150), loader words
F.append((0x120, "u32", bp["life"], "life")); F.append((0x124, "u32", bp["defense"], "defense"))
F.append((0x14c, "u32", bp["expPointRateByFulton"] or 150, "expPointRateByFulton"))
for o, k in ((0x128, "partsLife_rightLeg"), (0x12a, "partsLife_leftReg"), (0x12c, "partsLife_rightW"), (0x12e, "partsLife_leftW"), (0x130, "partsLife_camera"),
             (0x138, "defense_weak"), (0x13a, "defense_Leg"), (0x13c, "defense_Gun"), (0x13e, "defense_Locket"), (0x148, "bullet_defense")):
    F.append((o, "u16", bp[k] & 0xffff, k))
# buff_normal list 0x140b985f0: level byte slots at +0x150 (4), dword table at +0x154 (4 rows of 4); first free slot/row
lv, tab = [0] * 4, [0] * 16
for e in g["buff_normal"]:
    l, c = e["level"] & 0xff, e["buffCode"] & 0xffffffff
    s = next((i for i in range(4) if lv[i] == 0 or lv[i] == l), None)
    if s is None: continue
    lv[s] = l
    j = next((j for j in range(s * 4, s * 4 + 4) if tab[j] == 0), None)
    if j is not None: tab[j] = c
for i in range(4):
    if lv[i]: F.append((0x150 + i, "u8", lv[i], "buff_normal[].level"))
for j in range(16):
    if tab[j]: F.append((0x154 + 4 * j, "u32", tab[j], "buff_normal[].buffCode"))
# coop_exp 0x140b98760: last element's value for key "Gecko", 0 -> 100
ce = 0
for e in g["coop_exp"]: ce = e.get("Gecko", 0)
F.append((0x194, "u32", ce or 100, "coop_exp[].Gecko"))
# key check both ways (levelParam is a separate loader and is not part of this struct)
used = {k.split(" ")[0] for _, _, _, k in F if not k.startswith(("buff_", "coop_"))}
miss = sorted(used - set(bp)); extra = sorted(set(bp) - used)
print("loader keys read: %d, JSON baseParam keys: %d, loader keys missing in JSON: %s, JSON keys the loader does not read: %s" % (len(used), len(bp), miss, extra))
if miss or extra: sys.exit(1)
F.sort(key=lambda t: t[0])
bits = {"f32": lambda v: struct.unpack("<I", struct.pack("<f", v))[0]}
with open(OUTDIR / "GekkoParamBlock.h", "w") as h:
    h.write("// GekkoParamBlock.h 03/10/2026. Gecko parameter values from the community server capture in alicealys/mgv-server-emulator (commit defe443cba93b2f2ee0dcc19ddbb142b0d610bba, src/server/resources/data/parameter/SsdNpcLevelParameters.json, sha256 f7afa10ab3301942cf0098ae7174636d215b810c141b7b427fd94da4a0dcf019; capture provenance unverified). GENERATED by tools/gen_gekko_params.py, do not edit.\n")
    h.write("#pragma once\n#include <cstdint>\nnamespace GekkoParamBlock {\n// kind: 0 f32 (raw bits), 1 u32, 2 u16, 3 u8. off = byte offset in SsdGeckoParameter (loader 0x140b983f0).\n")
    h.write("struct Entry { uint16_t off; uint8_t kind; uint32_t value; };\nstatic const Entry kTable[] = {\n")
    for o, k, v, key in F:
        kind = {"f32": 0, "u32": 1, "u16": 2, "u8": 3}[k]
        raw = bits["f32"](v) if k == "f32" else int(v)
        h.write("    {0x%03x, %d, 0x%08xu},  // %s\n" % (o, kind, raw, key))
    h.write("};\nstatic const int kCount = %d;\n}\n" % len(F))
with open(TSVDIR / "gekko_param_block.tsv", "w") as t:
    t.write("struct_offset\tkind\tvalue\tsource (emulator JSON key under Gecko)\n")
    for o, k, v, key in F: t.write("+0x%03x\t%s\t%s\t%s\n" % (o, k, v, key))
print("entries", len(F))
