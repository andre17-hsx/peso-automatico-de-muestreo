# Genera el esquema (KiCad 10) de la placa de la caja: caja_balanza.kicad_sch
# Ejecutar con el Python de KiCad:
#   "C:\Program Files\KiCad\10.0\bin\python.exe" gen_sch.py          (v1: caja_balanza.kicad_sch)
#   "C:\Program Files\KiCad\10.0\bin\python.exe" gen_sch.py --v2     (v2/caja_balanza_v2.kicad_sch)
#   "C:\Program Files\KiCad\10.0\bin\python.exe" gen_sch.py --v3     (v3/caja_balanza_v3.kicad_sch)
# La v2 y la v3 llevan el modulo TPS63020 en la placa (U2) en lugar de la bornera J3.
# Coordenadas en mm sobre hoja A3 apaisada; rejilla de 1,27 mm (pines en multiplos de 2,54).
import math
import os
import sys

import kisexpr as K
from caja_parts import PARTS, PARTS_V2, PARTS_V3, any_uuid, datasheet, sym_uuid

VER = "v2" if "--v2" in sys.argv else "v3" if "--v3" in sys.argv else "v1"
TPS_EN_PLACA = VER != "v1"
HERE = os.path.dirname(os.path.abspath(__file__))
# revision, medidas de la placa y generador de la placa de cada version
VINFO = {"v1": ("1.0", "70 × 50 mm", "gen_pcb.py"),
         "v2": ("2.0", "96 × 53 mm", "gen_pcb_v2.py"),
         "v3": ("3.0", "70 × 50 mm", "gen_pcb_v3.py")}[VER]
if TPS_EN_PLACA:
    PARTS = PARTS_V2 if VER == "v2" else PARTS_V3
    LIBDIR = os.path.join(HERE, VER)
    PROJECT = "caja_balanza_" + VER
else:
    LIBDIR = HERE
    PROJECT = "caja_balanza"
OUT = os.path.join(LIBDIR, PROJECT + ".kicad_sch")
ROOT = any_uuid("root-sheet")

# ---------------------------------------------------------------- simbolo propio de la ESP
LEFT = [("43", "TX/IO43"), ("44", "RX/IO44"), ("1", "IO1"), ("2", "IO2"), ("3", "IO3"),
        ("4", "IO4"), ("5", "IO5"), ("6", "IO6"), ("7", "IO7")]
RIGHT = [("5V", "5V"), ("GND", "GND"), ("3V3", "3V3"), ("13", "IO13"), ("12", "IO12"),
         ("11", "IO11"), ("10", "IO10"), ("9", "IO9"), ("8", "IO8")]


def esp_symbol_text(name):
    def pin(num, nm, x, y, ang):
        typ = "power_in" if nm in ("GND", "3V3", "5V") else "bidirectional"
        return ('(pin %s line (at %.2f %.2f %d) (length 2.54) (name "%s" (effects (font (size 1.27 1.27)))) '
                '(number "%s" (effects (font (size 1.27 1.27)))))' % (typ, x, y, ang, nm, num))
    pins = [pin(n, nm, -12.7, 10.16 - 2.54 * i, 0) for i, (n, nm) in enumerate(LEFT)]
    pins += [pin(n, nm, 12.7, 10.16 - 2.54 * i, 180) for i, (n, nm) in enumerate(RIGHT)]
    short = name.split(":")[-1]
    return ('(symbol "%s" (pin_numbers (hide yes)) (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes) '
            '(property "Reference" "U" (at 0 13.97 0) (effects (font (size 1.27 1.27)))) '
            '(property "Value" "ESP32-S3 Super Mini" (at 0 -13.97 0) (effects (font (size 1.27 1.27)))) '
            '(property "Footprint" "caja_balanza:ESP32-S3_SuperMini_Socket" (at 0 -16.51 0) (hide yes) '
            '(effects (font (size 1.27 1.27)))) '
            '(property "Datasheet" "https://www.espboards.dev/esp32/esp32-s3-super-mini/" (at 0 0 0) (hide yes) '
            '(effects (font (size 1.27 1.27)))) '
            '(property "Description" "ESP32-S3 Super Mini module (18 x 22.5 mm), 2 rows of 9 pins, 15.24 mm apart" '
            '(at 0 0 0) (hide yes) (effects (font (size 1.27 1.27)))) '
            '(symbol "%s_0_1" (rectangle (start -10.16 12.7) (end 10.16 -12.7) (stroke (width 0.254) (type default)) '
            '(fill (type background)))) '
            '(symbol "%s_1_1" %s))' % (name, short, short, " ".join(pins)))


def tps_symbol_text(name):
    """Modulo TPS63020 (26 x 18 mm): VIN y GND de entrada a un lado, VOUT y GND de salida al otro."""
    def pin(typ, num, nm, x, y, ang):
        return ('(pin %s line (at %.2f %.2f %d) (length 2.54) (name "%s" (effects (font (size 1.27 1.27)))) '
                '(number "%s" (effects (font (size 1.27 1.27)))))' % (typ, x, y, ang, nm, num))
    pins = [pin("power_in", "1", "VIN", -10.16, 0, 0), pin("power_out", "4", "VOUT", 10.16, 0, 180),
            pin("power_in", "2", "GND_IN", -2.54, -7.62, 90), pin("power_in", "3", "GND_OUT", 2.54, -7.62, 90)]
    short = name.split(":")[-1]
    return ('(symbol "%s" (pin_numbers (hide yes)) (pin_names (offset 0.762)) (exclude_from_sim no) (in_bom yes) (on_board yes) '
            '(property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27)))) '
            '(property "Value" "TPS63020 module" (at 0 -10.16 0) (effects (font (size 1.27 1.27)))) '
            '(property "Footprint" "caja_balanza:TPS63020_Module_26x18" (at 0 -12.7 0) (hide yes) '
            '(effects (font (size 1.27 1.27)))) '
            '(property "Datasheet" "https://www.ti.com/lit/ds/symlink/tps63020.pdf" (at 0 0 0) (hide yes) '
            '(effects (font (size 1.27 1.27)))) '
            '(property "Description" "TPS63020 buck-boost module 26 x 18 mm (3V3 output set by jumper); EN and PS handled on the module" '
            '(at 0 0 0) (hide yes) (effects (font (size 1.27 1.27)))) '
            '(symbol "%s_0_1" (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) '
            '(fill (type background)))) '
            '(symbol "%s_1_1" %s))' % (name, short, short, " ".join(pins)))


ESP_LIB_ID = "caja_balanza:ESP32-S3_SuperMini"
TPS_LIB_ID = "caja_balanza:TPS63020_Module"
LIBSYMS = {ESP_LIB_ID: K.parse(esp_symbol_text(ESP_LIB_ID))}
if TPS_EN_PLACA:
    LIBSYMS[TPS_LIB_ID] = K.parse(tps_symbol_text(TPS_LIB_ID))


def libsym(lib_id):
    if lib_id not in LIBSYMS:
        LIBSYMS[lib_id] = K.symbol(lib_id)
    return LIBSYMS[lib_id]


# ---------------------------------------------------------------- objetos de la hoja
symbols, wires, labels, glabels, noconn, texts, rects = [], [], [], [], [], [], []
pinpts = []           # (x, y) de todos los pines colocados
power_n = [0, 0]
OFF = [0.0, 0.0]      # desplazamiento del bloque actual: se dibuja en coordenadas locales


def r2(v):
    return round(v + 0.0, 3)


def at(p):
    return (r2(p[0] + OFF[0]), r2(p[1] + OFF[1]))


class Sym:
    def __init__(self, ref, lib_id, x, y, rot=0, mirror=False, value=None, fields=None):
        self.ref, self.lib_id, self.x, self.y, self.rot, self.mirror = ref, lib_id, x, y, rot, mirror
        self.ox, self.oy = OFF
        sym = libsym(lib_id)
        self.libpins = K.pins(sym)
        if ref in PARTS:
            self.value, self.fp = PARTS[ref]
        else:
            self.value, self.fp = value, ""
        if value is not None:
            self.value = value
        self.fields = fields or {}
        for n in self.libpins:
            pinpts.append(at(self.pin(n)))
        symbols.append(self)

    def pin(self, num):
        """Posicion del pin en coordenadas locales del bloque."""
        px, py, _ = self.libpins[str(num)]
        if self.mirror:
            px = -px
        c, s = math.cos(math.radians(self.rot)), math.sin(math.radians(self.rot))
        rx, ry = px * c - py * s, px * s + py * c
        return (r2(self.x + rx), r2(self.y - ry))


def fld(dx, dy, just=None, hide=False):
    return (dx, dy, just, hide)


# posiciones tipicas de Referencia / Valor
V2 = {"Reference": fld(2.54, -1.27, "left"), "Value": fld(2.54, 1.27, "left")}          # 2 pines vertical
H2 = {"Reference": fld(0, -2.54), "Value": fld(0, 2.54)}                               # 2 pines horizontal
QF = {"Reference": fld(5.08, -1.27, "left"), "Value": fld(5.08, 1.27, "left")}


def part(ref, lib_id, x, y, rot=0, mirror=False, fields=None):
    return Sym(ref, lib_id, x, y, rot, mirror, fields=fields)


def power(lib_id, x, y, value=None, rot=0):
    """Simbolo de alimentacion (GND, +3V3, VBAT, +V_VENT) o PWR_FLAG."""
    flag = lib_id.endswith("PWR_FLAG")
    power_n[1 if flag else 0] += 1
    ref = ("#FLG%02d" if flag else "#PWR%02d") % power_n[1 if flag else 0]
    val = value or lib_id.split(":")[1]
    if lib_id.endswith(":GND"):
        vf = {0: fld(0, 3.81), 90: fld(3.81, 0, "left"), 270: fld(-3.81, 0, "right")}[rot]
    else:
        vf = {0: fld(0, -3.81), 180: fld(0, 3.81), 270: fld(3.81, 0, "left"), 90: fld(-3.81, 0, "right")}[rot]
    return Sym(ref, lib_id, x, y, rot, value=val,
               fields={"Reference": fld(0, 0, hide=True), "Value": vf})


def W(*pts):
    for a, b in zip(pts, pts[1:]):
        wires.append((at(a), at(b)))


def label(name, x, y, ang=0):
    labels.append((name,) + at((x, y)) + (ang,))


def glabel(name, x, y, ang=0, shape="bidirectional"):
    glabels.append((name,) + at((x, y)) + (ang, shape))


def nc(p):
    noconn.append(at(p))


def text(s, x, y, size=1.27, bold=False, just="left"):
    texts.append((s,) + at((x, y)) + (size, bold, just))


def block(x0, y0, x1, y1, title):
    """Recuadro de bloque en coordenadas absolutas de la hoja."""
    rects.append((x0, y0, x1, y1))
    texts.append((title, x0 + 2.54, y0 + 4.445, 2.0, True, "left"))


# Hoja A4 apaisada: fila 1 = alimentacion | ESP | mecanica; fila 2 = ventiladores | sniffer;
# fila 3 = notas (el cajetin ocupa la esquina inferior derecha).

# ================================================================= 1 · alimentacion
block(12.7, 12.7, 152.4, 87.63, "1 · BATTERY, SWITCH AND 3.3 V REGULATOR")
OFF[:] = [0, -2.54]
RAIL = 45.72
j1 = part("J1", "Connector:Screw_Terminal_01x02", 27.94, RAIL, mirror=True,
          fields={"Reference": fld(-1.27, -3.81, "right"), "Value": fld(-1.27, 6.35, "right")})
f1 = part("F1", "Device:Polyfuse", 45.72, RAIL, rot=90, fields=H2)
W(j1.pin(1), f1.pin(1)); label("BAT_IN", 35.56, RAIL)
W(j1.pin(2), (35.56, j1.pin(2)[1]), (35.56, 58.42)); power("power:GND", 35.56, 58.42)
W((35.56, 55.88), (40.64, 55.88)); power("power:PWR_FLAG", 40.64, 55.88, rot=180)
j2 = part("J2", "Connector:Screw_Terminal_01x02", 60.96, 35.56, rot=90,
          fields={"Reference": fld(-3.81, -2.54, "right"), "Value": fld(-3.81, 0, "right")})
W(f1.pin(2), (j2.pin(1)[0], RAIL), j2.pin(1)); label("BAT_F", 50.8, RAIL)
if TPS_EN_PLACA:
    u2 = Sym("U2", TPS_LIB_ID, 111.76, RAIL,
             fields={"Reference": fld(0, -8.89), "Value": fld(0, 15.24)})
    x_vin = u2.pin("1")[0]
    W(j2.pin(2), (j2.pin(2)[0], RAIL), u2.pin("1"))
else:
    j3 = part("J3", "Connector:Screw_Terminal_01x04", 104.14, 35.56, rot=90,
              fields={"Reference": fld(-6.35, -2.54, "right"), "Value": fld(-6.35, 0, "right")})
    x_vin = j3.pin(1)[0]
    W(j2.pin(2), (j2.pin(2)[0], RAIL), (x_vin, RAIL), j3.pin(1))
power("power:VCC", 66.04, RAIL, "VBAT")
power("power:PWR_FLAG", 71.12, RAIL, rot=180)
for ref, lib_id, x in (("C1", "Device:C_Polarized", 81.28), ("C8", "Device:C", 96.52)):
    c = part(ref, lib_id, x, 52.07, fields=V2)
    W((x, RAIL), c.pin(1)); power("power:GND", *c.pin(2))
if TPS_EN_PLACA:
    for n in ("2", "3"):
        W(u2.pin(n), (u2.pin(n)[0], RAIL + 10.16)); power("power:GND", u2.pin(n)[0], RAIL + 10.16)
    W(u2.pin("4"), (144.78, RAIL))
    power("power:+3V3", 124.46, RAIL)              # VOUT es salida de potencia: no lleva PWR_FLAG
else:
    W(j3.pin(2), (j3.pin(2)[0], 48.26), (j3.pin(3)[0], 48.26))
    W(j3.pin(3), (j3.pin(3)[0], 50.8)); power("power:GND", j3.pin(3)[0], 50.8)
    x3 = j3.pin(4)[0]
    W(j3.pin(4), (x3, RAIL), (144.78, RAIL))
    power("power:+3V3", 111.76, RAIL)
    power("power:PWR_FLAG", 116.84, RAIL, rot=180)
for ref, lib_id, x in (("C2", "Device:C_Polarized", 129.54), ("C3", "Device:C", 144.78)):
    c = part(ref, lib_id, x, 52.07, fields=V2)
    W((x, RAIL), c.pin(1)); power("power:GND", *c.pin(2))
text("J2 → panel switch", 48.26, 27.94)
if TPS_EN_PLACA:
    text("U2: TPS63020 module on the board", 93.98, 27.94)
    text("(output jumper on 3V3; EN and PS on the module)", 93.98, 30.48)
    text("F1 protects the whole board. C1/C8: U2 input.", 15.24, 68.58)
else:
    text("J3 → TPS63020 module (external)", 86.36, 27.94)
    text("1 VIN · 2 GND · 3 GND · 4 3V3", 86.36, 30.48)
    text("F1 protects the whole board. C1/C8: TPS63020 input.", 15.24, 68.58)
text("C2/C3 next to the 3V3 pin of U1 (WiFi peaks of ~0.45 A).", 15.24, 71.12)
text("TPS63020: VIN 1.8–5.5 V. With the scale charger connected, BAT must stay < 5.5 V.", 15.24, 73.66)

# ================================================================= 2 · ESP32-S3
block(157.48, 12.7, 233.68, 87.63, "2 · ESP32-S3 SUPER MINI (IN SOCKET)")
OFF[:] = [-3.81, -2.54]
u1 = Sym("U1", ESP_LIB_ID, 203.2, 48.26,
         fields={"Reference": fld(0, -15.24), "Value": fld(0, 15.24)})
for num, net_ in (("1", "GPIO1"), ("4", "GPIO4"), ("5", "GPIO5"), ("6", "GPIO6")):
    p = u1.pin(num)
    W(p, (180.34, p[1])); glabel(net_, 180.34, p[1], 180)
pg, p3 = u1.pin("GND"), u1.pin("3V3")
W(pg, (220.98, pg[1])); power("power:GND", 220.98, pg[1], rot=90)
W(p3, (220.98, p3[1])); power("power:+3V3", 220.98, p3[1], rot=270)
for num, _ in LEFT + RIGHT:
    if num not in ("1", "4", "5", "6", "GND", "3V3"):
        nc(u1.pin(num))
text("GPIO1 = SL, GPIO4 = DA (bus read only).", 167.64, 71.12)
text("GPIO5 / GPIO6 = fans 1 and 2 (active high).", 167.64, 73.66)
text("Powered through 3V3 only; the 5V pin is not connected.", 167.64, 78.74)
text("No USB to the PC while the battery is switched on.", 167.64, 81.28)

# ================================================================= 5 · mecanica
block(238.76, 12.7, 284.48, 87.63, "5 · MECHANICAL")
OFF[:] = [0, 0]
for i, (hx, hy) in enumerate(((246.38, 30.48), (266.7, 30.48), (246.38, 45.72), (266.7, 45.72))):
    Sym("H%d" % (i + 1), "Mechanical:MountingHole", hx, hy,
        fields={"Reference": fld(2.54, -1.27, "left"), "Value": fld(2.54, 1.27, "left")})
text("M3 holes (3.2 mm)", 241.3, 60.96)
text("Board " + VINFO[1], 241.3, 63.5)
text("2 layers, FR-4 1.6 mm", 241.3, 66.04)
text("1 oz copper, HASL", 241.3, 68.58)

# ================================================================= 4 · ventiladores
block(12.7, 92.71, 190.5, 165.1, "4 · 5 V FANS (LOW-SIDE SWITCHED)")
OFF[:] = [0, -36.83]
VR = 157.48
power("power:VCC", 25.4, 142.24, "VBAT")
f2 = part("F2", "Device:Polyfuse", 25.4, 149.86, fields=V2)
W((25.4, 142.24), f2.pin(1))
CHX = (88.9, 152.4)
W(f2.pin(2), (25.4, VR), (CHX[1] + 17.78, VR))
power("power:PWR_FLAG", 33.02, VR, rot=180)
power("power:VCC", 38.1, VR, "+V_VENT")
for ref, lib_id, x in (("C4", "Device:C_Polarized", 48.26), ("C7", "Device:C", 63.5)):
    c = part(ref, lib_id, x, 165.1, fields=V2)
    W((x, VR), c.pin(1)); power("power:GND", *c.pin(2))

for k, xc in enumerate(CHX):
    n = k + 1
    d = part("D%d" % n, "Diode:1N5819", xc, 166.37, rot=270,
             fields={"Reference": fld(-2.54, -1.27, "right"), "Value": fld(-2.54, 1.27, "right")})
    c = part("C%d" % (4 + n), "Device:C", xc + 7.62, 166.37, fields=V2)
    j = part("J%d" % (3 + n), "Connector:Screw_Terminal_01x02", xc + 25.4, 165.1,
             fields={"Reference": fld(1.27, -3.81, "left"), "Value": fld(1.27, 6.35, "left")})
    q = part("Q%d" % n, "Transistor_BJT:PN2222A", xc + 7.62, 185.42, fields=QF)
    r_b = part("R%d" % (2 * n - 1), "Device:R", xc - 8.89, 185.42, rot=90, fields=H2)
    r_pd = part("R%d" % (2 * n), "Device:R", xc, 189.23, fields=V2)
    W((xc, VR), d.pin(1)); W((xc + 7.62, VR), c.pin(1))
    xj = j.pin(1)[0]
    W(j.pin(1), (xj - 2.54, j.pin(1)[1]), (xj - 2.54, VR))
    YN = 175.26
    W(d.pin(2), (xc, YN), (xj - 5.08, YN)); W(c.pin(2), (xc + 7.62, YN))
    W(j.pin(2), (xj - 5.08, j.pin(2)[1]), (xj - 5.08, YN))
    W(q.pin(3), (q.pin(3)[0], YN)); label("FAN%dN" % n, xc + 11.43, YN)
    W(q.pin(1), (q.pin(1)[0], 193.04)); power("power:GND", q.pin(1)[0], 193.04)
    W(r_b.pin(2), q.pin(2)); label("B%d" % n, xc - 3.81, 185.42)
    power("power:GND", *r_pd.pin(2))
    W(r_b.pin(1), (xc - 17.78, 185.42)); glabel("GPIO%d" % (4 + n), xc - 17.78, 185.42, 180)

# ================================================================= 3 · sniffer
block(195.58, 92.71, 284.48, 165.1, "3 · PASSIVE DISPLAY READING (TM1640)")
OFF[:] = [-60.96, 59.69]
j6 = part("J6", "Connector:Screw_Terminal_01x03", 271.78, 60.96, mirror=True,
          fields={"Reference": fld(-1.27, -6.35, "right"), "Value": fld(-1.27, 6.35, "right")})
r5 = part("R5", "Device:R", 292.1, 50.8, rot=90, fields=H2)
r6 = part("R6", "Device:R", 292.1, 66.04, rot=90, fields=H2)
d3 = part("D3", "Diode:1N5819", 309.88, 66.04, rot=180, fields=H2)
W(j6.pin(1), (279.4, j6.pin(1)[1]), (279.4, 50.8), r5.pin(1)); label("SL", 281.94, 50.8)
W(r5.pin(2), (325.12, 50.8)); glabel("GPIO1", 325.12, 50.8, 0)
W(j6.pin(2), (281.94, j6.pin(2)[1]), (281.94, 66.04), r6.pin(1)); label("DA", 283.21, 66.04)
W(r6.pin(2), d3.pin(2)); label("DA_R", 297.18, 66.04)
W(d3.pin(1), (325.12, 66.04)); glabel("GPIO4", 325.12, 66.04, 0)
W(j6.pin(3), (j6.pin(3)[0], 71.12)); power("power:GND", j6.pin(3)[0], 71.12)
text("R5/R6 (470 Ω) limit the current if a GPIO", 259.08, 81.28)
text("is configured as an output by mistake.", 259.08, 83.82)
text("D3 (Schottky, band toward the ESP) keeps the", 259.08, 88.9)
text("ESP from injecting current into DA.", 259.08, 91.44)
text("3.3 V bus: straight to the GPIO, no level shifting.", 259.08, 96.52)

# ================================================================= notas
block(12.7, 168.91, 172.72, 198.12, "NOTES")
OFF[:] = [0, 0]
NOTES = [
    "1. J1: scale Li-ion 1S battery (≤ 4.2 V). J2: panel switch ≥ 2 A. "
    + ("U2: TPS63020 module with the output jumper on 3V3." if TPS_EN_PLACA else "J3: TPS63020 module, wires < 10 cm."),
    "2. J4/J5: 5 V 0.16 A fan: red → 1 (+), black → 2 (−); yellow (tachometer) and blue (PWM) insulated.",
    "3. J6: display connector (SL, DA, GND); the display VCC is not connected.",
    ("4. U1 in a 2 × 1x9 female socket, rows 15.24 mm apart; U2 on 4 pins at its corners. Q1/Q2 2N2222 E-B-C."
     if TPS_EN_PLACA else "4. U1 in a 2 × 1x9 female socket, rows 15.24 mm apart. Q1/Q2 2N2222 E-B-C: check with a multimeter."),
    "5. Diodes: band = cathode (square pad). Electrolytics: long lead to +.",
    "6. PCB: tracks at 0/45/90°; power 1.0 mm, 3V3 and fans 0.8 mm, signal 0.3 mm; GND on both layers.",
    ("7. First power-up without U1 (with U2): measure 3.3 V between the 3V3 and GND pins of the U1 socket."
     if TPS_EN_PLACA else "7. First power-up without U1: measure 3.3 V between the 3V3 and GND pins of the socket."),
]
for i, s in enumerate(NOTES):
    text(s, 15.24, 177.8 + 2.54 * i)


# ================================================================= nudos y uniones
def on_seg(p, a, b):
    if a[0] == b[0] == p[0]:
        return min(a[1], b[1]) < p[1] < max(a[1], b[1])
    if a[1] == b[1] == p[1]:
        return min(a[0], b[0]) < p[0] < max(a[0], b[0])
    return False


points = set(pinpts)
for a, b in wires:
    points.add(a); points.add(b)
points |= {(x, y) for _, x, y, _ in labels}
split = []
for a, b in wires:
    cuts = sorted([p for p in points if on_seg(p, a, b)], key=lambda p: (p[0] - a[0]) ** 2 + (p[1] - a[1]) ** 2)
    seq = [a] + cuts + [b]
    split += [(s, e) for s, e in zip(seq, seq[1:]) if s != e and not
              any(on_seg(lp, s, e) for lp in [])]
wires = split
deg = {}
for a, b in wires:
    deg[a] = deg.get(a, 0) + 1
    deg[b] = deg.get(b, 0) + 1
for p in pinpts:
    if p in deg:
        deg[p] += 1
junctions = [p for p, n in deg.items() if n >= 3]


# ================================================================= escritura
def eff(size=1.27, just=None, hide=False, bold=False):
    s = "(effects (font (size %s %s)%s)" % (size, size, " (bold yes)" if bold else "")
    if just:
        s += " (justify %s)" % just
    if hide:
        s += " (hide yes)"
    return s + ")"


def prop(name, val, x, y, ang=0, just=None, hide=False):
    return '(property %s %s (at %s %s %s) %s)' % (K.q(name), K.q(val), r2(x), r2(y), ang, eff(just=just, hide=hide))


out = ['(kicad_sch (version 20250610) (generator "eeschema") (generator_version "10.0")',
       '(uuid "%s")' % ROOT, '(paper "A4")',
       '(title_block (title "Scale box ESP32-S3") '
       '(date "2026-10-29") (rev "%s") (comment 1 "TM1640 sniffer and control of 2 fans; board %s") '
       '(comment 2 "Generated by gen_sch.py; board by %s") '
       % (VINFO[0], VINFO[1].replace("×", "x") + (", TPS63020 on the board" if TPS_EN_PLACA else ""),
          VINFO[2]) +
       '(comment 3 "Verified: KiCad ERC and schematic-PCB parity"))',
       "(lib_symbols"]
for lid, s in LIBSYMS.items():
    out.append(K.dump(s, 1))
out.append(")")

for p in junctions:
    out.append('(junction (at %s %s) (diameter 0) (color 0 0 0 0) (uuid "%s"))' % (p[0], p[1], any_uuid("j%s" % (p,))))
for p in noconn:
    out.append('(no_connect (at %s %s) (uuid "%s"))' % (p[0], p[1], any_uuid("nc%s" % (p,))))
for i, (a, b) in enumerate(wires):
    out.append('(wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))'
               % (a[0], a[1], b[0], b[1], any_uuid("w%d%s%s" % (i, a, b))))
for name, x, y, ang in labels:
    out.append('(label %s (at %s %s %d) %s (uuid "%s"))'
               % (K.q(name), x, y, ang, eff(just="left bottom" if ang == 0 else "right bottom"),
                  any_uuid("l%s%s%s" % (name, x, y))))
for name, x, y, ang, shape in glabels:
    just = "left" if ang == 0 else "right"
    out.append('(global_label %s (shape %s) (at %s %s %d) (fields_autoplaced yes) %s (uuid "%s") '
               '(property "Intersheetrefs" "${INTERSHEET_REFS}" (at %s %s 0) %s))'
               % (K.q(name), shape, x, y, ang, eff(just=just), any_uuid("g%s%s%s" % (name, x, y)),
                  x, y, eff(just=just, hide=True)))
for s, x, y, size, bold, just in texts:
    out.append('(text %s (exclude_from_sim no) (at %s %s 0) %s (uuid "%s"))'
               % (K.q(s), r2(x), r2(y), eff(size, just, bold=bold), any_uuid("t%s%s%s" % (s, x, y))))
for x0, y0, x1, y1 in rects:
    out.append('(rectangle (start %s %s) (end %s %s) (stroke (width 0.254) (type dash)) (fill (type none)) '
               '(uuid "%s"))' % (x0, y0, x1, y1, any_uuid("r%s%s" % (x0, y0))))

for s in symbols:
    sym = libsym(s.lib_id)
    props = {K.uq(p[1]): K.uq(p[2]) for p in K.find(sym, "property")}
    uid = sym_uuid(s.ref) if not s.ref.startswith("#") else any_uuid("pwr" + s.ref)
    lines = ['(symbol (lib_id %s) (at %s %s %d)%s (unit 1) (exclude_from_sim no) (in_bom %s) (on_board %s) '
             '(dnp no) (uuid "%s")' % (K.q(s.lib_id), r2(s.x + s.ox), r2(s.y + s.oy), s.rot,
                                       " (mirror y)" if s.mirror else "",
                                       "no" if s.ref[0] in "#H" else "yes",
                                       "no" if s.ref.startswith("#") else "yes", uid)]
    for name, val in (("Reference", s.ref), ("Value", s.value)):
        dx, dy, just, hide = s.fields.get(name, fld(0, 0, hide=True))
        fang = 90 if s.rot in (90, 270) else 0          # texto horizontal en simbolos girados
        if s.rot == 90 and just in ("left", "right"):     # KiCad invierte la justificacion a 90 grados
            just = {"left": "right", "right": "left"}[just]
        lines.append(prop(name, val, s.x + s.ox + dx, s.y + s.oy + dy, fang, just, hide))
    lines.append(prop("Footprint", s.fp or props.get("Footprint", ""), s.x + s.ox, s.y + s.oy, 0, None, True))
    lines.append(prop("Datasheet", datasheet(s.ref) if not s.ref.startswith("#") else "~", s.x + s.ox, s.y + s.oy, 0, None, True))
    lines.append(prop("Description", props.get("Description", ""), s.x + s.ox, s.y + s.oy, 0, None, True))
    for n in s.libpins:
        lines.append('(pin %s (uuid "%s"))' % (K.q(n), any_uuid("p%s%s" % (s.ref, n))))
    lines.append('(instances (project %s (path "/%s" (reference %s) (unit 1))))'
                 % (K.q(PROJECT), ROOT, K.q(s.ref)))
    lines.append(")")
    out.append(" ".join(lines))

out.append('(sheet_instances (path "/" (page "1")))')
out.append("(embedded_fonts no)")
out.append(")")
with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")

# libreria de simbolos del proyecto (para editar el esquema en KiCad)
with open(os.path.join(LIBDIR, "caja_balanza.kicad_sym"), "w", encoding="utf-8") as f:
    f.write('(kicad_symbol_lib (version 20251024) (generator "gen_sch.py")\n%s\n)\n'
            % (esp_symbol_text("ESP32-S3_SuperMini")
               + ("\n" + tps_symbol_text("TPS63020_Module") if TPS_EN_PLACA else "")))
with open(os.path.join(LIBDIR, "sym-lib-table"), "w", encoding="utf-8") as f:
    f.write('(sym_lib_table\n  (version 7)\n  (lib (name "caja_balanza") (type "KiCad") '
            '(uri "${KIPRJMOD}/caja_balanza.kicad_sym") (options "") (descr "Project symbols"))\n)\n')
print("guardado", OUT, "| simbolos:", len(symbols), "| cables:", len(wires), "| uniones:", len(junctions))
