# Version 2 de la placa de la caja: todas las borneras en una fila en el borde inferior y los
# componentes repartidos por zonas, cada una encima de su bornera:
#   izquierda  canales de los ventiladores (en columna, encima de FAN1 y FAN2)
#   centro     ESP32-S3 arriba y sniffer del display debajo (encima de DISPLAY)
#   derecha    alimentacion: modulo TPS63020 (U2) arriba con C2/C3 a su salida, F1 (BAT/SW),
#              C1/C8 a su entrada y F2/C4/C7 (ventiladores) abajo
# Cambios respecto a la v1 (gen_pcb.py, que no se modifica): el modulo TPS63020 va en la placa
# (U2) en lugar de la bornera J3, y R2/R4 (10k) van tumbadas. Esquema: gen_sch.py --v2.
# Ejecutar con el Python de KiCad, despues del esquema:
#   "C:\Program Files\KiCad\10.0\bin\python.exe" gen_sch.py --v2
#   "C:\Program Files\KiCad\10.0\bin\python.exe" gen_pcb_v2.py
# Coordenadas en mm, origen arriba a la izquierda de la placa.
import math
import os

import pcbnew

import kisexpr
from caja_parts import PARTS_V2 as PARTS, esp_nc_net, sym_uuid

HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.join(HERE, "v2")
NAME = "caja_balanza_v2"
OUT = os.path.join(OUTDIR, NAME + ".kicad_pcb")
FPLIB = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
OX, OY = 100.0, 100.0          # desplazamiento en la hoja de KiCad
W, H = 96.0, 53.0
YJ = H - 5.6                   # fila de borneras (pads); la entrada del cable da al borde
YG, YL = 38.6, 40.2            # rotulos: grupo y nombre de cada borne, encima de las borneras
YBAR = 39.5                    # barra +V_VENT (cara inferior) entre la alimentacion y los canales

# Modulo TPS63020 (26 x 18 mm): un pin en cada esquina, VIN/GND de entrada a la izquierda y
# VOUT/GND de salida a la derecha (mirandolo con el texto derecho). Distancia entre centros de
# los agujeros: MEDIR CON EL CALIBRE antes de pedir la placa (valores estimados de la foto).
TPS_W, TPS_H = 26.0, 18.0
TPS_DX, TPS_DY = 24.0, 12.4

mm = pcbnew.FromMM
os.makedirs(OUTDIR, exist_ok=True)


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


def write_esp_footprint():
    """ESP32-S3 Super Mini sobre dos tiras hembra 1x9 (filas a 15,24 mm), USB-C hacia arriba.
    Igual que en la v1; se escribe en la libreria de v2/ y se carga desde ahi."""
    left = ["43", "44", "1", "2", "3", "4", "5", "6", "7"]
    right = ["5V", "GND", "3V3", "13", "12", "11", "10", "9", "8"]
    bx, by = 9.0, 11.26                      # media placa: 18 x 22,52 mm
    mod = ['(footprint "%s"' % ESP_FP, '  (version 20241229)', '  (generator "gen_pcb_v2.py")',
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


TPS_FP = "TPS63020_Module_26x18"


def write_tps_footprint():
    """Modulo TPS63020 26 x 18 mm sobre 4 pines (uno por esquina). Pads: 1 VIN, 2 GND entrada,
    3 GND salida, 4 VOUT. Los puentes 3V3/4V2/5V y los pads EN/PS del modulo no van a la placa."""
    hx, hy, bx, by = TPS_DX / 2, TPS_DY / 2, TPS_W / 2, TPS_H / 2
    mod = ['(footprint "%s"' % TPS_FP, '  (version 20241229)', '  (generator "gen_pcb_v2.py")',
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
# Fila de borneras (giro 0: pad 1 a la izquierda, entrada del cable hacia el borde inferior).
# Orden: FAN1, FAN2 | DISPLAY | BAT, SW; cada una debajo de su zona.
A1, A2 = 6.5, 20.0                                   # pad + de FAN1 y FAN2 (canales a 13,5 mm)
XJ6, XJ1, XJ2 = 33.26, 51.58, 63.44
place("J6", *TB3, XJ6, YJ, 0, "DISPLAY", {1: "SL", 2: "DA", 3: "GND"})
place("J1", *TB2, XJ1, YJ, 0, "BAT", {1: "BAT_IN", 2: "GND"})
place("J2", *TB2, XJ2, YJ, 0, "SW", {1: "BAT_F", 2: "VBAT"})

# --- canales de ventilador, en columna de arriba abajo:
#     R 470 (GPIO -> base), R 10k (base -> masa), transistor, C 100n, diodo, bornera.
# Columnas: a = +V_VENT (C, diodo K, borne +), a+2,54 = base, a+5,08 = colector / borne -.
CH = ((A1, "GPIO5"), (A2, "GPIO6"))
YRB, YRP, YQ, YC, YD = 9.0, 14.0, 20.0, 26.0, 31.5
for i, (a, gpio) in enumerate(CH):
    k = i + 1
    fan = "FAN%dN" % k
    b = "B%d" % k
    place("J%d" % (4 + i), *TB2, a, YJ, 0, "FAN%d" % k, {1: "+V_VENT", 2: fan})
    place("R%d" % (2 * i + 1), *R762, a + 10.16, YRB, 180, "470", {1: gpio, 2: b})
    place("R%d" % (2 * i + 2), *R762, a + 2.54, YRP, 0, "10k", {1: b, 2: "GND"})
    place("Q%d" % k, *TO92, a, YQ, 0, "2N2222", {1: "GND", 2: b, 3: fan})          # E-B-C
    place("C%d" % (5 + i), *CDISC, a, YC, 0, "100n", {1: "+V_VENT", 2: fan})
    place("D%d" % k, *DO41, a, YD, 0, "1N5819", {1: "+V_VENT", 2: fan})            # K en a

# --- ESP en el centro (filas en x=33,38 GPIO y x=48,62 alimentacion), USB-C arriba
XE = 41.0
XL, XR = XE - 7.62, XE + 7.62
esp32s3_supermini("U1", XE, 13.66)
for nm, n in {"1": "GPIO1", "4": "GPIO4", "5": "GPIO5", "6": "GPIO6",
              "GND": "GND", "3V3": "+3V3"}.items():
    parts["U1"].FindPadByNumber(nm).SetNet(net(n))

# --- sniffer debajo de la ESP, encima de J6 (piezas tumbadas en columna)
YS1, YS2 = 36.5, 36.5 - 7.62
place("R5", *R762, 35.0, YS1, 90, "470", {1: "SL", 2: "GPIO1"})
place("R6", *R762, XJ6 + 5.08, YS1, 90, "470", {1: "DA", 2: "DA_R"})
place("D3", *DO41S, 41.6, YS1, 90, "BAT54/1N5819", {1: "GPIO4", 2: "DA_R"})      # banda abajo

# --- alimentacion (derecha)
# U2 girado 180: salida (VOUT, GND) a la izquierda, hacia C2/C3 y la ESP; entrada (VIN, GND) a la
# derecha, con C8 pegado a VIN. Guia de TI (10.1): condensadores de entrada y salida junto al
# regulador, pistas cortas y anchas, y las masas del modulo directas al plano.
UX, UY = 75.0, 14.0
tps63020_module("U2", UX, UY, 180)
XO, YO = UX - TPS_DX / 2, UY - TPS_DY / 2                 # pad VOUT
XI, YI = UX + TPS_DX / 2, UY - TPS_DY / 2                 # pad VIN
place("C2", *CP8, 56.0, 10.0, 90, "470u 10V", {1: "+3V3", 2: "GND"})              # salida de U2
place("C3", *CDISC, 51.5, 16.0, 0, "100n", {1: "+3V3", 2: "GND"})                # junto al pin 3V3
place("C8", *CDISC, 92.5, 20.0, 90, "10u", {1: "VBAT", 2: "GND"})                 # junto a VIN de U2
place("F1", *PTC, 55.0, 35.0, 0, "PTC 1,1A", {1: "BAT_IN", 2: "BAT_F"})           # entre BAT y SW
place("C1", *CP63, 75.0, 32.5, 0, "100u 16V", {1: "VBAT", 2: "GND"})              # VBAT a granel
place("F2", *PTC, 83.0, 31.5, 270, "PTC 0,75A", {1: "VBAT", 2: "+V_VENT"})         # rama ventiladores
place("C4", *CP8, 78.25, 44.5, 0, "470u 16V", {1: "+V_VENT", 2: "GND"})
place("C7", *CDISC, 86.0, 43.0, 0, "100n", {1: "+V_VENT", 2: "GND"})

for i, (x, y) in enumerate(((3.0, 37.0), (W - 3.5, H - 3.5), (W - 3.5, 3.5), (3.5, 3.5))):
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

# canales: base, colector y +V_VENT en columnas rectas (cara superior)
for k, (a, _) in enumerate(CH):
    fan, b = "FAN%dN" % (k + 1), "B%d" % (k + 1)
    track(b, F, SW, (a + 2.54, YRB), (a + 2.54, YRP), (a + 2.54, YQ))
    track(fan, F, VW, (a + 5.08, YQ), (a + 5.08, YC), (a + 5.08, YD), (a + 5.08, YJ))
    track(fan, F, VW, (a + 5.08, YD), (a + 10.16, YD))                       # anodo del diodo
    track("+V_VENT", F, PW, (a, YC), (a, YD), (a, YJ))

# GPIO6 -> R3 por la cara superior, entre el canal 2 y la fila de la ESP
xg6 = XL - 1.63
track("GPIO6", F, SW, (XL, 21.28), (xg6, 21.28 - 1.63), (xg6, YRB + xg6 - (A2 + 10.16)),
      (A2 + 10.16, YRB))
# GPIO5 -> R1 por la cara inferior, entre los dos canales
xg5 = 18.5
track("GPIO5", B, SW, (XL, 18.74), (XL - 1.74, 17.0), (xg5, 17.0),
      (xg5, YRB + xg5 - (A1 + 10.16)), (A1 + 10.16, YRB))

# sniffer: GPIO1 baja por la cara inferior, GPIO4 por la superior, dentro de la huella de la ESP
track("GPIO1", B, SW, (XL, 8.58), (35.0, 8.58 + 35.0 - XL), (35.0, YS2))
track("GPIO4", F, SW, (XL, 16.2), (44.0, 16.2), (44.0, YS1 - 2.4), (41.6, YS1))
track("DA_R", F, SW, (XJ6 + 5.08, YS2), (41.6, YS2))
track("SL", F, SW, (35.0, YS1), (35.0, YJ - (35.0 - XJ6)), (XJ6, YJ))
track("DA", F, SW, (XJ6 + 5.08, YS1), (XJ6 + 5.08, YJ))

# bateria -> F1 -> interruptor
track("BAT_IN", F, PW, (XJ1, YJ), (XJ1, 38.42), (55.0, 35.0))
track("BAT_F", F, PW, (60.0, 35.0), (XJ2, 35.0 + XJ2 - 60.0), (XJ2, YJ))

# VBAT: interruptor -> C1 -> F2 -> C8 -> VIN de U2 (por la derecha del modulo)
xv, xr = XJ2 + 5.08, 89.4
track("VBAT", F, PW, (xv, YJ), (xv, 32.0), (xv + 3.0, 29.0), (xr, 29.0), (xr, YI + 2.4), (XI, YI))
track("VBAT", F, PW, (75.0, 29.0), (75.0, 32.5))                                     # C1
track("VBAT", F, PW, (83.0, 29.0), (83.0, 31.5))                                     # F2
track("VBAT", F, PW, (xr, 20.0), (92.5, 20.0))                                       # C8

# +V_VENT: F2 -> C4/C7 -> barra por la cara inferior hasta los dos bornes +
track("+V_VENT", B, PW, (86.0, 43.0), (86.0, YBAR), (83.0, YBAR), (78.25, YBAR), (A2, YBAR),
      (A1, YBAR), (A1, YJ))
track("+V_VENT", B, PW, (83.0, YBAR), (83.0, 36.5))                                  # F2
track("+V_VENT", B, PW, (78.25, YBAR), (78.25, 44.5))                                # C4
track("+V_VENT", B, PW, (A2, YBAR), (A2, YJ))

# 3V3: VOUT de U2 -> C2 -> C3 -> pin 3V3 de la ESP (pista corta)
track("+3V3", F, VW, (XO, YO), (XO, 10.0), (56.0, 10.0), (51.5, 10.0), (XR + 1.42, 10.0), (XR, 8.58))
track("+3V3", F, VW, (51.5, 10.0), (51.5, 16.0))                                     # C3

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
for gx in range(0, int(W / 5)):
    for gy in range(0, int(H / 5)):
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


for d in ("D1", "D2", "D3"):
    for it in list(parts[d].GraphicalItems()):
        if isinstance(it, pcbnew.PCB_TEXT) and it.GetText() == "K" and it.GetLayer() == pcbnew.F_SilkS:
            parts[d].Remove(it)

# electroliticos: rotulo fuera del circulo
REF_AT = {"C1": (76.25, 37.0, 0), "C2": (56.0, 13.3, 0), "C4": (80.0, 39.5, 0)}

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
    if ref[0] == "Q":
        cy -= 1.6
    elif ref in REF_AT:
        cx, cy, rot = REF_AT[ref]
    rf.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8)))
    rf.SetTextThickness(mm(0.12))
    rf.SetPosition(P(cx, cy))
    rf.SetTextAngleDegrees(rot)

# nombre de cada borne, encima de su tornillo (cara superior) y debajo de su pad (cara inferior)
BORNES = [(A1, ("FAN1+", "FAN1-")), (A2, ("FAN2+", "FAN2-")), (XJ6, ("SL", "DA", "GND")),
          (XJ1, ("BAT+", "BAT-")), (XJ2, ("SW", "SW"))]
for x, names in BORNES:
    for i, s_ in enumerate(names):
        text(s_, x + 5.08 * i, YL, 0.8, 0, pcbnew.F_SilkS, 0)
        text(s_, x + 5.08 * i, YJ + 3.2, 0.8, 0, pcbnew.B_SilkS, 0)

L = [  # texto, x, y, tamano, giro, alineacion (-1 izq, 0 centro, 1 der)
    ("VENTILADORES", (A1 + A2 + 5.08) / 2, YG, 0.8, 0, 0), ("DISPLAY", XJ6 + 5.08, YG, 0.8, 0, 0),
    ("BATERIA", XJ1 + 2.54, YG, 0.8, 0, 0), ("INTERRUPTOR", XJ2 + 2.54, YG, 0.8, 0, 0),
    ("ESP32-S3", XE, 12.4, 1.0, 0, 0), ("Super Mini", XE, 14.2, 0.9, 0, 0), ("USB-C", XE, 5.2, 0.8, 0, 0),
    ("FAN1", A1 + 5.08, 5.0, 0.8, 0, 0), ("FAN2", A2 + 5.08, 5.0, 0.8, 0, 0),
    ("TPS63020", UX, UY - 1.0, 1.0, 0, 0), ("salida en 3V3", UX, UY + 1.0, 0.8, 0, 0),
    ("ENTRADA", XI - 1.2, UY + 2.0, 0.8, 90, 0), ("SALIDA", XO + 1.2, UY + 2.0, 0.8, 90, 0),
]
for s_, x, y, sz, rot, just in L:
    text(s_, x, y, sz, rot, pcbnew.F_SilkS, just)

# cara inferior: titulo entre las filas de la ESP
LB = [("CAJA BALANZA", XE + 1.0, 6.5), ("ESP32-S3", XE + 1.0, 8.5), ("Super Mini", XE + 1.0, 10.5),
      ("sniffer TM1640", XE + 1.0, 21.0), ("+ ventiladores", XE + 1.0, 23.0), ("v2  2026-10", XE + 1.0, 14.5)]
for s_, x, y in LB:
    text(s_, x, y, 0.8, 0, pcbnew.B_SilkS, 0)

# lagrimas (teardrops) en todos los pads con pista; kicad-cli las genera al rellenar los planos
for fp in board.GetFootprints():
    for pd in fp.Pads():
        if pd.GetNetname():
            pd.SetTeardropsEnabled(True)
            pd.SetTeardropCurved(True)

# ---------------------------------------------------------------- enlace con el esquema
# Esquema de la v2: v2/caja_balanza_v2.kicad_sch (generar antes con gen_sch.py --v2). De ahi se
# copian Datasheet y Description de cada pieza.
SCH = os.path.join(OUTDIR, NAME + ".kicad_sch")
if not os.path.exists(SCH):
    raise SystemExit("falta %s: ejecutar antes gen_sch.py --v2" % SCH)
with open(SCH, encoding="utf-8") as f:
    sch = f.read()

SCH_FIELDS = {}
for _s in kisexpr.find(kisexpr.parse(sch), "symbol"):
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
