# Box PCB (v3): v1 with the TPS63020 on the board

This is v1 (parent folder, 70 × 50 mm) with the **TPS63020 module mounted on the board** (U2)
instead of the 4-way terminal block J3, as in v2. **It is the same size as v1: 70 × 50 mm.**

| | v1 | v2 | v3 |
|---|---|---|---|
| Size | 70 × 50 mm | 96 × 53 mm | **70 × 50 mm** |
| TPS63020 module | Off-board, through terminal block J3 and wires | On the board | On the board |
| 3V3 track to the ESP | ~45 mm + wires to the module | ~15 mm | **~15 mm** |
| Terminal blocks | On three edges | All in one row at the bottom | On three edges (as in v1) |
| R2 / R4 (10k) | Standing | Lying flat | Standing |

## What changes from v1

```
 ┌────────────────────────────────────────────────────────────────┐
 │ o  R5    ┌ESP32-S3┐  C3   o  ┌──── U2 TPS63020 ─────────────┐  │
 │    D3    │        │          │ OUT                      VIN │  │
 │    R6    │        │  C2      │ GND                      GND │  │
 │[J6]      └────────┘          └──────────────────────────────┘  │
 │ DISPLAY  R1 R2  R3 R4        C1   C8                   [J2 SW] │
 │          Q1     Q2           F2   F1                           │
 │          C5 D1  C6 D2                                  [J1 BAT]│
 │ o       [FAN1] [FAN2]  C4  C7                               o  │
 └────────────────────────────────────────────────────────────────┘
```

- **Same as v1:** the ESP, the sniffer, both fan channels with their FAN1 and FAN2 terminal
  blocks, and the display terminal block (J6).
- **U2 at the top right**, where the BAT and SW terminal blocks used to be. It is **rotated 180°**
  relative to how its own silkscreen reads: the **output (OUT, GND) is on the left**, right next
  to C3/C2 and the ESP's 3V3 pin; the **input (VIN, GND) is on the right**. The square pad is VIN.
- **BAT (J1) and SWITCH (J2) move to the right edge**, where J3 was, with the wire entry toward
  the edge. F1 sits between them.
- VBAT: J2 feeds C8, C1 and F2 on the top layer, and reaches U2's input on the bottom layer,
  underneath the module.
- The top-right mounting hole moves to the top centre (x = 39 mm), because the module takes that
  corner.

## TPS63020 module (U2)

Same notes as in v2:
- It is mounted with **4 male pins** in the module's corner holes, soldered straight to the board
  or plugged into 4 loose female sockets.
- **Output jumper on 3V3.** 4V2 or 5V will damage the ESP.
- EN and PS are not connected. If it gives no output, tie EN to VIN on the module.
- **Measure before ordering:** the centre-to-centre distance between the holes. The board uses
  **24.0 × 12.4 mm**, estimated from a photo. If they do not match, change `TPS_DX` and `TPS_DY` in
  `gen_pcb_v3.py` and regenerate.

The silkscreen labels are in Spanish (INTERRUPTOR = switch, ENTRADA = input, SALIDA = output).

## Files

| File | Purpose |
|---|---|
| `fab/caja_balanza_v3_gerber_JLCPCB.zip` | Gerbers and drill files for JLCPCB / PCBWay |
| `esquema_caja_balanza_v3.pdf` | Schematic (rev 3.0) |
| `caja_balanza_v3.kicad_pro` | KiCad 10 project (linked board and schematic) |
| `BOM.csv` | Bill of materials |
| `render_*.png` | 3D views (the TPS module has no 3D model: only its outline is shown) |

## Verification

| Check | Result |
|---|---|
| Schematic ERC | 0 errors, 0 warnings |
| Board DRC | 0 errors, 0 warnings, 0 unconnected pads |
| Schematic ↔ board parity | 0 differences |
| `check_routing.py` | 0 problems (63 segments): no 90° corners, no acute angles, widths per class |

## Regenerate (from the parent folder)

```
set KP="C:\Program Files\KiCad\10.0\bin"
%KP%\python.exe gen_sch.py --v3
%KP%\python.exe gen_pcb_v3.py
cd v3
%KP%\kicad-cli.exe sch erc caja_balanza_v3.kicad_sch
%KP%\kicad-cli.exe pcb drc --refill-zones --save-board --schematic-parity caja_balanza_v3.kicad_pcb
%KP%\python.exe ..\check_routing.py caja_balanza_v3.kicad_pcb
```
