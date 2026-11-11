"""Write the Kestrel schematic as a KiCad .kicad_sch file.

Every pin carries a net label, so the sheet has no wires. Parts are grouped
into titled blocks and packed in rows.
"""
import math
import uuid

import symlib
from design import NC
from sexpr import Q, dump

GRID = 1.27
SCH_VERSION = "20260306"
NAMESPACE = uuid.UUID("6b657374-7265-6c2d-6b69-636164000000")
CHAR_W = 1.05
ROW_WIDTH = 400.0
COLUMN_HEIGHT = 560.0

BLOCK_TITLES = [
    ("usb", "USB-C input"),
    ("charger", "Charger and power path"),
    ("battery", "Battery, reverse protection, fuel gauge"),
    ("rails", "3.3 V, switched rails, analogue supply"),
    ("mcu", "MCU module"),
    ("analog", "Analogue input filters"),
    ("sticks", "Thumbsticks"),
    ("triggers", "Trigger sensing and motor driver"),
    ("rumble", "Rumble and paddles"),
    ("inputs", "Button shift registers"),
    ("buttons", "Buttons and bumpers"),
    ("leds", "LED chain"),
    ("display", "Display connector"),
]


def uid(*parts):
    return str(uuid.uuid5(NAMESPACE, "/".join(parts)))


def snap(v):
    return round(round(v / GRID) * GRID, 4)


def resolve(part):
    """Return (symbol, pins, {pin number: net}) and insist every pin is decided."""
    lib, name = part.sym.split(":")
    sym = symlib.get_symbol(lib, name)
    pins = symlib.pins(sym, 1)
    by_name, by_number = {}, {}
    for p in pins:
        by_name.setdefault(p["name"], []).append(p)
        by_number[p["number"]] = p
    nets = {p["number"]: None for p in pins}
    for key, net in part.pins.items():
        if key in by_name:
            targets = by_name[key]
        elif key in by_number:
            targets = [by_number[key]]
        else:
            raise KeyError("%s (%s) has no pin %r" % (part.ref, part.sym, key))
        for p in targets:
            nets[p["number"]] = net
    undecided = sorted(n for n, net in nets.items() if net is None)
    if undecided:
        raise ValueError("%s: pins with no net and no NC mark: %s" % (part.ref, undecided))
    return sym, pins, nets


def pin_offset(pin, rot):
    """Pin connection point relative to the symbol origin, in sheet axes (Y down)."""
    if rot == 0:
        return pin["x"], -pin["y"]
    return -pin["y"], -pin["x"]


def pin_outward(pin, rot):
    """Unit vector pointing away from the symbol body, in sheet axes."""
    a = math.radians(pin["angle"])
    c, s = round(math.cos(a)), round(math.sin(a))
    if rot == 0:
        return -c, s
    return s, c


LABEL_STYLE = {(1, 0): (0, "left"), (-1, 0): (180, "right"), (0, -1): (90, "left"), (0, 1): (270, "right")}


class Cell:
    """A placed symbol with the space its labels need."""

    def __init__(self, part):
        self.part = part
        self.sym, self.pins, self.nets = resolve(part)
        self.rot = 90 if part.horizontal else 0
        xs, ys = [-2.54, 2.54], [-2.54, 2.54]
        for p in self.pins:
            x, y = pin_offset(p, self.rot)
            dx, dy = pin_outward(p, self.rot)
            net = self.nets[p["number"]]
            reach = 2.0 if net == NC else 1.5 + CHAR_W * len(net)
            xs += [x, x + dx * reach]
            ys += [y, y + dy * reach]
        self.left, self.right = min(xs) - 2.5, max(xs) + 2.5
        self.top, self.bottom = min(ys) - 5.0, max(ys) + 5.0

    @property
    def width(self):
        return self.right - self.left

    @property
    def height(self):
        return self.bottom - self.top


def prop(name, value, x, y, hide=False):
    effects = ["effects", ["font", ["size", 1.27, 1.27]]]
    if hide:
        effects.append(["hide", "yes"])
    return ["property", Q(name), Q(value), ["at", x, y, 0], effects]


def in_bom(part):
    """Power flags, solder jumpers, and test pads are not purchased parts."""
    return part.fp is not None and not part.ref.startswith(("JP", "TP"))


def symbol_node(cell, x, y, root_uuid):
    part = cell.part
    is_flag = part.fp is None
    ref = part.ref if not is_flag else "#" + part.ref
    node = ["symbol", ["lib_id", Q(part.sym)], ["at", x, y, cell.rot], ["unit", 1],
            ["exclude_from_sim", "no"], ["in_bom", "yes" if in_bom(part) else "no"],
            ["on_board", "no" if is_flag else "yes"], ["dnp", "no"],
            ["uuid", Q(uid("sym", part.ref))],
            prop("Reference", ref, x, snap(y + cell.top + 2.0), hide=is_flag),
            prop("Value", part.value, x, snap(y + cell.bottom - 2.0)),
            prop("Footprint", part.fp or "", x, y, hide=True),
            prop("Datasheet", "", x, y, hide=True),
            prop("Description", "", x, y, hide=True)]
    if part.mpn:
        node.append(prop("MPN", part.mpn, x, y, hide=True))
    for p in cell.pins:
        node.append(["pin", Q(p["number"]), ["uuid", Q(uid("pin", part.ref, p["number"]))]])
    node.append(["instances", ["project", Q("kestrel"),
                               ["path", Q("/" + root_uuid), ["reference", Q(ref)], ["unit", 1]]]])
    return node


def pin_nodes(cell, x, y):
    out = []
    for p in cell.pins:
        ox, oy = pin_offset(p, cell.rot)
        px, py = round(x + ox, 4), round(y + oy, 4)
        net = cell.nets[p["number"]]
        tag = uid("pinmark", cell.part.ref, p["number"])
        if net == NC:
            out.append(["no_connect", ["at", px, py], ["uuid", Q(tag)]])
            continue
        angle, justify = LABEL_STYLE[pin_outward(p, cell.rot)]
        out.append(["label", Q(net), ["at", px, py, angle],
                    ["effects", ["font", ["size", 1.27, 1.27]], ["justify", justify, "bottom"]],
                    ["uuid", Q(tag)]])
    return out


def layout(design):
    """Pack cells into blocks and blocks into columns. Returns placements and sheet size."""
    blocks = {}
    for part in design.parts.values():
        blocks.setdefault(part.block, []).append(Cell(part))
    known = [key for key, _ in BLOCK_TITLES]
    unknown = sorted(set(blocks) - set(known))
    if unknown:
        raise ValueError("parts in untitled blocks: %s" % unknown)

    placed, frames = [], []
    col_x, cursor_y, col_w = 20.0, 20.0, 0.0
    for key, title in BLOCK_TITLES:
        cells = blocks.get(key, [])
        if not cells:
            continue
        rows, row, row_w = [], [], 0.0
        for cell in cells:
            if row and row_w + cell.width > ROW_WIDTH:
                rows.append(row)
                row, row_w = [], 0.0
            row.append(cell)
            row_w += cell.width
        rows.append(row)
        block_w = max(sum(c.width for c in r) for r in rows) + 10.0
        block_h = sum(max(c.height for c in r) for r in rows) + 16.0
        if cursor_y > 20.0 and cursor_y + block_h > COLUMN_HEIGHT:
            col_x += col_w + 12.0
            cursor_y, col_w = 20.0, 0.0
        y = cursor_y + 12.0
        for r in rows:
            x = col_x + 5.0
            row_h = max(c.height for c in r)
            for cell in r:
                placed.append((cell, snap(x - cell.left), snap(y - cell.top)))
                x += cell.width
            y += row_h
        frames.append((title, col_x, cursor_y, block_w, block_h))
        col_w = max(col_w, block_w)
        cursor_y += block_h + 8.0
    sheet_w = col_x + col_w + 20.0
    sheet_h = max(fy + fh for _, _, fy, _, fh in frames) + 20.0
    return placed, frames, sheet_w, sheet_h


def build(design, title="Kestrel controller main board", rev="A", date="2026-11-10"):
    root_uuid = uid("root")
    placed, frames, sheet_w, sheet_h = layout(design)

    lib_symbols = ["lib_symbols"]
    seen = set()
    for cell, _, _ in placed:
        if cell.part.sym in seen:
            continue
        seen.add(cell.part.sym)
        node = cell.sym
        node[1] = Q(cell.part.sym)
        lib_symbols.append(node)

    root = ["kicad_sch", ["version", SCH_VERSION], ["generator", Q("kestrel_gen")],
            ["generator_version", Q("10.0")], ["uuid", Q(root_uuid)],
            ["paper", Q("User"), round(sheet_w, 1), round(sheet_h, 1)],
            ["title_block", ["title", Q(title)], ["date", Q(date)], ["rev", Q(rev)],
             ["comment", 1, Q("Generated by hardware/gen/build.py from hardware/gen/design.py")]],
            lib_symbols]
    for title_text, fx, fy, fw, fh in frames:
        root.append(["rectangle", ["start", round(fx, 2), round(fy, 2)],
                     ["end", round(fx + fw, 2), round(fy + fh, 2)],
                     ["stroke", ["width", 0], ["type", "dash"]], ["fill", ["type", "none"]],
                     ["uuid", Q(uid("frame", title_text))]])
        root.append(["text", Q(title_text), ["exclude_from_sim", "no"],
                     ["at", round(fx + 3, 2), round(fy + 7, 2), 0],
                     ["effects", ["font", ["size", 3.5, 3.5], ["bold", "yes"]], ["justify", "left", "bottom"]],
                     ["uuid", Q(uid("title", title_text))]])
    for cell, x, y in placed:
        root.extend(pin_nodes(cell, x, y))
    for cell, x, y in placed:
        root.append(symbol_node(cell, x, y, root_uuid))
    root.append(["sheet_instances", ["path", Q("/"), ["page", Q("1")]]])
    root.append(["embedded_fonts", "no"])
    return dump(root) + "\n"


def write(design, path):
    text = build(design)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return len(design.parts)
