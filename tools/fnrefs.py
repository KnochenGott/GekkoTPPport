# fnrefs.py 01/10/2026. Throwaway: rip-relative lea targets (string or first qwords), large immediates and direct calls of one mgv function. Usage: fnrefs.py VA
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys,struct,pefile
from capstone import Cs,CS_ARCH_X86,CS_MODE_64
from capstone.x86 import X86_OP_MEM,X86_OP_IMM
pe=pefile.PE(str(paths.MGV_EXE),fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
img=pe.get_memory_mapped_image();B=0x140000000
va=int(sys.argv[1],16)
e=next(e for e in pe.DIRECTORY_ENTRY_EXCEPTION if B+e.struct.BeginAddress<=va<B+e.struct.EndAddress)
a,b=B+e.struct.BeginAddress,B+e.struct.EndAddress
md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
def s(v):
    o=v-B
    if not 0<=o<len(img)-16:return None
    r=img[o:o+60].split(b"\0")[0]
    return r.decode() if len(r)>3 and all(32<=c<127 for c in r) else None
print("func %x-%x"%(a,b))
for i in md.disasm(img[a-B:b-B],a):
    for op in i.operands:
        if op.type==X86_OP_MEM and op.mem.base==41 and i.mnemonic in("lea","mov"):  # rip
            t=i.address+i.size+op.mem.disp
            q=struct.unpack_from("<Q",img,t-B)[0] if 0<=t-B<len(img)-8 else 0
            print("%x %s %x str=%r q0=%x"%(i.address,i.mnemonic,t,s(t),q))
        elif op.type==X86_OP_IMM and abs(op.imm)>0x100000 and i.mnemonic in("mov","cmp","push","and"):
            print("%x imm %x"%(i.address,op.imm&0xffffffffffffffff))
    if i.mnemonic=="call" and i.operands[0].type==X86_OP_IMM:print("%x call %x"%(i.address,i.operands[0].imm))
