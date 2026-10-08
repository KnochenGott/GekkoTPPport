# fox2_patch.py 25/09/2026. Rewrites strings (paths, names) inside a binary fox2 without converting to XML.
# Binary layout, MEASURED on TPP parasite_metal.fox2 / zombie_assets.fox2:
#   header: magic f2 'box5', u32 at 0x08 = entity count, u32 at 0x0c = string table offset
#   body:   entities; every string value is stored as its StrCode64 (8 bytes)
#   table:  [u64 StrCode64][u32 length][bytes] ..., ended by a zero hash; pad 16, "\0\0end", pad 16
# Replacing a string = new table entry + every body occurrence of the old hash rewritten to the new one.
#   python tools/fox2_patch.py in.fox2 out.fox2 "old=new" ["old=new" ...]
import struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pathcode


def read_table(b):
    off = struct.unpack_from("<I", b, 0x0c)[0]
    table, i = [], off
    while True:
        h, n = struct.unpack_from("<QI", b, i)
        if h == 0:
            break
        table.append((h, b[i + 12:i + 12 + n].decode("utf-8")))
        i += 12 + n
    return off, table


def patch(b, repl):
    off, table = read_table(b)
    body = bytearray(b[:off])
    new_table = []
    for h, s in table:
        if s not in repl:
            new_table.append((h, s))
            continue
        ns = repl[s]
        nh = pathcode.string_id(ns)
        old8, new8 = struct.pack("<Q", h), struct.pack("<Q", nh)
        count = 0
        for k in range(0, len(body) - 7):
            if body[k:k + 8] == old8:
                body[k:k + 8] = new8
                count += 1
        if count == 0:
            raise ValueError(f"{s!r} has no reference in the body")
        new_table.append((nh, ns))
    out = bytearray(body)
    for h, s in new_table:
        raw = s.encode("utf-8")
        out += struct.pack("<QI", h, len(raw)) + raw
    out += bytes(8)                  # zero hash ends the table
    out += bytes(-len(out) % 16)
    out += b"\0\0end"
    out += bytes(-len(out) % 16)
    return bytes(out)


def main(src, dst, *pairs):
    repl = dict(p.split("=", 1) for p in pairs)
    b = Path(src).read_bytes()
    _, table = read_table(b)
    missing = [k for k in repl if k not in {s for _, s in table}]
    if missing:
        sys.exit(f"not in string table: {missing}")
    Path(dst).write_bytes(patch(b, repl))
    print(f"wrote {dst}: {len(repl)} strings replaced")


if __name__ == "__main__":
    main(*sys.argv[1:])
