# make_gekko_sigs.py 25/09/2026. Writes src/GekkoHook/GekkoSigs.h from the retail exe: prologue signatures for every
# address the gekko DLL calls, and the expected vtable slot targets, so the DLL can verify the exe at load.
# Addresses come from docs/DOCUMENTATION.md (measured against retail 1.0.15.4). Bytes that are rip-relative displacements,
# call/jmp targets or large immediates are masked (they move between builds even when the code does not).
# File::Find(PathId) is resolved here as the first direct call inside File::Find(Path), and written out too.
#   pip install pefile capstone
#   python tools/make_gekko_sigs.py <TPP folder>\mgsvtpp.exe
import struct, sys
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

SIG_BYTES = 24
OUT = Path(__file__).resolve().parent.parent / "src" / "GekkoHook" / "GekkoSigs.h"

FUNCS = [  # name, retail address (measured, docs/DOCUMENTATION.md)
    ("AnimControl_AddMtarFile", 0x141a6b8f0),
    ("AnimControl_AddPlugins", 0x141a6bca0),  # (AnimControl*, AnimPlugin*): = Survive 0x14198d440 (shape 1.000), body = decomp AnimControl.cpp AddPlugins (dev 0x1418dcf80), read 07/10 from the retail disassembly
    ("Dog_CreatePlugin", 0x140b82cc0),        # tpp::gm::buddydog::PluginDogIk::Create(SharedPtr* out, Model*, RigDef*, flags): BlockHeapAlloc 0x110, ctor 0x140b81d70 (name hash 0xf1ff84cf, base PluginIk 0x141aba3c0); found by the name hash in the retail exe, 07/10
    ("File_FindByPath", 0x1400cd010),
    # Game Lua C API, as used by the working material_probe.dll (OctoCamo); names matched to the steamless decomp
    ("Lua_Register", 0x14006b8c0),   # (L, libname, luaL_Reg*): what probe_dll calls to register its table
    ("lua_pushboolean", 0x141a11750),
    ("lua_pushinteger", 0x141a11880),
    ("lua_pushstring", 0x141a11970),
    ("lua_toboolean", 0x141a120c0),
    ("lua_tolstring", 0x141a12150),
    # fox block system (research/tpp/block_api_fuzzy.txt, fuzzy_addr.py score 1.00 against the steamless decomp)
    ("BlockGroup_SendActModeChangedToAll", 0x14006f5c0),
    ("Block_GetFileForPathId", 0x14006d9a0),
    ("BlockPackage_GetFileInPackageForPathId", 0x1400dda00),
    # fox::geo, named in the retail exe (research/tpp/ground_hit_rig_dump.txt)
    ("Geo_CollisionLineCheck", 0x141b9b130),  # (GeoCheckControl&, uint flags, Vector3 const& from, to, float radius)
    ("Geo_CheckControlN127", 0x14171b450),    # GeoCheckControlN<127> ctor (this, uint)
    # fox::gm game object system, named in the retail exe (research/tpp/game_object_dump.txt, docs/DOCUMENTATION.md)
    ("GameObject_RegisterFactory", 0x140bf4620),  # fox::gm::RegisterGameObjectFactory(GameObjectFactoryInterface*)
    ("GameObject_Create", 0x140bf3dc0),           # fox::gm::CreateGameObject(GameObject* out, GameObjectDesc const&)
    ("GameObject_Destroy", 0x140bf3e90),          # (GameObject): GameObjectController vtbl[3], as SetUpGameObject uses it
    ("GameObject_Activate", 0x140bf3c90),  # (GameObjectImpl*): impl+0x38 |= 0x10, iface slot 4; natives call it after Create (0x140570170)
    ("AnimCtrlDesc_Ctor", 0x1409ba280),  # AnimationControllerDesc default ctor: +0x70 valid empty TagContainer (Ghidra)
    ("FullCtrl_Create", 0x140af0790),        # AnimationControllerImpl create(desc) -> Init 0x140af1ae0 (Ghidra)
    ("AnimLayerDesc_Ctor", 0x141a6e3c0),     # AnimControl layer entry ctor (0x1c bytes)
    ("AnimControlDesc_Init", 0x141a6e390),   # (animDesc, layers, count, arg): AnimControlDesc init
    # fox::cc, 29/09/2026: only caller of the VelocityControlAction ctor 0x141ace8d0 (id 0x9dcbbbc4); shape 1.000 vs
    # Survive 0x1419ee930, which the gekko setup uses through 0x140c56ea0
    ("VelocityCtl_Create", 0x141ace9e0),     # (SharedPtr<VelocityControlAction>* out)
    ("TargetBuilder_RegisterCategory", 0x1404fa8f0),  # fox::gk::TargetBuilder::RegisterTargetSpecifiedCategory(builder, StringId); body = decomp 0x1404d25a0 (+0x28350)
    ("CoreFramework_GetGameTime", 0x1400641e0),  # fox::CoreFramework::GetGameTime(): the GameTime whose Update (0x1400816a0) stores the frame dt at +8 and rate*dt at +0xe0 and forces dt to 0 while the pause system mask (+8 & 0x7fffffff of GetPauseSystem 0x1400f1d80) is set (READ 01/10 retail disassembly + decomp FoxGameTime.cpp:188)
    ("CcAction_RegistAction", 0x141ace850),  # (action, CharaControl*, AnimControl*): AddPlugins + action+0x68; = mgv 0x1419ee790
    # fox::gs route system, 01/10/2026 (docs/DOCUMENTATION.md 4.7): RouteSystemImpl::Init 0x140513930 (allocates 0x68, stores s_instance 0x142baf2a8); vtable 0x1421872a0 slot 0 GetRoute;
    # RouteGroupImpl vtable 0x142187500 slots 1 / 2. GetVertexCount found by exact bytes from the dev map symbol (mgsv_addr.py), the rest by vtable; all same bodies as Survive (tools/ctrl_slot_map.py norm)
    ("RouteSystem_GetRoute", 0x140513860),          # (this, RouteHandle* out, uint routeId) -> out {+8 group, +0x10 index}; = mgv 0x140580430
    ("RouteGroup_GetVertexCount", 0x140514cb0),     # (this, ushort route) -> ushort; = mgv 0x140581880
    ("QuarkSystemTable_Get", 0x140bff050),  # fox::GetQuarkSystemTable (call GetInstance; mov rax,[rax+8]); found 06/10 as the call target in front of 2864 "mov r,[rax+0x98]" sites in retail; dev 0x140b4b630
    ("RouteGroup_GetVertexPosition", 0x140514d40),  # (this, Vector3* out, ushort route, ushort vertex); = mgv 0x140581910
]


class Img:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.cs = Cs(CS_ARCH_X86, CS_MODE_64)
        self.cs.detail = True

    def u64(self, va):
        return struct.unpack_from("<Q", self.data, va - self.base)[0]

    def sig(self, va, n=SIG_BYTES):
        """(bytes, mask) for the first n bytes; mask 0 where the byte is a displacement or far target."""
        off = va - self.base
        raw = bytearray(self.data[off:off + n + 16])
        mask = bytearray(b"\xff" * len(raw))
        for ins in self.cs.disasm(bytes(raw), va):
            s = ins.address - va
            if s >= n:
                break
            if ins.disp_size and any(op.type == X86_OP_MEM and ins.reg_name(op.mem.base) == "rip" for op in ins.operands):
                for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
                    mask[s + k] = 0
            if ins.imm_size and ins.mnemonic in ("call", "jmp") or (ins.imm_size and any(
                    op.type == X86_OP_IMM and abs(op.imm) >= 0x10000 for op in ins.operands)):
                for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                    mask[s + k] = 0
        return bytes(raw[:n]), bytes(mask[:n])

    def first_call(self, va, limit=60):
        off = va - self.base
        for i, ins in enumerate(self.cs.disasm(self.data[off:off + 0x200], va)):
            if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == X86_OP_IMM:
                return ins.operands[0].imm
            if i > limit or ins.mnemonic == "ret":
                break
        return None


def c_bytes(b):
    return ", ".join(f"0x{x:02X}" for x in b)


def main(exe):
    img = Img(exe)
    lines = [
        "// GekkoSigs.h generated by tools/make_gekko_sigs.py from " + Path(exe).name + ". Do not edit by hand.",
        "// Prologue bytes + masks (0 = don't care) for every retail address the gekko DLL calls. Checked at use.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace GekkoSigs",
        "{",
        "    struct FuncSig { const char* name; std::uintptr_t address; std::uint8_t bytes[%d]; std::uint8_t mask[%d]; };" % (SIG_BYTES, SIG_BYTES),
        "",
        "    static const FuncSig kFuncs[] =",
        "    {",
    ]
    funcs = list(FUNCS)
    find_id = img.first_call(0x1400cd010)
    if find_id:
        funcs.append(("File_FindById", find_id))
        print(f"File::Find(PathId) = first call in File::Find(Path): {find_id:#x}")
    for name, va in funcs:
        b, m = img.sig(va)
        lines.append(f'        {{ "{name}", 0x{va:X}ull, {{ {c_bytes(b)} }}, {{ {c_bytes(m)} }} }},')
        print(f"{name:32s} {va:#x}  {b.hex(' ')}")
    lines += ["    };", "}", ""]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main(sys.argv[1])
