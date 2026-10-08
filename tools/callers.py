# 29/09/2026 function containing VA (.pdata) and every direct call to that function start: callers.py va [mgv|tpp]
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import sys, struct, pefile
EXE = {"mgv": str(paths.MGV_EXE), "tpp": str(paths.TPP_EXE)}
pe = pefile.PE(EXE[sys.argv[2] if len(sys.argv) > 2 else "mgv"], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
img, B = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
va = int(sys.argv[1], 16)
fn = next((B + e.struct.BeginAddress for e in pe.DIRECTORY_ENTRY_EXCEPTION if B + e.struct.BeginAddress <= va < B + e.struct.EndAddress), va)
print("function %x" % fn)
def owner(a):
    return next((B + e.struct.BeginAddress for e in pe.DIRECTORY_ENTRY_EXCEPTION if B + e.struct.BeginAddress <= a < B + e.struct.EndAddress), 0)
for s in pe.sections:
    if not s.Characteristics & 0x20000000: continue
    lo, d = s.VirtualAddress, img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
    i = d.find(b"\xe8")
    while i != -1 and i + 5 <= len(d):
        if B + lo + i + 5 + struct.unpack_from("<i", d, i + 1)[0] == fn: print("  call at %x in %x" % (B + lo + i, owner(B + lo + i)))
        i = d.find(b"\xe8", i + 1)
