# shape_scan.py 01/10/2026. Throwaway: finds mgv.exe functions (small, .pdata or leaf) whose text holds all of the given operand fragments. Usage: shape_scan.py frag1 frag2 ... [--max N]
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys,pefile,re
from capstone import Cs,CS_ARCH_X86,CS_MODE_64
args=sys.argv[1:]
mx=120
if "--max" in args:
    i=args.index("--max");mx=int(args[i+1]);args=args[:i]+args[i+2:]
pe=pefile.PE(str(paths.MGV_EXE),fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
img=pe.get_memory_mapped_image();B=0x140000000
md=Cs(CS_ARCH_X86,CS_MODE_64)
for e in pe.DIRECTORY_ENTRY_EXCEPTION:
    a=B+e.struct.BeginAddress;b=B+e.struct.EndAddress
    if b-a>mx*8:continue
    t="\n".join("%s %s"%(i.mnemonic,i.op_str) for i in md.disasm(img[a-B:b-B],a))
    if all(f in t for f in args): print(hex(a),b-a)
