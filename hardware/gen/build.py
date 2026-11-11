"""Command-line entry point for generating the Kestrel hardware files.

Run with KiCad's bundled Python so the pcbnew module is available:

    <KiCad>/bin/python.exe build.py sch       schematic, ERC, PDF, netlist
    <KiCad>/bin/python.exe build.py pcb       placed board, placement DRC, DSN
    <KiCad>/bin/python.exe build.py route     autoroute, pours, DRC
    <KiCad>/bin/python.exe build.py outputs   Gerbers, drill, position, BOM, STEP, renders
"""
import collections
import csv
import json
import os
import shutil
import subprocess
import sys

import customlib
import design as design_mod
import schematic

HERE = os.path.dirname(os.path.abspath(__file__))
HW = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(HW, "out")
KICAD_ROOT = os.environ.get("KICAD_ROOT", r"D:\Tools\KiCad-10.0.8")
CLI = os.path.join(KICAD_ROOT, "bin", "kicad-cli.exe")
FREEROUTING = os.environ.get("FREEROUTING_JAR", r"D:\Tools\freerouting\freerouting.jar")
CONFIG_HOME = os.environ.get("KICAD_CONFIG_HOME", r"D:\Tools\kicad-config")
SCH = os.path.join(HW, "kestrel.kicad_sch")
PCB = os.path.join(HW, "kestrel.kicad_pcb")
DSN = os.path.join(OUT, "route", "kestrel.dsn")
SES = os.path.join(OUT, "route", "kestrel.ses")


def kicad_env():
    """Environment that gives kicad-cli the stock library tables."""
    share = os.path.join(KICAD_ROOT, "share", "kicad")
    version_dir = os.path.join(CONFIG_HOME, "10.0")
    os.makedirs(version_dir, exist_ok=True)
    for table in ("sym-lib-table", "fp-lib-table"):
        target = os.path.join(version_dir, table)
        if not os.path.exists(target):
            shutil.copyfile(os.path.join(share, "template", table), target)
    env = dict(os.environ)
    env.update({
        "KICAD_CONFIG_HOME": CONFIG_HOME,
        "KICAD10_SYMBOL_DIR": os.path.join(share, "symbols"),
        "KICAD10_FOOTPRINT_DIR": os.path.join(share, "footprints"),
        "KICAD10_3DMODEL_DIR": os.path.join(share, "3dmodels"),
    })
    return env


def cli(*args, check=True):
    result = subprocess.run([CLI] + list(args), cwd=HW, env=kicad_env(),
                            capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError("kicad-cli %s failed:\n%s\n%s" % (" ".join(args), result.stdout, result.stderr))
    return result


def summarise(report_path, keys):
    """Count violations in a kicad-cli JSON report by severity and type."""
    with open(report_path, encoding="utf-8") as f:
        report = json.load(f)
    items = []
    for key in keys:
        value = report.get(key, [])
        if key == "sheets":
            for sheet in value:
                items.extend(sheet.get("violations", []))
        else:
            items.extend(value)
    counts = collections.Counter((v["severity"], v["type"]) for v in items)
    return items, counts


def print_counts(title, items, counts, show=12, only_errors=True):
    print("%s: %d items" % (title, len(items)))
    for (severity, kind), n in sorted(counts.items()):
        print("   %-8s %-34s %d" % (severity, kind, n))
    shown = 0
    for v in items:
        if only_errors and v["severity"] != "error":
            continue
        where = "; ".join(i.get("description", "") for i in v.get("items", []))
        print("      %s | %s | %s" % (v["type"], v["description"], where[:170]))
        shown += 1
        if shown >= show:
            break


def stage_sch():
    os.makedirs(OUT, exist_ok=True)
    customlib.write_all()
    d = design_mod.build()
    count = schematic.write(d, SCH)
    cli("sch", "upgrade", "--force", SCH)
    print("schematic: %d symbols, %d nets" % (count, len(d.nets())))
    erc = os.path.join(OUT, "erc.json")
    cli("sch", "erc", "--format", "json", "--severity-all", "-o", erc, SCH)
    items, counts = summarise(erc, ["sheets"])
    print_counts("ERC", items, counts)
    cli("sch", "export", "pdf", "-o", os.path.join(OUT, "kestrel-schematic.pdf"), SCH)
    cli("sch", "export", "netlist", "-o", os.path.join(OUT, "kestrel.net"), SCH)
    return d


def stage_pcb():
    import board as board_mod
    os.makedirs(os.path.dirname(DSN), exist_ok=True)
    d = design_mod.build()
    brd, placed = board_mod.build(d, PCB)
    for line in board_mod.describe(placed, [("U1", ("1", "14", "15", "26", "27", "40")),
                                            ("J1", ("A1", "A12")), ("U17", ("1", "4", "a")),
                                            ("J2", ("1", "2")), ("J9", ("1", "8")),
                                            ("J3", ("1",)), ("U9", ("3", "11"))]):
        print("  ", line)
    drc = os.path.join(OUT, "drc-placement.json")
    cli("pcb", "drc", "--format", "json", "--severity-all", "-o", drc, PCB, check=False)
    items, counts = summarise(drc, ["violations"])
    placement = [v for v in items if v["type"] not in ("unconnected_items",)]
    print_counts("placement DRC", placement, collections.Counter((v["severity"], v["type"]) for v in placement), show=70)
    import pcbnew
    if not pcbnew.ExportSpecctraDSN(brd, DSN):
        raise RuntimeError("DSN export failed")
    print("wrote", DSN)


def stage_route(passes="60"):
    import board as board_mod
    import pcbnew
    if os.path.exists(SES):
        os.remove(SES)
    cmd = ["java", "-jar", FREEROUTING, "-de", DSN, "-do", SES, "-mp", passes,
           "--gui.enabled=false", "--router.optimizer.enabled=false",
           "--usage_and_diagnostic_data.disable_analytics=true", "--profile.allow_telemetry=false"]
    print("running:", " ".join(cmd))
    log = os.path.join(OUT, "route", "freerouting.log")
    with open(log, "w", encoding="utf-8") as f:
        subprocess.run(cmd, cwd=os.path.dirname(DSN), stdout=f, stderr=subprocess.STDOUT)
    if not os.path.exists(SES):
        raise RuntimeError("Freerouting wrote no session file, see " + log)
    stage_finish("route")


def stage_route3(passes="40"):
    """Autoroute with In1 kept as a solid ground plane and three signal layers.

    The stock DSN export marks every copper layer as a signal layer, and the
    autorouter then runs long tracks through both planes. This variant marks
    In1 as a power layer and leaves VLED without a plane, so VLED is routed as
    tracks on F.Cu, In2, and B.Cu.
    """
    import board as board_mod
    import pcbnew
    folder = os.path.join(OUT, "route3")
    os.makedirs(folder, exist_ok=True)
    pcb3 = os.path.join(folder, "kestrel3.kicad_pcb")
    dsn3 = os.path.join(folder, "kestrel3.dsn")
    ses3 = os.path.join(folder, "kestrel3.ses")
    d = design_mod.build()
    brd, _ = board_mod.build(d, pcb3, vled_plane=False)
    if not pcbnew.ExportSpecctraDSN(brd, dsn3):
        raise RuntimeError("DSN export failed")
    with open(dsn3, encoding="utf-8") as f:
        text = f.read()
    marker = "(layer In1.Cu\n      (type signal)"
    if marker not in text:
        raise RuntimeError("could not find the In1.Cu layer block in the DSN")
    with open(dsn3, "w", encoding="utf-8", newline="\n") as f:
        f.write(text.replace(marker, "(layer In1.Cu\n      (type power)"))
    if os.path.exists(ses3):
        os.remove(ses3)
    cmd = ["java", "-jar", FREEROUTING, "-de", dsn3, "-do", ses3, "-mp", passes,
           "--gui.enabled=false", "--router.optimizer.enabled=false",
           "--usage_and_diagnostic_data.disable_analytics=true", "--profile.allow_telemetry=false"]
    with open(os.path.join(folder, "freerouting.log"), "w", encoding="utf-8") as f:
        subprocess.run(cmd, cwd=folder, stdout=f, stderr=subprocess.STDOUT)
    print("session written:", os.path.exists(ses3))


def unconnected_pairs():
    """Run DRC and return each unconnected pair as two ends: (net, layer, x, y, is_via) in KiCad mm."""
    import pcbnew
    drc = os.path.join(OUT, "drc.json")
    cli("pcb", "drc", "--format", "json", "--severity-all", "-o", drc, PCB, check=False)
    with open(drc, encoding="utf-8") as f:
        report = json.load(f)
    pairs = []
    for v in report.get("unconnected_items", []):
        ends = []
        for item in v["items"]:
            text = item["description"]
            net = text[text.index("[") + 1:text.index("]")]
            back_only = "on B.Cu" in text and "F.Cu - B.Cu" not in text
            ends.append((net, pcbnew.B_Cu if back_only else pcbnew.F_Cu, item["pos"]["x"], item["pos"]["y"],
                         text.startswith("Via")))
        pairs.append(ends)
    return pairs


def link_ends(board_mod, brd, copper, ends):
    """Join the two ends of an unconnected pair with a track when both are on one outer layer."""
    import math
    (net, layer_a, xa, ya, _), (_, layer_b, xb, yb, _) = ends
    if layer_a != layer_b:
        return False
    a, b = copper.anchor(net, xa, ya, layer_a), copper.anchor(net, xb, yb, layer_b)
    if not a or not b or a == b:
        return False
    for start, goal in ((a, b), (b, a)):
        at_goal = lambda x, y, goal=goal: math.hypot(x - goal[0], y - goal[1]) < 0.08
        if board_mod.escape_route(brd, copper, net, start[0], start[1], layer_a, at_goal, with_via=False, reach=4.0):
            return True
    return False


def stage_finish(variant="route"):
    """Rebuild the placed board, import an autorouter session, pour, stitch, and check.

    variant "route3" uses the session from the three-signal-layer run, where
    VLED is tracks plus a pour. variant "route" uses the first run, where VLED
    is a plane on In2.
    """
    import board as board_mod
    import pcbnew
    three_layer = variant == "route3"
    ses = os.path.join(OUT, "route3", "kestrel3.ses") if three_layer else SES
    d = design_mod.build()
    board_mod.build(d, PCB, vled_plane=not three_layer)
    # Reload from disk. A board that has only lived in memory fills its zones
    # without the hole and edge clearances from the board setup.
    brd = pcbnew.LoadBoard(PCB)
    if not pcbnew.ImportSpecctraSES(brd, ses):
        raise RuntimeError("SES import failed")
    if three_layer:
        board_mod.add_vled_pour(brd)
    board_mod.add_outer_pours(brd, vled_front=not three_layer)
    copper = board_mod.Copper(brd)
    vsys = board_mod.net_name("VSYS")
    gnd, vled = board_mod.net_name("GND"), board_mod.net_name("VLED")
    pour_nets = {gnd, vled, vsys}
    # Each rail has copper on two layers. Tie every island of one to the other.
    rails = [(vsys, "VSYS pour front", pcbnew.F_Cu, "VSYS pour back", pcbnew.B_Cu),
             (gnd, "GND plane", pcbnew.In1_Cu, "GND pour back", pcbnew.B_Cu)]
    if not three_layer:
        rails.append((vled, "VLED plane", pcbnew.In2_Cu, "VLED pour front", pcbnew.F_Cu))
    for round_no in range(6):
        board_mod.fill(brd)
        added = 0
        for net, name_a, layer_a, name_b, layer_b in rails:
            a, b = board_mod.zone_named(brd, name_a), board_mod.zone_named(brd, name_b)
            added += board_mod.join_rail(brd, copper, net, a, layer_a, b, layer_b)
        print("island round %d: %d vias" % (round_no + 1, added))
        if not added:
            break
    tried = set()
    for attempt in range(14):
        board_mod.fill(brd)
        pcbnew.SaveBoard(PCB, brd)
        pairs = unconnected_pairs()
        print("pass %d: %d unconnected" % (attempt + 1, len(pairs)))
        if not pairs:
            break
        added = 0
        for ends in pairs:
            # Prefer the end that is not already a via, and do not retry an end.
            for net, layer, x, y, is_via in sorted(ends, key=lambda e: e[4]):
                key = (net, layer, round(x, 2), round(y, 2))
                if key in tried or net not in pour_nets:
                    continue    # a via only helps a net that has a plane or pour to reach
                tried.add(key)
                if link_ends(board_mod, brd, copper, ends) or board_mod.stitch_near(brd, copper, net, x, y, layer):
                    added += 1
                    break
        print("   stitches added:", added)
        if not added:
            break
    board_mod.fill(brd)
    pcbnew.SaveBoard(PCB, brd)
    print("routed:", board_mod.stats(brd))
    stage_drc()


def stage_drc():
    drc = os.path.join(OUT, "drc.json")
    cli("pcb", "drc", "--format", "json", "--severity-all", "--schematic-parity", "-o", drc, PCB, check=False)
    with open(drc, encoding="utf-8") as f:
        report = json.load(f)
    for key in ("violations", "unconnected_items", "schematic_parity"):
        items = report.get(key, [])
        counts = collections.Counter((v["severity"], v["type"]) for v in items)
        print_counts("DRC " + key, items, counts, show=25)


def stage_outputs():
    fab = os.path.join(OUT, "fab")
    os.makedirs(fab, exist_ok=True)
    layers = "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts"
    cli("pcb", "export", "gerbers", "-o", fab + os.sep, "--layers", layers, "--subtract-soldermask", PCB)
    cli("pcb", "export", "drill", "-o", fab + os.sep, "--format", "excellon", "--excellon-separate-th",
        "--generate-map", "--map-format", "pdf", PCB)
    cli("pcb", "export", "pos", "-o", os.path.join(fab, "kestrel-pos.csv"), "--format", "csv",
        "--units", "mm", "--side", "both", PCB)
    cli("pcb", "export", "step", "-o", os.path.join(OUT, "kestrel-pcb.step"), "--subst-models", "--force", PCB, check=False)
    cli("pcb", "export", "pdf", "-o", os.path.join(OUT, "kestrel-pcb-layers.pdf"),
        "--layers", "F.Cu,B.Cu,F.SilkS,B.SilkS,Edge.Cuts", "--mode-multipage", PCB, check=False)
    renders = os.path.join(OUT, "renders")
    os.makedirs(renders, exist_ok=True)
    for name, side in (("pcb-top.png", "top"), ("pcb-bottom.png", "bottom")):
        cli("pcb", "render", "-o", os.path.join(renders, name), "--side", side,
            "--width", "2000", "--height", "1300", "--quality", "high", PCB, check=False)
    write_bom()
    print("outputs written to", OUT)


def write_bom():
    d = design_mod.build()
    groups = collections.OrderedDict()
    for part in d.parts.values():
        if part.fp is None:
            continue
        if part.ref.startswith(("JP", "TP")):
            continue
        key = (part.value if not part.mpn else part.mpn, part.fp, part.mpn)
        groups.setdefault(key, {"value": part.value, "refs": []})["refs"].append(part.ref)
    with open(os.path.join(OUT, "kestrel-bom.csv"), "w", newline="", encoding="utf-8") as f:
        out = csv.writer(f)
        out.writerow(["Qty", "References", "Value", "Footprint", "MPN"])
        for (_, fp, mpn), group in groups.items():
            out.writerow([len(group["refs"]), " ".join(group["refs"]), group["value"], fp.split(":")[1], mpn])


STAGES = {"sch": stage_sch, "pcb": stage_pcb, "route": stage_route, "route3": stage_route3,
          "finish": stage_finish, "drc": stage_drc, "outputs": stage_outputs}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in STAGES:
        sys.exit(__doc__)
    STAGES[sys.argv[1]](*sys.argv[2:])
