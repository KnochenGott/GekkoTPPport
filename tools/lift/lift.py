# lift.py 29/09/2026. Static x64 lifter: emits C++ that reproduces the machine code of chosen functions of a PE image over the
# emulated register file in lift_rt.h. Calls to functions with a TPP twin become native calls; data references become embedded
# windows of the original image; Survive code addresses become tokens (TPP shares the address range). Gaps (unsupported
# instructions, calls with no twin, unresolved jump tables) are reported and abort at run time. Design: docs/DOCUMENTATION.md.
#   python tools/lift/lift.py IMAGE ADDR [ADDR...] --out build/lift/name [--follow LO HI] [--resolver port_kit]
import argparse, bisect, collections, sys
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

TOKEN_BASE = 0x00007A0000000000
CTYPE = {1: "u8", 2: "u16", 4: "u32", 8: "u64"}
STYPE = {1: "s8", 2: "s16", 4: "s32", 8: "s64"}
R64 = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + ["r%d" % i for i in range(8, 16)]
R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"] + ["r%dd" % i for i in range(8, 16)]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"] + ["r%dw" % i for i in range(8, 16)]
R8 = ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"] + ["r%db" % i for i in range(8, 16)]
R8H = {"ah": 0, "ch": 1, "dh": 2, "bh": 3}
GPR = {}
for _i in range(16):
    GPR[R64[_i]] = (_i, 8, False); GPR[R32[_i]] = (_i, 4, False); GPR[R16[_i]] = (_i, 2, False); GPR[R8[_i]] = (_i, 1, False)
for _n, _i in R8H.items():
    GPR[_n] = (_i, 1, True)
RNAME = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI"] + ["R%d" % i for i in range(8, 16)]
CONDS = {"e": "e", "z": "e", "ne": "ne", "nz": "ne", "b": "b", "c": "b", "nae": "b", "ae": "ae", "nb": "ae", "nc": "ae",
         "be": "be", "na": "be", "a": "a", "nbe": "a", "l": "l", "nge": "l", "ge": "ge", "nl": "ge", "le": "le", "ng": "le",
         "g": "g", "nle": "g", "s": "s", "ns": "ns", "p": "p", "pe": "p", "np": "np", "po": "np", "o": "o", "no": "no"}
CMP_PRED = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"}
CMP_IMM = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]

cs = Cs(CS_ARCH_X86, CS_MODE_64)
cs.detail = True


class Unsupported(Exception):
    pass


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories([3])
        self.img = self.pe.get_memory_mapped_image()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.funcs = sorted({(self.base + e.struct.BeginAddress, self.base + e.struct.EndAddress) for e in self.pe.DIRECTORY_ENTRY_EXCEPTION})
        self.starts = [f[0] for f in self.funcs]
        self.imports = {}   # IAT slot VA -> (dll, function); a read of the slot yields the function's address in the running process
        try:
            self.pe.parse_data_directories([1])
            for ent in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
                for imp in ent.imports:
                    name = imp.name.decode() if imp.name else "#%d" % imp.ordinal
                    self.imports[imp.address] = (ent.dll.decode(), name)
        except Exception:
            pass
        self.text = [(self.base + s.VirtualAddress, self.base + s.VirtualAddress + s.Misc_VirtualSize)
                     for s in self.pe.sections if s.Characteristics & 0x20000000]

    def chunk(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        return self.funcs[i] if i >= 0 and va < self.funcs[i][1] else None

    def is_code(self, va):
        return any(a <= va < b for a, b in self.text)

    def extent(self, start):
        """(start, end) of the function at start: its .pdata chunk, else (leaf functions have no unwind info) a linear scan
        that stops at the first ret or jmp lying beyond every forward branch target seen, or at int3 padding."""
        ch = self.chunk(start)
        if ch:
            s, e = start, ch[1]
            while True:   # a chunk that does not end in ret/jmp/int3 falls through into the next one (split prologues)
                last = list(cs.disasm(self.bytes(s, e - s), s))[-1]
                nxt = self.chunk(e)
                if last.mnemonic in ("ret", "jmp", "int3", "ud2") or not nxt or nxt[0] != e:
                    return (s, e)
                e = nxt[1]
        end, far = start, start
        for i in cs.disasm(self.bytes(start, 0x4000), start):
            if i.mnemonic == "int3":
                break
            end = i.address + i.size
            if (i.mnemonic == "jmp" or i.mnemonic.startswith("j")) and i.operands and i.operands[0].type == X86_OP_IMM                     and i.operands[0].imm > i.address:
                far = max(far, i.operands[0].imm)
            if i.mnemonic in ("ret", "jmp") and end > far:
                break
        return (start, end)

    def is_entry(self, va):
        ch = self.chunk(va)
        return bool(ch) and ch[0] == va and self.is_code(va)

    def bytes(self, va, n):
        return self.img[va - self.base:va - self.base + n]

    def u32(self, va):
        return int.from_bytes(self.bytes(va, 4), "little")


def imm(v, size):
    return "(%s)0x%xull" % (CTYPE[size], v & ((1 << (size * 8)) - 1))


class Lifter:
    def __init__(self, image, resolver, follow):
        self.im, self.resolver, self.follow = image, resolver, follow
        self.lifted = {}          # chunk start -> C++ text (None while in progress)
        self.queue = []
        self.gaps = collections.OrderedDict()   # description -> where
        self.windows = []         # referenced data addresses
        self.tables = 0
        self.small_used, self.small_budget = 0, 400
        self.pc = False           # emit lift::g_pc = <address> before every instruction (fault location)
        self.force = set()        # --lift-twins: Survive callees lifted in place even though a TPP twin exists (the body is Survive's own, a TPP twin would walk TPP globals)
        self.env = {}             # call target -> C++ statement (environment hooks, e.g. env_mailbox(c);)
        self.cur = ""

    # ---------- addresses and operands
    def want(self, addr):
        """Queue a callee for lifting if it lies in the follow range; True when it is (or will be) lifted."""
        if addr in self.lifted or addr in self.queue:
            return True
        if addr in self.force and self.im.is_code(addr):
            self.queue.append(addr)
            return True
        if any(lo <= addr < hi for lo, hi in self.follow) and self.im.is_code(addr) and self.im.chunk(addr):
            self.queue.append(addr)
            return True
        return False

    def data_ref(self, addr):
        """C++ expression for the address of a data byte (embedded window) or a code token."""
        if self.im.is_entry(addr) or addr in self.lifted or addr in self.queue:   # leaf functions have no .pdata entry: lifted roots count too
            self.want(addr)
            return "(u64)0x%xull" % (TOKEN_BASE + addr)
        self.windows.append(addr)
        return "WIN(0x%xull)" % addr

    def ea(self, insn, op):
        m = op.mem
        if m.segment:
            raise Unsupported("segment override")
        if m.base and insn.reg_name(m.base) == "rip":
            return self.data_ref(insn.address + insn.size + m.disp)
        parts = []
        if m.base:
            parts.append("c.r[%s]" % RNAME[GPR[insn.reg_name(m.base)][0]])
        if m.index:
            idx = "c.r[%s]" % RNAME[GPR[insn.reg_name(m.index)][0]]
            parts.append("%s * %d" % (idx, m.scale) if m.scale != 1 else idx)
        if m.disp or not parts:
            parts.append("(u64)%dll" % m.disp if m.disp >= 0 else "(u64)(%dll)" % m.disp)
        return "(" + " + ".join(parts) + ")"

    def read(self, insn, op, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            i, sz, hi = GPR[insn.reg_name(op.reg)]
            if hi:
                return "(u8)(c.r[%s] >> 8)" % RNAME[i]
            return "c.r[%s]" % RNAME[i] if sz == 8 else "(%s)c.r[%s]" % (CTYPE[sz], RNAME[i])
        if op.type == X86_OP_IMM:
            return imm(op.imm, size)
        if op.type == X86_OP_MEM:
            m = op.mem
            if size == 8 and m.base and insn.reg_name(m.base) == "rip" and (insn.address + insn.size + m.disp) in self.im.imports:
                dll, fn = self.im.imports[insn.address + insn.size + m.disp]
                return 'lift::import_addr("%s", "%s")' % (dll, fn)
            return "lift::mem<%s>(%s)" % (CTYPE[size], self.ea(insn, op))
        raise Unsupported("operand type")

    def write(self, insn, op, expr, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            i, sz, hi = GPR[insn.reg_name(op.reg)]
            if hi:
                return "lift::wr8h(c, %s, (u8)(%s));" % (RNAME[i], expr)
            if sz == 8:
                return "c.r[%s] = (u64)(%s);" % (RNAME[i], expr)
            return "lift::wr%d(c, %s, (%s)(%s));" % (sz * 8, RNAME[i], CTYPE[sz], expr)
        if op.type == X86_OP_MEM:
            return "lift::mem<%s>(%s) = (%s)(%s);" % (CTYPE[size], self.ea(insn, op), CTYPE[size], expr)
        raise Unsupported("write operand")

    def is_xmm(self, insn, op):
        return op.type == X86_OP_REG and insn.reg_name(op.reg).startswith("xmm")

    def xr(self, insn, op, kind="ps"):
        """SSE source operand as __m128."""
        if op.type == X86_OP_REG:
            return "c.x[%s]" % insn.reg_name(op.reg)[3:]
        a = self.ea(insn, op)
        if kind in ("ss", "d32"):
            return "lift::loadss(%s)" % a
        if kind in ("sd", "q64"):
            return "lift::cf(_mm_loadl_epi64((const __m128i*)(%s)))" % a
        return "lift::loadps(%s)" % a

    def xd(self, insn, op):
        return "c.x[%s]" % insn.reg_name(op.reg)[3:]

    # ---------- calls
    def leaf(self, addr):
        """A callee with no .pdata entry is a leaf routine (tiny getter, thunk, CRT loop): lift it instead of hunting a twin."""
        if not self.im.is_code(addr) or addr in self.lifted or addr in self.queue:
            return False
        if self.resolver and self.resolver(addr)[0] == "native":
            return False   # a real twin is better than a copy
        lo, hi = self.im.extent(addr)
        has_pdata = self.im.chunk(addr) is not None
        if hi - lo > (0x300 if has_pdata else 0x400):
            return False
        if has_pdata:   # small engine functions are lifted too, within a budget so this cannot creep through the whole engine
            if self.small_used >= self.small_budget:
                return False
            self.small_used += 1
        self.queue.append(addr)
        return True

    def callee(self, addr):
        """C++ statement performing a direct call to address addr."""
        if addr in self.env:
            return self.env[addr]
        if self.want(addr) or self.leaf(addr):
            return "lift::push_ret(c); L_%x(c);" % addr
        r = self.resolver(addr) if self.resolver else ("native", "0x%xull" % addr, "raw address")
        if r[0] == "native":
            return "lift::call_native(c, %s); /* %s */" % (r[1], r[2])
        self.gaps["call to 0x%x: %s" % (addr, r[2])] = self.cur
        return 'lift::fatal("call to a function with no TPP twin", 0x%xull);' % addr

    def tail(self, t):
        """jmp out of the chunk: a tail call. The emulated return slot is consumed by the callee's ret."""
        if t in self.env:
            return ["%s c.r[RSP] += 8; return;" % self.env[t]]
        if self.want(t) or self.leaf(t):
            return ["L_%x(c); return;" % t]
        r = self.resolver(t) if self.resolver else ("native", "0x%xull" % t, "raw address")
        if r[0] == "native":
            return ["lift::call_native(c, %s); c.r[RSP] += 8; return;" % r[1]]
        self.gaps["tail call to 0x%x: %s" % (t, r[2])] = self.cur
        return ['lift::fatal("tail call with no TPP twin", 0x%xull);' % t]
    # ---------- integer instructions
    def stmts(self, insn, chunk, labels):
        m = insn.mnemonic
        ops = insn.operands
        a = insn.address
        pre = ""
        for p in ("rep ", "repe ", "repne ", "lock ", "notrack ", "bnd ") if m != "repz ret" else ():
            if m.startswith(p):
                pre = p.strip()
                m = m[len(p):]
        if m == "int3":
            return ['lift::fatal("int3", 0x%xull);' % a]
        if m.startswith("nop") or m in ("endbr64", "pause", "mfence", "lfence", "sfence") or m.startswith("prefetch"):
            return ["/* nop */"]
        if m[:4] in ("stos", "movs") and len(m) == 5 and m[-1] in "bwdq" and not any(self.is_xmm(insn, o) for o in ops):
            return self.string_op(m, pre)
        if m in ("mov", "movabs"):
            return [self.write(insn, ops[0], self.read(insn, ops[1], ops[0].size), ops[0].size)]
        if m == "movzx":
            return [self.write(insn, ops[0], "(%s)%s" % (CTYPE[ops[0].size], self.read(insn, ops[1])))]
        if m in ("movsx", "movsxd"):
            return [self.write(insn, ops[0], "(%s)(s64)(%s)%s" % (CTYPE[ops[0].size], STYPE[ops[1].size], self.read(insn, ops[1])))]
        if m == "lea":
            return [self.write(insn, ops[0], self.ea(insn, ops[1]))]
        if m in ("add", "sub", "and", "or", "xor", "adc", "sbb", "cmp", "test"):
            sz = ops[0].size
            T = CTYPE[sz]
            b = self.read(insn, ops[1], sz)
            av = self.read(insn, ops[0], sz)
            f = {"add": "add", "sub": "sub", "cmp": "sub", "adc": "adc", "sbb": "sbb"}.get(m)
            if f:
                expr = "lift::%s<%s>(f, %s, %s)" % (f, T, av, b)
            elif m == "test":
                expr = "lift::logic<%s>(f, (%s)(%s & %s))" % (T, T, av, b)
            else:
                expr = "lift::logic<%s>(f, (%s)(%s %s %s))" % (T, T, av, {"and": "&", "or": "|", "xor": "^"}[m], b)
            if m in ("cmp", "test"):
                return ["(void)%s;" % expr]
            return [self.write(insn, ops[0], expr, sz)]
        if m in ("inc", "dec", "neg"):
            sz = ops[0].size
            return [self.write(insn, ops[0], "lift::%s<%s>(f, %s)" % (m, CTYPE[sz], self.read(insn, ops[0], sz)), sz)]
        if m == "not":
            sz = ops[0].size
            return [self.write(insn, ops[0], "(%s)~%s" % (CTYPE[sz], self.read(insn, ops[0], sz)), sz)]
        if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            sz = ops[0].size
            f = {"sal": "shl"}.get(m, m)
            cnt = "1u" if len(ops) == 1 else "(unsigned)%s" % self.read(insn, ops[1], 1)
            return [self.write(insn, ops[0], "lift::%s<%s>(f, %s, %s)" % (f, CTYPE[sz], self.read(insn, ops[0], sz), cnt), sz)]
        if m == "imul":
            if len(ops) == 1:
                sz = ops[0].size
                if sz == 8:
                    return ["{ s64 hi; s64 lo = _mul128((s64)c.r[RAX], (s64)%s, &hi); c.r[RAX] = (u64)lo; c.r[RDX] = (u64)hi; f.CF = f.OF = hi != (lo >> 63); }" % self.read(insn, ops[0], 8)]
                if sz == 4:
                    return ["{ s64 p = (s64)(s32)c.r[RAX] * (s64)(s32)%s; c.r[RAX] = (u32)p; c.r[RDX] = (u32)((u64)p >> 32); f.CF = f.OF = p != (s64)(s32)p; }" % self.read(insn, ops[0], 4)]
                raise Unsupported("one-operand imul size")
            sz = ops[0].size
            if sz == 1:
                raise Unsupported("8-bit imul")
            x = self.read(insn, ops[1], sz)
            y = self.read(insn, ops[0], sz) if len(ops) == 2 else self.read(insn, ops[2], sz)
            return [self.write(insn, ops[0], "lift::imul%d(f, %s, %s)" % (sz * 8, y, x), sz)]
        if m == "mul" and len(ops) == 1:
            sz = ops[0].size
            if sz == 8:
                return ["{ u64 hi; u64 lo = _umul128(c.r[RAX], %s, &hi); c.r[RAX] = lo; c.r[RDX] = hi; f.CF = f.OF = hi != 0; }" % self.read(insn, ops[0], 8)]
            if sz == 4:
                return ["{ u64 p = (u64)(u32)c.r[RAX] * %s; c.r[RAX] = (u32)p; c.r[RDX] = (u32)(p >> 32); f.CF = f.OF = (p >> 32) != 0; }" % self.read(insn, ops[0], 4)]
            raise Unsupported("mul size")
        if m in ("div", "idiv") and len(ops) == 1 and ops[0].size == 8:
            fn = "_udiv128" if m == "div" else "_div128"
            ty = "u64" if m == "div" else "s64"
            return ["{ %s rem; %s q = %s(%s)c.r[RDX], %s)c.r[RAX], (%s)%s, &rem); c.r[RAX] = (u64)q; c.r[RDX] = (u64)rem; }" % (
                ty, ty, fn, "(" + ty, "(" + ty, ty, self.read(insn, ops[0], 8))]
        if m in ("div", "idiv") and len(ops) == 1 and ops[0].size == 4:
            if m == "div":
                return ["{ u64 n = ((u64)(u32)c.r[RDX] << 32) | (u32)c.r[RAX]; u32 d = %s; c.r[RAX] = (u32)(n / d); c.r[RDX] = (u32)(n %% d); }" % self.read(insn, ops[0], 4)]
            return ["{ s64 n = (s64)(((u64)(u32)c.r[RDX] << 32) | (u32)c.r[RAX]); s32 d = (s32)%s; c.r[RAX] = (u32)(s32)(n / d); c.r[RDX] = (u32)(s32)(n %% d); }" % self.read(insn, ops[0], 4)]
        if m == "cdq":
            return ["c.r[RDX] = (u32)((s32)c.r[RAX] >> 31);"]
        if m == "cqo":
            return ["c.r[RDX] = (u64)((s64)c.r[RAX] >> 63);"]
        if m == "cdqe":
            return ["c.r[RAX] = (u64)(s64)(s32)c.r[RAX];"]
        if m == "cwde":
            return ["c.r[RAX] = (u32)(s32)(s16)c.r[RAX];"]
        if m == "push":
            v = imm(ops[0].imm, 8) if ops[0].type == X86_OP_IMM else self.read(insn, ops[0], 8)
            return ["{ u64 v = %s; c.r[RSP] -= 8; lift::mem<u64>(c.r[RSP]) = v; }" % v]
        if m == "pop":
            return ["{ u64 v = lift::mem<u64>(c.r[RSP]); c.r[RSP] += 8; %s }" % self.write(insn, ops[0], "v", 8)]
        if m == "xadd":   # lock is irrelevant on the private emulated state
            sz = ops[0].size
            T = CTYPE[sz]
            return ["{ %s d = %s; %s s = %s; %s r = lift::add<%s>(f, d, s); %s %s }" % (T, self.read(insn, ops[0], sz), T, self.read(insn, ops[1], sz), T, T,
                                                                                    self.write(insn, ops[1], "d", sz), self.write(insn, ops[0], "r", sz))]
        if m == "cmpxchg":   # lock is irrelevant on the private emulated state; flags as cmp acc, dest; equal: dest = src, else acc = dest
            sz = ops[0].size
            T = CTYPE[sz]
            wracc = "c.r[RAX] = (u64)a;" if sz == 8 else "lift::wr%d(c, RAX, a);" % (sz * 8)
            return ["{ %s d = %s; %s s = %s; %s a = (%s)c.r[RAX]; lift::sub<%s>(f, a, d); if (a == d) { %s } else { a = d; %s } }" % (T, self.read(insn, ops[0], sz), T, self.read(insn, ops[1], sz), T, T, T,
                                                                                                     self.write(insn, ops[0], "s", sz), wracc)]
        if m == "xchg":
            sz = ops[0].size
            T = CTYPE[sz]
            return ["{ %s x = %s; %s y = %s; %s %s }" % (T, self.read(insn, ops[0], sz), T, self.read(insn, ops[1], sz),
                                                        self.write(insn, ops[0], "y", sz), self.write(insn, ops[1], "x", sz))]
        if m in ("bt", "bts", "btr", "btc"):
            sz = ops[0].size
            if ops[0].type == X86_OP_MEM and ops[1].type != X86_OP_IMM:
                raise Unsupported("bit test with memory and register offset")
            T = CTYPE[sz]
            out = ["{ %s v = %s; unsigned n = (unsigned)%s & %d; f.CF = (v >> n) & 1;" % (T, self.read(insn, ops[0], sz), self.read(insn, ops[1], sz), sz * 8 - 1)]
            if m == "bts":
                out.append(self.write(insn, ops[0], "(%s)(v | ((%s)1 << n))" % (T, T), sz))
            elif m == "btr":
                out.append(self.write(insn, ops[0], "(%s)(v & ~((%s)1 << n))" % (T, T), sz))
            elif m == "btc":
                out.append(self.write(insn, ops[0], "(%s)(v ^ ((%s)1 << n))" % (T, T), sz))
            out.append("}")
            return out
        if m in ("bsf", "bsr", "tzcnt", "lzcnt", "popcnt"):
            sz = ops[0].size
            if sz == 2:
                raise Unsupported("16-bit " + m)
            v = self.read(insn, ops[1], sz)
            bits = sz * 8
            if m == "popcnt":
                return ["{ u64 v = %s; u64 r = __popcnt64(v); f.ZF = v == 0; f.CF = f.OF = f.SF = f.PF = false; %s }" % (v, self.write(insn, ops[0], "r", sz))]
            if m in ("bsf", "tzcnt"):
                return ["{ u64 v = %s; unsigned long i = 0; bool nz = _BitScanForward64(&i, v); f.ZF = !nz; if (nz) %s else if (%s) %s }" % (
                    v, self.write(insn, ops[0], "i", sz), "true" if m == "tzcnt" else "false", self.write(insn, ops[0], str(bits), sz))]
            val = "i" if m == "bsr" else "%d - i" % (bits - 1)
            return ["{ u64 v = %s; unsigned long i = 0; bool nz = _BitScanReverse64(&i, v); f.ZF = !nz; if (nz) %s else if (%s) %s }" % (
                v, self.write(insn, ops[0], val, sz), "true" if m == "lzcnt" else "false", self.write(insn, ops[0], str(bits), sz))]
        if m.startswith("set") and m[3:] in CONDS:
            return [self.write(insn, ops[0], "lift::cc_%s(f) ? 1 : 0" % CONDS[m[3:]], 1)]
        if m.startswith("cmov") and m[4:] in CONDS:
            sz = ops[0].size
            els = " else { %s }" % self.write(insn, ops[0], self.read(insn, ops[0], 4), 4) if sz == 4 else ""
            return ["{ %s v = %s; if (lift::cc_%s(f)) { %s }%s }" % (CTYPE[sz], self.read(insn, ops[1], sz), CONDS[m[4:]], self.write(insn, ops[0], "v", sz), els)]
        if m in ("ret", "repz ret"):
            return ["c.r[RSP] += %d; return;" % (8 + (ops[0].imm if ops else 0))]
        if m == "call":
            if ops[0].type == X86_OP_IMM:
                return [self.callee(ops[0].imm)]
            return ["lift::call_ptr(c, %s);" % self.read(insn, ops[0], 8)]
        if m == "jmp":
            if ops[0].type == X86_OP_IMM:
                t = ops[0].imm
                return ["goto L_%x;" % t] if chunk[0] <= t < chunk[1] else self.tail(t)
            return self.indirect_jmp(insn, chunk, labels)
        if m.startswith("j") and m[1:] in CONDS:
            t = ops[0].imm
            cond = "lift::cc_%s(f)" % CONDS[m[1:]]
            if chunk[0] <= t < chunk[1]:
                return ["if (%s) goto L_%x;" % (cond, t)]
            return ["if (%s) { %s }" % (cond, " ".join(self.tail(t)))]
        return self.sse(insn, m, ops)

    def string_op(self, m, pre):
        sz = {"b": 1, "w": 2, "d": 4, "q": 8}[m[-1]]
        T = CTYPE[sz]
        if m.startswith("stos"):
            body = "lift::mem<%s>(c.r[RDI]) = (%s)c.r[RAX]; c.r[RDI] += %d;" % (T, T, sz)
        else:
            body = "lift::mem<%s>(c.r[RDI]) = lift::mem<%s>(c.r[RSI]); c.r[RDI] += %d; c.r[RSI] += %d;" % (T, T, sz, sz)
        if pre:
            return ["while (c.r[RCX]) { %s c.r[RCX]--; }" % body]
        return ["{ %s }" % body]

    def indirect_jmp(self, insn, chunk, labels):
        op = insn.operands[0]
        if op.type == X86_OP_MEM:
            return ["{ u64 t = %s; lift::call_ptr(c, t); c.r[RSP] += 8; return; }" % self.read(insn, op, 8)]
        tab = self.jump_table(insn, chunk)
        if not tab:   # jmp reg with a pointer loaded from memory: a tail call (member-function-pointer dispatch)
            return ["{ u64 t = %s; lift::call_ptr(c, t); c.r[RSP] += 8; return; }" % self.read(insn, op, 8)]
        idx, targets = tab
        out = ["switch (%s) {" % idx]
        for k, t in enumerate(targets):
            if chunk[0] <= t < chunk[1]:
                labels.add(t)
                out.append("case %d: goto L_%x;" % (k, t))
            elif self.im.is_entry(t):   # entry of another function: a tail call
                out.append("case %d: { %s }" % (k, " ".join(self.tail(t))))
            # anything else is not code: falls to default
        out.append('default: lift::fatal("jump table index out of range", 0x%xull); }' % insn.address)
        self.tables += 1
        return out

    def jump_table(self, insn, chunk):
        """Resolves  mov/movsxd r,[base+idx*4+off] ; add r,base ; jmp r  using the table of 32-bit offsets in the image."""
        prev = self.window_insns[max(0, self.pos - 14):self.pos]
        reg = insn.reg_name(insn.operands[0].reg)
        load = None
        for p in reversed(prev):
            if p.mnemonic in ("mov", "movsxd") and p.operands[0].type == X86_OP_REG and p.operands[1].type == X86_OP_MEM:
                mm = p.operands[1].mem
                dest = p.reg_name(p.operands[0].reg)
                if mm.index and mm.scale == 4 and GPR[dest][0] == GPR[reg][0]:
                    load = p
                    break
        if not load:
            return None
        self.table_loads.add(load.address)   # the load only feeds the switch; executing it would read the image base window, which is not embedded
        mm = load.operands[1].mem
        base_name = load.reg_name(mm.base) if mm.base else None
        base_val = self.im.base
        for p in reversed(prev):
            if p.mnemonic == "lea" and p.operands[0].type == X86_OP_REG and p.reg_name(p.operands[0].reg) == base_name \
                    and p.operands[1].mem.base and p.reg_name(p.operands[1].mem.base) == "rip":
                base_val = p.address + p.size + p.operands[1].mem.disp
        table = base_val + mm.disp
        idxreg = load.reg_name(mm.index)
        # Two-level table (MSVC for sparse switches): movzx r, byte [base + idx + off1] feeds the dword load, so the dword table is indexed by the BYTE value. The byte load is skipped like the
        # dword load and the switch runs on the original index (the register is not modified because the load never executes). Seen at 0x140f7f354 (key type switch of 0x140f7f240).
        byte_load = None
        for p in reversed(prev):
            if p.address >= load.address:
                continue
            if p.mnemonic == "movzx" and p.operands[0].type == X86_OP_REG and p.operands[1].type == X86_OP_MEM and p.operands[1].size == 1                     and GPR[p.reg_name(p.operands[0].reg)][0] == GPR[idxreg][0] and p.operands[1].mem.index                     and p.reg_name(p.operands[1].mem.base) == base_name:
                byte_load = p
                break
            if p.operands and p.operands[0].type == X86_OP_REG and GPR[p.reg_name(p.operands[0].reg)][0] == GPR[idxreg][0]:
                break   # the index register was redefined by something else
        if byte_load:
            self.table_loads.add(byte_load.address)
            bm = byte_load.operands[1].mem
            idxreg = byte_load.reg_name(bm.index)
            n = None
            for p in reversed(prev):
                if p.mnemonic == "cmp" and p.operands[0].type == X86_OP_REG and p.operands[1].type == X86_OP_IMM                         and GPR[p.reg_name(p.operands[0].reg)][0] == GPR[idxreg][0]:
                    n = p.operands[1].imm + 1
                    break
            if n is None:
                return None
            targets = []
            for k in range(n):
                b = self.im.bytes(base_val + bm.disp + k, 1)[0]
                targets.append(base_val + self.im.u32(table + 4 * b))
            return "(u64)c.r[%s]" % RNAME[GPR[idxreg][0]], targets
        n = None
        for p in reversed(prev):
            if p.mnemonic == "cmp" and p.operands[0].type == X86_OP_REG and p.operands[1].type == X86_OP_IMM \
                    and GPR[p.reg_name(p.operands[0].reg)][0] == GPR[idxreg][0]:
                n = p.operands[1].imm + 1
                break
        targets = []
        k = 0
        while (n is None or k < n) and k < 512:
            t = base_val + self.im.u32(table + 4 * k)
            if n is None and not (chunk[0] <= t < chunk[1]):
                break
            targets.append(t)
            k += 1
        if not targets:
            return None
        return "(u64)c.r[%s]" % RNAME[GPR[idxreg][0]], targets
    # ---------- SSE
    def sse(self, insn, m, ops):
        D = lambda: self.xd(insn, ops[0])
        S = lambda k="ps": self.xr(insn, ops[1], k)
        BIN = {"addps": "_mm_add_ps", "subps": "_mm_sub_ps", "mulps": "_mm_mul_ps", "divps": "_mm_div_ps", "minps": "_mm_min_ps", "maxps": "_mm_max_ps",
               "andps": "_mm_and_ps", "andnps": "_mm_andnot_ps", "orps": "_mm_or_ps", "xorps": "_mm_xor_ps", "unpcklps": "_mm_unpacklo_ps",
               "unpckhps": "_mm_unpackhi_ps", "movlhps": "_mm_movelh_ps", "movhlps": "_mm_movehl_ps",
               "andpd": "_mm_and_ps", "andnpd": "_mm_andnot_ps", "orpd": "_mm_or_ps", "xorpd": "_mm_xor_ps"}
        SC = {"addss": "_mm_add_ss", "subss": "_mm_sub_ss", "mulss": "_mm_mul_ss", "divss": "_mm_div_ss", "minss": "_mm_min_ss", "maxss": "_mm_max_ss"}
        SD = {"addsd": "_mm_add_sd", "subsd": "_mm_sub_sd", "mulsd": "_mm_mul_sd", "divsd": "_mm_div_sd", "minsd": "_mm_min_sd", "maxsd": "_mm_max_sd"}
        INTB = {"pxor": "_mm_xor_si128", "por": "_mm_or_si128", "pand": "_mm_and_si128", "pandn": "_mm_andnot_si128", "paddd": "_mm_add_epi32",
                "psubd": "_mm_sub_epi32", "paddq": "_mm_add_epi64", "psubq": "_mm_sub_epi64", "pcmpeqd": "_mm_cmpeq_epi32", "pcmpgtd": "_mm_cmpgt_epi32",
                "punpckldq": "_mm_unpacklo_epi32", "punpckhdq": "_mm_unpackhi_epi32", "punpcklqdq": "_mm_unpacklo_epi64", "punpckhqdq": "_mm_unpackhi_epi64",
                "punpcklwd": "_mm_unpacklo_epi16", "punpcklbw": "_mm_unpacklo_epi8", "paddw": "_mm_add_epi16", "paddb": "_mm_add_epi8"}
        if m in BIN and len(ops) == 2:
            return ["%s = %s(%s, %s);" % (D(), BIN[m], D(), S())]
        if m in SC:
            return ["%s = %s(%s, %s);" % (D(), SC[m], D(), S("ss"))]
        if m in SD:
            return ["%s = lift::cf(_mm_castpd_si128(%s(_mm_castps_pd(%s), _mm_castps_pd(%s))));" % (D(), SD[m], D(), S("sd"))]
        if m in INTB:
            return ["%s = lift::cf(%s(lift::ci(%s), lift::ci(%s)));" % (D(), INTB[m], D(), S("i"))]
        if m in ("sqrtps", "rsqrtps", "rcpps"):
            return ["%s = %s(%s);" % (D(), {"sqrtps": "_mm_sqrt_ps", "rsqrtps": "_mm_rsqrt_ps", "rcpps": "_mm_rcp_ps"}[m], S())]
        if m in ("sqrtss", "rsqrtss", "rcpss"):
            f = {"sqrtss": "_mm_sqrt_ss", "rsqrtss": "_mm_rsqrt_ss", "rcpss": "_mm_rcp_ss"}[m]
            return ["%s = _mm_move_ss(%s, %s(%s));" % (D(), D(), f, S("ss"))]
        if m == "shufps":
            return ["%s = _mm_shuffle_ps(%s, %s, %d);" % (D(), D(), S(), ops[2].imm & 0xFF)]
        if m == "shufpd":
            return ["%s = lift::cf(_mm_castpd_si128(_mm_shuffle_pd(_mm_castps_pd(%s), _mm_castps_pd(%s), %d)));" % (D(), D(), S(), ops[2].imm & 3)]
        if m == "pshufd":
            return ["%s = lift::cf(_mm_shuffle_epi32(lift::ci(%s), %d));" % (D(), S(), ops[2].imm & 0xFF)]
        if m in ("pslld", "psrld", "psrad", "psllq", "psrlq") and ops[1].type == X86_OP_IMM:
            f = {"pslld": "_mm_slli_epi32", "psrld": "_mm_srli_epi32", "psrad": "_mm_srai_epi32", "psllq": "_mm_slli_epi64", "psrlq": "_mm_srli_epi64"}[m]
            return ["%s = lift::cf(%s(lift::ci(%s), %d));" % (D(), f, D(), ops[1].imm)]
        if m in ("cmpps", "cmpss") and len(ops) == 3:
            k = "ps" if m == "cmpps" else "ss"
            return ["%s = _mm_cmp%s_%s(%s, %s);" % (D(), CMP_IMM[ops[2].imm & 7], k, D(), S(k))]
        for suf in ("ps", "ss"):
            if m.startswith("cmp") and m.endswith(suf) and m[3:-2] in CMP_PRED:
                return ["%s = _mm_cmp%s_%s(%s, %s);" % (D(), m[3:-2], suf, D(), S(suf))]
        if m in ("movaps", "movups", "movdqa", "movdqu", "movapd", "movupd", "lddqu"):
            if ops[0].type == X86_OP_REG:
                return ["%s = %s;" % (D(), S())]
            return ["lift::storeps(%s, %s);" % (self.ea(insn, ops[0]), self.xr(insn, ops[1]))]
        if m == "movss":
            if ops[0].type == X86_OP_MEM:
                return ["lift::storess(%s, %s);" % (self.ea(insn, ops[0]), self.xr(insn, ops[1]))]
            if ops[1].type == X86_OP_MEM:
                return ["%s = %s;" % (D(), S("ss"))]
            return ["%s = _mm_move_ss(%s, %s);" % (D(), D(), S())]
        if m == "movsd" and any(self.is_xmm(insn, o) for o in ops):
            if ops[0].type == X86_OP_MEM:
                return ["_mm_storel_epi64((__m128i*)(%s), lift::ci(%s));" % (self.ea(insn, ops[0]), self.xr(insn, ops[1]))]
            if ops[1].type == X86_OP_MEM:
                return ["%s = %s;" % (D(), S("sd"))]
            return ["%s = lift::cf(_mm_castpd_si128(_mm_move_sd(_mm_castps_pd(%s), _mm_castps_pd(%s))));" % (D(), D(), S())]
        if m in ("movlps", "movlpd", "movhps", "movhpd"):
            h = "h" if m.startswith("movh") else "l"
            if ops[0].type == X86_OP_MEM:
                return ["_mm_store%s_pi((__m64*)(%s), %s);" % (h, self.ea(insn, ops[0]), self.xr(insn, ops[1]))]
            return ["%s = _mm_load%s_pi(%s, (const __m64*)(%s));" % (D(), h, D(), self.ea(insn, ops[1]))]
        if m == "movmskps":
            return [self.write(insn, ops[0], "(u32)_mm_movemask_ps(%s)" % S(), 4)]
        if m == "movd":
            if self.is_xmm(insn, ops[0]):
                return ["%s = lift::cf(_mm_cvtsi32_si128((int)%s));" % (D(), self.read(insn, ops[1], 4))]
            return [self.write(insn, ops[0], "(u32)_mm_cvtsi128_si32(lift::ci(%s))" % self.xr(insn, ops[1]), 4)]
        if m == "movq":
            if self.is_xmm(insn, ops[0]):
                if self.is_xmm(insn, ops[1]):
                    return ["%s = lift::cf(_mm_move_epi64(lift::ci(%s)));" % (D(), S())]
                if ops[1].type == X86_OP_MEM:
                    return ["%s = lift::cf(_mm_loadl_epi64((const __m128i*)(%s)));" % (D(), self.ea(insn, ops[1]))]
                return ["%s = lift::cf(_mm_cvtsi64_si128((s64)%s));" % (D(), self.read(insn, ops[1], 8))]
            if ops[0].type == X86_OP_MEM:
                return ["_mm_storel_epi64((__m128i*)(%s), lift::ci(%s));" % (self.ea(insn, ops[0]), self.xr(insn, ops[1]))]
            return [self.write(insn, ops[0], "(u64)_mm_cvtsi128_si64(lift::ci(%s))" % self.xr(insn, ops[1]), 8)]
        if m in ("comiss", "ucomiss"):
            return ["lift::comi(f, _mm_cvtss_f32(%s), _mm_cvtss_f32(%s));" % (D(), S("ss"))]
        if m in ("comisd", "ucomisd"):
            return ["lift::comi(f, _mm_cvtsd_f64(_mm_castps_pd(%s)), _mm_cvtsd_f64(_mm_castps_pd(%s)));" % (D(), S("sd"))]
        if m in ("cvtsi2ss", "cvtsi2sd"):
            sz = ops[1].size
            v = "(%s)%s" % ("s64" if sz == 8 else "s32", self.read(insn, ops[1], sz))
            if m == "cvtsi2ss":
                return ["%s = %s(%s, %s);" % (D(), "_mm_cvtsi64_ss" if sz == 8 else "_mm_cvtsi32_ss", D(), v)]
            return ["%s = lift::cf(_mm_castpd_si128(%s(_mm_castps_pd(%s), %s)));" % (D(), "_mm_cvtsi64_sd" if sz == 8 else "_mm_cvtsi32_sd", D(), v)]
        if m in ("cvttss2si", "cvtss2si"):
            sz = ops[0].size
            f = ("_mm_cvttss_si64" if m == "cvttss2si" else "_mm_cvtss_si64") if sz == 8 else ("_mm_cvttss_si32" if m == "cvttss2si" else "_mm_cvtss_si32")
            return [self.write(insn, ops[0], "(%s)%s(%s)" % (CTYPE[sz], f, S("ss")), sz)]
        if m in ("cvttsd2si", "cvtsd2si"):
            sz = ops[0].size
            f = ("_mm_cvttsd_si64" if m == "cvttsd2si" else "_mm_cvtsd_si64") if sz == 8 else ("_mm_cvttsd_si32" if m == "cvttsd2si" else "_mm_cvtsd_si32")
            return [self.write(insn, ops[0], "(%s)%s(_mm_castps_pd(%s))" % (CTYPE[sz], f, S("sd")), sz)]
        if m == "cvtss2sd":
            return ["%s = lift::cf(_mm_castpd_si128(_mm_cvtss_sd(_mm_castps_pd(%s), %s)));" % (D(), D(), S("ss"))]
        if m == "cvtsd2ss":
            return ["%s = _mm_cvtsd_ss(%s, _mm_castps_pd(%s));" % (D(), D(), S("sd"))]
        if m == "cvtps2pd":
            return ["%s = lift::cf(_mm_castpd_si128(_mm_cvtps_pd(%s)));" % (D(), S("sd"))]
        if m == "cvtpd2ps":
            return ["%s = _mm_cvtpd_ps(_mm_castps_pd(%s));" % (D(), S())]
        if m == "cvtdq2ps":
            return ["%s = _mm_cvtepi32_ps(lift::ci(%s));" % (D(), S())]
        if m == "cvttps2dq":
            return ["%s = lift::cf(_mm_cvttps_epi32(%s));" % (D(), S())]
        if m == "cvtps2dq":
            return ["%s = lift::cf(_mm_cvtps_epi32(%s));" % (D(), S())]
        raise Unsupported(m)

    # ---------- chunks
    def lift_chunk(self, start):
        if not self.im.is_code(start):
            self.gaps["0x%x is not in a code section" % start] = self.cur
            self.lifted[start] = "static void L_%x(Cpu&) {}" % start
            return
        ch = self.im.extent(start)
        self.lifted[start] = None
        insns = list(cs.disasm(self.im.bytes(ch[0], ch[1] - ch[0]), ch[0]))
        self.window_insns = insns
        labels = set()
        self.table_loads = set()
        for i in insns:
            if (i.mnemonic == "jmp" or i.mnemonic.startswith("j") and i.mnemonic[1:] in CONDS) and i.operands[0].type == X86_OP_IMM:
                if ch[0] <= i.operands[0].imm < ch[1]:
                    labels.add(i.operands[0].imm)
        index = {i.address: k for k, i in enumerate(insns)}
        seen, work = set(), [ch[0]]
        while work:
            ad = work.pop()
            if ad in seen or ad not in index:
                continue
            seen.add(ad)
            i = insns[index[ad]]
            self.pos = index[ad]
            nxt = []
            if i.mnemonic not in ("ret", "repz ret", "jmp", "int3", "ud2", "hlt"):
                nxt.append(i.address + i.size)
            if (i.mnemonic == "jmp" or i.mnemonic.startswith("j") and i.mnemonic[1:] in CONDS) and i.operands and i.operands[0].type == X86_OP_IMM:
                nxt.append(i.operands[0].imm)
            elif i.mnemonic == "jmp" and i.operands and i.operands[0].type == X86_OP_REG:
                try:
                    tab = self.jump_table(i, ch)
                except (KeyError, IndexError):
                    tab = None
                nxt += tab[1] if tab else []
            work += nxt
        body = []
        for k, i in enumerate(insns):
            if i.address not in seen:
                continue
            self.pos = k
            self.cur = "0x%x %s %s" % (i.address, i.mnemonic, i.op_str)
            if i.address in self.table_loads:
                body.append((i.address, "%s %s" % (i.mnemonic, i.op_str), []))
                continue
            try:
                st = self.stmts(i, ch, labels)
            except (Unsupported, KeyError, IndexError) as e:
                self.gaps["unsupported %s (%s)" % (i.mnemonic, e)] = self.cur
                st = ['LIFT_UNSUPPORTED(0x%xull, "%s %s");' % (i.address, i.mnemonic, i.op_str.replace('"', "'"))]
            body.append((i.address, "%s %s" % (i.mnemonic, i.op_str), st))
        out = ["void L_%x(Cpu& c)" % start, "{", "    Fl f; (void)f;"]
        for addr, text, st in body:
            if addr in labels:
                out.append("L_%x:;" % addr)
            out.append("    // %x: %s" % (addr, text))
            if self.pc:
                out.append("    lift::g_pc = 0x%xull;" % addr)
            out += ["    " + s for s in st]
        out += ["}", ""]
        self.lifted[start] = "\n".join(out)

    def run(self, roots):
        self.queue = list(roots)
        while self.queue:
            a = self.queue.pop(0)
            if a not in self.lifted:
                self.lift_chunk(a)

    def windows_cpp(self):
        """Embedded copies of the referenced data (merged windows); qwords that point at code entries become tokens."""
        spans = []
        for a in sorted(set(self.windows)):
            lo, hi = (a - 0x100) & ~0xF, a + 0x1000
            if spans and lo <= spans[-1][1]:
                spans[-1][1] = max(spans[-1][1], hi)
            else:
                spans.append([lo, hi])
        out, entries = [], []
        for k, (lo, hi) in enumerate(spans):
            data = bytearray(self.im.bytes(lo, hi - lo).ljust(hi - lo, b"\0"))
            for o in range(0, len(data) - 7, 8):
                v = int.from_bytes(data[o:o + 8], "little")
                if self.im.is_entry(v) or v in self.lifted:
                    data[o:o + 8] = (TOKEN_BASE + v).to_bytes(8, "little")
            out.append("alignas(16) static std::uint8_t W%d[%d] = {%s};" % (k, len(data), ",".join(str(b) for b in data)))
            entries.append("{0x%xull, %d, W%d}" % (lo, len(data), k))
        out.append("struct Win { std::uint64_t base, size; std::uint8_t* p; };")
        out.append("static Win kWins[] = {%s};" % (", ".join(entries) if entries else "{0, 0, nullptr}"))
        out.append("static std::uint64_t WIN(std::uint64_t a) { for (auto& w : kWins) if (a >= w.base && a < w.base + w.size) return (std::uint64_t)(w.p + (a - w.base)); ::lift::fatal(\"data outside the embedded windows\", a); }")
        # Qwords of the embedded data that hold the address of other embedded data (a static object's vtable pointer, a table of pointers) become host addresses at load; raw image addresses
        # would fault when the lifted code follows them. Code entries became tokens above.
        out.append("static void reloc_windows() { for (auto& w : kWins) for (std::uint64_t o = 0; o + 8 <= w.size; o += 8) { std::uint64_t& q = *(std::uint64_t*)(w.p + o); if (q < 0x140000000ull || q >= 0x150000000ull) continue; for (auto& x : kWins) if (q >= x.base && q < x.base + x.size) { q = (std::uint64_t)(x.p + (q - x.base)); break; } } }")
        return "\n".join(out), len(spans)


def build_resolver(name):
    if name != "port_kit":
        return None
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    import port_kit
    exe, tpp = port_kit.Exe(port_kit.MGV), port_kit.Exe(port_kit.TPP)
    idx = port_kit.tpp_index(tpp)

    def resolve(addr):
        if addr in port_kit.KNOWN_TWINS:
            t, why = port_kit.KNOWN_TWINS[addr]
            return ("native", "0x%xull" % t, why) if t else ("gap", None, why)
        kind, detail = port_kit.classify(exe, tpp, idx, addr, (0, 0), False)
        if kind == "twin":
            return ("native", "0x%xull" % int(detail.split()[1], 16), "twin " + detail)
        if kind == "helper":
            return ("gap", None, "compiler/CRT helper: give it a runtime implementation in lift_rt.h")
        return ("gap", None, "%s %s" % (kind, detail))
    return resolve


def main():
    ap = argparse.ArgumentParser(fromfile_prefix_chars="@")   # "@file": one argument per line (the gekko module has too many roots for a Windows command line)
    ap.add_argument("image")
    ap.add_argument("addrs", nargs="*", type=lambda x: int(x, 16))
    ap.add_argument("--all-in", nargs=2, type=lambda x: int(x, 16), help="root every .pdata chunk in this address range (and follow calls inside it)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--follow", nargs=2, action="append", type=lambda x: int(x, 16), help="also lift direct callees inside this range (repeatable)")
    ap.add_argument("--resolver", default="")
    ap.add_argument("--pc", action="store_true", help="record the current Survive address before every instruction (debug builds)")
    ap.add_argument("--lift-twins", default="", help="file of hex Survive addresses (one per line, # comments) lifted in place instead of called through their TPP twin")
    ap.add_argument("--env", action="append", default=[], help="ADDR=name: call becomes name(c); ADDR=nop: call is skipped")
    ap.add_argument("--env-header", default="", help="header declaring the environment functions, included by the output")
    a = ap.parse_args()
    im = Image(a.image)
    follow = [tuple(f) for f in a.follow] if a.follow else []
    if a.all_in: follow.append(tuple(a.all_in))
    if a.all_in:
        a.addrs += [st for st, en in im.funcs if a.all_in[0] <= st < a.all_in[1]]
    lf = Lifter(im, build_resolver(a.resolver), follow)
    if a.lift_twins:
        lf.force = {int(w, 16) for l in open(a.lift_twins) for w in [l.split("#")[0].strip()] if w}
    for e in a.env:
        k, v = e.split("=")
        lf.env[int(k, 16)] = "/* skipped */" if v == "nop" else "%s(c);" % v
    lf.pc = a.pc
    lf.run(a.addrs)
    win, nwin = lf.windows_cpp()
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fns = sorted(lf.lifted)
    cpp = ["// generated by tools/lift/lift.py from %s. Do not edit." % Path(a.image).name,
           '#include "lift_rt.h"'] + (['#include "%s"' % a.env_header] if a.env_header else []) + ["using namespace lift;",
           "using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;",
           "using s8 = std::int8_t; using s16 = std::int16_t; using s32 = std::int32_t; using s64 = std::int64_t;", "", win, ""]
    cpp += ["static void L_%x(Cpu& c);" % f for f in fns] + [""]
    cpp += [lf.lifted[f] for f in fns]
    cpp += ["void %s_register()" % out.stem, "{", "    reloc_windows();"] + ["    lift::register_fn(0x%xull, &L_%x);" % (f, f) for f in fns] + ["}", ""]
    Path(str(out) + ".cpp").write_text("\n".join(cpp), encoding="utf-8")
    print("lifted %d chunk(s), %d data window(s), %d jump table(s) -> %s.cpp" % (len(fns), nwin, lf.tables, out))
    if lf.gaps:
        print("GAPS (%d):" % len(lf.gaps))
        for g, w in list(lf.gaps.items())[:60]:
            print("  %s   [%s]" % (g, w))


if __name__ == "__main__":
    main()