# 29/09/2026 Survive mgv.exe: find a string and every rip-relative lea/mov that references it (from disk): mgv_xref.py text
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys, re, struct, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(str(paths.MGV_EXE), fast_load=True)
img, B = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
s = sys.argv[1].encode()
targets = [B + m.start() for m in re.finditer(re.escape(s) + b"\0", img)]
print("string at", [hex(t) for t in targets])
text = next(x for x in pe.sections if x.Name.startswith(b".text"))
lo, data = text.VirtualAddress, img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]
for i in range(len(data) - 7):   # 48/4C 8D modrm(rip) disp32  = lea r64,[rip+x]
    if data[i] in (0x48, 0x4c) and data[i+1] == 0x8d and (data[i+2] & 0xc7) == 0x05:
        t = B + lo + i + 7 + struct.unpack_from("<i", data, i + 3)[0]
        if t in targets: print("lea at %x" % (B + lo + i))
