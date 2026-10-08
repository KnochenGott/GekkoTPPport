# 24/09/2026 Compress converted Fox XML (fox2/parts/sim/tgt/fv2/mtar) into short text summaries in db/sum/.
# Usage: python tools/foxsum.py <xml or folder> [...]   -> db/sum/<file>.txt, one line per entity.
import sys
import pathlib
import xml.etree.ElementTree as ET
from collections import Counter

from env import ROOT

OUT = ROOT / "db" / "sum"
# PathCode/StrCode of the empty string: the Fox XML writes it for every unset String/FilePtr.
EMPTY_HASH = "0xB8A0BF169F98"
SKIP_PROPS = {"dataSet", "name", "dataList"}
MAX_VALUES = 12
MAX_TEXT = 120


def value_text(v):
    if v.get("hash") == EMPTY_HASH:
        return None
    if v.get("archivePath"):
        return f"{v.get('archivePath')}#{v.get('nameInArchive')}"
    if "x" in v.attrib:
        return "(" + ",".join(v.get(k) for k in ("x", "y", "z", "w") if k in v.attrib) + ")"
    t = (v.text or v.get("hash") or "").strip()
    if t in ("", "0x00000000"):
        return None
    key = v.get("key")
    return f"{key}={t}" if key else t


def fox_lines(root):
    names = {}
    for e in root.iter("entity"):
        for p in e.iter("property"):
            if p.get("name") == "name":
                v = p.find("value")
                if v is not None and v.text:
                    names[e.get("addr")] = v.text
    lines = []
    for e in root.iter("entity"):
        if e.get("class") == "DataSet":
            continue
        addr = e.get("addr")
        parts = []
        for p in e.iter("property"):
            if p.get("name") in SKIP_PROPS:
                continue
            vals = [x for x in (value_text(v) for v in p.findall("value")) if x]
            vals = [names.get(x, x) for x in vals]
            if not vals or all(x in ("false", "0", "-1", "(0,0,0,0)", "(0,0,0)") for x in vals):
                continue
            more = f"..+{len(vals) - MAX_VALUES}" if len(vals) > MAX_VALUES else ""
            parts.append(f"{p.get('name')}=" + "|".join(vals[:MAX_VALUES]) + more)
        lines.append(f"[{e.get('class')}] {names.get(addr, addr)}: " + "; ".join(parts))
    return lines


def generic_lines(root):
    # Non-entity XML (fv2, mtar): attributes of the root, then element counts and a few leaf samples.
    lines = [f"[{root.tag}] " + " ".join(f"{k.split('}')[-1]}={v}" for k, v in root.attrib.items())]
    counts = Counter(el.tag for el in root.iter())
    lines.append("counts: " + ", ".join(f"{t}={n}" for t, n in counts.items() if n > 1))
    seen = Counter()
    for el in root.iter():
        if len(el) == 0 and seen[el.tag] < 3 and ((el.text or "").strip() or el.attrib):
            seen[el.tag] += 1
            attrs = " ".join(f"{k.split('}')[-1]}={v}" for k, v in el.attrib.items())
            text = (el.text or "").strip()
            text = text if len(text) <= MAX_TEXT else f"<{len(text)} chars>"  # mog embeds the raw file as base64
            lines.append(f"  {el.tag}: {text} {attrs}".rstrip())
    return lines


def summarize(xml_path):
    root = ET.parse(xml_path).getroot()
    lines = fox_lines(root) if root.tag == "fox" else generic_lines(root)
    out = OUT / (xml_path.name[:-4] + ".txt")
    out.write_text(f"# {xml_path.relative_to(ROOT).as_posix()}\n" + "\n".join(lines) + "\n", encoding="utf-8")
    print(f"{out.relative_to(ROOT).as_posix()}  {xml_path.stat().st_size} -> {out.stat().st_size} bytes")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for arg in sys.argv[1:]:
        p = pathlib.Path(arg)
        for x in sorted(p.rglob("*.xml")) if p.is_dir() else [p]:
            summarize(x.resolve())


if __name__ == "__main__":
    main()
