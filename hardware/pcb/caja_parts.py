# Fuente unica de referencia, valor y huella: la leen gen_sch.py (esquema) y gen_pcb.py (placa).
import uuid

NS = uuid.UUID("6f1c2a52-3b7e-4d0e-9a51-0c0ffee0ba1a")


def sym_uuid(ref):
    """UUID fijo de cada simbolo: enlaza el simbolo con su huella (ruta '/<uuid>')."""
    return str(uuid.uuid5(NS, "sym:" + ref))


def any_uuid(tag):
    return str(uuid.uuid5(NS, tag))


R762 = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal"
R254V = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P2.54mm_Vertical"
CDISC = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
PTC = "Capacitor_THT:C_Disc_D7.5mm_W5.0mm_P5.00mm"
CP63 = "Capacitor_THT:CP_Radial_D6.3mm_P2.50mm"
CP8 = "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm"
DO41 = "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal"
DO41S = "Diode_THT:D_DO-41_SOD81_P7.62mm_Horizontal"
TO92 = "Package_TO_SOT_THT:TO-92_Inline_Wide"
TB = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-%d-5.08_1x0%d_P5.08mm_Horizontal"
HOLE = "MountingHole:MountingHole_3.2mm_M3"
ESP = "caja_balanza:ESP32-S3_SuperMini_Socket"

PARTS = {
    "U1": ("ESP32-S3 Super Mini", ESP),
    "J1": ("BAT", TB % (2, 2)), "J2": ("SW", TB % (2, 2)), "J3": ("TPS63020", TB % (4, 4)),
    "J4": ("FAN1", TB % (2, 2)), "J5": ("FAN2", TB % (2, 2)), "J6": ("DISPLAY", TB % (3, 3)),
    "F1": ("MF-R110", PTC), "F2": ("MF-R075", PTC),
    "C1": ("100u 16V", CP63), "C8": ("10u", CDISC), "C2": ("470u 10V", CP8), "C3": ("100n", CDISC),
    "C4": ("470u 16V", CP8), "C5": ("100n", CDISC), "C6": ("100n", CDISC), "C7": ("100n", CDISC),
    "R1": ("470", R762), "R2": ("10k", R254V), "R3": ("470", R762), "R4": ("10k", R254V),
    "R5": ("470", R762), "R6": ("470", R762),
    "D1": ("1N5819", DO41), "D2": ("1N5819", DO41), "D3": ("1N5819", DO41S),
    "Q1": ("2N2222", TO92), "Q2": ("2N2222", TO92),
    "H1": ("M3", HOLE), "H2": ("M3", HOLE), "H3": ("M3", HOLE), "H4": ("M3", HOLE),
}

# v2: modulo TPS63020 en la placa (U2) en lugar de la bornera J3, y R2/R4 tumbadas
TPS = "caja_balanza:TPS63020_Module_26x18"
PARTS_V2 = {k: v for k, v in PARTS.items() if k != "J3"}
PARTS_V2.update({"U2": ("TPS63020 module", TPS), "R2": ("10k", R762), "R4": ("10k", R762)})
# v3: placa de la v1 (70 x 50, R2/R4 de pie) con el modulo TPS63020 en la placa
PARTS_V3 = dict(PARTS_V2)
PARTS_V3.update({"R2": ("10k", R254V), "R4": ("10k", R254V)})

DATASHEET = {
    "U2": "https://www.ti.com/lit/ds/symlink/tps63020.pdf",
    "D1": "http://www.vishay.com/docs/88525/1n5817.pdf", "D2": "http://www.vishay.com/docs/88525/1n5817.pdf",
    "D3": "http://www.vishay.com/docs/88525/1n5817.pdf",
    "Q1": "https://www.onsemi.com/pub/Collateral/PN2222-D.PDF", "Q2": "https://www.onsemi.com/pub/Collateral/PN2222-D.PDF",
    "U1": "https://www.espboards.dev/esp32/esp32-s3-super-mini/",
}


def datasheet(ref):
    return DATASHEET.get(ref, "~")


# pines de la ESP sin conectar: nombre de red que les da KiCad desde el esquema
ESP_PIN_NAMES = {"43": "TX{slash}IO43", "44": "RX{slash}IO44", "5V": "5V"}


def esp_nc_net(num):
    name = ESP_PIN_NAMES.get(num, "IO" + num)
    return "unconnected-(U1-%sPad%s)" % ("" if num == "5V" else name + "-", num)
