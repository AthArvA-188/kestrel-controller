"""Build the Kestrel PCB with KiCad's pcbnew module. Run with KiCad's Python.

Board coordinates come from the controller frame in shared/layout.json. The
board is drawn face side up, so KiCad X equals controller X and KiCad Y is the
controller Y mirrored.
"""
import math
import os

import pcbnew
from pcbnew import FromMM, ToMM, VECTOR2I

from design import LAYOUT, NC, POWER_NETS
from schematic import resolve, uid

KICAD_ROOT = os.environ.get("KICAD_ROOT", r"D:\Tools\KiCad-10.0.8")
STOCK_FP = os.path.join(KICAD_ROOT, "share", "kicad", "footprints")
HW = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
LOCAL_FP = {
    "kestrel": os.path.join(HW, "lib", "kestrel.pretty"),
    "kestrel_easyeda": os.path.join(HW, "lib", "easyeda", "kestrel_easyeda.pretty"),
}
ORIGIN = (150.0, 100.0)

TRACK = 0.2
# 0.4 mm is the widest track that can leave a 0.5 mm pitch pin and keep 0.15 mm clearance.
POWER_TRACK = 0.4
CLEARANCE = 0.15
VIA_DIAMETER = 0.6
VIA_DRILL = 0.3


def pt(x, y):
    return VECTOR2I(FromMM(ORIGIN[0] + x), FromMM(ORIGIN[1] - y))


def controller_xy(v):
    return round(ToMM(v.x) - ORIGIN[0], 3), round(ORIGIN[1] - ToMM(v.y), 3)


def lib_path(nick):
    return LOCAL_FP.get(nick, os.path.join(STOCK_FP, nick + ".pretty"))


def flip(fp):
    try:
        fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
    except (AttributeError, TypeError):
        fp.Flip(fp.GetPosition(), True)


def new_board(path):
    for ext in (".kicad_pcb", ".kicad_prl"):
        stale = os.path.splitext(path)[0] + ext
        if os.path.exists(stale):
            os.remove(stale)
    board = pcbnew.NewBoard(path)
    board.SetCopperLayerCount(4)
    board.SetLayerName(pcbnew.In1_Cu, "In1.Cu")
    board.SetLayerName(pcbnew.In2_Cu, "In2.Cu")
    rules = board.GetDesignSettings()
    rules.m_MinClearance = FromMM(0.127)
    rules.m_TrackMinWidth = FromMM(0.1)
    rules.m_ViasMinSize = FromMM(0.45)
    rules.m_MinThroughDrill = FromMM(0.2)
    rules.m_CopperEdgeClearance = FromMM(0.3)
    rules.m_HoleClearance = FromMM(0.25)
    rules.m_HoleToHoleMin = FromMM(0.25)
    rules.SetBoardThickness(FromMM(LAYOUT["pcb"]["thickness"]))
    settings = rules.m_NetSettings
    default = settings.GetDefaultNetclass()
    default.SetClearance(FromMM(CLEARANCE))
    default.SetTrackWidth(FromMM(TRACK))
    default.SetViaDiameter(FromMM(VIA_DIAMETER))
    default.SetViaDrill(FromMM(VIA_DRILL))
    power = pcbnew.NETCLASS("Power")
    power.SetClearance(FromMM(CLEARANCE))
    power.SetTrackWidth(FromMM(POWER_TRACK))
    power.SetViaDiameter(FromMM(VIA_DIAMETER))
    power.SetViaDrill(FromMM(VIA_DRILL))
    settings.SetNetclass("Power", power)
    for name in POWER_NETS:
        settings.SetNetclassPatternAssignment(net_name(name), "Power")
    return board


def net_name(name):
    """KiCad names a net from a label on the root sheet with a leading slash."""
    return "/" + name


def add_nets(board, design):
    nets = {}
    for name in sorted(design.nets()):
        item = pcbnew.NETINFO_ITEM(board, net_name(name))
        board.Add(item)
        nets[name] = item
    return nets


def unconnected_net(board, ref, pin_name, number):
    """KiCad's automatic net for a pin that carries a no-connect mark."""
    if pin_name and pin_name != number:
        name = "unconnected-(%s-%s-Pad%s)" % (ref, pin_name, number)
    else:
        name = "unconnected-(%s-Pad%s)" % (ref, number)
    item = board.FindNet(name)
    if item is None:
        item = pcbnew.NETINFO_ITEM(board, name)
        board.Add(item)
    return item


def place_part(board, part, nets):
    lib, name = part.fp.split(":")
    fp = pcbnew.FootprintLoad(lib_path(lib), name)
    if fp is None:
        raise ValueError("footprint not found: " + part.fp)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    fp.SetReference(part.ref)
    fp.SetValue(part.value)
    fp.SetPath(pcbnew.KIID_PATH("/" + uid("sym", part.ref)))
    if part.mpn:
        fp.SetField("MPN", part.mpn)
        fp.GetField("MPN").SetVisible(False)
    board.Add(fp)
    if part.ref[0] in "RC" and part.ref[1].isdigit():
        # Passive references go on the fabrication layer. The silkscreen has no room for them.
        fp.Reference().SetLayer(pcbnew.F_Fab)
    x, y, rot = part.at
    target = pt(x, y)
    fp.SetPosition(target)
    if part.side == "B":
        flip(fp)
    fp.SetOrientationDegrees(rot)
    if part.anchor != (0.0, 0.0):
        if part.side != "F":
            raise ValueError("anchor offsets are only handled on the face side")
        ax, ay = part.anchor
        t = math.radians(rot)
        dx = ax * math.cos(t) + ay * math.sin(t)
        dy = -ax * math.sin(t) + ay * math.cos(t)
        fp.SetPosition(VECTOR2I(target.x - FromMM(dx), target.y - FromMM(dy)))
    _, pins, pin_nets = resolve(part)
    pin_names = {p["number"]: p["name"] for p in pins}
    pad_numbers = set()
    for pad in fp.Pads():
        number = pad.GetNumber()
        pad_numbers.add(number)
        net = pin_nets.get(number)
        if net == NC:
            pad.SetNet(unconnected_net(board, part.ref, pin_names[number], number))
        elif net:
            pad.SetNet(nets[net])
        if number in ("SH", "MP"):
            # Shield and mounting lugs take the pour solid. Thermal spokes there can end up too few.
            pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
    missing = sorted(n for n, net in pin_nets.items() if net != NC and n not in pad_numbers)
    if missing:
        raise ValueError("%s: footprint %s has no pad for pins %s" % (part.ref, part.fp, missing))
    return fp


def add_outline(board):
    points = LAYOUT["pcb"]["outline"]
    for a, b in zip(points, points[1:] + points[:1]):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(pt(*a))
        seg.SetEnd(pt(*b))
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(FromMM(0.1))
        board.Add(seg)


def add_mount_holes(board):
    holes = LAYOUT["pcb"]["mount_holes"]["positions"]
    for i, (x, y) in enumerate(holes, start=1):
        fp = pcbnew.FootprintLoad(lib_path("MountingHole"), "MountingHole_2.2mm_M2")
        fp.SetFPID(pcbnew.LIB_ID("MountingHole", "MountingHole_2.2mm_M2"))
        fp.SetReference("H%d" % i)
        fp.SetValue("M2")
        fp.SetAttributes(pcbnew.FP_BOARD_ONLY | pcbnew.FP_EXCLUDE_FROM_BOM | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
        board.Add(fp)
        fp.SetPosition(pt(x, y))


def add_zone(board, net, layer, name, priority=0, outline_xy=None):
    zone = pcbnew.ZONE(board)
    zone.SetLayer(layer)
    zone.SetNet(net)
    zone.SetZoneName(name)
    zone.SetAssignedPriority(priority)
    zone.SetLocalClearance(FromMM(0.2))
    zone.SetMinThickness(FromMM(0.2))
    # Solid copper to SMD pads, thermal relief on through-hole pads.
    zone.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)
    zone.SetThermalReliefGap(FromMM(0.25))
    zone.SetThermalReliefSpokeWidth(FromMM(0.35))
    outline = zone.Outline()
    outline.NewOutline()
    for x, y in outline_xy or LAYOUT["pcb"]["outline"]:
        p = pt(x, y)
        outline.Append(p.x, p.y)
    board.Add(zone)
    return zone


def add_label(board):
    text = pcbnew.PCB_TEXT(board)
    text.SetText("KESTREL MAIN  REV A")
    text.SetLayer(pcbnew.B_SilkS)
    text.SetPosition(pt(0, 20))
    text.SetTextSize(VECTOR2I(FromMM(1.2), FromMM(1.2)))
    text.SetTextThickness(FromMM(0.18))
    text.SetMirrored(True)
    board.Add(text)


def build(design, path, vled_plane=True):
    """Place every part and add the inner planes. Tracks come later.

    With vled_plane off, VLED has no zone yet, so the autorouter joins it with
    tracks. add_vled_pour() then fills the spare room on In2 after routing.
    """
    board = new_board(path)
    nets = add_nets(board, design)
    placed = {}
    for part in design.parts.values():
        if part.fp is None:
            continue
        placed[part.ref] = place_part(board, part, nets)
    add_outline(board)
    add_mount_holes(board)
    add_zone(board, nets["GND"], pcbnew.In1_Cu, "GND plane")
    if vled_plane:
        add_zone(board, nets["VLED"], pcbnew.In2_Cu, "VLED plane")
    add_label(board)
    pcbnew.SaveBoard(path, board)
    return board, placed


def describe(placed, refs):
    """Controller-frame pad positions of a few parts, to check orientation."""
    lines = []
    for ref, pad_numbers in refs:
        fp = placed[ref]
        for pad in fp.Pads():
            if pad.GetNumber() in pad_numbers:
                lines.append("%s pad %s at %s" % (ref, pad.GetNumber(), controller_xy(pad.GetPosition())))
    return lines


def add_vled_pour(board):
    """VLED copper in the room left on In2 after routing. The tracks already join every VLED pin."""
    add_zone(board, board.FindNet(net_name("VLED")), pcbnew.In2_Cu, "VLED pour")


VSYS_FRONT = [(-16, 4.5), (23, 4.5), (23, 34), (-16, 34)]
VSYS_BACK = [(-16, 4.5), (23, 4.5), (23, 34), (-16, 34)]


def add_outer_pours(board, vled_front=False):
    """Pours on both outer layers, with VSYS pours over the power section.

    The autorouter cannot finish VSYS with tracks alone. Every VSYS pin sits in
    these two rectangles, so a pour on each side joins them.

    With vled_front on, the front pour carries VLED and the back pour carries
    ground. The autorouter runs signal tracks through both inner planes, which
    cuts them into pieces. A second copper layer for each rail, tied to its
    plane by a grid of vias, joins the pieces again.
    """
    gnd = board.FindNet(net_name("GND"))
    vsys = board.FindNet(net_name("VSYS"))
    if vled_front:
        add_zone(board, board.FindNet(net_name("VLED")), pcbnew.F_Cu, "VLED pour front", priority=0)
    else:
        add_zone(board, gnd, pcbnew.F_Cu, "GND pour front", priority=0)
    add_zone(board, gnd, pcbnew.B_Cu, "GND pour back", priority=0)
    add_zone(board, vsys, pcbnew.F_Cu, "VSYS pour front", priority=1, outline_xy=VSYS_FRONT)
    add_zone(board, vsys, pcbnew.B_Cu, "VSYS pour back", priority=1, outline_xy=VSYS_BACK)


# ---------- stitching: vias and short tracks added after autorouting ----------

STITCH_CLEARANCE = CLEARANCE + 0.04
# A via must clear tracks on every layer. The autorouter also uses the inner layers.
ALL_COPPER = (pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu)
VIA_R = VIA_DIAMETER / 2
STITCH_HALF = TRACK / 2


def _seg_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    length2 = dx * dx + dy * dy
    t = 0.0 if length2 == 0 else max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / length2))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def _rect_dist(px, py, rect):
    xmin, ymin, xmax, ymax = rect
    return math.hypot(max(xmin - px, 0.0, px - xmax), max(ymin - py, 0.0, py - ymax))


class Copper:
    """Tracks, vias, and pads of a board in KiCad millimetres, for clearance tests."""

    def __init__(self, board):
        self.tracks, self.vias, self.pads, self.keepouts = [], [], [], []
        for t in board.GetTracks():
            net = t.GetNetname()
            if t.GetClass() == "PCB_VIA":
                self.vias.append((net, ToMM(t.GetPosition().x), ToMM(t.GetPosition().y), VIA_R))
            else:
                self.tracks.append((t.GetLayer(), net, ToMM(t.GetStart().x), ToMM(t.GetStart().y),
                                    ToMM(t.GetEnd().x), ToMM(t.GetEnd().y), ToMM(t.GetWidth()) / 2))
        for fp in board.GetFootprints():
            for pad in fp.Pads():
                bb = pad.GetBoundingBox()
                rect = (ToMM(bb.GetLeft()), ToMM(bb.GetTop()), ToMM(bb.GetRight()), ToMM(bb.GetBottom()))
                self.pads.append((pad.IsOnLayer(pcbnew.F_Cu), pad.IsOnLayer(pcbnew.B_Cu), pad.GetNetname(), rect,
                                  ToMM(pad.GetPosition().x), ToMM(pad.GetPosition().y)))
            for zone in fp.Zones():
                bb = zone.GetBoundingBox()
                self.keepouts.append((ToMM(bb.GetLeft()), ToMM(bb.GetTop()), ToMM(bb.GetRight()), ToMM(bb.GetBottom())))
        outline = [(ORIGIN[0] + x, ORIGIN[1] - y) for x, y in LAYOUT["pcb"]["outline"]]
        self.edges = list(zip(outline, outline[1:] + outline[:1]))

    def near(self, x, y, reach):
        """A copy holding only the items within `reach` of (x, y)."""
        local = Copper.__new__(Copper)
        local.edges, local.keepouts = self.edges, self.keepouts
        local.vias = [v for v in self.vias if math.hypot(v[1] - x, v[2] - y) < reach + 1]
        local.pads = [p for p in self.pads if _rect_dist(x, y, p[3]) < reach + 1]
        local.tracks = [t for t in self.tracks if _seg_dist(x, y, t[2], t[3], t[4], t[5]) < reach + 1]
        return local

    def inside_board(self, x, y, margin):
        crossings = 0
        for (x1, y1), (x2, y2) in self.edges:
            if _seg_dist(x, y, x1, y1, x2, y2) < margin:
                return False
            if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
                crossings += 1
        return crossings % 2 == 1

    def point_clear(self, x, y, radius, net, layers):
        """True if a round copper item of this radius and net fits at (x, y) on the given layers."""
        need = radius + STITCH_CLEARANCE
        for rect in self.keepouts:
            if _rect_dist(x, y, rect) < radius:
                return False
        for vnet, vx, vy, vr in self.vias:
            gap = math.hypot(x - vx, y - vy)
            if vnet != net and gap < need + vr:
                return False
            if vnet == net and len(layers) > 1 and gap < radius + vr + 0.3:
                return False                      # keep drilled holes apart
        for front, back, pnet, rect, _, _ in self.pads:
            if not ((front and pcbnew.F_Cu in layers) or (back and pcbnew.B_Cu in layers)):
                continue
            gap = _rect_dist(x, y, rect)
            if pnet != net and gap < need:
                return False
            if pnet == net and len(layers) > 1 and gap < radius:
                return False                      # keep vias out of pads
        for layer, tnet, x1, y1, x2, y2, half in self.tracks:
            if layer in layers and tnet != net and _seg_dist(x, y, x1, y1, x2, y2) < need + half:
                return False
        return True

    def track_clear(self, x1, y1, x2, y2, net, layer):
        steps = max(2, int(math.hypot(x2 - x1, y2 - y1) / 0.1))
        for i in range(steps + 1):
            t = i / steps
            if not self.point_clear(x1 + t * (x2 - x1), y1 + t * (y2 - y1), STITCH_HALF, net, (layer,)):
                return False
        return True

    def anchor(self, net, x, y, layer):
        """Nearest pad centre or track end of this net on this layer."""
        best, best_d = None, 1e9
        for front, back, pnet, _, cx, cy in self.pads:
            if pnet == net and ((front and layer == pcbnew.F_Cu) or (back and layer == pcbnew.B_Cu)):
                d = math.hypot(cx - x, cy - y)
                if d < best_d:
                    best, best_d = (cx, cy), d
        for tlayer, tnet, x1, y1, x2, y2, _ in self.tracks:
            if tnet == net and tlayer == layer:
                for ex, ey in ((x1, y1), (x2, y2)):
                    d = math.hypot(ex - x, ey - y)
                    if d < best_d:
                        best, best_d = (ex, ey), d
        return best if best_d < 3.0 else None


def add_via(board, net_item, x, y):
    via = pcbnew.PCB_VIA(board)
    via.SetPosition(VECTOR2I(FromMM(x), FromMM(y)))
    via.SetViaType(pcbnew.VIATYPE_THROUGH)
    via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    try:
        via.SetWidth(pcbnew.F_Cu, FromMM(VIA_DIAMETER))
    except TypeError:
        via.SetWidth(FromMM(VIA_DIAMETER))
    via.SetDrill(FromMM(VIA_DRILL))
    via.SetNet(net_item)
    board.Add(via)


def add_track(board, net_item, layer, x1, y1, x2, y2):
    track = pcbnew.PCB_TRACK(board)
    track.SetStart(VECTOR2I(FromMM(x1), FromMM(y1)))
    track.SetEnd(VECTOR2I(FromMM(x2), FromMM(y2)))
    track.SetWidth(FromMM(TRACK))
    track.SetLayer(layer)
    track.SetNet(net_item)
    board.Add(track)


def stitch_near(board, copper, net, x, y, layer, accept=None):
    """Drop a via beside (x, y) and join it with a short track. Returns the via position or None.

    accept(x, y), when given, must also approve the via position.
    """
    start = copper.anchor(net, x, y, layer)
    if start is None:
        return None
    net_item = board.FindNet(net)
    for radius in (0.9, 1.2, 1.6, 2.0, 2.5, 3.0, 3.6):
        for k in range(24):
            ang = 2 * math.pi * k / 24
            vx, vy = start[0] + radius * math.cos(ang), start[1] + radius * math.sin(ang)
            if not copper.inside_board(vx, vy, 0.9) or (accept and not accept(vx, vy)):
                continue
            if not copper.point_clear(vx, vy, VIA_R, net, ALL_COPPER):
                continue
            if not copper.track_clear(start[0], start[1], vx, vy, net, layer):
                continue
            add_via(board, net_item, vx, vy)
            add_track(board, net_item, layer, start[0], start[1], vx, vy)
            copper.vias.append((net, vx, vy, VIA_R))
            copper.tracks.append((layer, net, start[0], start[1], vx, vy, STITCH_HALF))
            return vx, vy
    return None


def escape_route(board, copper, net, x0, y0, layer, accept, with_via=True, reach=6.0, cell=0.1):
    """Route a track from a pad at (x0, y0) to the nearest spot where accept(x, y) holds.

    With with_via on, the spot must also have room for a via, and one is
    placed there. A breadth-first search over a grid on one outer layer, so
    the track can turn corners. Returns True when the track was added.
    """
    from collections import deque
    local = copper.near(x0, y0, reach * 1.5)
    limit = int(reach / cell)
    free = {}

    def is_free(i, j):
        if (i, j) not in free:
            x, y = x0 + i * cell, y0 + j * cell
            free[(i, j)] = local.inside_board(x, y, 0.5) and local.point_clear(x, y, STITCH_HALF, net, (layer,))
        return free[(i, j)]

    prev = {(0, 0): None}
    queue = deque([(0, 0)])
    while queue:
        i, j = queue.popleft()
        x, y = x0 + i * cell, y0 + j * cell
        far_enough = not with_via or math.hypot(i, j) * cell > 0.9     # keep a via out of its pad
        if (far_enough and accept(x, y) and local.inside_board(x, y, 0.9)
                and (not with_via or local.point_clear(x, y, VIA_R, net, ALL_COPPER))):
            path, node = [], (i, j)
            while node is not None:
                path.append(node)
                node = prev[node]
            path.reverse()
            corners = [path[0]]
            for a, b, c in zip(path, path[1:], path[2:]):
                if (b[0] - a[0], b[1] - a[1]) != (c[0] - b[0], c[1] - b[1]):
                    corners.append(b)
            corners.append(path[-1])
            net_item = board.FindNet(net)
            points = [(x0 + ci * cell, y0 + cj * cell) for ci, cj in corners]
            for (ax, ay), (bx, by) in zip(points, points[1:]):
                add_track(board, net_item, layer, ax, ay, bx, by)
                copper.tracks.append((layer, net, ax, ay, bx, by, STITCH_HALF))
            if with_via:
                add_via(board, net_item, x, y)
                copper.vias.append((net, x, y, VIA_R))
            return True
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nxt = (i + di, j + dj)
            if abs(nxt[0]) > limit or abs(nxt[1]) > limit or nxt in prev or not is_free(*nxt):
                continue
            prev[nxt] = (i, j)
            queue.append(nxt)
    return False


def stitch_grid(board, copper, net, step, offset, rect_xy=None):
    """Via of this net at every free point of a square grid over the board or a rectangle."""
    if rect_xy:
        xs = [ORIGIN[0] + x for x, _ in rect_xy]
        ys = [ORIGIN[1] - y for _, y in rect_xy]
    else:
        xs = [x for (x, _), _ in copper.edges]
        ys = [y for (_, y), _ in copper.edges]
    net_item = board.FindNet(net)
    count = 0
    y = min(ys) + offset
    while y < max(ys):
        x = min(xs) + offset
        while x < max(xs):
            if copper.inside_board(x, y, 1.0) and copper.point_clear(x, y, VIA_R + 0.1, net, ALL_COPPER):
                add_via(board, net_item, x, y)
                copper.vias.append((net, x, y, VIA_R))
                count += 1
            x += step
        y += step
    return count


def zone_named(board, name):
    for zone in board.Zones():
        if zone.GetZoneName() == name:
            return zone
    raise KeyError(name)


_ESCAPED = set()    # pads the escape router has already handled, so no round repeats one


def join_rail(board, copper, net, zone_a, layer_a, zone_b, layer_b):
    """Tie the copper islands of one rail's two zones into a single piece.

    Signal tracks cut each pour and plane into islands. Two islands on
    different layers are joined by a via where they overlap. An island that
    overlaps nothing gets a via on a short track from one of its pads.
    Returns the number of vias added.
    """
    net_item = board.FindNet(net)
    polys = {"a": zone_a.GetFilledPolysList(layer_a), "b": zone_b.GetFilledPolysList(layer_b)}
    zones = {"a": (zone_a, layer_a), "b": (zone_b, layer_b)}
    islands = {k: [polys[k].Outline(i) for i in range(polys[k].OutlineCount())] for k in polys}
    boxes = {k: [o.BBox() for o in islands[k]] for k in islands}

    def island_at(k, x, y):
        p = VECTOR2I(FromMM(x), FromMM(y))
        zone, layer = zones[k]
        if not zone.HitTestFilledArea(layer, p, 0):
            return None
        for i, outline in enumerate(islands[k]):
            if boxes[k][i].Contains(p) and outline.PointInside(p):
                return i
        return None

    parent = {(k, i): (k, i) for k in islands for i in range(len(islands[k]))}

    def find(node):
        while parent[node] != node:
            parent[node] = parent[parent[node]]
            node = parent[node]
        return node

    def link(x, y):
        i, j = island_at("a", x, y), island_at("b", x, y)
        if i is not None and j is not None:
            parent[find(("a", i))] = find(("b", j))

    for vnet, vx, vy, _ in copper.vias:
        if vnet == net:
            link(vx, vy)
    for front, back, pnet, _, cx, cy in copper.pads:
        if pnet == net and front and back:
            link(cx, cy)

    added = 0
    for i in range(len(islands["a"])):
        box = boxes["a"][i]
        y = ToMM(box.GetTop()) + 0.4
        while y < ToMM(box.GetBottom()):
            x = ToMM(box.GetLeft()) + 0.4
            while x < ToMM(box.GetRight()):
                if island_at("a", x, y) == i:
                    j = island_at("b", x, y)
                    if (j is not None and find(("a", i)) != find(("b", j)) and copper.inside_board(x, y, 1.0)
                            and copper.point_clear(x, y, VIA_R + 0.1, net, ALL_COPPER)):
                        add_via(board, net_item, x, y)
                        copper.vias.append((net, x, y, VIA_R))
                        parent[find(("a", i))] = find(("b", j))
                        added += 1
                x += 0.6
            y += 0.6

    # Islands still on their own: reach the other zone from one of their pads.
    sizes = {}
    for node in parent:
        sizes[find(node)] = sizes.get(find(node), 0) + 1
    main = max(sizes, key=sizes.get)
    for k, other in (("a", "b"), ("b", "a")):
        outer_layer = zones[k][1]
        if outer_layer not in (pcbnew.F_Cu, pcbnew.B_Cu):
            continue
        for i in range(len(islands[k])):
            if find((k, i)) == main:
                continue
            for front, back, pnet, _, cx, cy in copper.pads:
                on_layer = front if outer_layer == pcbnew.F_Cu else back
                if pnet != net or not on_layer or island_at(k, cx, cy) != i:
                    continue
                key = (net, outer_layer, round(cx, 3), round(cy, 3))
                if key in _ESCAPED:
                    continue
                _ESCAPED.add(key)

                def reaches_main(x, y, other=other):
                    j = island_at(other, x, y)
                    return j is not None and find((other, j)) == main

                def same_layer_main(x, y, k=k):
                    j = island_at(k, x, y)
                    return j is not None and find((k, j)) == main

                # A plain track into the main copper on the pad's own layer, or else a via to the other layer.
                if (escape_route(board, copper, net, cx, cy, outer_layer, same_layer_main, with_via=False)
                        or escape_route(board, copper, net, cx, cy, outer_layer, reaches_main)):
                    parent[find((k, i))] = main
                    added += 1
                    break
    return added


def stitch_region(board, copper, net, rect_xy, count):
    """Place up to `count` vias of this net at free spots inside a controller-frame rectangle."""
    xs = [ORIGIN[0] + x for x, _ in rect_xy]
    ys = [ORIGIN[1] - y for _, y in rect_xy]
    net_item = board.FindNet(net)
    placed = []
    y = min(ys) + 1.0
    while y < max(ys) - 1.0 and len(placed) < count:
        x = min(xs) + 1.0
        while x < max(xs) - 1.0 and len(placed) < count:
            far = all(math.hypot(x - px, y - py) > 6.0 for px, py in placed)
            if far and copper.point_clear(x, y, VIA_R + 0.15, net, ALL_COPPER):
                add_via(board, net_item, x, y)
                copper.vias.append((net, x, y, VIA_R))
                placed.append((x, y))
            x += 0.5
        y += 0.5
    return placed


def fill(board):
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def stats(board):
    tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
    vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
    length = sum(ToMM(t.GetLength()) for t in tracks)
    board.BuildConnectivity()
    return {"tracks": len(tracks), "vias": len(vias), "track_length_mm": round(length, 1),
            "unrouted": board.GetConnectivity().GetUnconnectedCount(True)}
