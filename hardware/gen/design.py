"""Kestrel main board: parts, nets, and placement.

This module is the single source for the schematic and the PCB. Coordinates are
in the controller frame from shared/layout.json: millimetres, +X right, +Y away
from the player, seen from the face side. Rotation is counter-clockwise in that
view. Side "F" is the face side of the board and "B" is the battery side.
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

with open(os.path.join(ROOT, "shared", "layout.json"), encoding="utf-8") as f:
    LAYOUT = json.load(f)
with open(os.path.join(ROOT, "shared", "pinmap.json"), encoding="utf-8") as f:
    PINMAP = json.load(f)

NC = "__NC__"
CAP_GAP = 2.7

PASSIVE_FP = {
    ("R", "0402"): "Resistor_SMD:R_0402_1005Metric",
    ("R", "1206"): "Resistor_SMD:R_1206_3216Metric",
    ("C", "0402"): "Capacitor_SMD:C_0402_1005Metric",
    ("C", "0603"): "Capacitor_SMD:C_0603_1608Metric",
    ("C", "0805"): "Capacitor_SMD:C_0805_2012Metric",
}

POWER_NETS = [
    "VBUS", "VSYS", "VBAT", "VBAT_RAW", "3V3", "3V3_PERIPH", "AVDD", "VLED",
    "BB_L1", "BB_L2", "TRIG_L_M1", "TRIG_L_M2", "TRIG_R_M1", "TRIG_R_M2",
    "TRIG_L_ISEN_RAW", "TRIG_R_ISEN_RAW", "RUMBLE_L_D", "RUMBLE_R_D",
]


class Part:
    def __init__(self, ref, sym, value, fp, pins, at, side, block, mpn, anchor, horizontal):
        self.ref = ref
        self.sym = sym
        self.value = value
        self.fp = fp
        self.pins = pins
        self.at = at
        self.side = side
        self.block = block
        self.mpn = mpn
        self.anchor = anchor
        self.horizontal = horizontal


class Design:
    def __init__(self):
        self.parts = {}
        self._count = {}

    def add(self, ref, sym, value, fp, pins, at=None, side="F", block="misc",
            mpn="", anchor=(0.0, 0.0), horizontal=False):
        if ref in self.parts:
            raise ValueError("duplicate reference " + ref)
        self.parts[ref] = Part(ref, sym, value, fp, pins, at, side, block, mpn, anchor, horizontal)
        return ref

    def _auto(self, prefix):
        self._count[prefix] = self._count.get(prefix, 0) + 1
        return "%s%d" % (prefix, self._count[prefix])

    def R(self, value, a, b, at, side="B", block="misc", size="0402"):
        return self.add(self._auto("R"), "Device:R", value, PASSIVE_FP[("R", size)],
                        {"1": a, "2": b}, at, side, block, horizontal=True)

    def C(self, value, a, at, side="B", block="misc", size="0402", b="GND"):
        return self.add(self._auto("C"), "Device:C", value, PASSIVE_FP[("C", size)],
                        {"1": a, "2": b}, at, side, block, horizontal=True)

    def nets(self):
        """Map net name to a sorted list of (ref, pin key)."""
        out = {}
        for part in self.parts.values():
            for key, net in part.pins.items():
                if net != NC:
                    out.setdefault(net, []).append((part.ref, key))
        return out


def build():
    d = Design()
    lay = LAYOUT

    # ---- USB-C input and ESD protection (face side, behind the port) ----
    d.add("J1", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C",
          "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
          {"VBUS": "VBUS", "GND": "GND", "SHIELD": "GND", "CC1": "CC1", "CC2": "CC2",
           "D+": "USB_DP", "D-": "USB_DM", "SBU1": NC, "SBU2": NC},
          at=(0, lay["usb_c"]["edge_y"] - 3.2, 180), block="usb", mpn="TYPE-C-31-M-12")
    d.R("5.1k", "CC1", "GND", (-7.2, 35.5, 90), "F", "usb")
    d.R("5.1k", "CC2", "GND", (7.2, 35.5, 90), "F", "usb")
    d.add("U2", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6", "Package_TO_SOT_SMD:SOT-23-6",
          {"I/O1": "USB_DP", "I/O2": "USB_DM", "VBUS": "VBUS", "GND": "GND"},
          at=(0, 31.2, 0), block="usb", mpn="USBLC6-2SC6")
    d.C("10u 25V", "VBUS", (6.5, 31.5, 0), "F", "usb", "0805")
    d.add("FLG1", "power:PWR_FLAG", "PWR_FLAG", None, {"1": "VBUS"}, block="usb")
    d.add("FLG2", "power:PWR_FLAG", "PWR_FLAG", None, {"1": "GND"}, block="usb")

    # ---- Charger with power path ----
    d.add("U3", "Battery_Management:BQ24074RGT", "BQ24074",
          "Package_DFN_QFN:VQFN-16-1EP_3x3mm_P0.5mm_EP1.6x1.6mm",
          {"IN": "VBUS", "OUT": "VSYS", "BAT": "VBAT", "TS": "CHG_TS", "~{CE}": "GND",
           "EN2": "GND", "EN1": "CHG_EN1", "~{PGOOD}": "PGOOD_N", "~{CHG}": "CHG_N",
           "ILIM": "CHG_ILIM", "ISET": "CHG_ISET", "TMR": "CHG_TMR", "ITERM": NC, "VSS": "GND"},
          at=(13.5, 27, 0), block="charger", mpn="BQ24074RGTR")
    d.C("4.7u", "VBUS", (13.5, 31, 0), "F", "charger", "0603")
    d.C("4.7u", "VSYS", (18.2, 28, 90), "F", "charger", "0603")
    d.C("10u", "VSYS", (20.6, 28, 90), "F", "charger", "0805")
    d.C("4.7u", "VBAT", (9.4, 27, 90), "F", "charger", "0603")
    d.R("887", "CHG_ISET", "GND", (11.2, 23.4, 0), "F", "charger")
    d.R("1.13k", "CHG_ILIM", "GND", (13.5, 23.4, 0), "F", "charger")
    d.R("68k", "CHG_TMR", "GND", (15.8, 23.4, 0), "F", "charger")
    d.R("10k", "CHG_TS", "GND", (9.4, 24, 90), "F", "charger")
    # EN1 high with EN2 low selects the 500 mA input limit. The pull-up gives the pin
    # its own net, which routes out of the QFN far more easily than a power track.
    d.R("10k", "CHG_EN1", "VSYS", (13.5, 21.6, 0), "F", "charger")

    # ---- Battery connector, reverse-polarity FET, fuel gauge ----
    d.add("J2", "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "BAT",
          "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal",
          {"Pin_1": "VBAT_RAW", "Pin_2": "GND", "MountPin": "GND"},
          at=(-41, -5.5, 270), side="B", block="battery", mpn="S2B-PH-SM4-TB")
    d.add("Q1", "Transistor_FET:AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23",
          {"G": "GND", "S": "VBAT", "D": "VBAT_RAW"},
          at=(-33, -12, 0), side="B", block="battery", mpn="AO3401A")
    d.add("U4", "kestrel:MAX17048", "MAX17048", "Package_DFN_QFN:DFN-8-1EP_2x2mm_P0.5mm_EP0.8x1.6mm",
          {"CTG": "GND", "CELL": "VBAT", "VDD": "VBAT", "GND": "GND", "ALRT": "FUEL_ALRT_N",
           "QSTRT": "GND", "SCL": "I2C_SCL", "SDA": "I2C_SDA", "EP": "GND"},
          at=(23, 23, 0), block="battery", mpn="MAX17048G+T10")
    d.C("100n", "VBAT", (23, 20.5, 0), "F", "battery")
    d.R("4.7k", "I2C_SDA", "3V3", (23, 26.2, 0), "F", "battery")
    d.R("4.7k", "I2C_SCL", "3V3", (23, 27.6, 0), "F", "battery")
    d.R("10k", "FUEL_ALRT_N", "3V3", (23, 29, 0), "F", "battery")

    # ---- 3.3 V buck-boost ----
    d.add("U5", "Regulator_Switching:TPS63001", "TPS63001", "Package_SON:Texas_DRC0010J",
          {"VIN": "VSYS", "VINA": "VSYS", "EN": "VSYS", "PS/SYNC": "GND", "L1": "BB_L1",
           "L2": "BB_L2", "VOUT": "3V3", "FB": "3V3", "GND": "GND", "PGND": "GND"},
          at=(5, 13.5, 0), block="rails", mpn="TPS63001DRCR")
    d.add("L1", "Device:L", "2.2u", "Inductor_SMD:L_Bourns-SRN4018",
          {"1": "BB_L1", "2": "BB_L2"}, at=(-0.5, 13.5, 90), block="rails",
          mpn="SRN4018-2R2M", horizontal=True)
    d.C("10u", "VSYS", (5, 9.2, 0), "F", "rails", "0805")
    d.C("10u", "3V3", (3.2, 18, 0), "F", "rails", "0805")
    d.C("10u", "3V3", (7, 18, 0), "F", "rails", "0805")
    d.C("100n", "VSYS", (9, 13.5, 90), "F", "rails")

    # ---- Switched rails: 3V3_PERIPH, VLED, and the 3.0 V analogue supply ----
    d.add("U6", "Power_Management:TPS22917DBV", "TPS22917", "Package_TO_SOT_SMD:SOT-23-6",
          {"VIN": "3V3", "ON": "PERIPH_EN", "VOUT": "3V3_PERIPH", "QOD": "SWP_QOD",
           "CT": "SWP_CT", "GND": "GND"},
          at=(-5.5, 8.5, 0), block="rails", mpn="TPS22917DBVR")
    d.R("220", "SWP_QOD", "3V3_PERIPH", (-6.5, 11.5, 0), "F", "rails")
    d.C("1u", "3V3", (-9, 8, 90), "F", "rails")
    d.C("1u", "3V3_PERIPH", (-2, 8, 90), "F", "rails")
    d.C("1n", "SWP_CT", (-5.5, 5.6, 0), "F", "rails")
    d.add("U7", "Power_Management:TPS22917DBV", "TPS22917", "Package_TO_SOT_SMD:SOT-23-6",
          {"VIN": "VSYS", "ON": "PERIPH_EN", "VOUT": "VLED", "QOD": "SWL_QOD",
           "CT": "SWL_CT", "GND": "GND"},
          at=(13, 8.5, 0), block="rails", mpn="TPS22917DBVR")
    d.R("220", "SWL_QOD", "VLED", (13, 11.5, 0), "F", "rails")
    d.C("1u", "VSYS", (9.6, 8, 90), "F", "rails")
    d.C("10u", "VLED", (17.6, 8.5, 90), "F", "rails", "0805")
    d.C("1n", "SWL_CT", (13, 5.6, 0), "F", "rails")
    d.add("U8", "Regulator_Linear:TLV70030_SOT23-5", "TLV70030", "Package_TO_SOT_SMD:SOT-23-5",
          {"IN": "VSYS", "EN": "PERIPH_EN", "OUT": "AVDD", "NC": NC, "GND": "GND"},
          at=(14, 16, 0), block="rails", mpn="TLV70030DDCR")
    d.C("1u", "VSYS", (11, 17, 90), "F", "rails")
    d.C("1u", "AVDD", (17.5, 16, 90), "F", "rails")

    # ---- MCU module (battery side, antenna at the rear edge) ----
    gpio = {k: v["net"] for k, v in PINMAP["gpio"].items()}
    mcu = {"3V3": "3V3", "GND": "GND", "EN": "EN", "USB_D-": gpio["19"], "USB_D+": gpio["20"],
           "TXD0": gpio["43"], "RXD0": gpio["44"]}
    for n in list(range(0, 19)) + [21] + list(range(35, 43)) + [45, 46, 47, 48]:
        mcu["IO%d" % n] = gpio[str(n)]
    esp = lay["esp32_module"]
    d.add("U1", "RF_Module:ESP32-S3-WROOM-1", "ESP32-S3-WROOM-1-N16R2", "RF_Module:ESP32-S3-WROOM-1",
          mcu, at=(esp["centre"][0], esp["centre"][1], 0), side="B", block="mcu",
          mpn="ESP32-S3-WROOM-1-N16R2")
    d.C("22u", "3V3", (-13.5, -31.2, 90), "B", "mcu", "0805")
    d.C("100n", "3V3", (-15.7, -31.2, 90), "B", "mcu")
    d.R("10k", "EN", "3V3", (-13.4, -27.0, 0), "B", "mcu")
    d.C("1u", "EN", (-13.4, -25.7, 0), "B", "mcu")
    d.R("10k", "BOOT", "3V3", (10.6, -16, 0), "B", "mcu")
    d.R("10k", "BTN_HOME", "3V3", (-13.4, -17.9, 0), "B", "mcu")
    d.R("100k", "PERIPH_EN", "GND", (5.5, -12.6, 0), "B", "mcu")
    d.R("100k", "MOTOR_NSLEEP", "GND", (-9, -12.6, 0), "B", "mcu")
    d.add("SW13", "Switch:SW_Push", "BOOT", "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A",
          {"1": "BOOT", "2": "GND"}, at=(-45, -30, 0), side="B", block="mcu", mpn="TS-1187A-B-A-B")
    d.add("SW14", "Switch:SW_Push", "RESET", "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A",
          {"1": "EN", "2": "GND"}, at=(-37, -29, 0), side="B", block="mcu", mpn="TS-1187A-B-A-B")
    d.add("J10", "Connector_Generic:Conn_01x06", "DEBUG",
          "Connector_PinHeader_1.27mm:PinHeader_1x06_P1.27mm_Vertical",
          {"Pin_1": "3V3", "Pin_2": "GND", "Pin_3": "UART0_TX", "Pin_4": "UART0_RX",
           "Pin_5": "EN", "Pin_6": "BOOT"},
          at=(-36.5, -37, 270), side="B", block="mcu")
    d.add("TP1", "Connector:TestPoint", "GPIO3", "TestPoint:TestPoint_Pad_D1.0mm",
          {"1": "TP_GPIO3"}, at=(-10.5, -11, 0), side="B", block="mcu")

    # ---- Analogue input filters next to the module ----
    rows = [("STICK_RX", -24.4), ("STICK_RY", -23.1), ("TRIG_L_POS", -21.8),
            ("TRIG_R_POS", -20.5)]
    for net, y in rows:
        d.R("1k", net + "_RAW", net, (-13.4, y, 0), "B", "analog")
        d.C("100n", net, (-15.7, y, 0), "B", "analog")
    # The current-sense filter resistors sit beside the sense resistors, so the
    # long run to the module carries no motor current.
    d.R("1k", "TRIG_L_ISEN_RAW", "TRIG_L_ISEN", (-10.6, 31.5, 90), "B", "analog")
    d.C("100n", "TRIG_L_ISEN", (-15.7, -19.2, 0), "B", "analog")
    d.R("1k", "TRIG_R_ISEN_RAW", "TRIG_R_ISEN", (10.6, 31.5, 90), "B", "analog")
    d.C("100n", "TRIG_R_ISEN", (-6, -11.3, 0), "B", "analog")
    for net, y in (("STICK_LY", -30.0), ("STICK_LX", -31.3)):
        d.R("1k", net + "_RAW", net, (10.6, y, 0), "B", "analog")
        d.C("100n", net, (12.9, y, 0), "B", "analog")

    # ---- Thumbsticks with supply-polarity jumpers ----
    stick_fp = "kestrel_easyeda:SW-TH_RKJXV1224005"
    stick_axis = (-1.205, -0.78)
    for tag, key, l3, jp, cap_at, jp_at in (
            ("L", "left", "BTN_L3", ("JP1", "JP2"), ((-33, 13.2), (-33, 10.6)), ((-57, 27.6), (-57, 30.6))),
            ("R", "right", "BTN_R3", ("JP3", "JP4"), ((14.8, -11.3), (14.8, -13.4)), ((40, -15), (40, -18.5)))):
        x, y = lay["sticks"][key]
        a_net, b_net = "STICK_%s_A" % tag, "STICK_%s_B" % tag
        d.add("U17" if tag == "L" else "U18", "kestrel_easyeda:RKJXV1224005",
              "TMR stick", stick_fp,
              {"1": b_net, "2": "STICK_%sY_RAW" % tag, "3": a_net,
               "4": a_net, "5": "STICK_%sX_RAW" % tag, "6": b_net,
               "7": "GND", "8": "GND", "9": "GND", "10": "GND",
               "a": l3, "b": NC, "c": NC, "d": "GND"},
              at=(x, y, 0), block="sticks", anchor=stick_axis,
              mpn="TMR module, RKJXV1224005 footprint")
        d.add(jp[0], "Jumper:SolderJumper_3_Bridged12", "A=AVDD",
              "Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm",
              {"A": "AVDD", "C": a_net, "B": "GND"}, at=(jp_at[0][0], jp_at[0][1], 0),
              side="B", block="sticks")
        d.add(jp[1], "Jumper:SolderJumper_3_Bridged12", "B=GND",
              "Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm",
              {"A": "GND", "C": b_net, "B": "AVDD"}, at=(jp_at[1][0], jp_at[1][1], 0),
              side="B", block="sticks")
        d.C("1u", "AVDD", (cap_at[0][0], cap_at[0][1], 90), "B", "sticks")
        d.C("100n", "AVDD", (cap_at[1][0], cap_at[1][1], 90), "B", "sticks")

    # ---- Trigger Hall sensors ----
    hall = lay["triggers"]["hall_sensor_bottom_side"]
    for ref, tag, key in (("U12", "L", "LT"), ("U13", "R", "RT")):
        x, y = hall[key]
        d.add(ref, "Sensor_Magnetic:DRV5055A4xDBZxQ1", "DRV5055A4", "Package_TO_SOT_SMD:SOT-23",
              {"VCC": "3V3_PERIPH", "OUT": "TRIG_%s_POS_RAW" % tag, "GND": "GND"},
              at=(x, y, 0), side="B", block="triggers", mpn="DRV5055A4QDBZR")
        d.C("100n", "3V3_PERIPH", (x, y - 3.4, 0), "B", "triggers")

    # ---- Trigger motor driver ----
    d.add("U14", "Driver_Motor:DRV8833PWP", "DRV8833",
          "Package_SO:HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3.4x5mm_Mask2.46x2.31mm",
          {"~{SLEEP}": "MOTOR_NSLEEP", "AIN1": "TRIG_L_IN1", "AIN2": "TRIG_L_IN2",
           "BIN1": "TRIG_R_IN1", "BIN2": "TRIG_R_IN2", "AOUT1": "TRIG_L_M1", "AOUT2": "TRIG_L_M2",
           "BOUT1": "TRIG_R_M1", "BOUT2": "TRIG_R_M2", "AISEN": "TRIG_L_ISEN_RAW",
           "BISEN": "TRIG_R_ISEN_RAW", "~{FAULT}": "MOTOR_FAULT_N", "VCP": "DRV_VCP",
           "VM": "VSYS", "VINT": "DRV_VINT", "GND": "GND"},
          at=(0, 30, 270), side="B", block="triggers", mpn="DRV8833PWPR")
    d.R("0.2 1%", "TRIG_L_ISEN_RAW", "GND", (-6.5, 31.5, 0), "B", "triggers", "1206")
    d.R("0.2 1%", "TRIG_R_ISEN_RAW", "GND", (6.5, 31.5, 0), "B", "triggers", "1206")
    d.C("10u", "VSYS", (0, 24.6, 0), "B", "triggers", "0805")
    d.C("100n", "VSYS", (4, 24.6, 0), "B", "triggers")
    d.C("2.2u", "DRV_VINT", (-4.3, 24.6, 0), "B", "triggers", "0603")
    # VINT is the driver's internal regulator output. The flag tells ERC it is driven.
    d.add("FLG3", "power:PWR_FLAG", "PWR_FLAG", None, {"1": "DRV_VINT"}, block="triggers")
    d.C("10n", "DRV_VCP", (6.2, 24.6, 0), "B", "triggers", b="VSYS")
    d.C("22u", "VSYS", (-12, 26, 0), "B", "triggers", "0805")
    for ref, tag, x in (("J3", "L", -30), ("J4", "R", 30)):
        d.add(ref, "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "TRIG_" + tag,
              "Connector_JST:JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal",
              {"Pin_1": "TRIG_%s_M1" % tag, "Pin_2": "TRIG_%s_M2" % tag, "MountPin": "GND"},
              at=(x, 34, 0), side="B", block="triggers", mpn="SM02B-SRSS-TB")

    # ---- Rumble drivers and paddle connectors ----
    for tag, sx, q, dref, jr, jp, p1, p2 in (
            ("L", -1, "Q2", "D25", "J5", "J7", "PADDLE_1", "PADDLE_2"),
            ("R", 1, "Q3", "D26", "J6", "J8", "PADDLE_3", "PADDLE_4")):
        drain = "RUMBLE_%s_D" % tag
        d.add(q, "Transistor_FET:AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23",
              {"G": "RUMBLE_" + tag, "S": "GND", "D": drain},
              at=(sx * 59.5, -10.5, 0), side="B", block="rumble", mpn="AO3400A")
        d.add(dref, "Device:D_Schottky", "B5819W", "Diode_SMD:D_SOD-123",
              {"K": "VLED", "A": drain}, at=(sx * 54.5, -14, 0), side="B", block="rumble",
              mpn="B5819W", horizontal=True)
        d.R("100k", "RUMBLE_" + tag, "GND", (sx * 59.5, -13.6, 0), "B", "rumble")
        d.C("22u", "VLED", (sx * 60, 2, 0), "B", "rumble", "0805")
        d.add(jr, "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "RUMBLE_" + tag,
              "Connector_JST:JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal",
              {"Pin_1": "VLED", "Pin_2": drain, "MountPin": "GND"},
              at=(sx * 60, -3.5, 90 if sx < 0 else 270), side="B", block="rumble", mpn="SM02B-SRSS-TB")
        d.add(jp, "Connector_Generic_MountingPin:Conn_01x03_MountingPin", "PADDLE_" + tag,
              "Connector_JST:JST_SH_SM03B-SRSS-TB_1x03-1MP_P1.00mm_Horizontal",
              {"Pin_1": p1, "Pin_2": p2, "Pin_3": "GND", "MountPin": "GND"},
              at=(sx * 51.5, -3.5, 180), side="B", block="rumble", mpn="SM03B-SRSS-TB")

    # ---- Shift registers with pull-up arrays ----
    sr = PINMAP["shift_registers"]
    chain = (("U9", "SR1", "SR_DATA", "SR_Q1", (44, 14), ("RN1", "RN2")),
             ("U10", "SR2", "SR_Q1", "SR_Q2", (-22, -6), ("RN3", "RN4")),
             ("U11", "SR3", "SR_Q2", "GND", (22, 4), ("RN5", "RN6")))
    for ref, key, q7, ds, (x, y), packs in chain:
        inputs = [sr[key][letter] for letter in "ABCDEFGH"]
        pins = {"~{PL}": "SR_LOAD", "CP": "SR_CLK", "~{CE}": "GND", "VCC": "3V3_PERIPH",
                "GND": "GND", "Q7": q7, "~{Q7}": NC, "DS": ds}
        for i, net in enumerate(inputs):
            pins["D%d" % i] = net
        d.add(ref, "74xx:74HC165", "74HC165", "Package_SO:TSSOP-16_4.4x5mm_P0.65mm",
              pins, at=(x, y, 0), side="B", block="inputs", mpn="SN74HC165PWR")
        d.C("100n", "3V3_PERIPH", (x, y + 4.2, 0), "B", "inputs")
        for pack, group, px in ((packs[0], inputs[4:], x - 5.5), (packs[1], inputs[:4], x + 5.5)):
            d.add(pack, "Device:R_Pack04", "10k", "Resistor_SMD:R_Array_Convex_4x0603",
                  {"R1.1": "3V3_PERIPH", "R2.1": "3V3_PERIPH", "R3.1": "3V3_PERIPH",
                   "R4.1": "3V3_PERIPH", "R1.2": group[0], "R2.2": group[1],
                   "R3.2": group[2], "R4.2": group[3]},
                  at=(px, y, 0), side="B", block="inputs", mpn="YC164-JR-0710KL")

    # ---- Face buttons, D-pad, and system buttons ----
    tact = "Button_Switch_SMD:SW_SPST_PTS645Sx43SMTR92"
    ab, dp, sysb = lay["abxy"], lay["dpad"], lay["system_buttons"]
    buttons = [("SW1", "BTN_A", ab["A"], 0), ("SW2", "BTN_B", ab["B"], 90),
               ("SW3", "BTN_X", ab["X"], 90), ("SW4", "BTN_Y", ab["Y"], 0),
               ("SW5", "DPAD_UP", dp["UP"], 90), ("SW6", "DPAD_DOWN", dp["DOWN"], 90),
               ("SW7", "DPAD_LEFT", dp["LEFT"], 0), ("SW8", "DPAD_RIGHT", dp["RIGHT"], 0),
               ("SW9", "BTN_VIEW", sysb["VIEW"], 0), ("SW10", "BTN_HOME", sysb["HOME"], 0),
               ("SW11", "BTN_MENU", sysb["MENU"], 0), ("SW12", "BTN_FN", sysb["FN"], 0)]
    for ref, net, (x, y), rot in buttons:
        d.add(ref, "Switch:SW_Push", net.split("_", 1)[1], tact, {"1": net, "2": "GND"},
              at=(x, y, rot), block="buttons", mpn="PTS645SM43SMTR92")

    # ---- Bumper micro switches ----
    for ref, net, key in (("SW15", "BTN_LB", "LB"), ("SW16", "BTN_RB", "RB")):
        x, y = lay["bumpers"][key]
        d.add(ref, "Switch:SW_SPDT", key, "kestrel:SW_D2FC_THT",
              {"A": "GND", "B": net, "C": net + "_ALT"},
              at=(x, y, 0), block="buttons", mpn="D2FC-F-7N")

    # ---- LED chain with level shifter ----
    d.add("U16", "Logic_LevelTranslator:SN74LVC1T45DBV", "SN74LVC1T45", "Package_TO_SOT_SMD:SOT-23-6",
          {"VCCA": "3V3_PERIPH", "VCCB": "VLED", "A": "LED_DATA", "B": "LED_DATA_5V",
           "DIR": "3V3_PERIPH", "GND": "GND"},
          at=(-19.5, -17, 90), side="B", block="leds", mpn="SN74LVC1T45DBVR")
    d.C("100n", "3V3_PERIPH", (-22.6, -18, 90), "B", "leds")
    d.C("100n", "VLED", (-22.6, -15.6, 90), "B", "leds")
    d.R("330", "LED_DATA_5V", "LED_D0", (-19.5, -13.2, 0), "B", "leds")
    chain_leds = lay["leds"]["chain"]
    for led in chain_leds:
        i, name, (x, y) = led["index"], led["name"], led["xy"]
        rot, cx, cy, crot = led_orientation(name, x, y, lay)
        d.add("D%d" % (i + 1), "LED:SK6812MINI", name, "LED_SMD:LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm",
              {"VDD": "VLED", "VSS": "GND", "DIN": "LED_D%d" % i,
               "DOUT": "LED_D%d" % (i + 1) if i + 1 < len(chain_leds) else NC},
              at=(x, y, rot), block="leds", mpn="SK6812MINI")
        d.C("100n", "VLED", (cx, cy, crot), "F", "leds")

    # ---- Display module connector ----
    d.add("J9", "Connector_Generic_MountingPin:Conn_01x08_MountingPin", "LCD",
          "Connector_JST:JST_GH_SM08B-GHS-TB_1x08-1MP_P1.25mm_Horizontal",
          {"Pin_1": "3V3_PERIPH", "Pin_2": "GND", "Pin_3": "LCD_MOSI", "Pin_4": "LCD_SCLK",
           "Pin_5": "LCD_CS", "Pin_6": "LCD_DC", "Pin_7": "LCD_RST", "Pin_8": "LCD_BL",
           "MountPin": "GND"},
          at=(-14, 18.5, 270), block="display", mpn="SM08B-GHS-TB")
    d.C("10u", "3V3_PERIPH", (-20, 9, 0), "F", "display", "0805")
    d.C("100n", "3V3_PERIPH", (-20, 7, 0), "F", "display")
    return d


def led_orientation(name, x, y, lay):
    """Rotation of an LED and the position of its decoupling capacitor.

    The capacitor sits CAP_GAP from the LED centre on the side facing away from
    the switch or stick the LED belongs to. Ring LEDs lie tangent to the ring.
    """
    if name.startswith(("LRING", "RRING")):
        cx0, cy0 = lay["sticks"]["left" if name[0] == "L" else "right"]
        deg = int(name.split("_")[1])
        ang = math.radians(deg)
        rot = (deg + 90) % 180
        if name == "RRING_270":
            # The board edge is too close for the usual outboard position.
            return rot, x + 4.2, y, 90
        r = lay["leds"]["ring_led_radius"] + CAP_GAP
        return rot, cx0 + r * math.cos(ang), cy0 + r * math.sin(ang), rot
    if name in ("X", "B"):
        away = 1 if name == "X" else -1
        return 90, x + CAP_GAP * away, y, 90
    if name in ("A", "Y"):
        away = 1 if name == "A" else -1
        return 0, x, y + CAP_GAP * away, 0
    if name.startswith("DPAD"):
        away = 1 if name in ("DPAD_NE", "DPAD_NW") else -1
        return 0, x, y + CAP_GAP * away, 0
    if name == "FN":
        return 0, x, y + CAP_GAP, 0
    if name == "MENU":
        # The usual spot below the LED runs into the right stick's ring LED.
        return 0, x - 4.0, y, 90
    return 0, x, y - CAP_GAP, 0


if __name__ == "__main__":
    design = build()
    nets = design.nets()
    print("parts:", len(design.parts), "nets:", len(nets))
    for name in sorted(nets):
        if len(nets[name]) == 1:
            print("single-pin net:", name, nets[name])
