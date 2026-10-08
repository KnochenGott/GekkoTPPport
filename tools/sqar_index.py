# 23/09/2026 SQAR v2 index reader for Survive master/*.dat (headers + entry headers, no data decryption).
"""Port of Fox_Parser MgsvModBldr.Tools.Qar QarFile.DecryptSectionList / QarEntry.Read.

Measured 23/09/2026: all 83,658 sections across the 5 master archives land in-bounds and
entry sizes are plausible. Usage: python tools/sqar_index.py [archive.dat ...]
"""
import struct
import sys

from env import MASTER

XOR = (0x41441043, 0x11C22050, 0xD05608C3, 0x532C7319)
SQAR = 0x52415153
PATH_CODE_USER_FLAG = 0x4000000000000


def ror32(x, r):
    # ROR, not ROL: QarFile.cs line 114 / GzsTool QarFile.cs line 128. A ROL port decodes
    # the first entry correctly and then derails, which is what the _port.py attempt hit.
    return ((x >> r) | (x << (32 - r))) & 0xFFFFFFFF if r else x


def decrypt_sections(blob, count):
    """v2 rolling cipher: seed starts 0xA2C18EC3, table index = (seed + byte_off // 5) % 4."""
    out, seed = [], 0xA2C18EC3
    for i in range(count):
        o1, o2 = i * 8, i * 8 + 4
        s1, s2 = struct.unpack_from("<II", blob, o1)
        i1 = s1 ^ XOR[(seed + o1 // 5) % 4]
        i2 = s2 ^ XOR[(seed + o2 // 5) % 4]
        out.append((i2 << 32) | i1)
        seed ^= ror32(i1, (i2 // 256) % 19)
    return out


def read_header(f):
    h = f.read(32)
    if struct.unpack_from("<I", h, 0)[0] != SQAR:
        raise ValueError("not an SQAR archive")
    return {
        "flags": struct.unpack_from("<I", h, 4)[0] ^ XOR[0],
        "count": struct.unpack_from("<I", h, 8)[0] ^ XOR[1],
        "version": struct.unpack_from("<I", h, 24)[0] ^ XOR[0],
    }


def entries(path):
    """Yield (offset, path_hash, compressed_size, uncompressed_size) per entry."""
    with open(path, "rb") as f:
        hdr = read_header(f)
        if hdr["version"] != 2:
            raise ValueError(f"version {hdr['version']} not handled here")
        shift = 12 if hdr["flags"] & 0x800 else 10
        for sec in decrypt_sections(f.read(8 * hdr["count"]), hdr["count"]):
            off = (sec >> 40) << shift
            f.seek(off)
            lo, hi, c, u = struct.unpack("<IIII", f.read(16))
            yield off, ((hi ^ XOR[0]) << 32) | (lo ^ XOR[0]), c ^ XOR[1], u ^ XOR[2]


def main(paths):
    for p in paths:
        n = comp = 0
        for off, h, c, u in entries(p):
            n += 1
            comp += c != u
        print(f"{p.name:14} entries={n:7,} compressed={comp:7,}")


if __name__ == "__main__":
    import pathlib
    args = [pathlib.Path(a) for a in sys.argv[1:]]
    main(args or sorted(MASTER.glob("*.dat")))
