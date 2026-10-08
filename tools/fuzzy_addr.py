# fuzzy_addr.py 25/09/2026. Maps dev-exe functions to the retail exe when their bytes differ only in addresses.
# mgsv_addr.py needs identical opening bytes, so any function whose prologue holds a rip-relative displacement or a
# call target fails there. This tool disassembles both functions and compares instruction shapes (mnemonic + operand
# kinds, displacements and immediates blanked). Candidates are only the retail function starts listed in .pdata,
# near dev + delta. delta is found per target: dev functions around it (addresses from the decomp headers in
# devSourceInferred) are exact-matched by their first --anchor-bytes bytes until one is unique in retail; that
# local delta places the guess. Candidates: the guess itself, .pdata starts, and 16-aligned addresses after int3/ret
# padding within --window (leaf functions have no .pdata entry, 25/09/2026 run missed them).
# A result is accepted only if the best score is >= --min-score and beats the runner-up by --margin; else "no match".
#   pip install pefile capstone
#   python tools/fuzzy_addr.py --dev-exe DEV --retail-exe RETAIL --api tools/gekko_api.tsv
import argparse, bisect, re, sys
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

MAX_INSNS = 48
ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "devSourceInferred" / "tpp_decomp" / "src"
HEADER = re.compile(r"^//\s*\S.*?\s+@\s+0x(14[0-9a-fA-F]{7})\s+\[", re.M)


def decomp_starts():
    """Every dev-exe function address named in the decomp headers."""
    out = set()
    for f in DECOMP.rglob("*.cpp"):
        out.update(int(h, 16) for h in HEADER.findall(f.read_text(errors="ignore")))
    return sorted(out)


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.starts = sorted({self.base + e.struct.BeginAddress for e in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", [])})
        self.cs = Cs(CS_ARCH_X86, CS_MODE_64)
        self.cs.detail = True

    def shape(self, va):
        """Normalised instruction list for the function at va, stopping at the first ret or int3."""
        off = va - self.base
        out = []
        for ins in self.cs.disasm(self.data[off:off + 0x400], va):
            ops = []
            for op in ins.operands:
                if op.type == X86_OP_REG:
                    ops.append(ins.reg_name(op.reg))
                elif op.type == X86_OP_IMM:
                    # small immediates are real constants (sizes, flags, offsets); large ones are addresses
                    ops.append(f"#{op.imm}" if abs(op.imm) < 0x10000 else "#A")
                elif op.type == X86_OP_MEM:
                    m = op.mem
                    base = ins.reg_name(m.base) if m.base else ""
                    disp = "A" if base == "rip" else str(m.disp)
                    ops.append(f"[{base}+{ins.reg_name(m.index) if m.index else ''}*{m.scale}+{disp}]")
            out.append(ins.mnemonic + " " + ",".join(ops))
            if ins.mnemonic in ("ret", "int3") or len(out) >= MAX_INSNS:
                break
        return out


def score(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return 0.0
    same = sum(1 for x, y in zip(a, b) if x == y)
    return same / max(len(a), len(b))


def unique_hit(img, needle):
    i = img.data.find(needle)
    if i < 0 or img.data.find(needle, i + 1) >= 0:
        return None
    return img.base + i


def local_delta(dev, ret, dva, starts, nbytes, tries=400):
    """(delta, anchor) from the nearest dev function whose first nbytes are unique in both exes."""
    for d in sorted(starts, key=lambda a: abs(a - dva))[:tries]:
        needle = dev.data[d - dev.base:d - dev.base + nbytes]
        if len(needle) < nbytes or unique_hit(dev, needle) != d:
            continue
        r = unique_hit(ret, needle)
        if r is not None:
            return r - d, d
    return None, None


def candidates(ret, guess, window):
    lo = bisect.bisect_left(ret.starts, guess - window)
    hi = bisect.bisect_right(ret.starts, guess + window)
    out = set(ret.starts[lo:hi]) | {guess}
    a = (guess - window) & ~0xF
    while a <= guess + window:
        off = a - ret.base
        if 0 < off < len(ret.data) and ret.data[off - 1] in (0xCC, 0xC3):
            out.add(a)
        a += 0x10
    return out


def resolve(dev, ret, dva, starts, window, min_score, margin, nbytes):
    delta, anchor = local_delta(dev, ret, dva, starts, nbytes)
    if delta is None:
        return None, "no unique anchor near the target", []
    guess = dva + delta
    want = dev.shape(dva)
    scored = sorted(((score(want, ret.shape(r)), r) for r in candidates(ret, guess, window)), reverse=True)
    at_guess = score(want, ret.shape(guess))
    if at_guess >= min_score and all(s <= at_guess for s, r in scored):
        return guess, f"score {at_guess:.2f} at guess, anchor dev {anchor:#x} delta {delta:#x}, {len(want)} insns", scored
    if not scored:
        return None, "no retail function starts in window", scored
    best = scored[0]
    second = scored[1][0] if len(scored) > 1 else 0.0
    if best[0] >= min_score and best[0] - second >= margin:
        return best[1], f"score {best[0]:.2f}, runner-up {second:.2f}, {len(want)} insns, guess {guess:#x}", scored
    return None, f"best {best[0]:.2f} at {best[1]:#x}, runner-up {second:.2f}: not accepted", scored


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev-exe", required=True)
    ap.add_argument("--retail-exe", required=True)
    ap.add_argument("--api", required=True, help="tsv: name, dev addr, ... (functions to resolve)")
    ap.add_argument("--window", type=lambda s: int(s, 0), default=0x4000)
    ap.add_argument("--min-score", type=float, default=0.9)
    ap.add_argument("--margin", type=float, default=0.15)
    ap.add_argument("--anchor-bytes", type=int, default=32)
    a = ap.parse_args()
    dev, ret = Image(a.dev_exe), Image(a.retail_exe)
    starts = decomp_starts()
    for line in Path(a.api).read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        name, d = line.split("\t")[:2]
        va, note, _ = resolve(dev, ret, int(d, 16), starts, a.window, a.min_score, a.margin, a.anchor_bytes)
        print(f"{name}\t{d}\t{va:#x}\t{note}" if va else f"{name}\t{d}\t-\t{note}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
