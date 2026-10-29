# Box PCB (v2): terminal blocks in one row, TPS63020 on the board

Same circuit as v1 (parent folder) with three changes:

1. **All terminal blocks in one row** on the bottom edge, with the wire entry facing outward.
2. **The TPS63020 module is mounted on the board** (U2), like the ESP, instead of terminal block J3.
3. R2 and R4 (10k) lie flat, like the other resistors.

2-layer board, **96 × 53 mm** (v1 is 70 × 50 mm).

```
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  FAN1     FAN2   │  ESP32-S3 (USB-C up)      │ C2  ┌─ U2 TPS63020 ──────┐  C8 │
 │  R 470    R 470  │                           │ C3  │ OUTPUT     INPUT   │     │
 │  R 10k    R 10k  │                           │     └────────────────────┘     │
 │  Q1       Q2     │                           │                C1   F2         │
 │  C5       C6     │   sniffer: R5 R6 D3       │   F1                           │
 │  D1       D2     │                           │                   C4   C7      │
 │ [FAN1] [FAN2]    │      [DISPLAY]            │ [BAT] [SW]                     │
 └──────────────────────────────────────────────────────────────────────────────┘
```

| Terminal block | Terminals (left → right) | Goes to |
|---|---|---|
| J4 | FAN1+, FAN1- | Fan 1 |
| J5 | FAN2+, FAN2- | Fan 2 |
| J6 DISPLAY | SL, DA, GND | Display connector |
| J1 BATERIA | BAT+, BAT- | Scale battery |
| J2 INTERRUPTOR | SW, SW | Panel switch |

Each terminal has its name printed on both sides of the board: above the screw and below the pad.
The silkscreen is in Spanish (BATERIA = battery, INTERRUPTOR = switch, VENTILADORES = fans,
ENTRADA = input, SALIDA = output).

## TPS63020 module (U2)

- It is **rotated 180°** relative to how its own silkscreen reads: the **output (OUT, GND) is on
  the left**, toward the ESP, and the **input (VIN, GND) on the right**. The board shows the
  module outline and the SALIDA / ENTRADA, VIN and OUT labels; the square pad is VIN.
- It is mounted with **4 male pins** soldered into the module's corner holes. The pins are either
  soldered straight to the board, or plugged into 4 loose female sockets if it must be removable.
- **Output jumper on 3V3** (the module's 3V3 / 4V2 / 5V pads). Check it before plugging in the
  ESP: 4V2 or 5V will damage it.
- EN and PS are not connected: the module already handles them (it starts enabled). If it gives no
  output when tested, tie EN to VIN on the module itself.
- The recommendations of TI's TPS63020 datasheet (section 10.1) are followed. The ones internal to
  the regulator (inductor, capacitors right at the chip, control ground and FB divider) are
  already met by the module. On this board:
  - C8 (10 µF) sits right at the module's VIN, and C2 (470 µF) and C3 (100 nF) at its output and
    at the ESP's 3V3 pin.
  - The 3V3 track is about 15 mm long (in v1 it was about 70 mm through the terminal block and the
    wires).
  - Wide tracks: VBAT 1.0 mm, 3V3 0.8 mm.
  - Both module grounds go straight into the ground plane, which covers both layers.
  - The display signals run far from the module.

**Measure before ordering:** the centre-to-centre distance between the module's holes. The board
uses **24.0 mm** (horizontal) × **12.4 mm** (vertical), estimated from a photo. If they do not
match, change `TPS_DX` and `TPS_DY` in `gen_pcb_v2.py` and regenerate.

## Files

| File | Purpose |
|---|---|
| `fab/caja_balanza_v2_gerber_JLCPCB.zip` | Gerbers and drill files for JLCPCB / PCBWay |
| `esquema_caja_balanza_v2.pdf` | v2 schematic |
| `caja_balanza_v2.kicad_pro` | KiCad 10 project (linked board and schematic) |
| `BOM.csv` | Bill of materials |
| `render_*.png` | 3D views (the TPS module has no 3D model: only its outline is shown) |

## Verification

| Check | Result |
|---|---|
| Schematic ERC | 0 errors, 0 warnings |
| Board DRC | 0 errors, 0 warnings, 0 unconnected pads |
| Schematic ↔ board parity | 0 differences |
| `check_routing.py` | 0 problems (65 segments): no 90° corners, no acute angles, widths per class |

Routing rules, the other measurements to check, assembly and the first test are the same as in
the v1 README. In the first test (without the ESP), measure 3.3 V between the 3V3 and GND pins of
the ESP socket.

## Regenerate (from the parent folder)

```
set KP="C:\Program Files\KiCad\10.0\bin"
%KP%\python.exe gen_sch.py --v2
%KP%\python.exe gen_pcb_v2.py
cd v2
%KP%\kicad-cli.exe sch erc caja_balanza_v2.kicad_sch
%KP%\kicad-cli.exe pcb drc --refill-zones --save-board --schematic-parity caja_balanza_v2.kicad_pcb
%KP%\python.exe ..\check_routing.py caja_balanza_v2.kicad_pcb
```
