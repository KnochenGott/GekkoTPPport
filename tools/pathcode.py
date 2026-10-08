# 23/09/2026 PathCode64 / StringId / ExtensionCode for SurviveResearch.
"""Line-by-line Python port of Fox_Parser MgsvModBldr.Tools.GameHashing.

GameHash.cs + GameCityHash.cs, translated with no algorithmic changes. All arithmetic is
u64 and wrapped mod 2**64 exactly as C# ulong does (mask after every multiply / shift that
can grow the value). CityHash64WithSeeds is the shared Fox engine hash; PathCode is the
per-path code stored in SQAR entry headers.

Usage:
    python tools/pathcode.py path [path ...]      # print PathCode for each
    python tools/pathcode.py --id path [path...]  # print StringId
    python tools/pathcode.py --ext .mtar          # print ExtensionCode
"""
import sys
import argparse

_MASK64 = 0xFFFFFFFFFFFFFFFF

# ---- GameCityHash.cs ----

K0 = 0xC3A5C85C97CB3127
K1 = 0xB492B66FBE98F273
K2 = 0x9AE16A3B2F90404F
K3 = 0xC949D7C7509E6557


def _hash128to64(low, high):
    k_mul = 0x9DDFEA08EB382D69
    a = ((low ^ high) * k_mul) & _MASK64
    a ^= a >> 47
    b = ((high ^ a) * k_mul) & _MASK64
    b ^= b >> 47
    b = (b * k_mul) & _MASK64
    return b


def _rotate(val, shift):
    # RotateByAtLeast1 and Rotate share the body; Rotate special-cases shift == 0.
    val &= _MASK64  # Python ints are unbounded; negative or >64-bit inputs must wrap like C# ulong
    if shift == 0:
        return val
    return ((val >> shift) | (val << (64 - shift))) & _MASK64


def _shift_mix(val):
    val &= _MASK64
    return (val ^ (val >> 47)) & _MASK64


def _hash_len16(u, v):
    return _hash128to64(u, v)


def _fetch64(buf, index):
    return int.from_bytes(buf[index:index + 8], "little")


def _fetch32(buf, index):
    return int.from_bytes(buf[index:index + 4], "little")


def _hash_len0_to16(s, offset):
    length = len(s) - offset
    if length > 8:
        a = _fetch64(s, offset)
        b = _fetch64(s, offset + length - 8)
        return _hash_len16(a, _rotate(b + length, length) & _MASK64) ^ b
    if length >= 4:
        a = _fetch32(s, offset)
        return _hash_len16((length + (a << 3)) & _MASK64, _fetch32(s, offset + length - 4))
    if length > 0:
        a = s[offset]
        b = s[offset + (length >> 1)]
        c = s[offset + (length - 1)]
        y = (a + ((b << 8))) & 0xFFFFFFFF
        z = (length + (c << 2)) & 0xFFFFFFFF
        return (((_shift_mix(y * K2 ^ z * K3)) & _MASK64) * K2) & _MASK64
    return K2


def _hash_len17_to32(s):
    length = len(s)
    a = (_fetch64(s, 0) * K1) & _MASK64
    b = _fetch64(s, 8)
    c = (_fetch64(s, length - 8) * K2) & _MASK64
    d = (_fetch64(s, length - 16) * K0) & _MASK64
    return _hash_len16(
        (_rotate(a - b, 43) + _rotate(c, 30) + d) & _MASK64,
        (a + _rotate(b ^ K3, 20) - c + length) & _MASK64,
    )


def _weak32_core(w, x, y, z, a, b):
    a = (a + w) & _MASK64
    b = _rotate((b + a + z) & _MASK64, 21)
    c = a
    a = (a + x + y) & _MASK64
    b = (b + _rotate(a, 44)) & _MASK64
    return (a + z) & _MASK64, (b + c) & _MASK64


def _weak_hash_len32_with_seeds(buf, offset, a, b):
    # Span overload of the core: reads four little-endian u64s at offset..offset+24.
    return _weak32_core(
        _fetch64(buf, offset), _fetch64(buf, offset + 8),
        _fetch64(buf, offset + 16), _fetch64(buf, offset + 24), a, b)


def _hash_len33_to64(s):
    # CityHash 1.0.3 HashLen33to64 (Fox uses 1.0.x; 1.1 differs here)
    length = len(s)
    z = _fetch64(s, 24)
    a = (_fetch64(s, 0) + ((length + _fetch64(s, length - 16)) * K0)) & _MASK64
    b = _rotate((a + z) & _MASK64, 52)
    c = _rotate(a, 37)
    a = (a + _fetch64(s, 8)) & _MASK64
    c = (c + _rotate(a, 7)) & _MASK64
    a = (a + _fetch64(s, 16)) & _MASK64
    vf = (a + z) & _MASK64
    vs = (b + _rotate(a, 31) + c) & _MASK64
    a = (_fetch64(s, 16) + _fetch64(s, length - 32)) & _MASK64
    z = _fetch64(s, length - 8)
    b = _rotate((a + z) & _MASK64, 52)
    c = _rotate(a, 37)
    a = (a + _fetch64(s, length - 24)) & _MASK64
    c = (c + _rotate(a, 7)) & _MASK64
    a = (a + _fetch64(s, length - 16)) & _MASK64
    wf = (a + z) & _MASK64
    ws = (b + _rotate(a, 31) + c) & _MASK64
    r = _shift_mix(((vf + ws) * K2 + (wf + vs) * K0) & _MASK64)
    return (_shift_mix((r * K0 + vs) & _MASK64) * K2) & _MASK64


def _city_hash64(s):
    # Short strings: CityHash 1.0.3 branches. Over 64 bytes: the CityHash 1.1 loop, as in GameCityHash.cs.
    # Every product is masked to 64 bits; unmasked Python ints broke this branch before 25/09/2026.
    length = len(s)
    if length <= 32:
        return _hash_len0_to16(s, 0) if length <= 16 else _hash_len17_to32(s)
    if length <= 64:
        return _hash_len33_to64(s)

    x = _fetch64(s, length - 40)
    y = (_fetch64(s, length - 16) + _fetch64(s, length - 56)) & _MASK64
    z = _hash_len16((_fetch64(s, length - 48) + length) & _MASK64, _fetch64(s, length - 24))
    v_low, v_high = _weak_hash_len32_with_seeds(s, length - 64, length, z)
    w_low, w_high = _weak_hash_len32_with_seeds(s, length - 32, (y + K1) & _MASK64, x)
    x = (x * K1 + _fetch64(s, 0)) & _MASK64

    n = (length - 1) & ~63
    offset = 0
    while n != 0:
        x = (_rotate((x + y + v_low + _fetch64(s, offset + 8)) & _MASK64, 37) * K1) & _MASK64
        y = (_rotate((y + v_high + _fetch64(s, offset + 48)) & _MASK64, 42) * K1) & _MASK64
        x ^= w_high
        y = (y + v_low + _fetch64(s, offset + 40)) & _MASK64
        z = (_rotate((z + w_low) & _MASK64, 33) * K1) & _MASK64
        v_low, v_high = _weak_hash_len32_with_seeds(s, offset, (v_high * K1) & _MASK64, (x + w_low) & _MASK64)
        w_low, w_high = _weak_hash_len32_with_seeds(s, offset + 32, (z + w_high) & _MASK64, (y + _fetch64(s, offset + 16)) & _MASK64)
        z, x = x, z
        offset += 64
        n -= 64

    return _hash_len16(
        (_hash_len16(v_low, w_low) + _shift_mix(y) * K1 + z) & _MASK64,
        (_hash_len16(v_high, w_high) + x) & _MASK64,
    )


def city_hash64_with_seeds_bytes(s, seed0, seed1):
    return _hash_len16((_city_hash64(s) - seed0) & _MASK64, seed1)


# ---- GameHash.cs ----

STRING_ID_MASK = 0xFFFFFFFFFFFF
PATH_CODE_BASE_MASK = 0x3FFFFFFFFFFFF
PATH_CODE_EXT_MASK = 0x1FFF
PATH_CODE_EXTENSION_OFFSET = 0x33
PATH_CODE_USER_FLAG_MASK = 0x4000000000000
PATH_CODE_USER_FLAG_ANTIMASK = ((-0x4000000000000) - 1) & ((1 << 64) - 1)
ASSET_PATH = "/Assets/"

_RUNTIME_PROJECT_LIST = ("fox", "fox_export", "tpp", "sh", "mgo")


def _raw_path_hash_code(path):
    # path is a str (chars), matching ReadOnlySpan<char> in the C#.
    seed0 = 0x9AE16A3B2F90404F
    seed1 = 0
    i = len(path) - 1
    j = 0
    while i >= 0 and j < 8:
        seed1 |= ord(path[i]) << (j * 8)
        i -= 1
        j += 1
    return city_hash64_with_seeds_bytes(path.encode("ascii"), seed0, seed1)


def _city_hash_str_code_inner(str_in):
    seed0 = 0x9AE16A3B2F90404F
    seed1 = (ord(str_in[0]) << 16) + len(str_in) if len(str_in) > 0 else 0
    h = city_hash64_with_seeds_bytes(str_in.encode("ascii") + b"\0", seed0, seed1)
    return h & STRING_ID_MASK


def _ff_path_ext_code(ext):
    if len(ext) > 0 and ext[0] == ".":
        ext = ext[1:]
    return _raw_path_hash_code(ext) & PATH_CODE_EXT_MASK


def extension_code(ext):
    return _ff_path_ext_code(ext)


def ext_code_of(path_code):
    return (path_code >> PATH_CODE_EXTENSION_OFFSET) & 0xFFFFFFFF


def _path_hash_code(path):
    final_base_segment_index = path.rfind("/")
    if final_base_segment_index == -1:
        final_base_segment_index = path.rfind("\\")
        if final_base_segment_index == -1:
            final_base_segment_index = 0

    final_segment = path[final_base_segment_index:]

    ext_index = final_segment.find(".")
    if ext_index == -1:
        ext_index = len(path)
    else:
        ext_index = final_base_segment_index + ext_index

    ext_span = path[ext_index:]
    path_span = path[: len(path) - len(ext_span)]

    if path_span.startswith(ASSET_PATH):
        path_span = path_span[len(ASSET_PATH):]

    base_hash = (_raw_path_hash_code(path_span) & PATH_CODE_BASE_MASK)
    extension_hash = (_ff_path_ext_code(ext_span) << PATH_CODE_EXTENSION_OFFSET)
    return extension_hash | base_hash


def _strip_leading_slash(s):
    return s[1:] if len(s) > 0 and s[0:1] == "/" else s


def _set_user_flag(hash_code, user_flag):
    mask = 0 if user_flag == 0 else ((1 << 64) - 1)
    return (mask & PATH_CODE_USER_FLAG_MASK) | (hash_code & PATH_CODE_USER_FLAG_ANTIMASK)


def _is_implicit_runtime_project(path):
    # IsImplicitRuntimeProject
    assets_index = path.find(ASSET_PATH)
    if assets_index == -1:
        return False
    path = path[assets_index + len(ASSET_PATH):]
    next_slash = path.find("/")
    if next_slash == -1:
        return False
    path = path[:next_slash]
    return path in _RUNTIME_PROJECT_LIST


def _path_code_inner(path):
    if path.startswith(ASSET_PATH):
        code = _path_hash_code(path)
        return code if _is_implicit_runtime_project(path) else _set_user_flag(code, 1)

    if len(path) == 0:
        return 0

    must_reallocate = "\\" in path
    if must_reallocate:
        modified = path.replace("\\", "/")
        span = modified
        starter_prefix = modified.find("./")
        if starter_prefix != -1:
            span = span[2:]
        span = _strip_leading_slash(span)
        code = _path_hash_code(span)
        return _set_user_flag(code, 1)
    else:
        starter_prefix = path.find("./")
        if starter_prefix != -1:
            path = path[2:]
        path = _strip_leading_slash(path)
        code = _path_hash_code(path)
        return _set_user_flag(code, 1)


def path_code(str_in):
    if len(str_in) > 2 and str_in[:2] == "0x":
        try:
            return int(str_in[2:], 16)
        except ValueError:
            pass
    return _path_code_inner(str_in)


def string_id(str_in):
    if len(str_in) > 2 and str_in[:2] == "0x":
        try:
            return int(str_in[2:], 16)
        except ValueError:
            pass
    return _city_hash_str_code_inner(str_in)


def main(argv):
    parser = argparse.ArgumentParser(description="PathCode64 / StringId / ExtensionCode (Fox_Parser port).")
    parser.add_argument("paths", nargs="*")
    grp = parser.add_mutually_exclusive_group()
    grp.add_argument("--id", action="store_true", help="print StringId instead of PathCode")
    grp.add_argument("--ext", action="store_true", help="treat each arg as an extension and print ExtensionCode")
    args = parser.parse_args(argv)

    if args.ext:
        for p in args.paths:
            print(extension_code(p.encode("ascii")))
        return

    for p in args.paths:
        if args.id:
            print(string_id(p))
        else:
            print(path_code(p))


if __name__ == "__main__":
    main(sys.argv[1:])
