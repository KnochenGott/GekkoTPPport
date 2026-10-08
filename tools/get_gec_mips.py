# get_gec_mips.py 25/09/2026. Pulls the missing high-mip .ftexs chunks of the gekko textures out of Survive's archives.
# Measured 25/09/2026: every gec .ftex mip table (16-byte entries at the end, byte 13 = ftexs number) puts mip 0 in
# .3.ftexs and mip 1 in .2.ftexs, but pack b2f8f37652082c2b.pftxs only has .1.ftexs. Search by NAME found nothing
# (research/archives/gec_search_texture*_gec0.txt: "no match"), so this streams them by PathCode64 hash instead.
# Hash extension codes checked: the unnamed pftxs entries start 156.. (.ftex) and b2c.. (.1.ftexs), as pathcode.py gives.
#   python tools/get_gec_mips.py      -> writes extracted/gec/mips/Assets/ssd/mecha/gec/Pictures/*.N.ftexs
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pathcode
from env import MASTER
from fox import EXTRACTED, mb

PICTURES = EXTRACTED / "b2f8f37652082c2b_pftxs" / "Assets/ssd/mecha/gec/Pictures"
OUT = EXTRACTED / "mips"
USER_FLAG = 0x4000000000000  # PATH_CODE_USER_FLAG_MASK; 25/09/2026 run: flagged codes not in any fixed archive


def archives():
    """Every .dat under Survive's master folder, subfolders included, texture archives first."""
    dats = sorted(MASTER.rglob("*.dat"))
    return sorted(dats, key=lambda p: (0 if "texture" in p.name.lower() else 1, str(p)))


def needed_chunks(ftex):
    b = ftex.read_bytes()
    mips = b[16]
    table = len(b) - 16 * mips
    return sorted({b[table + 16 * i + 13] for i in range(mips)} - {1})


def main():
    dats = archives()
    print(f"searching {len(dats)} archives under {MASTER}:")
    for d in dats:
        print(f"  {d}")
    found = missing = 0
    for ftex in sorted(PICTURES.glob("*.ftex")):
        for n in needed_chunks(ftex):
            game = f"/Assets/ssd/mecha/gec/Pictures/{ftex.stem}.{n}.ftexs"
            full = pathcode.path_code(game)
            codes = [f"{full:016x}", f"{full & ~USER_FLAG:016x}"]
            code = codes[0]
            dest = OUT / game.lstrip("/")
            dest.parent.mkdir(parents=True, exist_ok=True)
            hit = None
            out = ""
            for dat in dats:
                for c in codes:
                    rc, out = mb("stream", dat, c, "-o", dest)
                    if rc == 0 and dest.exists() and dest.stat().st_size > 0:
                        hit, code = dat.relative_to(MASTER), c
                        break
                if hit:
                    break
            if hit:
                found += 1
                print(f"OK      {game}  {code}  from {hit}  {dest.stat().st_size} bytes")
            else:
                missing += 1
                print(f"MISSING {game}  {code}  (tried {' and '.join(codes)} in {len(dats)} archives)")
    print(f"{found} found, {missing} missing -> {OUT}")


if __name__ == "__main__":
    main()
