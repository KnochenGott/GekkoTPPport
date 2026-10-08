# populate_project.py 29/09/2026. Fills ProjectFolder/GekkoPort for MakeBite: copies (overwrites) the gekko_dll
# fpk/fpkd folders, the loose .2/.3 mips, the packed gekko_dll.pftxs and Gekko.dll. Nothing is wiped any more (changed
# 29/09/2026): only the stale boss-event folder is removed. GameDir/mod/modules
# (InfGekkoSpawn.lua) is tracked in git and left alone.
#   python tools/populate_project.py [--no-dll]
import os, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "extracted" / "gec"
PROJECT = ROOT / "ProjectFolder" / "GekkoPort"
ASSETS = PROJECT / "Assets"
GAMEDIR = PROJECT / "GameDir"
PACK = ASSETS / "tpp" / "pack" / "gekko"
FPK = PACK / "gekko_dll_fpk"
FPKD = PACK / "gekko_dll_fpkd"
# Texture pack target: gekko_dll.pftxs next to gekko_dll.fpk, holding .ftex + .1.ftexs; .2/.3 loose (decided 26/09/2026).
# MakeBite did NOT build a pftxs from a _pftxs folder: the 26/09/2026 ih_log pack list shows gekko_dll.fpk/.fpkd and
# the stray gekko_dll.pftxs.json, no .pftxs. Until a pftxs packer is known, textures install loose (worked before)
# and the pftxs folder + manifest are staged under build/ for that packer.
PFTXS = ROOT / "build" / "pftxs" / "gekko_dll_pftxs"

# (source folder in extracted/gec, glob, destination). Split the TPP way, measured on IH's kaz0_main0_mdl,
# ih_parasite_camo and ih_rat packs: fpk = fmdl/frdv/fcnp/frig/mtar/fv2, fpkd = fox2/parts/sim.
# .tgt and .vfx go to the fpkd (IH ih_puppy / ih_raven / ih_parasite_camo packs). Left out: any authored .mog.
RULES = [
    ("5228_fpkd", "Assets/ssd/mecha/gec/Scenes/*", FPK),
    ("5228_fpkd", "Assets/ssd/rig/frig/ssd_gekkou.frig", FPK),
    ("5228_fpkd", "Assets/ssd/motion/mtar/zombie/Gecko_layers.mtar", FPK),
    ("5228_fpkd", "Assets/ssd/fova/mecha/gec/*.fv2", FPK),
    ("ed50_fpkd", "Assets/ssd/parts/mecha/gec/*.parts", FPKD),
    ("ed50_fpkd", "Assets/ssd/mecha/gec/Fox_Files/gec0_main0_def.sim", FPKD),
    ("ed50_fpkd", "Assets/ssd/parts/zombie/GeckoTargetDefense.tgt", FPKD),
    ("ed50_fpkd", "Assets/tpp/parts/weapon/hew/hw01_gun_power.tgt", FPKD),
    ("b2f8f37652082c2b_pftxs", "Assets/ssd/mecha/gec/Pictures/*", PFTXS),
    ("b2f8f37652082c2b_pftxs", "Assets/ssd/mecha/gec/Pictures/*", ASSETS.parent),
    # .2/.3.ftexs high mips pulled by hash with tools/get_gec_mips.py (the pftxs only carries .1.ftexs)
    ("mips", "Assets/ssd/mecha/gec/Pictures/*", ASSETS.parent),
]
FOX2 = ROOT / "assets" / "fox2" / "gekko_dll.fox2"   # binary of assets/fox2/gekko_dll.fox2.xml (modbldr-tools)
FOX2_OUT = FPKD / "Assets" / "tpp" / "level_asset" / "gekko" / "gekko_dll.fox2"
# Left over from the abandoned boss-event route; removed so MakeBite does not ship them.
STALE = [GAMEDIR / "mod" / "bosses"]


# The packed texture pack (built outside this script), shipped next to gekko_dll.fpk.
PFTXS_PACKED = ROOT / "gekko_dll.pftxs"


def clean():
    for d in STALE:
        if d.exists():
            shutil.rmtree(d)
            print(f"removed {d.relative_to(ROOT)}")


def build_dll():
    """Runs build_gekko.bat (sigs + MSVC build) and copies Gekko.dll to GameDir/plugins."""
    if os.name != "nt":
        print("not on Windows: skipping the DLL build")
        return True
    if subprocess.call(["cmd", "/c", str(ROOT / "build_gekko.bat"), "nopause"], cwd=ROOT) != 0:
        print("DLL build FAILED, see build/gekko/build.log")
        return False
    out = GAMEDIR / "plugins" / "Gekko.dll"
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "build" / "gekko" / "Gekko.dll", out)
    print(f"Gekko.dll -> {out.relative_to(ROOT)}")
    return True


def referenced_vfx():
    """Every .vfx the gekko parts and tgt files reference (EffectDescription), found anywhere in extracted/gec."""
    refs = set()
    for x in list((SRC / "ed50_fpkd").glob("Assets/ssd/parts/mecha/gec/*.parts.xml")) + \
            list((SRC / "ed50_fpkd").rglob("*.tgt.xml")):
        refs |= set(re.findall(r">(/Assets/[^<]+\.vfx)<", x.read_text(encoding="utf-8-sig", errors="ignore")))
    found = []
    for r in sorted(refs):
        hit = next((f for f in SRC.glob("*_fpkd" + r) if f.is_file()), None)
        if hit:
            found.append((hit, FPKD / r.lstrip("/")))
        else:
            print(f"MISSING vfx {r}")
    return found


def copy_assets():
    n = 0
    for src, out in referenced_vfx():
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, out)
        n += 1
    for folder, pattern, dest in RULES:
        base = SRC / folder
        for f in base.glob(pattern):
            if not f.is_file() or f.suffix == ".xml":
                continue
            out = dest / f.relative_to(base)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            n += 1
    if PFTXS_PACKED.exists():
        PACK.mkdir(parents=True, exist_ok=True)
        shutil.copy2(PFTXS_PACKED, PACK / PFTXS_PACKED.name)
        n += 1
    else:
        print(f"MISSING {PFTXS_PACKED.name} (packed texture pack) in the repo root")
    if FOX2.exists():
        FOX2_OUT.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(FOX2, FOX2_OUT)
        n += 1
    else:
        print(f"MISSING {FOX2.relative_to(ROOT)}: convert gekko_dll.fox2.xml with modbldr-tools, then re-run")
    return n


def write_pftxs_manifest():
    """gekko_dll.pftxs.json next to the folder, same schema as the unpacked Survive b2f8f37652082c2b.pftxs.json."""
    import json
    pics = PFTXS / "Assets/ssd/mecha/gec/Pictures"
    groups = []
    for ftex in sorted(pics.glob("*.ftex")):
        rel = "/" + ftex.relative_to(PFTXS).as_posix()
        groups.append({"entries": [{"filePath": rel}, {"filePath": rel[:-5] + ".1.ftexs"}]})
    out = PFTXS.parent / "gekko_dll.pftxs.json"
    out.write_text(json.dumps({"type": "pftxs", "groups": groups}, indent=2), encoding="utf-8")
    print(f"pftxs manifest: {len(groups)} textures -> {out.relative_to(ROOT)}")


def main():
    # Assets first: a failed DLL build must not leave the project emptied by clean() (29/09/2026, user report).
    clean()
    n = copy_assets()
    write_pftxs_manifest()
    print(f"copied {n} files into {PROJECT.relative_to(ROOT)}")
    dll_ok = "--no-dll" in sys.argv or build_dll()
    return 0 if n and FOX2.exists() and dll_ok else 1


if __name__ == "__main__":
    sys.exit(main())
