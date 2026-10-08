# dis.py 30/09/2026. Throwaway: disassemble the installed mgv.exe. Usage: dis.py START END  (hex VAs)
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
EXE = str(paths.MGV_EXE)
pe = pefile.PE(EXE, fast_load=True)
img, base = pe.get_memory_mapped_image(), 0x140000000
a, b = int(sys.argv[1], 16), int(sys.argv[2], 16)
for i in Cs(CS_ARCH_X86, CS_MODE_64).disasm(img[a - base:b - base], a):
    print("%x  %-6s %s" % (i.address, i.mnemonic, i.op_str))
