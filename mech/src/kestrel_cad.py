"""Kestrel enclosure and mechanisms as parametric CadQuery geometry.

Run from the mech folder:

    .venv\\Scripts\\python.exe src\\kestrel_cad.py              build, export, check, render
    .venv\\Scripts\\python.exe src\\kestrel_cad.py --quick      build and render only

Coordinates follow shared/layout.json: millimetres, +X right, +Y away from the
player, +Z out of the face, Z=0 at the top of the PCB.
"""
import base64
import json
import math
import os
import sys
import time

import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.abspath(os.path.join(HERE, "..", "out"))

with open(os.path.join(ROOT, "shared", "layout.json"), encoding="utf-8") as f:
    L = json.load(f)

BODY, ST, TM = L["body"], L["sticks"], L["tension_mechanism"]
FACE_Z = BODY["face_z"]
WALL = BODY["wall"]
BOTTOM_Z = BODY["centre_bottom_z"]
INNER_Z = FACE_Z - WALL          # underside of the top wall
FLOOR_Z = BOTTOM_Z + WALL        # top of the centre floor
SPLIT_Z = 1.0                    # parting plane between the two shells
PCB_T = L["pcb"]["thickness"]
TACT_TOP = 4.3                   # actuator top of the PTS645 switch
PIVOT_Z = ST["pivot_z"]
TILT = ST["max_tilt_deg"]

# Stick stack. Dimensions that layout.json does not carry are set here and listed in the README.
CAP_TOP_Z = ST["cap_top_z"]
CAP_R = ST["cap_diameter"] / 2
DISC_R = TM["disc_radius"]
DISC_TOP_Z = TM["disc_top_z"]
SLEEVE_R = 2.8
PLUNGER_BORE_R = TM["plunger_bore_radius"]
BORE_R = 13.0
TURRET_TOP_Z = ST["turret_base_z"]
GLOW_RI, GLOW_RO = ST["glow_ring_inner_r"], ST["glow_ring_outer_r"]
RING_RO = ST["tension_ring_od"] / 2
RING_RI = 11.7
RING_TRAVEL = TM["preload_travel"]
SPRING_SEAT_Z = DISC_TOP_Z - 2.5       # top of the plunger's spring flange
SPRING_LEN_MID = 7.8
SPRING_FREE = 9.35
SPRING_RATE = 0.47                     # N/mm
RING_FLANGE_T = 1.6

TRIG_PIVOT_Y, TRIG_PIVOT_Z = L["triggers"]["pivot"]["y"], L["triggers"]["pivot"]["z"]
TRIG_BACK_Y = 43.5                     # rear face of the trigger blade
TRIG_BOTTOM_Z = -16.5                  # the blade stops above a ledge of the floor
TRIG_TRAVEL = L["triggers"]["travel_deg"]
ARM_PARK_DEG = 56.0
WHEEL_X = 54.0

COLORS = {
    "shell_top": (0.17, 0.18, 0.21), "shell_bottom": (0.12, 0.13, 0.15),
    "cap": (0.07, 0.07, 0.08), "ring": (0.74, 0.75, 0.78), "glow": (0.20, 0.85, 0.95),
    "pom": (0.93, 0.93, 0.90), "spring": (0.55, 0.57, 0.60),
    "A": (0.25, 0.78, 0.40), "B": (0.90, 0.28, 0.25), "X": (0.25, 0.50, 0.92), "Y": (0.95, 0.80, 0.22),
    "dpad": (0.09, 0.09, 0.10), "system": (0.34, 0.35, 0.38), "bumper": (0.27, 0.28, 0.32),
    "trigger": (0.22, 0.23, 0.27), "paddle": (0.60, 0.61, 0.65), "pcb": (0.05, 0.36, 0.22),
    "part": (0.10, 0.10, 0.11), "metal": (0.72, 0.72, 0.74), "battery": (0.30, 0.42, 0.72),
    "lens": (0.02, 0.02, 0.03), "display": (0.10, 0.12, 0.30), "brass": (0.80, 0.66, 0.30),
    "magnet": (0.75, 0.20, 0.20),
}


# ---------- small geometry helpers ----------

def wp(shape):
    return cq.Workplane("XY").add(shape)


def box(x0, x1, y0, y1, z0, z1):
    lo = (min(x0, x1), min(y0, y1), min(z0, z1))
    size = (abs(x1 - x0), abs(y1 - y0), abs(z1 - z0))
    return cq.Workplane("XY").box(*size, centered=False).translate(lo)


def cyl(x, y, z0, z1, r):
    return cq.Workplane("XY").circle(r).extrude(z1 - z0).translate((x, y, z0))


def tube(x, y, z0, z1, ro, ri):
    return cq.Workplane("XY").circle(ro).circle(ri).extrude(z1 - z0).translate((x, y, z0))


def cyl_x(x0, x1, y, z, r):
    return cq.Workplane("YZ").circle(r).extrude(x1 - x0).translate((x0, y, z))


def cyl_y(x, y0, y1, z, r):
    return cq.Workplane("XZ").circle(r).extrude(-(y1 - y0)).translate((x, y0, z))


def cone(x, y, z0, z1, r0, r1):
    return wp(cq.Solid.makeCone(r0, r1, z1 - z0, pnt=cq.Vector(x, y, z0)))


def ellipsoid(cx, cy, cz, rx, ry, rz):
    sphere = cq.Solid.makeSphere(1.0, angleDegrees1=-90, angleDegrees2=90, angleDegrees3=360)
    return wp(sphere.transformGeometry(cq.Matrix([[rx, 0, 0, cx], [0, ry, 0, cy], [0, 0, rz, cz]])))


def compound(items):
    return wp(cq.Compound.makeCompound([s for item in items for s in item.vals()]))


def union_all(items):
    out = items[0]
    for item in items[1:]:
        out = out.union(item)
    return out


def volume(shape):
    return sum(v.Volume() for v in shape.vals()) if shape.vals() else 0.0


def bbox(shape):
    return shape.val().BoundingBox() if len(shape.vals()) == 1 else compound([shape]).val().BoundingBox()


# ---------- body ----------

def outline_wire(inset=0.0):
    right = [(40, 50), (64, 47.5), (79, 37), (86, 16), (86, -12), (84, -40), (78, -64),
             (68, -79), (57, -77), (47, -64), (39, -51), (24, -46)]
    scale = BODY["overall_width"] / 172.0
    right = [(x * scale, y) for x, y in right]
    pts = [(0, BODY["front_edge_y"])] + right + [(0, BODY["centre_rear_edge_y"])] + [(-x, y) for x, y in reversed(right)]
    edge = cq.Edge.makeSpline([cq.Vector(x, y, 0) for x, y in pts], periodic=True)
    wire = cq.Wire.assembleEdges([edge])
    if inset:
        wire = wire.offset2D(-inset)[0]
    return wire


def prism(wire, z0, z1):
    face = cq.Face.makeFromWires(wire)
    return wp(cq.Solid.extrudeLinear(face, cq.Vector(0, 0, z1 - z0)).translate(cq.Vector(0, 0, z0)))


def body_solid(inset=0.0):
    """Outer body, or the cavity when inset equals the wall thickness."""
    wire = outline_wire(inset)
    slab = prism(wire, BOTTOM_Z + inset, FACE_Z - inset)
    for radius in (5.0 - inset, 3.0 - inset * 0.6, 0):
        if radius <= 0.2:
            break
        try:
            slab = slab.edges("not |Z").fillet(radius)
            break
        except Exception:
            continue
    grips = []
    for side in (-1, 1):
        grips.append(ellipsoid(side * 64, -46, -14, 22 - inset, 36 - inset, 28 - inset))
    clip = prism(wire, -70, FACE_Z - inset)
    lobes = union_all(grips).intersect(clip)
    return slab.union(lobes)


# ---------- part registry ----------

PARTS = []


def add(name, shape, color, printed=False, material=""):
    PARTS.append({"name": name, "shape": shape, "color": COLORS[color], "printed": printed,
                  "material": material})
    return shape


def get(name):
    for p in PARTS:
        if p["name"] == name:
            return p["shape"]
    raise KeyError(name)


def ring_leds(tag):
    return [led["xy"] for led in L["leds"]["chain"] if led["name"].startswith(tag + "RING")]


def ring_seat_z(offset=0.0):
    """Underside of the tension ring flange. offset is -1 (tightest) to +1 (loosest)."""
    return SPRING_SEAT_Z + SPRING_LEN_MID + offset * RING_TRAVEL / 2


def stick_parts(tag, sx, sy):
    """Internal and user-facing parts of one stick. Returns shell additions and cuts."""
    m = ST["module_envelope_from_axis"]
    module = union_all([
        box(sx - 8, sx + 8, sy - 8, sy + 8, 0, ST["module_top_z"]),
        box(sx + 8, sx + m["plus_x"], sy - 5, sy + 5, 0, 9),
        box(sx - 5, sx + 5, sy + 8, sy + m["plus_y"], 0, 9),
        box(sx - 4, sx + 4, sy - m["minus_y"], sy - 8, 0, 6),
        cyl(sx, sy, ST["module_top_z"], ST["shaft_top_z"], 2.0),
    ])
    add("stick_module_" + tag, module, "metal", material="purchased")

    stem = cyl(sx, sy, 13.0, CAP_TOP_Z - 1.0, SLEEVE_R).union(
        cyl(sx, sy, DISC_TOP_Z - TM["disc_thickness"], DISC_TOP_Z, DISC_R)).cut(
        cyl(sx, sy, 11.0, ST["shaft_top_z"] + 0.3, 2.05))
    add("stick_stem_disc_" + tag, stem, "pom", True, "POM or SLA resin")

    cap = cyl(sx, sy, CAP_TOP_Z - 4.0, CAP_TOP_Z, CAP_R).cut(
        cyl(sx, sy, CAP_TOP_Z - 4.5, CAP_TOP_Z - 1.0, SLEEVE_R + 0.05))
    cap = cap.cut(wp(cq.Solid.makeSphere(40, pnt=cq.Vector(sx, sy, CAP_TOP_Z + 39.2), angleDegrees1=-90,
                                         angleDegrees2=90, angleDegrees3=360)))
    add("stick_cap_" + tag, cap, "cap", True, "TPU or SLA resin")

    plunger = union_all([
        tube(sx, sy, DISC_TOP_Z, DISC_TOP_Z + 1.0, 10.7, PLUNGER_BORE_R),
        tube(sx, sy, SPRING_SEAT_Z, DISC_TOP_Z, 10.7, 9.9),
        tube(sx, sy, SPRING_SEAT_Z - 1.0, SPRING_SEAT_Z, TM["plunger_outer_radius"], 9.9),
    ])
    add("stick_plunger_" + tag, plunger, "pom", True, "POM or SLA resin")

    seat = ring_seat_z()
    add("stick_spring_" + tag, tube(sx, sy, SPRING_SEAT_Z, seat, 12.1, 11.5), "spring", material="purchased")

    ring = tube(sx, sy, seat, seat + RING_FLANGE_T, RING_RO, RING_RI).union(
        tube(sx, sy, seat - 2.75, seat, 13.6, 12.6))
    ring = ring.faces(">Z").edges("%CIRCLE").edges(cq.selectors.RadiusNthSelector(1)).chamfer(1.4)
    add("tension_ring_" + tag, ring, "ring", True, "aluminium or SLA resin")

    glow = tube(sx, sy, 14.5, TURRET_TOP_Z + 0.4, GLOW_RO, GLOW_RI)
    for lx, ly in ring_leds(tag):
        glow = glow.union(cyl(lx, ly, 2.4, 14.6, 1.5))
    add("glow_ring_" + tag, glow, "glow", True, "clear resin or translucent PETG")

    adds = [cone(sx, sy, FACE_Z - 0.2, TURRET_TOP_Z, 22.5, 20.5)]
    cuts = [cyl(sx, sy, 5, TURRET_TOP_Z + 2, BORE_R),
            cyl(sx, sy, 16.0, TURRET_TOP_Z + 2, 13.8),
            tube(sx, sy, 14.3, TURRET_TOP_Z + 2, GLOW_RO + 0.2, GLOW_RI - 0.2)]
    for lx, ly in ring_leds(tag):
        cuts.append(cyl(lx, ly, 6, 15, 1.8))
    return adds, cuts


def button_cap(x, y, r, top_z, stem_r=2.0, flange=1.2):
    return union_all([cyl(x, y, 9.0, top_z, r), cyl(x, y, 9.0, 10.2, r + flange),
                      cyl(x, y, TACT_TOP + 0.2, 9.0, stem_r)])


def build():
    t0 = time.time()
    outer = body_solid(0.0)
    cavity = body_solid(WALL)
    shell = outer.cut(cavity)
    print("body %.1fs, shell volume %.0f mm3" % (time.time() - t0, volume(shell)))
    top = shell.intersect(box(-200, 200, -200, 200, SPLIT_Z, 80))
    bottom = shell.intersect(box(-200, 200, -200, 200, -80, SPLIT_Z))
    top_adds, top_cuts, bot_adds, bot_cuts = [], [], [], []

    # --- PCB and what is soldered to it (proxies for fit checks) ---
    pcb = cq.Workplane("XY").polyline(L["pcb"]["outline"]).close().extrude(PCB_T).translate((0, 0, -PCB_T))
    holes = L["pcb"]["mount_holes"]
    for x, y in holes["positions"]:
        pcb = pcb.cut(cyl(x, y, -PCB_T - 1, 1, holes["drill"] / 2))
    add("pcb", pcb, "pcb", material="fabricated")
    esp = L["esp32_module"]
    ex, ey = esp["centre"]
    add("esp32_module", box(ex - 9, ex + 9, ey - 12.75, ey + 12.75, -PCB_T - esp["size"][2], -PCB_T),
        "metal", material="purchased")
    usb = L["usb_c"]
    add("usb_c", box(-4.5, 4.5, usb["edge_y"] - 6.9, usb["edge_y"] + 0.5, 0, 3.3), "metal", material="purchased")
    tacts = []
    for group in (L["abxy"], L["dpad"], L["system_buttons"]):
        for key in ("A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "VIEW", "HOME", "MENU", "FN"):
            if key in group:
                x, y = group[key]
                tacts.append(box(x - 3, x + 3, y - 3, y + 3, 0, 3.5).union(cyl(x, y, 3.5, TACT_TOP, 1.75)))
    add("tact_switches", compound(tacts), "part", material="purchased")
    for key in ("LB", "RB"):
        x, y = L["bumpers"][key]
        add("bumper_switch_" + key, box(x - 6.4, x + 6.4, y - 2.9, y + 2.9, 0, 6.5).union(
            box(x - 1.5, x + 1.5, y - 1.0, y + 1.0, 6.5, 7.3)), "part", material="purchased")
    bat = L["battery"]
    add("battery", box(bat["centre"][0] - bat["size"][0] / 2, bat["centre"][0] + bat["size"][0] / 2,
                       bat["centre"][1] - bat["size"][1] / 2, bat["centre"][1] + bat["size"][1] / 2,
                       bat["z"][0], bat["z"][1]), "battery", material="purchased")

    # --- sticks ---
    for tag, key in (("L", "left"), ("R", "right")):
        adds, cuts = stick_parts(tag, *ST[key])
        top_adds += adds
        top_cuts += cuts

    # --- face buttons ---
    ab = L["abxy"]
    for key in ("A", "B", "X", "Y"):
        x, y = ab[key]
        add("button_" + key, button_cap(x, y, ab["cap_diameter"] / 2, ab["cap_top_z"]), key, True,
            "translucent resin")
        top_cuts.append(cyl(x, y, 8, 20, ab["cap_diameter"] / 2 + 0.2))
    dp = L["dpad"]
    dx, dy = dp["centre"]
    dpad = union_all([cyl(dx, dy, 9.2, dp["top_z"] - 1.0, dp["diameter"] / 2),
                      cyl(dx, dy, 9.2, 10.3, dp["diameter"] / 2 + 1.3),
                      box(dx - 11, dx + 11, dy - 3.5, dy + 3.5, dp["top_z"] - 1.0, dp["top_z"]),
                      box(dx - 3.5, dx + 3.5, dy - 11, dy + 11, dp["top_z"] - 1.0, dp["top_z"]),
                      cyl(dx, dy, 0.3, 9.2, 2.0)])
    for key in ("UP", "DOWN", "LEFT", "RIGHT"):
        x, y = dp[key]
        dpad = dpad.union(cyl(x, y, TACT_TOP + 0.2, 9.2, 2.0))
    add("dpad", dpad, "dpad", True, "translucent resin, painted top")
    top_cuts.append(cyl(dx, dy, 8, 20, dp["diameter"] / 2 + 0.3))
    sb = L["system_buttons"]
    for key in ("VIEW", "HOME", "MENU", "FN"):
        x, y = sb[key]
        r = (sb["home_cap_diameter"] if key == "HOME" else sb["cap_diameter"]) / 2
        add("button_" + key, button_cap(x, y, r, sb["cap_top_z"], 1.6, 0.6), "system", True, "translucent resin")
        top_cuts.append(cyl(x, y, 8, 20, r + 0.2))

    # --- display ---
    disp = L["display"]
    cx, cy = disp["centre"]
    glass_top = INNER_Z + 0.7
    lens_z = FACE_Z - 1.0
    ow, oh = disp["outline"]
    add("display_module", union_all([
        box(cx - 24.85, cx + 24.85, cy - 12.9, cy + 12.9, glass_top - 1.8, glass_top),
        box(cx - ow / 2, cx + ow / 2, cy - oh / 2, cy + oh / 2, glass_top - 3.4, glass_top - 1.8),
        box(cx - ow / 2 + 1, cx - ow / 2 + 5, cy - 6, cy + 6, glass_top - 5.0, glass_top - 3.4),
    ]), "display", material="purchased")
    add("display_lens", box(cx - 23.3, cx + 23.3, cy - 13.5, cy + 13.5, lens_z, FACE_Z), "lens",
        material="laser-cut 1 mm acrylic")
    ww, wh = disp["window"]
    top_cuts += [box(cx - ww / 2, cx + ww / 2, cy - wh / 2, cy + wh / 2, 8, 20),
                 box(cx - 23.5, cx + 23.5, cy - 13.7, cy + 13.7, lens_z, 20),
                 box(cx - 25.2, cx + 25.2, cy - 13.2, cy + 13.2, INNER_Z - 1, glass_top + 0.1)]

    # --- USB opening ---
    for cuts in (top_cuts, bot_cuts):
        cuts.append(box(-6.5, 6.5, usb["edge_y"] + 1.5, 60, -1.6, 4.9))

    # --- PCB bosses and shell joining posts ---
    for x, y in holes["positions"]:
        top_adds.append(cyl(x, y, 0, INNER_Z + 0.5, 2.75))
        top_cuts.append(cyl(x, y, -1, 8, 0.85))
    posts = [(76, 5), (-76, 5), (72, -30), (-72, -30), (20, 45.4), (-20, 45.4)]
    for x, y in posts:
        top_adds.append(cyl(x, y, SPLIT_Z, INNER_Z + 0.5, 2.8).intersect(outer))
        top_cuts.append(cyl(x, y, SPLIT_Z - 1, 8, 0.85))
        post = cyl(x, y, -70, SPLIT_Z, 2.8).intersect(outer)
        floor = bbox(post).zmin
        bot_adds.append(post)
        bot_cuts += [cyl(x, y, -70, SPLIT_Z + 1, 1.2), cyl(x, y, -70, floor + 3.0, 2.3)]

    # --- bumpers and triggers ---
    for side, tag in ((-1, "L"), (1, "R")):
        key = "LB" if side < 0 else "RB"
        bx, by = L["bumpers"][key]
        top_cuts.append(box(side * 31.5, side * 79, 40.5, 70, 2.5, 20))
        bumper = outer.intersect(box(side * 32, side * 78.5, 41, 70, 3.0, 20))
        bumper = bumper.union(box(bx - 3, bx + 3, by - 2, 41.5, 7.6, 9.6))
        bumper = bumper.cut(cyl_y(side * 33.8, 40, 52, 8.0, 1.05))
        add("bumper_" + tag, bumper, "bumper", True, "ABS or resin")
        add("bumper_pin_" + tag, cyl_y(side * 33.8, 41, 49.5, 8.0, 1.0), "metal", material="2 mm steel pin")

        xr = L["triggers"]["x_range_abs"]
        bot_cuts.append(box(side * (xr[0] - 0.5), side * (xr[1] + 0.5), 40, 70, TRIG_BOTTOM_Z - 0.5, 1.2))
        hx, hy = L["triggers"]["hall_sensor_bottom_side"]["LT" if side < 0 else "RT"]
        mag_y, mag_z = magnet_rest_position(hy)
        trigger = outer.intersect(box(side * xr[0], side * xr[1], TRIG_BACK_Y, 70, TRIG_BOTTOM_Z, 0.2))
        trigger = trigger.union(box(hx - 2.2, hx + 2.2, mag_y - 2.2, TRIG_BACK_Y + 1, mag_z - 1.4, mag_z + 1.4))
        trigger = trigger.cut(cyl_x(-90, 90, TRIG_PIVOT_Y, TRIG_PIVOT_Z, 1.05))
        trigger = trigger.cut(box(hx - 1.55, hx + 1.55, mag_y - 1.55, mag_y + 1.55, mag_z - 1.05, mag_z + 1.7))
        add("trigger_" + tag, trigger, "trigger", True, "ABS or resin")
        add("trigger_magnet_" + tag, box(hx - 1.5, hx + 1.5, mag_y - 1.5, mag_y + 1.5, mag_z - 1.0, mag_z + 1.0),
            "magnet", material="3 x 3 x 2 mm N42")
        add("trigger_pin_" + tag, cyl_x(side * xr[0] if side > 0 else side * xr[1],
                                        side * xr[1] if side > 0 else side * xr[0],
                                        TRIG_PIVOT_Y, TRIG_PIVOT_Z, 1.0), "metal", material="2 mm steel pin")
        add("hall_sensor_" + tag, box(hx - 1.5, hx + 1.5, hy - 0.7, hy + 0.7, -PCB_T - 1.1, -PCB_T), "part",
            material="purchased")

        # adaptive trigger actuator
        mx0, mx1 = side * 34, side * 49
        add("trigger_motor_" + tag, box(mx0, mx1, 14, 26, -17, -7), "metal", material="N20-size DC motor")
        add("trigger_worm_" + tag, cyl_x(min(side * 49, side * 59), max(side * 49, side * 59), 20, -12, 3.0),
            "brass", material="module 0.5 single-start worm")
        wx, wy = side * WHEEL_X, 27.5
        wheel = cyl(wx, wy, -13.5, -10.5, 5.25).union(box(wx - 1.5, wx + 1.5, wy, TRIG_BACK_Y - 0.1, -13.5, -10.5))
        wheel = wheel.cut(cyl(wx, wy, -14, -10, 1.05))
        add("trigger_arm_" + tag, wheel, "pom", True, "SLA resin arm on a purchased 20T wheel")
        add("trigger_arm_pin_" + tag, cyl(wx, wy, FLOOR_Z, -9.5, 1.0), "metal", material="2 mm steel pin")
        bot_adds += [cyl(wx, wy, FLOOR_Z - 0.2, -13.7, 2.4), box(mx0, mx1, 13, 27, FLOOR_Z - 0.2, -17)]

    # --- paddles, their switches, rumble motors ---
    pads = L["paddles"]
    for i, key in enumerate(("P1_upper_left", "P2_lower_left", "P3_upper_right", "P4_lower_right"), start=1):
        px, py = pads[key]
        zs = bbox(outer.intersect(box(px - 0.5, px + 0.5, py - 0.5, py + 0.5, -80, 0))).zmin
        add("paddle_%d" % i, box(px - 5.5, px + 5.5, py - 10, py + 10, zs - 2.2, zs + 0.8).union(
            cyl(px, py, zs + 0.8, zs + 7.8, 2.0)), "paddle", True, "ABS or resin")
        add("paddle_switch_%d" % i, box(px - 2.9, px + 2.9, py - 6.4, py + 6.4, zs + 8.8, zs + 15.3).union(
            box(px - 1.0, px + 1.0, py - 1.5, py + 1.5, zs + 7.9, zs + 8.8)), "part", material="D2FC-type micro switch")
        bot_cuts += [box(px - 5.8, px + 5.8, py - 10.3, py + 10.3, zs - 4, zs + 1.0),
                     cyl(px, py, zs - 1, zs + 10, 2.3)]
    for side, tag in ((-1, "L"), (1, "R")):
        rx, ry = L["rumble"]["left" if side < 0 else "right"]
        add("rumble_motor_" + tag, cyl_y(rx, ry - 14.5, ry + 10.5, -22, 5.0), "metal", material="10 x 25 mm ERM")

    # --- service holes for BOOT and RESET ---
    for x, y in ((-45, -30), (-37, -29)):
        bot_cuts.append(cyl(x, y, -80, FLOOR_Z + 1, 1.0))

    t1 = time.time()
    top = apply_features("top shell", top, top_adds, top_cuts)
    bottom = apply_features("bottom shell", bottom, bot_adds, bot_cuts)
    print("shell features %.1fs" % (time.time() - t1))
    add("top_shell", top, "shell_top", True, "ABS, PETG, or nylon")
    add("bottom_shell", bottom, "shell_bottom", True, "ABS, PETG, or nylon")
    return outer


def apply_features(name, shell, adds, cuts):
    """Fuse the additions, then remove the cuts. Tools are fused first because
    cutting with a compound of overlapping solids gives a broken result."""
    before = volume(shell)
    out = shell.union(union_all(adds)).cut(union_all(cuts))
    if not out.val().isValid() or volume(out) < 0.5 * before:
        raise RuntimeError("%s is invalid after its features were applied" % name)
    return out


def magnet_rest_position(hall_y):
    """Where the trigger magnet sits at rest so it ends 2.5 mm under the sensor at full pull."""
    target = (hall_y - TRIG_PIVOT_Y, (-PCB_T - 1.1 - 2.5 - 1.0) - TRIG_PIVOT_Z)
    a = math.radians(TRIG_TRAVEL)
    y = target[0] * math.cos(a) - target[1] * math.sin(a)
    z = target[0] * math.sin(a) + target[1] * math.cos(a)
    return TRIG_PIVOT_Y + y, TRIG_PIVOT_Z + z


# ---------- checks ----------

ALLOWED_CONTACT = [
    ("stick_stem_disc", "stick_module"), ("stick_cap", "stick_stem_disc"),
    ("bumper_pin", "bumper_"), ("trigger_pin", "trigger_"), ("trigger_arm_pin", "trigger_arm"),
    ("trigger_magnet", "trigger_"), ("bumper_pin", "top_shell"), ("trigger_pin", "bottom_shell"),
    ("trigger_arm_pin", "bottom_shell"), ("trigger_motor", "bottom_shell"),
    ("trigger_worm", "trigger_arm"),
]


def allowed(a, b):
    for p, q in ALLOWED_CONTACT:
        if (a.startswith(p) and b.startswith(q)) or (b.startswith(p) and a.startswith(q)):
            return True
    return False


def boxes_overlap(a, b):
    return (a.xmin < b.xmax and b.xmin < a.xmax and a.ymin < b.ymax and b.ymin < a.ymax
            and a.zmin < b.zmax and b.zmin < a.zmax)


def interference():
    found = []
    boxes = [bbox(p["shape"]) for p in PARTS]
    for i in range(len(PARTS)):
        for j in range(i + 1, len(PARTS)):
            a, b = PARTS[i], PARTS[j]
            if not boxes_overlap(boxes[i], boxes[j]) or allowed(a["name"], b["name"]):
                continue
            v = volume(a["shape"].intersect(b["shape"]))
            if v > 0.05:
                found.append({"a": a["name"], "b": b["name"], "volume_mm3": round(v, 2)})
    return found


def tilt_check():
    """Tilt each stick 25 degrees in eight directions and look for contact with fixed parts."""
    results = []
    for tag, key in (("L", "left"), ("R", "right")):
        sx, sy = ST[key]
        moving = get("stick_cap_" + tag).union(get("stick_stem_disc_" + tag))
        fixed = [("top_shell", get("top_shell")), ("glow_ring", get("glow_ring_" + tag)),
                 ("tension_ring_loosest", get("tension_ring_" + tag).translate((0, 0, RING_TRAVEL / 2))),
                 ("stick_module", get("stick_module_" + tag).cut(cyl(sx, sy, ST["module_top_z"], 30, 3)))]
        worst = 0.0
        for k in range(8):
            ang = math.radians(45 * k)
            axis_end = (sx + math.cos(ang), sy + math.sin(ang), PIVOT_Z)
            tilted = moving.rotate((sx, sy, PIVOT_Z), axis_end, TILT)
            for name, shape in fixed:
                v = volume(tilted.intersect(shape))
                worst = max(worst, v)
                if v > 0.05:
                    results.append({"stick": tag, "direction_deg": 45 * k, "hits": name, "volume_mm3": round(v, 2)})
        print("stick %s tilt check, worst contact %.2f mm3" % (tag, worst))
    return results


def trigger_check():
    results = []
    for side, tag in ((-1, "L"), (1, "R")):
        pulled = get("trigger_" + tag).union(get("trigger_magnet_" + tag)).rotate(
            (0, TRIG_PIVOT_Y, TRIG_PIVOT_Z), (1, TRIG_PIVOT_Y, TRIG_PIVOT_Z), -TRIG_TRAVEL)
        wx, wy = side * WHEEL_X, 27.5
        parked = get("trigger_arm_" + tag).rotate((wx, wy, 0), (wx, wy, 1), side * ARM_PARK_DEG)
        fixed = [("bottom_shell", get("bottom_shell")), ("top_shell", get("top_shell")), ("pcb", get("pcb")),
                 ("hall_sensor", get("hall_sensor_" + tag)), ("battery", get("battery")),
                 ("trigger_motor", get("trigger_motor_" + tag)), ("trigger_arm_parked", parked),
                 ("bumper_switch", get("bumper_switch_" + ("LB" if side < 0 else "RB"))),
                 ("bumper", get("bumper_" + tag))]
        for name, shape in fixed:
            v = volume(pulled.intersect(shape))
            if v > 0.05:
                results.append({"trigger": tag, "hits": name, "volume_mm3": round(v, 2)})
        mag = bbox(get("trigger_magnet_" + tag).rotate(
            (0, TRIG_PIVOT_Y, TRIG_PIVOT_Z), (1, TRIG_PIVOT_Y, TRIG_PIVOT_Z), -TRIG_TRAVEL))
        hall = bbox(get("hall_sensor_" + tag))
        results.append({"trigger": tag, "magnet_gap_at_full_pull_mm": round(hall.zmin - mag.zmax, 2),
                        "magnet_y_offset_mm": round((mag.ymin + mag.ymax) / 2 - (hall.ymin + hall.ymax) / 2, 2)})
        blocked = volume(pulled.intersect(get("trigger_arm_" + tag)))
        results.append({"trigger": tag, "arm_blocks_pull": blocked > 0.05})
    return results


def tension_numbers():
    lever = CAP_TOP_Z - PIVOT_Z
    out = {}
    for label, offset in (("loosest", 1), ("mid", 0), ("tightest", -1)):
        length = ring_seat_z(offset) - SPRING_SEAT_Z
        force = SPRING_RATE * (SPRING_FREE - length)
        out[label] = {"spring_length_mm": round(length, 2), "spring_force_n": round(force, 2),
                      "added_force_at_cap_gf": round(force * DISC_R / lever / 9.81 * 1000, 1)}
    a = math.radians(TILT)
    h = DISC_TOP_Z - PIVOT_Z
    lift = h / math.cos(a) + PLUNGER_BORE_R * math.tan(a) - h
    out["plunger_lift_at_full_tilt_mm"] = round(lift, 2)
    out["spring_length_at_full_tilt_tightest_mm"] = round(out["tightest"]["spring_length_mm"] - lift, 2)
    out["spring_force_at_full_tilt_tightest_n"] = round(
        SPRING_RATE * (SPRING_FREE - out["tightest"]["spring_length_mm"] + lift), 2)
    out["spring"] = "free length %.1f mm, rate %.2f N/mm, mean diameter 23.6 mm, 0.5 mm wire" % (SPRING_FREE, SPRING_RATE)
    out["planned_added_range_gf"] = TM["force_range_added_gf"]
    return out


def layout_assertions():
    """The top shell openings must sit over the frozen PCB positions."""
    top = get("top_shell")
    probes = [("stick " + k, ST[k]) for k in ("left", "right")]
    probes += [("button " + k, L["abxy"][k]) for k in "ABXY"]
    probes += [("button " + k, L["system_buttons"][k]) for k in ("VIEW", "HOME", "MENU", "FN")]
    probes.append(("dpad", L["dpad"]["centre"]))
    probes.append(("display", L["display"]["centre"]))
    failures = []
    for name, (x, y) in probes:
        if volume(top.intersect(cyl(x, y, INNER_Z - 1, 25, 0.5))) > 0.001:
            failures.append(name)
    return failures


# ---------- export and render ----------

def tessellate(shape, tol=0.08):
    import numpy as np
    verts, faces = [], []
    for solid in shape.vals():
        v, f = solid.tessellate(tol, 0.25)
        base = len(verts)
        verts += [(p.x, p.y, p.z) for p in v]
        faces += [(a + base, b + base, c + base) for a, b, c in f]
    return np.array(verts, dtype=float), np.array(faces, dtype=int)


def export_files():
    for sub in ("step", "stl"):
        os.makedirs(os.path.join(OUT, sub), exist_ok=True)
    assembly = cq.Assembly(name="kestrel")
    for p in PARTS:
        cq.exporters.export(p["shape"], os.path.join(OUT, "step", p["name"] + ".step"))
        if p["printed"]:
            cq.exporters.export(p["shape"], os.path.join(OUT, "stl", p["name"] + ".stl"),
                                tolerance=0.05, angularTolerance=0.2)
        assembly.add(p["shape"], name=p["name"], color=cq.Color(*p["color"], 1.0))
    assembly.save(os.path.join(OUT, "step", "kestrel_assembly.step"))


def meshes(selection=None, cutter=None):
    out = []
    for p in PARTS:
        if selection and not selection(p):
            continue
        shape = p["shape"].intersect(cutter) if cutter is not None else p["shape"]
        if not shape.vals():
            continue
        v, f = tessellate(shape)
        if len(f):
            out.append((p, v, f))
    return out


def render(path, items, camera, offsets=None, size=(1800, 1150), zoom=1.0):
    import numpy as np
    import pyvista as pv
    pl = pv.Plotter(off_screen=True, window_size=size)
    pl.set_background("white")
    for p, v, f in items:
        shift = np.array(offsets(p)) if offsets else np.zeros(3)
        mesh = pv.PolyData(v + shift, np.hstack([np.full((len(f), 1), 3), f]).astype(np.int64))
        opacity = 0.55 if p["name"].startswith("glow_ring") or p["name"] == "display_lens" else 1.0
        pl.add_mesh(mesh, color=p["color"], smooth_shading=True, split_sharp_edges=True,
                    specular=0.25, specular_power=20, opacity=opacity)
    pl.camera_position = camera
    pl.camera.zoom(zoom)
    try:
        pl.enable_anti_aliasing("ssaa")
    except Exception:
        pass
    pl.screenshot(path)
    pl.close()


def explode_offset(p):
    n = p["name"]
    if n == "top_shell":
        return (0, 0, 45)
    if n.startswith(("stick_cap", "tension_ring", "glow_ring", "button_", "dpad", "display_lens", "bumper_")):
        return (0, 0, 85)
    if n.startswith(("stick_plunger", "stick_spring", "stick_stem")):
        return (0, 0, 65)
    if n == "display_module":
        return (0, 0, 28)
    if n == "bottom_shell":
        return (0, 0, -75)
    if n.startswith(("paddle_",)) and "switch" not in n:
        return (0, 0, -105)
    if n.startswith(("battery", "trigger_motor", "trigger_worm", "trigger_arm", "rumble", "paddle_switch")):
        return (0, 0, -45)
    if n.startswith("trigger_"):
        return (0, 30, -30)
    return (0, 0, 0)


def render_all():
    folder = os.path.join(OUT, "renders")
    os.makedirs(folder, exist_ok=True)
    everything = meshes()
    up = (0, 0, 1)
    render(os.path.join(folder, "front_three_quarter.png"), everything, [(150, -300, 260), (0, -8, 0), up], zoom=1.25)
    render(os.path.join(folder, "top.png"), everything, [(0, -12, 520), (0, -12, 0), (0, 1, 0)], zoom=1.3)
    render(os.path.join(folder, "back_paddles.png"), everything, [(-150, -260, -300), (0, -15, -10), (0, 0, -1)], zoom=1.25)
    render(os.path.join(folder, "front_edge.png"), everything, [(110, 330, 90), (0, 20, -4), up], zoom=1.35)
    render(os.path.join(folder, "exploded.png"), everything, [(330, -520, 300), (0, -10, -10), up],
           offsets=explode_offset, size=(1800, 1500), zoom=1.15)
    sx, sy = ST["left"]
    near = lambda p: abs((bbox(p["shape"]).xmin + bbox(p["shape"]).xmax) / 2 - sx) < 70
    section = meshes(near, box(sx - 34, sx + 34, sy, sy + 60, -8, 40))
    render(os.path.join(folder, "section_stick.png"), section, [(sx, sy - 200, 16), (sx, sy, 13), up],
           size=(1800, 1100), zoom=3.3)
    cut = meshes(lambda p: True, box(-200, -WHEEL_X, 0, 70, -30, 20))
    render(os.path.join(folder, "section_trigger.png"), cut, [(160, 30, -2), (-WHEEL_X, 30, -5), up],
           size=(1800, 1100), zoom=3.2)


def export_glb():
    import numpy as np
    import trimesh
    scene = trimesh.Scene()
    for p, v, f in meshes():
        mesh = trimesh.Trimesh(vertices=v / 1000.0, faces=f, process=False)
        rgba = [int(c * 255) for c in p["color"]] + [255]
        mesh.visual.face_colors = np.tile(rgba, (len(f), 1))
        scene.add_geometry(mesh, node_name=p["name"], geom_name=p["name"])
    path = os.path.join(OUT, "kestrel_assembly.glb")
    scene.export(path)
    with open(path, "rb") as fh:
        data = base64.b64encode(fh.read()).decode("ascii")
    html = VIEWER.replace("__GLB__", data)
    with open(os.path.join(OUT, "viewer.html"), "w", encoding="utf-8") as fh:
        fh.write(html)


VIEWER = """<!doctype html>
<html><head><meta charset="utf-8"><title>Kestrel controller</title>
<style>html,body{margin:0;height:100%;background:#f3f4f6}canvas{display:block}</style>
<script type="importmap">{"imports":{"three":"https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.module.js","three/addons/":"https://cdn.jsdelivr.net/npm/three@0.160.0/examples/jsm/"}}</script>
</head><body><script type="module">
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
const renderer = new THREE.WebGLRenderer({antialias:true});
renderer.setSize(innerWidth, innerHeight); renderer.setPixelRatio(devicePixelRatio);
document.body.appendChild(renderer.domElement);
const scene = new THREE.Scene(); scene.background = new THREE.Color(0xf3f4f6);
const camera = new THREE.PerspectiveCamera(35, innerWidth/innerHeight, 0.01, 10);
camera.up.set(0,0,1); camera.position.set(0.16,-0.30,0.24);
scene.add(new THREE.HemisphereLight(0xffffff, 0x666677, 2.2));
const sun = new THREE.DirectionalLight(0xffffff, 2.0); sun.position.set(0.3,-0.4,0.8); scene.add(sun);
const controls = new OrbitControls(camera, renderer.domElement); controls.target.set(0,-0.01,0);
const bytes = Uint8Array.from(atob("__GLB__"), c => c.charCodeAt(0));
new GLTFLoader().parse(bytes.buffer, '', gltf => scene.add(gltf.scene));
addEventListener('resize', () => {camera.aspect = innerWidth/innerHeight; camera.updateProjectionMatrix(); renderer.setSize(innerWidth, innerHeight);});
(function loop(){requestAnimationFrame(loop); controls.update(); renderer.render(scene, camera);})();
</script></body></html>
"""


def main():
    quick = "--quick" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    t0 = time.time()
    outer = build()
    print("built %d parts in %.1fs" % (len(PARTS), time.time() - t0))
    report = {"parts": [{"name": p["name"], "printed": p["printed"], "material": p["material"],
                         "volume_mm3": round(volume(p["shape"]), 1)} for p in PARTS]}
    ob = bbox(outer)
    report["overall_size_mm"] = [round(ob.xlen, 1), round(ob.ylen, 1), round(ob.zlen + (CAP_TOP_Z - FACE_Z), 1)]
    report["layout_openings_blocked"] = layout_assertions()
    report["tension"] = tension_numbers()
    if not quick:
        t1 = time.time()
        report["interference"] = interference()
        report["stick_tilt_contacts"] = tilt_check()
        report["trigger_sweep"] = trigger_check()
        print("checks %.1fs" % (time.time() - t1))
        export_files()
        export_glb()
    with open(os.path.join(OUT, "checks.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k != "parts"}, indent=1))
    render_all()
    print("done in %.1fs" % (time.time() - t0))


if __name__ == "__main__":
    main()
