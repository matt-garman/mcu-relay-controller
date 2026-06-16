
= CD4053/TMUX4053 Wiring

== CD4053 Overview

    PIN 1 : B1
    PIN 2 : B0
    PIN 3 : C1
    PIN 4 : CCOM
    PIN 5 : C0
    PIN 6 : INH
    PIN 7 : VEE
    PIN 8 : GND
    PIN 9 : CSEL
    PIN 10: BSEL
    PIN 11: ASEL
    PIN 12: A0
    PIN 13: A1
    PIN 14: ACOM
    PIN 15: BCOM
    PIN 16: VDD

INH (inhibit) pin not used: tied to GND
VEE (negative supply): for single-supply, tied to GND

xSEL LOW  -> x0 active (xCOM tied to x0)
xSEL HIGH -> x1 active (xCOM tied to x1)


== Circuit Net Naming Conventions

    JOU = output jack, i.e. final output signal to next device/amp
    DRY = buffered input signal
    FXO = effect output/return
    FXN = effect input/send


== CD4053 vs TMUX4053 Notes

    - CD4053 available in through-hole and surface-mount
    - TMUX4053 surface-mount only
    - TMUX4053 can use lower-voltage logic true (e.g. CMOS, TTL)
    - CD4053 needs logic true to be at VDD
    - VDD will be effect volate 9-18v
    - Control from MCU running at 5v: can't be used directly on
      CD4053
    - Control switches held high via pullup resistor to VDD
    - Control switches gated to GND via mosfet
    - MCU signaling to GND-gate mosfets
    - MCU high -> Switch signal low, MCU low -> Switch sigal high
    - Mosfet scheme not needed for TMUX4053, but retained for
      schematic/wiring consistency across parts


== Initial Scheme - No Muting

Switch A not used

BCOM = JOU
B0 = FXO
B1 = C1

CCOM = DRY
C0 = FXN
C1 = B1

CTL = single control bus for both BSEL and CSEL
MCU = MCU signal to mosfet gate

Truth Table

| MCU | CTL | BCOM | JOU | CCOM | DRY | FXN      | FXO      | program state |
| --- | --- | ---  | --- | ---  | --- | ---      | ---      | ---           |
| 0   | 1   | B1   | DRY | C1   | JOU | floating | floating | BYPASS        |
| 1   | 0   | B0   | FXO | C0   | FXN | DRY      | JOU      | ENGAGE        |


== Improved Scheme With Muting

ACOM = FXN
A0 = DRY
A1 = GND

BCOM = GND
B0 = floating
B1 = FXO

CCOM = JOU
C0 = FXO
C1 = DRY

CTL1 = combined control bus for ASEL and BSEL
CTL2 = control bus for CSEL

MCU1 = MCU signal to mosfet gate for CTL1
MCU2 = MCU signal to msofet gate for CTL2


| MCU1 | CTL1 | MCU2 | CTL2 | ACOM | BCOM | CCOM | DRY      | FXN | FXO | JOU | program state |
| ---  | ---  | ---  | ---  | ---  | ---  | ---  | ---      | --- | --- | --- | ---           |
| 0    | 1    | 0    | 1    | A1   | B1   | C1   | JOU      | GND | GND | DRY | BYPASS        |
| 0    | 1    | 1    | 0    | A1   | B1   | C0   | floating | GND | GND | FXO | MUTE          |
| 1    | 0    | 0    | 1    | A0   | B0   | C1   | FXN+JOU  | DRY |     | DRY | *invalid*     |
| 1    | 0    | 1    | 0    | A0   | B0   | C0   | FXN      | DRY | FXO | FXO | ENGAGE        |


| STATE  | MCU1, MCU2 |
| ---    | ---        |
| BYPASS | 0, 0       |
| MUTE   | 0, 1       |
| ENGAGE | 1, 1       |
| MUTE   | 0, 1       |
| BYPASS | 0, 0       |

