# 24/09/2026 One entry point for archive work: the model types short names, never game paths or quotes.
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
# python tools/fox.py search <archive> <pattern>      archive: 00 01 chunk0 chunk1 data1 texture0 texture1
# python tools/fox.py extract <archive> <packhash>    stream + unpack + convert + summarize
# python tools/fox.py ls [subfolder of extracted/]    list extracted files (no .xml, no textures)
# python tools/fox.py unhash <hex> [...]              reverse-lookup hashes (gani names, fv2 file ids)
# python tools/fox.py hash <gamepath> [...]            PathCode64 etc. of candidate names
# python tools/fox.py match <mog.xml> <mtar.xml>       how many mog animations the mtar holds
# python tools/fox.py packhash <archive> <packname>    hash of a named pack (search shows names only)
import subprocess
import sys

from env import MASTER, ROOT
import foxsum

MB = str(paths.MODBLDR)
ARCHIVES = {
    "00": MASTER / "0" / "00.dat",
    "01": MASTER / "0" / "01.dat",
    "chunk0": MASTER / "chunk0.dat",
    "chunk1": MASTER / "chunk1.dat",
    "data1": MASTER / "data1.dat",
    "texture0": MASTER / "texture0.dat",
    "texture1": MASTER / "texture1.dat",
}
EXTRACTED = ROOT / "extracted" / "gec"
RESEARCH = ROOT / "research"
CONVERT = {".fox2", ".sim", ".parts", ".tgt", ".mtar", ".fv2", ".mog", ".ftex"}
# Stream output has no extension; the unpacker picks its reader by extension.
MAGIC = {b"foxfpkd": ".fpkd", b"foxfpk": ".fpk", b"PFTX": ".pftxs"}
SKIP_LS = {".xml", ".ftexs", ".json", ".enchnk"}


def mb(*args):
    r = subprocess.run([MB, *map(str, args)], capture_output=True, text=True, errors="replace")
    return r.returncode, (r.stdout + r.stderr).strip()


def archive(name):
    if name not in ARCHIVES:
        sys.exit(f"ERROR unknown archive '{name}'. Use one of: {' '.join(ARCHIVES)}")
    return ARCHIVES[name]


def search(name, pattern):
    code, out = mb("search", archive(name), pattern)
    safe = pattern.replace("*", "")
    dest = RESEARCH / "archives" / f"gec_search_{name}_{safe}.txt"
    dest.write_text(out + "\n", encoding="utf-8")
    lines = out.splitlines()
    print(f"exit {code}, {len(lines)} lines -> {dest.relative_to(ROOT).as_posix()}")
    print("\n".join(lines[:40]))
    if len(lines) > 40:
        print(f"... {len(lines) - 40} more lines in the file")


def extract(name, packhash):
    EXTRACTED.mkdir(parents=True, exist_ok=True)
    raw = EXTRACTED / f"{packhash}.bin"
    code, out = mb("stream", archive(name), packhash, "-o", raw)
    if code or not raw.exists():
        sys.exit(f"ERROR stream failed (exit {code}): {out}")
    magic = raw.read_bytes()[:8]
    ext = next((e for m, e in MAGIC.items() if magic.startswith(m)), None)
    if not ext:
        sys.exit(f"ERROR unknown pack magic {magic!r} in {raw.name}")
    pack = raw.replace(raw.with_suffix(ext))
    code, out = mb(pack)
    folder = EXTRACTED / f"{packhash}_{ext[1:]}"
    if code or not folder.exists():
        sys.exit(f"ERROR unpack failed (exit {code}): {out}")
    missing_mips = 0
    for f in sorted(folder.rglob("*")):
        if f.suffix in CONVERT:
            code, out = mb(f)
            xml = f.with_name(f.name + ".xml")
            if xml.exists():
                foxsum.summarize(xml)
            elif f.with_suffix(".dds").exists():
                print(f"texture {f.with_suffix('.dds').relative_to(EXTRACTED).as_posix()}")
            elif f.suffix == ".ftex" and "ftexs not found" in out:
                missing_mips += 1
            else:
                print(f"ERROR convert {f.name} (exit {code}): {out[-300:]}")
    if missing_mips:
        # Measured 24/09/2026: pack b2f8 holds only .1.ftexs; .2/.3 mips live elsewhere (not yet found).
        print(f"note: {missing_mips} .ftex not converted, higher mip .ftexs files are not in this pack")
    ls(folder.name)


def ls(sub="", limit=60):
    files = [f for f in sorted((EXTRACTED / sub).rglob("*")) if f.is_file() and f.suffix not in SKIP_LS]
    for f in files[:limit]:
        print(f.relative_to(EXTRACTED).as_posix())
    if len(files) > limit:
        print(f"... {len(files) - limit} more; narrow with: fox.py ls <subfolder>")


def packhash(name, pack):
    print(mb("stream", archive(name), "--list", pack)[1])


def unhash(*hashes):
    print(mb("unhash", *hashes)[1])


def hash_(*names):
    print(mb("hash", *names)[1])


# Measured 24/09/2026 on Zombie_layers: mog <gani id> is PathCode64 of the .gani path (extension bits on top);
# mtar <Gani FilePath> is the same hash without them (or the plain path when resolved). Low 51 bits match.
LOW51 = (1 << 51) - 1


def _gani_key(v):
    return v if v.startswith("/") else int(v, 16) & LOW51


def match(mog_xml, mtar_xml):
    import xml.etree.ElementTree as ET
    mog = {int(g.get("id"), 16) & LOW51 for g in ET.parse(mog_xml).getroot().iter("gani")}
    mtar = [g.get("FilePath") for g in ET.parse(mtar_xml).getroot().iter() if g.tag.endswith("Gani")]
    hashed = {_gani_key(v) for v in mtar if not v.startswith("/")}
    print(f"mog animations {len(mog)}, mtar entries {len(mtar)} ({len(mtar) - len(hashed)} named)")
    print(f"hashed mtar entries found in mog: {len(mog & hashed)} (named entries not compared; hash them first)")


def main():
    cmds = {"search": (search, 2), "extract": (extract, 2), "ls": (ls, None), "unhash": (unhash, None),
            "hash": (hash_, None), "match": (match, 2), "packhash": (packhash, 2)}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit("usage: fox.py search <archive> <pattern> | extract <archive> <packhash> | ls [folder] | unhash <hex>... | hash <path>... | match <mog.xml> <mtar.xml> | packhash <archive> <packname>")
    fn, n = cmds[sys.argv[1]]
    args = sys.argv[2:]
    if n is not None and len(args) != n:
        sys.exit(f"ERROR {sys.argv[1]} takes {n} arguments")
    fn(*args)


if __name__ == "__main__":
    main()
