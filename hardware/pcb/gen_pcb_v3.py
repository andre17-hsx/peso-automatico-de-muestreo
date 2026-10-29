# Version 3 de la placa de la caja: la v1 (gen_pcb.py, que no se modifica) con el modulo
# TPS63020 montado en la placa (U2) en lugar de la bornera J3. Misma medida (70 x 50 mm).
# Cambios en la zona de alimentacion (derecha):
#   - U2 arriba a la derecha, girado 180: salida a la izquierda, junto a C2/C3 y el pin 3V3 de
#     la ESP (pista de 3V3 de ~15 mm); entrada a la derecha.
#   - BAT (J1) e interruptor (J2) en el borde derecho, donde estaba J3; F1 entre los dos.
#   - VBAT sube a la entrada de U2 por la cara inferior; C1, C8 y F2 cuelgan de VBAT.
# Ejecutar con el Python de KiCad, despues del esquema:
#   "C:\Program Files\KiCad\10.0\bin\python.exe" gen_sch.py --v3
#   "C:\Program Files\KiCad\10.0\bin\python.exe" gen_pcb_v3.py
# Coordenadas en mm, origen arriba a la izquierda de la placa (70 x 50 mm).
import math
import os

import pcbnew

import kisexpr
from caja_parts import PARTS_V3 as PARTS, esp_nc_net, sym_uuid

HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.join(HERE, "v3")
NAME = "caja_balanza_v3"
OUT = os.path.join(OUTDIR, NAME + ".kicad_pcb")
# Modulo TPS63020 (26 x 18 mm, un pin en cada esquina). Distancia entre centros de los agujeros:
# MEDIR CON EL CALIBRE antes de pedir la placa (valores estimados de la foto).
TPS_W, TPS_H = 26.0, 18.0
TPS_DX, TPS_DY = 24.0, 12.4
FPLIB = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
OX, OY = 100.0, 100.0          # desplazamiento en la hoja de KiCad
W, H = 70.0, 50.0

mm = pcbnew.FromMM


def P(x, y):
    return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))


board = pcbnew.BOARD()
nets = {}


# redes con etiqueta local en el esquema: KiCad las nombra con la ruta de la hoja ("/SL")
LOCAL_NETS = {"BAT_IN", "BAT_F", "SL", "DA", "DA_R", "B1", "B2", "FAN1N", "FAN2N"}


def net(name):
    if name not in nets:
        n = pcbnew.NETINFO_ITEM(board, "/" + name if name in LOCAL_NETS else name)
        board.Add(n)
        nets[name] = n
    return nets[name]


# ---------------------------------------------------------------- reglas
ds = board.GetDesignSettings()
ds.SetCopperLayerCount(2)
ds.m_TrackMinWidth = mm(0.25)
ds.m_MinClearance = mm(0.25)
ds.m_ViasMinSize = mm(0.6)
ds.m_MinThroughDrill = mm(0.3)
ds.m_CopperEdgeClearance = mm(0.5)
ds.m_HoleToHoleMin = mm(0.25)
nc = ds.m_NetSettings.GetDefaultNetclass()
nc.SetClearance(mm(0.25))
nc.SetTrackWidth(mm(0.3))
nc.SetViaDiameter(mm(0.8))
nc.SetViaDrill(mm(0.4))

# ---------------------------------------------------------------- contorno
def edge_rect(x0, y0, x1, y1, r=2.0):
    """Rectangulo con esquinas redondeadas en Edge.Cuts."""
    segs = [((x0 + r, y0), (x1 - r, y0)), ((x1, y0 + r), (x1, y1 - r)),
            ((x1 - r, y1), (x0 + r, y1)), ((x0, y1 - r), (x0, y0 + r))]
    for a, b in segs:
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(P(*a)); s.SetEnd(P(*b))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.1))
        board.Add(s)
    for cx, cy, sx, sy in [(x0 + r, y0 + r, x0, y0 + r), (x1 - r, y0 + r, x1 - r, y0),
                           (x1 - r, y1 - r, x1, y1 - r), (x0 + r, y1 - r, x0 + r, y1)]:
        a = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        a.SetCenter(P(cx, cy)); a.SetStart(P(sx, sy))
        a.SetArcAngleAndEnd(pcbnew.EDA_ANGLE(90, pcbnew.DEGREES_T), True)
        a.SetLayer(pcbnew.Edge_Cuts); a.SetWidth(mm(0.1))
        board.Add(a)


edge_rect(0, 0, W, H)

# ---------------------------------------------------------------- huellas
parts = {}


def place(ref, lib, name, x, y, rot, value, pads):
    fp = pcbnew.FootprintLoad(os.path.join(FPLIB, lib + ".pretty"), name)
    if fp is None:
        raise SystemExit("no existe la huella %s:%s" % (lib, name))
    fp.SetReference(ref)
    fp.SetValue(value)
    board.Add(fp)
    fp.SetPosition(P(x, y))
    fp.SetOrientationDegrees(rot)
    fp.Value().SetVisible(False)
    for num, n in pads.items():
        p = fp.FindPadByNumber(str(num))
        if p is None:
            raise SystemExit("%s sin pad %s" % (ref, num))
        p.SetNet(net(n))
    parts[ref] = fp
    return fp


LIB = os.path.join(OUTDIR, "caja_balanza.pretty")
ESP_FP = "ESP32-S3_SuperMini_Socket"
TPS_FP = "TPS63020_Module_26x18"


def write_tps_footprint():
    """Modulo TPS63020 26 x 18 mm sobre 4 pines (uno por esquina). Pads: 1 VIN, 2 GND entrada,
    3 GND salida, 4 VOUT. Los puentes 3V3/4V2/5V y los pads EN/PS del modulo no van a la placa.
    Igual que en la v2."""
    hx, hy, bx, by = TPS_DX / 2, TPS_DY / 2, TPS_W / 2, TPS_H / 2
    mod = ['(footprint "%s"' % TPS_FP, '  (version 20241229)', '  (generator "gen_pcb_v3.py")',
           '  (layer "F.Cu")',
           '  (descr "TPS63020 buck-boost module 26 x 18 mm on 4 corner pins")',
           '  (property "Reference" "REF**" (at 0 -2 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))',
           '  (property "Value" "TPS63020 module" (at 0 2 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))',
           '  (attr through_hole)']
    box = [((-bx, -by), (bx, -by)), ((bx, -by), (bx, by)), ((bx, by), (-bx, by)), ((-bx, by), (-bx, -by))]
    cy = [((-bx - 0.25, -by - 0.25), (bx + 0.25, -by - 0.25)), ((bx + 0.25, -by - 0.25), (bx + 0.25, by + 0.25)),
          ((bx + 0.25, by + 0.25), (-bx - 0.25, by + 0.25)), ((-bx - 0.25, by + 0.25), (-bx - 0.25, -by - 0.25))]
    for (a, b), lay, w in [(s, "F.SilkS", 0.15) for s in box] + [(s, "F.CrtYd", 0.05) for s in cy]:
        mod.append('  (fp_line (start %.3f %.3f) (end %.3f %.3f) (stroke (width %.2f) (type solid)) (layer "%s"))'
                   % (a[0], a[1], b[0], b[1], w, lay))
    for num, x, y, nm in (("1", -hx, hy, "VIN"), ("2", -hx, -hy, "GND"), ("3", hx, -hy, "GND"), ("4", hx, hy, "OUT")):
        mod.append('  (pad "%s" thru_hole %s (at %.3f %.3f) (size 1.8 1.8) (drill 1) (layers "*.Cu" "*.Mask"))'
                   % (num, "rect" if num == "1" else "circle", x, y))
        mod.append('  (fp_text user "%s" (at %.3f %.3f 0) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))'
                   % (nm, x + (2.6 if x < 0 else -2.6), y))
    mod.append(")")
    os.makedirs(LIB, exist_ok=True)
    with open(os.path.join(LIB, TPS_FP + ".kicad_mod"), "w", encoding="utf-8") as f:
        f.write("\n".join(mod) + "\n")


def tps63020_module(ref, x, y, rot):
    write_tps_footprint()
    fp = pcbnew.FootprintLoad(LIB, TPS_FP)
    fp.SetFPID(pcbnew.LIB_ID("caja_balanza", TPS_FP))
    fp.SetReference(ref)
    board.Add(fp)
    fp.SetPosition(P(x, y))
    fp.SetOrientationDegrees(rot)
    fp.Value().SetVisible(False)
    for num, n in {"1": "VBAT", "2": "GND", "3": "GND", "4": "+3V3"}.items():
        fp.FindPadByNumber(num).SetNet(net(n))
    parts[ref] = fp
    return fp


def write_esp_footprint():
    """ESP32-S3 Super Mini sobre dos tiras hembra 1x9 (filas a 15,24 mm), USB-C hacia arriba.
    Se escribe en la libreria del proyecto y se carga desde ahi, para que la placa y la
    libreria coincidan."""
    left = ["43", "44", "1", "2", "3", "4", "5", "6", "7"]
    right = ["5V", "GND", "3V3", "13", "12", "11", "10", "9", "8"]
    bx, by = 9.0, 11.26                      # media placa: 18 x 22,52 mm
    mod = ['(footprint "%s"' % ESP_FP, '  (version 20241229)', '  (generator "gen_pcb.py")',
           '  (layer "F.Cu")',
           '  (descr "ESP32-S3 Super Mini on two 1x9 2.54 mm female headers, rows 15.24 mm apart, USB-C up")',
           '  (property "Reference" "REF**" (at 0 -3 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))',
           '  (property "Value" "ESP32-S3 Super Mini" (at 0 3 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))',
           '  (attr through_hole)']
    box = [((-bx, -by), (bx, -by)), ((bx, -by), (bx, by)), ((bx, by), (-bx, by)), ((-bx, by), (-bx, -by))]
    usb = [((-4.5, -by), (-4.5, -by - 1.5)), ((-4.5, -by - 1.5), (4.5, -by - 1.5)), ((4.5, -by - 1.5), (4.5, -by))]
    for (a, b), lay, w in [(s, "F.SilkS", 0.15) for s in box + usb] + [(s, "F.CrtYd", 0.05) for s in box]:
        mod.append('  (fp_line (start %.3f %.3f) (end %.3f %.3f) (stroke (width %.2f) (type solid)) (layer "%s"))'
                   % (a[0], a[1], b[0], b[1], w, lay))
    for col, names, xx in ((0, left, -7.62), (1, right, 7.62)):
        for i, nm in enumerate(names):
            shape = "rect" if (col == 0 and i == 0) else "circle"
            mod.append('  (pad "%s" thru_hole %s (at %.3f %.3f) (size 1.7 1.7) (drill 1) (layers "*.Cu" "*.Mask"))'
                       % (nm, shape, xx, -10.16 + 2.54 * i))
    mod.append(")")
    os.makedirs(LIB, exist_ok=True)
    with open(os.path.join(LIB, ESP_FP + ".kicad_mod"), "w", encoding="utf-8") as f:
        f.write("\n".join(mod) + "\n")


def esp32s3_supermini(ref, x, y):
    write_esp_footprint()
    fp = pcbnew.FootprintLoad(LIB, ESP_FP)
    fp.SetFPID(pcbnew.LIB_ID("caja_balanza", ESP_FP))
    fp.SetReference(ref)
    board.Add(fp)
    fp.SetPosition(P(x, y))
    fp.Value().SetVisible(False)
    parts[ref] = fp
    return fp


TB2 = ("TerminalBlock_Phoenix", "TerminalBlock_Phoenix_MKDS-1,5-2-5.08_1x02_P5.08mm_Horizontal")
TB3 = ("TerminalBlock_Phoenix", "TerminalBlock_Phoenix_MKDS-1,5-3-5.08_1x03_P5.08mm_Horizontal")
TB4 = ("TerminalBlock_Phoenix", "TerminalBlock_Phoenix_MKDS-1,5-4-5.08_1x04_P5.08mm_Horizontal")
R254V = ("Resistor_THT", "R_Axial_DIN0207_L6.3mm_D2.5mm_P2.54mm_Vertical")   # resistencia de pie
R762 = ("Resistor_THT", "R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal")
DO41 = ("Diode_THT", "D_DO-41_SOD81_P10.16mm_Horizontal")
DO41S = ("Diode_THT", "D_DO-41_SOD81_P7.62mm_Horizontal")
CDISC = ("Capacitor_THT", "C_Disc_D5.0mm_W2.5mm_P5.00mm")
PTC = ("Capacitor_THT", "C_Disc_D7.5mm_W5.0mm_P5.00mm")      # PTC radial, patas a 5,0-5,1 mm
CP63 = ("Capacitor_THT", "CP_Radial_D6.3mm_P2.50mm")
CP8 = ("Capacitor_THT", "CP_Radial_D8.0mm_P3.50mm")
TO92 = ("Package_TO_SOT_THT", "TO-92_Inline_Wide")         # patas a 2,54 mm: mas facil de soldar
HOLE = ("MountingHole", "MountingHole_3.2mm_M3")

# ---------------------------------------------------------------- colocacion
# ESP: filas en x=16 (GPIO) y x=31,24 (alimentacion); pines de y=3,5 a y=23,82; USB-C arriba
esp32s3_supermini("U1", 23.62, 13.66)
for nm, n in {"1": "GPIO1", "4": "GPIO4", "5": "GPIO5", "6": "GPIO6",
              "GND": "GND", "3V3": "+3V3"}.items():
    parts["U1"].FindPadByNumber(nm).SetNet(net(n))


# sniffer del display (franja izquierda): J6 abajo, R5/R6/D3 a la altura de GPIO1 y GPIO4
place("J6", *TB3, 5.6, 27.0, 270, "DISPLAY", {1: "SL", 2: "DA", 3: "GND"})
place("R5", *R762, 3.0, 8.58, 0, "470", {1: "SL", 2: "GPIO1"})
place("D3", *DO41S, 13.0, 16.2, 180, "BAT54/1N5819", {1: "GPIO4", 2: "DA_R"})   # banda hacia el GPIO
place("R6", *R762, 13.0, 20.0, 180, "470", {1: "DA", 2: "DA_R"})

# modulo TPS63020 (U2) arriba a la derecha, girado 180: salida (VOUT, GND) a la izquierda, hacia
# la ESP; entrada (VIN, GND) a la derecha. Guia de TI (10.1): condensadores junto al regulador,
# pistas cortas y anchas, masas del modulo directas al plano.
UX, UY = 56.0, 10.0
tps63020_module("U2", UX, UY, 180)
XO, YO = UX - TPS_DX / 2, UY - TPS_DY / 2                 # pad VOUT
XI, YI = UX + TPS_DX / 2, UY - TPS_DY / 2                 # pad VIN
place("C3", *CDISC, 34.5, 8.58, 270, "100n", {1: "+3V3", 2: "GND"})                # junto al pin 3V3
place("C2", *CP8, 38.0, 18.75, 270, "470u 10V", {1: "+3V3", 2: "GND"})

# bateria e interruptor en el borde derecho (donde estaba J3), entrada del cable hacia el borde
XJ, YJ2, YJ1 = 64.4, 27.93, 39.69
place("J2", *TB2, XJ, YJ2, 90, "SW", {1: "BAT_F", 2: "VBAT"})
place("J1", *TB2, XJ, YJ1, 90, "BAT", {1: "BAT_IN", 2: "GND"})
place("F1", *PTC, 55.6, 37.0, 90, "PTC 1,1A", {1: "BAT_IN", 2: "BAT_F"})
place("C8", *CDISC, 57.0, YJ2 - 5.08, 270, "10u", {1: "VBAT", 2: "GND"})
place("C1", *CP63, 50.0, 26.0, 0, "100u 16V", {1: "VBAT", 2: "GND"})

# rama de ventiladores
place("F2", *PTC, 50.0, 31.5, 270, "PTC 0,75A", {1: "VBAT", 2: "+V_VENT"})
place("C4", *CP8, 45.0, 45.0, 0, "470u 16V", {1: "+V_VENT", 2: "GND"})
place("C7", *CDISC, 53.0, 42.5, 270, "100n", {1: "+V_VENT", 2: "GND"})

CH = ((13.5, "GPIO5"), (28.7, "GPIO6"))      # x0 de cada canal (15,2 mm de ancho)
for i, (x0, gpio) in enumerate(CH):
    k = i + 1
    fan = "FAN%dN" % k
    b = "B%d" % k
    place("J%d" % (4 + i), *TB2, x0 + 4.0, 44.4, 0, "FAN%d" % k, {1: "+V_VENT", 2: fan})
    place("D%d" % k, *DO41, x0 + 1.0, 36.9, 0, "1N5819", {1: "+V_VENT", 2: fan})
    place("C%d" % (5 + i), *CDISC, x0 - 0.5, 33.7, 0, "100n", {1: "+V_VENT", 2: fan})
    place("Q%d" % k, *TO92, x0 + 7.0, 32.0, 0, "2N2222", {1: "GND", 2: b, 3: fan})   # E-B-C
    place("R%d" % (2 * i + 1), *R762, x0 + 0.5, 26.8, 0, "470", {1: gpio, 2: b})
    place("R%d" % (2 * i + 2), *R254V, x0 + 10.8, 26.8, 0, "10k", {1: b, 2: "GND"})

for i, (x, y) in enumerate(((3.5, 46.5), (66.5, 46.5), (39.0, 3.5), (3.5, 3.5))):
    place("H%d" % (i + 1), *HOLE, x, y, 0, "M3", {})

# ---------------------------------------------------------------- pistas
F, B = pcbnew.F_Cu, pcbnew.B_Cu
PW, VW, SW = 1.0, 0.8, 0.3      # potencia, 3V3 / retorno de ventilador, senal


def chamfer(pts, c_max=1.0):
    """Sustituye cada esquina de 90 grados por un tramo a 45 (ruteo octilineal)."""
    out = [pts[0]]
    for i in range(1, len(pts) - 1):
        (ax, ay), (bx, by), (cx, cy) = out[-1], pts[i], pts[i + 1]
        u = (bx - ax, by - ay); v = (cx - bx, cy - by)
        lu, lv = math.hypot(*u), math.hypot(*v)
        cosang = (u[0] * v[0] + u[1] * v[1]) / (lu * lv)
        if cosang < -0.01:
            raise SystemExit("giro agudo en %s" % (pts[i],))
        if cosang < 0.01:                                  # giro de 90
            c = min(c_max, 0.4 * lu, 0.4 * lv)
            out.append((bx - u[0] / lu * c, by - u[1] / lu * c))
            out.append((bx + v[0] / lv * c, by + v[1] / lv * c))
        else:
            out.append(pts[i])
    out.append(pts[-1])
    return out


def track(n, layer, w, *pts):
    pts = chamfer(list(pts))
    for a, b in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(P(*a)); t.SetEnd(P(*b))
        t.SetLayer(layer); t.SetWidth(mm(w)); t.SetNet(net(n))
        board.Add(t)


def via(n, x, y):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(P(x, y)); v.SetWidth(mm(0.8)); v.SetDrill(mm(0.4))
    v.SetNet(net(n))
    board.Add(v)


# Ruteo octilineal: solo tramos a 0/45/90 grados, sin esquinas de 90 (chaflan automatico),
# sin angulos agudos; las derivaciones llegan en T a 90 grados. Verificar con check_routing.py.

# bateria -> F1 -> interruptor
track("BAT_IN", F, PW, (XJ, YJ1), (58.29, YJ1), (55.6, 37.0))
track("BAT_F", F, PW, (55.6, 32.0), (55.6 + 4.07, YJ2), (XJ, YJ2))

# VBAT: interruptor -> C8 -> C1 -> F2 (cara superior) y -> VIN de U2 por la cara inferior
yv = YJ2 - 5.08
track("VBAT", F, PW, (XJ, yv), (57.0, yv), (50.0, yv), (50.0, 26.0), (50.0, 31.5))
track("VBAT", B, PW, (XJ, yv), (XJ, YI + XI - XJ), (XI, YI))

# 3V3: VOUT de U2 -> C3 -> pin 3V3 de la ESP; C2 cuelga debajo (pista de ~15 mm)
track("+3V3", F, VW, (XO, YO), (XO, 8.58 - 2.0), (XO - 2.0, 8.58), (38.0, 8.58), (34.5, 8.58),
      (31.24, 8.58))
track("+3V3", F, VW, (38.0, 8.58), (38.0, 18.75))

# +V_VENT: F2 -> C4/C7 -> barra a los dos ventiladores
track("+V_VENT", F, PW, (50.0, 36.5), (50.0, 41.0))
track("+V_VENT", F, PW, (45, 41), (45, 45))
x1, x2 = CH[0][0], CH[1][0]
track("+V_VENT", F, PW, (53, 42.5), (51.5, 41), (50.0, 41.0), (45, 41), (x1 + 1.0, 41), (x1 + 1.0, 35.2),
      (x1 - 0.5, 33.7))                                                                    # D1 (K) y C5
track("+V_VENT", F, PW, (x2 + 1.0, 41), (x2 + 1.0, 35.2), (x2 - 0.5, 33.7))             # D2 (K) y C6
for x0, _ in CH:
    track("+V_VENT", F, PW, (x0 + 4.0, 44.4), (x0 + 4.0, 41))                          # borne +

# retorno de cada ventilador (colector) por la cara inferior
for k, (x0, _) in enumerate(CH):
    n = "FAN%dN" % (k + 1)
    xq, xd, xj, xc = x0 + 12.08, x0 + 11.16, x0 + 9.08, x0 + 4.5
    track(n, B, VW, (xq, 32), (xq, 36.9 - (xq - xd)), (xd, 36.9), (xj, 36.9 + (xd - xj)), (xj, 44.4))
    track(n, B, VW, (xc, 33.7), (xq, 33.7))

# bases: R1 -> R2 (10k a masa) y derivacion en T hacia la base
for k, (x0, _) in enumerate(CH):
    n = "B%d" % (k + 1)
    xr, xb, xr2 = x0 + 8.12, x0 + 9.54, x0 + 10.8
    track(n, F, SW, (xr, 26.8), (xr2, 26.8))
    track(n, F, SW, (xb, 26.8), (xb, 32))
track("GPIO5", B, SW, (16, 18.74), (14.4, 20.34), (14.4, 25.2), (14, 25.6), (14, 26.8))
track("GPIO6", B, SW, (16, 21.28), (17.5, 22.78), (28.2, 22.78), (29.2, 23.78), (29.2, 26.8))

# sniffer
track("GPIO1", F, SW, (10.62, 8.58), (16, 8.58))
track("GPIO4", F, SW, (13, 16.2), (16, 16.2))
track("DA_R", F, SW, (5.38, 20), (5.38, 16.2))
track("SL", F, SW, (5.6, 27), (2.6, 24), (2, 23.4), (2, 9.58), (3, 8.58))
track("DA", F, SW, (13, 20), (12, 21), (12, 29.5), (9.42, 32.08), (5.6, 32.08))

# ---------------------------------------------------------------- planos de masa (dos caras)
def gnd_zone(layer):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net("GND"))
    z.SetLocalClearance(mm(0.4))
    z.SetMinThickness(mm(0.3))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(mm(0.5))
    z.SetThermalReliefSpokeWidth(mm(0.6))
    o = z.Outline(); o.NewOutline()
    for x, y in ((0.3, 0.3), (W - 0.3, 0.3), (W - 0.3, H - 0.3), (0.3, H - 0.3)):
        o.Append(mm(OX + x), mm(OY + y))
    board.Add(z)


gnd_zone(F)
gnd_zone(B)

# vias de costura entre los dos planos, solo donde no molestan
def clear_of_everything(x, y, d=1.6):
    pt = P(x, y)
    for fp in board.GetFootprints():
        for pd in fp.Pads():
            r = max(pcbnew.ToMM(pd.GetSize(pcbnew.F_Cu).x), pcbnew.ToMM(pd.GetSize(pcbnew.F_Cu).y)) / 2
            if r == 0:
                r = 3.2 / 2 + 1.0                  # agujero de montaje
            if (pd.GetPosition() - pt).EuclideanNorm() < mm(r + d):
                return False
    for t in board.GetTracks():
        a, b = t.GetStart(), t.GetEnd()
        ax, ay, bx, by = (pcbnew.ToMM(v) for v in (a.x, a.y, b.x, b.y))
        px, py = OX + x, OY + y
        dx, dy = bx - ax, by - ay
        L = dx * dx + dy * dy
        u = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
        cx, cy = ax + u * dx, ay + u * dy
        if ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5 < pcbnew.ToMM(t.GetWidth()) / 2 + d:
            return False
    return 1.5 < x < W - 1.5 and 1.5 < y < H - 1.5


stitch = []
for gx in range(0, 14):
    for gy in range(0, 10):
        x, y = 2.5 + gx * 5.0, 2.5 + gy * 5.0
        if clear_of_everything(x, y):
            stitch.append((x, y))
for x, y in stitch:
    via("GND", x, y)

# ---------------------------------------------------------------- rotulos
def text(s, x, y, size=1.0, rot=0, layer=pcbnew.F_SilkS, just=0):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s); t.SetPosition(P(x, y)); t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(0.15 if size <= 1.0 else 0.2))
    t.SetTextAngleDegrees(rot)
    if just < 0:
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    elif just > 0:
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT)
    if layer == pcbnew.B_SilkS:
        t.SetMirrored(True)
    board.Add(t)


for d in ("D1", "D2"):
    for it in list(parts[d].GraphicalItems()):
        if isinstance(it, pcbnew.PCB_TEXT) and it.GetText() == "K" and it.GetLayer() == pcbnew.F_SilkS:
            parts[d].Remove(it)

REF_AT = {"C1": (51.25, 21.4, 0), "C2": (43.6, 22.0, 90), "C4": (41.8, 45.0, 90)}

# referencias: dentro del cuerpo de cada componente (visibles al montar)
for fp in board.GetFootprints():
    ref = fp.GetReference()
    rf = fp.Reference()
    if ref[0] in "JHU":
        rf.SetVisible(False)
        continue
    pads = list(fp.Pads())
    cx = sum(pcbnew.ToMM(p.GetPosition().x) for p in pads) / len(pads) - OX
    cy = sum(pcbnew.ToMM(p.GetPosition().y) for p in pads) / len(pads) - OY
    rot = 90 if int(fp.GetOrientationDegrees()) % 180 == 90 else 0
    if ref in ("R2", "R4"):                     # de pie: el rotulo va debajo
        cy += 2.2
    elif ref[0] == "Q":
        cy -= 1.6
    elif ref in REF_AT:                         # electroliticos: fuera del circulo
        cx, cy, rot = REF_AT[ref]
    rf.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8)))
    rf.SetTextThickness(mm(0.12))
    rf.SetPosition(P(cx, cy))
    rf.SetTextAngleDegrees(rot)

L = [  # texto, x, y, tamano, giro, alineacion (-1 izq, 0 centro, 1 der)
    ("SW", 60.3, YJ2 - 5.08, 0.8, 90, 0), ("SW", 60.3, YJ2, 0.8, 90, 0),
    ("BAT-", 60.3, YJ1 - 5.08, 0.8, 90, 0), ("BAT+", 60.3, YJ1, 0.8, 90, 0),
    ("TPS63020", UX, UY - 1.0, 1.0, 0, 0), ("salida en 3V3", UX, UY + 1.0, 0.8, 0, 0),
    ("ENTRADA", XI - 1.2, UY + 2.0, 0.8, 90, 0), ("SALIDA", XO + 1.2, UY + 2.0, 0.8, 90, 0),
    ("SL", 11.6, 27.0, 0.8, 0, -1), ("DA", 11.6, 30.6, 0.8, 0, -1), ("GND", 11.6, 38.9, 0.8, 0, -1),
    ("FAN1", 13.3, 44.4, 0.8, 90, 0), ("FAN2", 28.5, 44.4, 0.8, 90, 0),
    ("ESP32-S3", 23.62, 12.4, 1.0, 0, 0), ("Super Mini", 23.62, 14.2, 0.9, 0, 0),
    ("USB-C", 23.62, 5.2, 0.8, 0, 0),
]
for s_, x, y, sz, rot, just in L:
    text(s_, x, y, sz, rot, pcbnew.F_SilkS, just)

# cara inferior: nombre de cada borne junto a su pad, y el titulo entre las filas de la ESP
LB = [
    ("SW", 61.0, YJ2 - 5.08), ("SW", 61.0, YJ2), ("BAT-", 61.0, YJ1 - 5.08), ("BAT+", 61.0, YJ1),
    ("SL", 9.2, 27.0), ("DA", 9.2, 32.08), ("GND", 9.4, 37.16),
    ("FAN1+", 17.5, 47.6), ("FAN1-", 22.58, 47.6), ("FAN2+", 32.7, 47.6), ("FAN2-", 37.78, 47.6),
    ("CAJA BALANZA", 23.62, 6.5), ("ESP32-S3", 23.62, 8.5), ("Super Mini", 23.62, 10.5),
    ("sniffer TM1640", 23.62, 14.5), ("+ ventiladores", 23.62, 16.5), ("v3  2026-10", 23.62, 19.5),
]
for s_, x, y in LB:
    text(s_, x, y, 0.8, 0, pcbnew.B_SilkS, 0)

# lagrimas (teardrops) en todos los pads con pista: refuerzan la union pista-pad al soldar a mano;
# kicad-cli las genera al rellenar los planos
for fp in board.GetFootprints():
    for pd in fp.Pads():
        if pd.GetNetname():
            pd.SetTeardropsEnabled(True)
            pd.SetTeardropCurved(True)

# el relleno de los planos lo hace kicad-cli (drc --refill-zones --save-board)
# valor, huella y enlace con su simbolo del esquema (misma tabla que gen_sch.py); Datasheet y
# Description se copian del esquema (generar antes con gen_sch.py)
SCH_FIELDS = {}
_sch = os.path.join(OUTDIR, NAME + ".kicad_sch")
if not os.path.exists(_sch):
    raise SystemExit("falta %s: ejecutar antes gen_sch.py --v3" % _sch)
if True:
    with open(_sch, encoding="utf-8") as f:
        _root = kisexpr.parse(f.read())
    for _s in kisexpr.find(_root, "symbol"):
        _p = {kisexpr.uq(x[1]): kisexpr.uq(x[2]) for x in kisexpr.find(_s, "property")}
        if "Reference" in _p:
            SCH_FIELDS[_p["Reference"]] = _p
for fp in board.GetFootprints():
    value, fpid = PARTS[fp.GetReference()]
    fp.SetValue(value)
    fp.SetFPID(pcbnew.LIB_ID(*fpid.split(":")))
    fp.SetPath(pcbnew.KIID_PATH("/" + sym_uuid(fp.GetReference())))
    for fname in ("Datasheet", "Description"):
        fp.SetField(fname, SCH_FIELDS.get(fp.GetReference(), {}).get(fname, ""))
for pd in parts["U1"].Pads():
    if not pd.GetNetname():
        pd.SetNet(net(esp_nc_net(pd.GetNumber())))

pcbnew.SaveBoard(OUT, board)

# tabla de librerias del proyecto (la huella de la ESP esta en caja_balanza.pretty)
LIBTABLE = """(fp_lib_table
  (version 7)
  (lib (name "caja_balanza") (type "KiCad") (uri "${KIPRJMOD}/caja_balanza.pretty") (options "") (descr "Project footprints"))
)
"""
with open(os.path.join(OUTDIR, "fp-lib-table"), "w", encoding="utf-8") as f:
    f.write(LIBTABLE)
PRO = os.path.join(OUTDIR, NAME + ".kicad_pro")
if not os.path.exists(PRO):
    with open(PRO, "w", encoding="utf-8") as f:
        f.write('{"meta": {"filename": "%s.kicad_pro", "version": 1}}\n' % NAME)
print("guardado", OUT, "| vias de costura:", len(stitch))
