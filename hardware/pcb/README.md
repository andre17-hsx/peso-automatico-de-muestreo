# Box PCB (v1)

2-layer, 70 × 50 mm board for the ESP32-S3 Super Mini, the display sniffer and the two fans. It
follows the box wiring diagram; the TPS63020 module stays off the board and connects through
terminal block J3.

Other versions: [v2](v2/) (all terminal blocks in one row, TPS63020 on the board, 96 × 53 mm) and
[v3](v3/) (this board with the TPS63020 on the board, 70 × 50 mm).

| File | Purpose |
|---|---|
| `fab/caja_balanza_gerber_JLCPCB.zip` | What gets uploaded to JLCPCB / PCBWay (Gerbers + drill) |
| `BOM.csv` | Bill of materials |
| `esquema_caja_balanza.pdf` | Schematic (A4: 5 blocks + notes) |
| `caja_balanza.kicad_pro` | KiCad 10 project: linked `.kicad_sch` schematic and `.kicad_pcb` board |
| `caja_parts.py` | Single table of references, values and footprints (used by both generators) |
| `gen_sch.py`, `gen_pcb.py` | Generate the schematic and the board from scratch (see "Regenerate") |
| `check_routing.py` | Checks track angles and widths |
| `render_*.png` | 3D views |

## Verification

| Check | Result |
|---|---|
| Schematic ERC (KiCad) | 0 errors, 0 warnings |
| Board DRC (KiCad) | 0 errors, 0 warnings, 0 unconnected pads |
| Schematic ↔ board parity (KiCad) | 0 differences: same parts, values, footprints and nets |
| `check_routing.py` | 0 problems (61 segments) |

## Regenerate

```
set KP="C:\Program Files\KiCad\10.0\bin"
%KP%\python.exe gen_sch.py
%KP%\python.exe gen_pcb.py
%KP%\kicad-cli.exe sch erc caja_balanza.kicad_sch
%KP%\kicad-cli.exe pcb drc --refill-zones --save-board --schematic-parity caja_balanza.kicad_pcb
%KP%\python.exe check_routing.py caja_balanza.kicad_pcb
```

The schematic goes first: the board copies each part's description and datasheet from it.
`gen_sch.py --v2` and `gen_sch.py --v3` generate the schematics of the other versions.

## Routing rules

- Tracks at 0°, 45° and 90° only (octilinear). 90° corners are cut with an automatic 45° segment
  (`chamfer()` in `gen_pcb.py`). No acute angles between tracks (they trap etchant). Branches
  join as 90° T-junctions.
- Widths (IPC-2221, 1 oz outer copper): power 1.0 mm (VBAT, +V_VENT, battery), 3V3 and fan return
  0.8 mm, signals 0.3 mm. At ≤ 0.5 A there is plenty of margin.
- Minimum clearance 0.25 mm (below 15 V, IPC-2221 asks for 0.1 mm).
- Ground plane on both layers, stitched with vias every 5 mm; thermal reliefs on ground pads so
  they can be soldered.
- Teardrops on every pad with a track.
- `check_routing.py` checks what KiCad's DRC does not (angles, octilinearity and widths per
  class):
  `"C:\Program Files\KiCad\10.0\bin\python.exe" check_routing.py caja_balanza.kicad_pcb`

## Before ordering: measure with calipers

1. ESP rows 15.24 mm apart: **measured and correct** (2026-10-29). If the module changes, the
   spacing is in `write_esp_footprint()` in `gen_pcb.py` (`±7.62`).
2. **Electrolytic diameter and pitch**: the 470 µF C2 and C4 use a Ø8 mm footprint with 3.5 mm
   lead spacing; C1 is Ø6.3 mm at 2.5 mm. A 470 µF 16 V cap of Ø10 mm will not fit.
3. **PTC**: leads 5.0–5.1 mm apart (MF-R110 and MF-R075 meet this).
4. **Terminal blocks**: 5.08 mm pitch (KF301 / MKDS 1.5); 3.5 mm ones will not fit.

## Ordering from JLCPCB

Upload the zip; keep 2 layers, 1.6 mm, HASL, 1 oz copper. The size (70 × 50 mm) is detected
automatically. No assembly service is needed: everything is through-hole.

## Assembly (lowest to tallest)

1. Flat resistors (R1, R3, R5, R6), diodes (band toward the square pad) and standing resistors
   (R2, R4).
2. Ceramic capacitors, PTCs, transistors (square = E; check the 2N2222 E-B-C order with a
   multimeter: some P2N2222A are C-B-E and are then mounted rotated 180°).
3. ESP female headers, electrolytics (long lead to **+**) and terminal blocks with the wire entry
   toward the board edge.

## First test, without the ESP plugged in

1. Continuity: there must be no short between `VIN` and `GND`, `3V3` and `GND`, or a fan `+` and
   `GND`.
2. Connect the TPS to J3 (VIN, GND, 3V3) and the battery to J1, with the switch on J2.
3. Measure 3.3 V between the `3V3` and `GND` pins of the ESP socket (right row, 3rd and 2nd from
   the USB end).
4. Switch off, plug in the ESP with the USB-C toward the top edge, and switch on again.

Never connect the ESP's USB to the PC while the battery is switched on.

The silkscreen labels on the board are in Spanish (BAT, INTERRUPTOR = switch, VENTILADORES = fans,
salida = output, entrada = input).
