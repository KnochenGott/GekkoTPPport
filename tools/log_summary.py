# log_summary.py 01/10/2026. Condenses MGS_TPP\Gekko.log (appended across runs) into one report per game run.
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import paths
import re, sys, collections, os
DEF = str(paths.GEKKO_LOG)
LINE = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d\d\d) (.*)$")
HITCH_S = 1.5   # [full] frame lines come every 120 frames (~1 s); a larger wall gap is a hitch
DT_HITCH = 0.05 # engine dt above this is a slow frame

def ts(m): return int(m[1])*3600 + int(m[2])*60 + int(m[3]) + int(m[4])/1000
def fmt(t): return "%02d:%02d:%06.3f" % (t//3600, t%3600//60, t%60)

def runs(path):
    out, cur = [], None
    for raw in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(raw.rstrip())
        if not m: continue
        t, msg = ts(m), m[5]
        if cur is None or msg.startswith("==== Gekko.dll loaded"):
            cur = {"start": t, "lines": []}; out.append(cur)
        cur["lines"].append((t, msg))
    return out

def summarize(r):
    L, out = r["lines"], []
    p = out.append
    t0, t1 = L[0][0], L[-1][0]
    p("RUN %s .. %s (%.0f s, %d lines)" % (fmt(t0), fmt(t1), t1 - t0, len(L)))
    tags = collections.Counter(re.match(r"\[(\w+)\]", m)[1] for _, m in L if re.match(r"\[(\w+)\]", m))
    p("  tags: " + " ".join("%s=%d" % kv for kv in tags.most_common()))
    # start/stop
    for t, m in L:
        if re.match(r"\[(Gekko\] Spawn|ai\] Survive brain started|type\] (Deactivate|Activate)|push\] stopped|brain\] fault)", m) or "Spawn at" in m:
            p("  %s %s" % (fmt(t), m[:150]))
    # faults
    faults = [(t, m) for t, m in L if m.startswith("[fault] code") or "fault" in m and m.startswith("[brain]")]
    p("  FAULTS: %d" % len(faults))
    for t, m in faults: p("    %s %s" % (fmt(t), m[:200]))
    # states: [act] t X action A finished F ai S loco C ; time in each (ai, action) until next [act]
    acts = [(t, re.search(r"action (\d+) finished (\d) ai (\d+) loco (\d+)", m)) for t, m in L if m.startswith("[act]")]
    acts = [(t, a) for t, a in acts if a]
    if acts:
        dur_a, dur_s, dur_l = (collections.defaultdict(float) for _ in range(3))
        for (t, a), (tn, _) in zip(acts, acts[1:] + [(L[-1][0], None)]):
            d = tn - t
            dur_a[a[1]] += d; dur_s[a[3]] += d; dur_l[a[4]] += d
        p("  [act] lines %d (logged on change only; time = until next [act] line, last one to end of run)" % len(acts))
        for name, dd in (("action", dur_a), ("ai state", dur_s), ("loco", dur_l)):
            p("    %s seconds: " % name + ", ".join("%s=%.1f" % kv for kv in sorted(dd.items(), key=lambda x: -x[1])))
    # events
    ev = collections.OrderedDict()
    for t, m in L:
        e = re.match(r"\[event\] (begin|delivered|key) ?key? ?([0-9a-f]{8})\s*(\(filtered\))?", m) or re.match(r"\[event\] key ([0-9a-f]{8}) delivered", m)
        if not e: continue
        k = e[2] if e.re.groups >= 2 else e[1]
        d = ev.setdefault(k, [0, t, 0]); d[0] += 1
        if "(filtered)" in m: d[2] += 1
    p("  EVENT edges: %d keys (key: count, first t, filtered)" % len(ev))
    for k, (n, t, f) in ev.items(): p("    %s x%d first %s filtered %d" % (k, n, fmt(t), f))
    dl = sum(1 for _, m in L if m.startswith("[event]") and "delivered" in m)
    p("  [event] 'delivered' lines: %d (T2 expects these; 'begin key' lines are the older format)" % dl)
    # push
    pl = [(t, m) for t, m in L if m.startswith("[push]")]
    p("  PUSH lines: %d" % len(pl))
    shown = 0
    for t, m in pl:
        if m.startswith("[push] set") and shown >= 3: continue
        if m.startswith("[push] set"): shown += 1
        p("    %s %s" % (fmt(t), m[:180]))
    # pause / dt
    dts = [(t, re.search(r"raw ([\d.]+) scaled ([\d.]+) \(frame (\d+)", m)) for t, m in L if m.startswith("[full] game dt")]
    dts = [(t, d) for t, d in dts if d]
    zero = [(t, d[3]) for t, d in dts if float(d[1]) == 0 and float(d[2]) == 0]
    big = [(t, float(d[1]), d[3]) for t, d in dts if float(d[1]) > DT_HITCH]
    p("  DT changes: %d, zero (paused) %d, raw dt > %.2f: %d" % (len(dts), len(zero), DT_HITCH, len(big)))
    for t, f in zero: p("    zero dt at %s frame %s" % (fmt(t), f))
    for t, v, f in big[:10]: p("    slow frame %s dt %.3f frame %s" % (fmt(t), v, f))
    if any("game time unreadable" in m for _, m in L): p("    'game time unreadable' PRESENT")
    # pause span from the per-second frame lines: frames counter stops while dt 0 ; report feet moving during zero dt
    # hitches: wall gaps in the [full] frame stream
    fr = [(t, int(re.search(r"frame (\d+)", m)[1])) for t, m in L if re.match(r"\[full\] frame \d+ feet", m)]
    gaps = [(a[0], b[0] - a[0], a[1], b[1]) for a, b in zip(fr, fr[1:]) if b[0] - a[0] > HITCH_S]
    p("  HITCHES (>%.1f s between [full] frame lines, normally ~1 s): %d" % (HITCH_S, len(gaps)))
    for t, g, fa, fb in gaps[:15]: p("    after %s gap %.2f s frames %d->%d" % (fmt(t), g, fa, fb))
    # unanswered qi
    qi = collections.Counter(re.search(r"id ([0-9a-f]+) from survive pc (0x[0-9a-f]+)", m)[1] + " pc " + re.search(r"pc (0x[0-9a-f]+)", m)[1] for _, m in L if m.startswith("[qi]") and re.search(r"id ([0-9a-f]+) from", m))
    p("  UNANSWERED [qi] ids: %d" % len(qi))
    for k, n in qi.items(): p("    %s" % k)
    # hits, refused
    p("  [hit] lines %d; refused/not-retail lines: %d" % (tags.get("hit", 0), sum(1 for _, m in L if "refused" in m)))
    return "\n".join(out)

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else DEF
    allr = "--all" in sys.argv
    all_runs = runs(path)
    sel = all_runs if allr else all_runs[-1:]
    print("log %s (%d bytes), %d runs, showing %s" % (path, os.path.getsize(path), len(all_runs), "all" if allr else "newest"))
    for r in sel: print(summarize(r))
