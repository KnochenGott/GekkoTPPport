# frt_read.py 02/10/2026. Reads a TPP route set (.frt, signature ROUT, version 3; layout from the modding wiki FRT page, shards/wiki.txt) and prints routes, nodes and events. Rewritten: the first version crashed on real files.
#   python tools/frt_read.py FILE.frt [--quiet]
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pathcode as P

NAMES = "Move RelaxedIdleAct CautionIdleAct CautionStandIdleReady CautionStandIdleAim CautionStandFire ConversationIdle SendMessage SwitchRoute SyncRoute UseSearchLight PutHostageInVehicle TakeHostageOutOfVehicle VehicleMoveSlow VehicleMoveNormal VehicleMoveFast VehicleBackSlow VehicleBackNormal VehicleBackFast VehicleDir Player".split()
KNOWN = {P.string_id(n) & 0xffffffff: n for n in NAMES}
AIM = ["none", "point", "character", "sight route", "object route"]


def parse(path, quiet=False):
    b = Path(path).read_bytes()
    sig, ver, n = struct.unpack_from("<4sHH", b, 0)
    if sig != b"ROUT" or ver != 3:
        raise SystemExit("%s: not a TPP route set (signature %r version %d)" % (path, sig, ver))
    ids_o, defs_o, pos_o, evt_tab_o, evt_o = struct.unpack_from("<5I", b, 8)
    for name, o in (("ids", ids_o), ("defs", defs_o), ("nodes", pos_o), ("event tables", evt_tab_o), ("events", evt_o)):
        if not 0 < o <= len(b):
            raise SystemExit("%s: %s offset 0x%x outside the file (%d bytes)" % (path, name, o, len(b)))
    print("%s: ROUT v3, %d routes, offsets ids 0x%x defs 0x%x nodes 0x%x event tables 0x%x events 0x%x" % (Path(path).name, n, ids_o, defs_o, pos_o, evt_tab_o, evt_o))
    node_total = event_total = 0
    for r in range(n):
        rid = struct.unpack_from("<I", b, ids_o + 4 * r)[0]
        d = defs_o + 16 * r   # offsets inside a definition are relative to the definition's own offset
        node_off, tab_off, evt_off, nodes, events = struct.unpack_from("<IIIHH", b, d)
        node_total += nodes
        event_total += events
        print("route %d id %08x: %d nodes, %d events" % (r, rid, nodes, events))
        if quiet:
            continue
        pos = d + node_off
        tab = d + tab_off
        ev = d + evt_off
        for i in range(nodes):
            x, y, z = struct.unpack_from("<3f", b, pos + 12 * i)
            cnt, first = struct.unpack_from("<HH", b, tab + 4 * i)
            print("  node %d (%.2f, %.2f, %.2f) events %d" % (i, x, y, z, cnt))
            for k in range(cnt):
                o = ev + 0x30 * (first + k)
                h, nodeb, aim, _, loop, wait, face = struct.unpack_from("<IBBBBHh", b, o)
                p = struct.unpack_from("<8I", b, o + 12)
                kind = "node" if nodeb else "edge"
                print("    %s event %08x %s aim %s loop %d wait %d facing %d params %s" % (kind, h, KNOWN.get(h, "?"), AIM[aim] if aim < len(AIM) else aim, loop, wait + 1 if nodeb else 0, face, " ".join("%x" % v for v in p)))
    return node_total, event_total


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: python tools/frt_read.py FILE.frt [--quiet]")
    parse(sys.argv[1], "--quiet" in sys.argv)
