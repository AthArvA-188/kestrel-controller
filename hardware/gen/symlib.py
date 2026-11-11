"""Load symbols from the KiCad symbol libraries and report their pins.

Run as a script to list the pins of one or more symbols:

    python symlib.py Device:R 74xx:74HC165
"""
import copy
import json
import os
import sys

from sexpr import Q, find, find_all, parse

KICAD_ROOT = os.environ.get("KICAD_ROOT", r"D:\Tools\KiCad-10.0.8")
SYMBOL_DIR = os.path.join(KICAD_ROOT, "share", "kicad", "symbols")
LIB_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
LOCAL_SYMBOL_DIRS = [LIB_DIR, os.path.join(LIB_DIR, "easyeda")]

_libs = {}


def _lib_path(lib):
    for folder in LOCAL_SYMBOL_DIRS:
        local = os.path.join(folder, lib + ".kicad_sym")
        if os.path.exists(local):
            return local
    return os.path.join(SYMBOL_DIR, lib + ".kicad_sym")


def load_lib(lib):
    if lib not in _libs:
        with open(_lib_path(lib), encoding="utf-8") as f:
            tree = parse(f.read())
        _libs[lib] = {s[1]: s for s in find_all(tree, "symbol")}
    return _libs[lib]


def get_symbol(lib, name):
    """Return a copy of the symbol with any `extends` parent merged in."""
    node = load_lib(lib)[name]
    ext = find(node, "extends")
    if ext is None:
        return copy.deepcopy(node)
    parent = get_symbol(lib, ext[1])
    parent_name = parent[1]
    merged = ["symbol", Q(name)]
    child_props = {p[1]: p for p in find_all(node, "property")}
    seen = set()
    for item in parent[2:]:
        if isinstance(item, list) and item[0] == "property":
            seen.add(item[1])
            merged.append(copy.deepcopy(child_props.get(item[1], item)))
        elif isinstance(item, list) and item[0] == "symbol":
            unit = copy.deepcopy(item)
            unit[1] = Q(name + unit[1][len(parent_name):])
            merged.append(unit)
        else:
            merged.append(item)
    for key, prop in child_props.items():
        if key not in seen:
            merged.insert(2 + len(seen), copy.deepcopy(prop))
            seen.add(key)
    return merged


def units(symbol):
    """Unit numbers the symbol defines, not counting the shared unit 0."""
    found = set()
    for sub in find_all(symbol, "symbol"):
        unit = int(sub[1].rsplit("_", 2)[1])
        if unit:
            found.add(unit)
    return sorted(found) or [1]


def pins(symbol, unit=1):
    """Pins of one unit as dicts: number, name, type, x, y, angle, length."""
    out = []
    for sub in find_all(symbol, "symbol"):
        _, sub_unit, style = sub[1].rsplit("_", 2)
        if int(sub_unit) not in (0, unit) or int(style) not in (0, 1):
            continue
        for pin in find_all(sub, "pin"):
            at = find(pin, "at")
            out.append({
                "number": str(find(pin, "number")[1]),
                "name": str(find(pin, "name")[1]),
                "type": pin[1],
                "x": float(at[1]),
                "y": float(at[2]),
                "angle": int(float(at[3])) if len(at) > 3 else 0,
                "length": float(find(pin, "length")[1]),
                "hidden": find(pin, "hide") is not None,
            })
    return out


def prop(symbol, key):
    for p in find_all(symbol, "property"):
        if p[1] == key:
            return str(p[2])
    return ""


if __name__ == "__main__":
    for spec in sys.argv[1:]:
        lib, name = spec.split(":")
        sym = get_symbol(lib, name)
        print("##", spec, "| footprint:", prop(sym, "Footprint"), "| units:", units(sym))
        for unit in units(sym):
            row = ["%s=%s(%s)" % (p["number"], p["name"], p["type"]) for p in pins(sym, unit)]
            print("  unit %d: %s" % (unit, json.dumps(row)))
