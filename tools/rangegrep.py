# rangegrep.py 01/10/2026. Throwaway: per-.pdata-function regex search over a mgv.exe address range. Usage: rangegrep.py LO HI REGEX
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys,re,pefile
from capstone import Cs,CS_ARCH_X86,CS_MODE_64
lo,hi,rx=int(sys.argv[1],16),int(sys.argv[2],16),re.compile(sys.argv[3])
pe=pefile.PE(str(paths.MGV_EXE),fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
img=pe.get_memory_mapped_image();B=0x140000000
md=Cs(CS_ARCH_X86,CS_MODE_64)
for e in pe.DIRECTORY_ENTRY_EXCEPTION:
    a=B+e.struct.BeginAddress;b=B+e.struct.EndAddress
    if not(lo<=a<hi):continue
    for i in md.disasm(img[a-B:b-B],a):
        t="%s %s"%(i.mnemonic,i.op_str)
        if rx.search(t): print("%x %x %s"%(a,i.address,t))
