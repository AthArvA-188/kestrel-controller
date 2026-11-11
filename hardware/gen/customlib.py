"""Write the project's own symbol and footprint libraries.

KiCad's stock libraries cover every other part. The thumbstick symbol and
footprint come from the EasyEDA library entry for the ALPS RKJXV1224005 and
live in hardware/lib/easyeda.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
HW = os.path.abspath(os.path.join(HERE, ".."))
LIB = os.path.join(HW, "lib")

FONT = "(effects (font (size 1.27 1.27)))"
HIDDEN = "(effects (font (size 1.27 1.27)) (hide yes))"


def _pin(kind, x, y, angle, name, number):
    return ('\t\t\t(pin %s line (at %s %s %d) (length 2.54) (name "%s" %s) (number "%s" %s))'
            % (kind, x, y, angle, name, FONT, number, FONT))


def max17048_symbol():
    left = [("power_in", "VDD", "3"), ("passive", "CELL", "2"), ("input", "QSTRT", "6"),
            ("passive", "CTG", "1")]
    right = [("bidirectional", "SDA", "8"), ("input", "SCL", "7"), ("open_collector", "ALRT", "5")]
    pins = []
    for i, (kind, name, number) in enumerate(left):
        pins.append(_pin(kind, -12.7, 3.81 - 2.54 * i, 0, name, number))
    for i, (kind, name, number) in enumerate(right):
        pins.append(_pin(kind, 12.7, 3.81 - 2.54 * i, 180, name, number))
    pins.append(_pin("power_in", -2.54, -10.16, 90, "GND", "4"))
    pins.append(_pin("passive", 2.54, -10.16, 90, "EP", "9"))
    return "\n".join([
        '\t(symbol "MAX17048"',
        "\t\t(exclude_from_sim no)",
        "\t\t(in_bom yes)",
        "\t\t(on_board yes)",
        '\t\t(property "Reference" "U" (at -10.16 8.89 0) %s)' % FONT,
        '\t\t(property "Value" "MAX17048" (at 5.08 8.89 0) %s)' % FONT,
        '\t\t(property "Footprint" "Package_DFN_QFN:DFN-8-1EP_2x2mm_P0.5mm_EP0.8x1.6mm" (at 0 0 0) %s)' % HIDDEN,
        '\t\t(property "Datasheet" "https://www.analog.com/media/en/technical-documentation/data-sheets/MAX17048-MAX17049.pdf" (at 0 0 0) %s)' % HIDDEN,
        '\t\t(property "Description" "Single-cell Li-ion fuel gauge, I2C, TDFN-8" (at 0 0 0) %s)' % HIDDEN,
        '\t\t(symbol "MAX17048_0_1"',
        "\t\t\t(rectangle (start -10.16 7.62) (end 10.16 -7.62) (stroke (width 0.254) (type default)) (fill (type background)))",
        "\t\t)",
        '\t\t(symbol "MAX17048_1_1"',
        "\n".join(pins),
        "\t\t)",
        "\t)",
    ])


def write_symbol_lib():
    text = "\n".join([
        "(kicad_symbol_lib",
        "\t(version 20251024)",
        '\t(generator "kestrel_gen")',
        '\t(generator_version "10.0")',
        max17048_symbol(),
        ")",
        "",
    ])
    with open(os.path.join(LIB, "kestrel.kicad_sym"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def d2fc_footprint():
    """D2FC-type micro switch: 12.8 x 5.8 mm body, three pins in line at 5.08 mm."""
    stroke = "(stroke (width %s) (type solid)) (fill no)"
    pad = '\t(pad "%s" thru_hole %s (at %s 0) (size 2 2) (drill 1.3) (layers "*.Cu" "*.Mask"))'
    return "\n".join([
        '(footprint "SW_D2FC_THT"',
        "\t(version 20241229)",
        '\t(generator "kestrel_gen")',
        '\t(layer "F.Cu")',
        '\t(descr "D2FC-type micro switch, 12.8 x 5.8 mm body, three pins at 5.08 mm pitch")',
        '\t(property "Reference" "REF**" (at 0 -4.3 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))',
        '\t(property "Value" "SW_D2FC_THT" (at 0 4.3 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))',
        "\t(attr through_hole)",
        '\t(fp_rect (start -6.4 -2.9) (end 6.4 2.9) %s (layer "F.Fab"))' % (stroke % "0.1"),
        '\t(fp_rect (start -6.52 -3.02) (end 6.52 3.02) %s (layer "F.SilkS"))' % (stroke % "0.12"),
        '\t(fp_rect (start -6.9 -3.4) (end 6.9 3.4) %s (layer "F.CrtYd"))' % (stroke % "0.05"),
        pad % ("1", "rect", "-5.08"),
        pad % ("2", "circle", "0"),
        pad % ("3", "circle", "5.08"),
        ")",
        "",
    ])


def write_footprint_lib():
    folder = os.path.join(LIB, "kestrel.pretty")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "SW_D2FC_THT.kicad_mod"), "w", encoding="utf-8", newline="\n") as f:
        f.write(d2fc_footprint())


def write_lib_tables():
    sym = "\n".join([
        "(sym_lib_table",
        "\t(version 7)",
        '\t(lib (name "kestrel")(type "KiCad")(uri "${KIPRJMOD}/lib/kestrel.kicad_sym")(options "")(descr "Kestrel parts"))',
        '\t(lib (name "kestrel_easyeda")(type "KiCad")(uri "${KIPRJMOD}/lib/easyeda/kestrel_easyeda.kicad_sym")(options "")(descr "Parts converted from EasyEDA"))',
        ")",
        "",
    ])
    fp = "\n".join([
        "(fp_lib_table",
        "\t(version 7)",
        '\t(lib (name "kestrel")(type "KiCad")(uri "${KIPRJMOD}/lib/kestrel.pretty")(options "")(descr "Kestrel footprints"))',
        '\t(lib (name "kestrel_easyeda")(type "KiCad")(uri "${KIPRJMOD}/lib/easyeda/kestrel_easyeda.pretty")(options "")(descr "Footprints converted from EasyEDA"))',
        ")",
        "",
    ])
    for name, text in (("sym-lib-table", sym), ("fp-lib-table", fp)):
        with open(os.path.join(HW, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def fix_easyeda_model_path():
    """Point the thumbstick footprint's 3D model at a project-relative path."""
    path = os.path.join(LIB, "easyeda", "kestrel_easyeda.pretty", "SW-TH_RKJXV1224005.kicad_mod")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    start = text.find('(model "')
    if start < 0:
        return
    end = text.find('"', start + 8)
    wanted = "${KIPRJMOD}/lib/easyeda/kestrel_easyeda.3dshapes/SW-TH_RKJXV1224005.wrl"
    fixed = text[:start + 8] + wanted + text[end:]
    # The two locating pegs are plain holes. EasyEDA exports them as plated pads with no copper ring.
    fixed = fixed.replace('(pad "" thru_hole circle', '(pad "" np_thru_hole circle')
    if fixed != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(fixed)


def fix_easyeda_pin_types():
    """EasyEDA marks every thumbstick pin 'unspecified', which ERC warns about."""
    path = os.path.join(LIB, "easyeda", "kestrel_easyeda.kicad_sym")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    fixed = text.replace("(pin unspecified ", "(pin passive ")
    if fixed != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(fixed)


def write_all():
    os.makedirs(LIB, exist_ok=True)
    write_symbol_lib()
    write_footprint_lib()
    write_lib_tables()
    fix_easyeda_model_path()
    fix_easyeda_pin_types()


if __name__ == "__main__":
    write_all()
    print("libraries written to", LIB)
