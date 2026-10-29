# Revisa la geometria de las pistas segun las practicas habituales de ruteo (IPC-2221 / IPC-7351):
#   1. Segmentos solo a 0, 45 o 90 grados (ruteo octilineal).
#   2. Ninguna esquina de 90 grados: los cambios de direccion se hacen con dos tramos a 45.
#   3. Ningun angulo agudo (< 90) entre pistas del mismo nodo: atrapa acido al grabar.
#   4. Ancho minimo por clase: potencia >= 0,8 mm, senal >= 0,3 mm.
# Las uniones en T (una pista que llega a otra recta) a 90 grados se aceptan.
# Uso: "C:\Program Files\KiCad\10.0\bin\python.exe" check_routing.py caja_balanza.kicad_pcb
import math
import sys
from collections import defaultdict

import pcbnew

POWER = {"VBAT", "BAT_IN", "BAT_F", "+V_VENT", "+3V3", "FAN1N", "FAN2N"}
TOL = 0.01    # mm para considerar dos extremos el mismo punto

b = pcbnew.LoadBoard(sys.argv[1])
segs = []
for t in b.GetTracks():
    if t.GetClass() != "PCB_TRACK":
        continue
    a, e = t.GetStart(), t.GetEnd()
    segs.append((t.GetNetname(), t.GetLayer(), pcbnew.ToMM(t.GetWidth()),
                 (pcbnew.ToMM(a.x), pcbnew.ToMM(a.y)), (pcbnew.ToMM(e.x), pcbnew.ToMM(e.y))))

problems = []


def where(p):
    return "(%.2f, %.2f)" % (p[0] - 100, p[1] - 100)


for n, lay, w, a, e in segs:
    ang = math.degrees(math.atan2(e[1] - a[1], e[0] - a[0])) % 45
    if min(ang, 45 - ang) > 0.6:
        problems.append("no octilineal  %-8s %s -> %s" % (n, where(a), where(e)))
    need = 0.8 if n.lstrip("/") in POWER else 0.3
    if w + 1e-6 < need:
        problems.append("ancho %.2f < %.2f  %-8s %s" % (w, need, n, where(a)))

# direcciones que salen de cada nodo (extremos y puntos interiores donde llega otra pista)
nodes = defaultdict(list)
for i, (n, lay, w, a, e) in enumerate(segs):
    for p, q in ((a, e), (e, a)):
        key = (n, lay, round(p[0] / TOL), round(p[1] / TOL))
        nodes[key].append((math.atan2(q[1] - p[1], q[0] - p[0]), i, p))
for i, (n, lay, w, a, e) in enumerate(segs):            # T: extremo sobre el interior de otra
    for j, (n2, lay2, w2, a2, e2) in enumerate(segs):
        if i == j or n != n2 or lay != lay2:
            continue
        for p in (a2, e2):
            dx, dy = e[0] - a[0], e[1] - a[1]
            L = dx * dx + dy * dy
            if L == 0:
                continue
            u = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L
            if 0.001 < u < 0.999 and math.hypot(a[0] + u * dx - p[0], a[1] + u * dy - p[1]) < TOL:
                key = (n, lay, round(p[0] / TOL), round(p[1] / TOL))
                for q in (a, e):
                    nodes[key].append((math.atan2(q[1] - p[1], q[0] - p[0]), i, p))

for (n, lay, _, _), dirs in nodes.items():
    dirs = {(round(math.degrees(d), 1), i): (d, p) for d, i, p in dirs}
    items = list(dirs.values())
    if len(items) < 2:
        continue
    p = items[0][1]
    for x in range(len(items)):
        for y in range(x + 1, len(items)):
            diff = abs(math.degrees(items[x][0] - items[y][0])) % 360
            diff = min(diff, 360 - diff)
            if diff < 89.4:
                problems.append("angulo agudo %.0f  %-8s %s" % (diff, n, where(p)))
            elif diff < 90.6 and len(items) == 2:
                problems.append("esquina de 90  %-8s %s" % (n, where(p)))

print("segmentos revisados:", len(segs))
for s in sorted(set(problems)):
    print("  " + s)
print("problemas:", len(set(problems)))
sys.exit(1 if problems else 0)
