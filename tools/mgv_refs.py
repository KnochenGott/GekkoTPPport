# 29/09/2026 Survive mgv.exe: pointers to VA in data (vtables) and rip-relative lea/mov to VA in code: mgv_refs.py va [tpp]
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys, struct, pefile
EXE = {"mgv": str(paths.MGV_EXE), "tpp": str(paths.TPP_EXE)}
pe = pefile.PE(EXE[sys.argv[2] if len(sys.argv) > 2 else "mgv"], fast_load=True)
img, B = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
t = int(sys.argv[1], 16)
for s in pe.sections:
    lo, d = s.VirtualAddress, img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
    name = s.Name.rstrip(b"\0").decode()
    if s.Characteristics & 0x20000000:
        for i in range(len(d) - 7):
            if d[i] in (0x48, 0x4c) and d[i+1] in (0x8d, 0x8b) and (d[i+2] & 0xc7) == 0x05 and B + lo + i + 7 + struct.unpack_from("<i", d, i + 3)[0] == t:
                print("code ref %x (%s)" % (B + lo + i, name))
    else:
        p = struct.pack("<Q", t); i = d.find(p)
        while i != -1:
            if i % 8 == 0: print("data ptr at %x (%s)" % (B + lo + i, name))
            i = d.find(p, i + 1)
