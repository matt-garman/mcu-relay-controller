# Remaining work toward textbook reference quality

Status note (2026-06-16): the firmware and test/validation suite have been
meta-reviewed. The firmware has no known correctness defects; `make test`
passes clean across all three output variants and both MCU families (ATtiny13a
and tinyx5), with 100% golden-model line coverage. The items below are
deferrable polish and credibility work — none are bugs. Anything that *is* a
bug gets fixed immediately, not parked here.

Completed since the previous revision (kept here only as a record; safe to
delete): README.md; design doc renamed to `DESIGN_DOCUMENTATION.adoc`; timer
formula `static_assert`; `__attribute__((OS_main))`; MISRA-C:2012 analysis +
`MISRA_COMPLIANCE.md` + `make analyze-misra` gate; KLEE support
(`make test-symbolic-klee`, `test_symbolic.c -DUSE_KLEE`).

---

## Tier 1 — high impact, low effort (do first)

**No CI/CD pipeline.** Still the most conspicuous gap for a project making
Boss-grade verification claims: a green "build passing" badge proves the suite
is reproducible off the developer's machine. Note this is now bigger than the
original "6-line apt + make test" estimate — to be honest it must exercise the
real matrix: all three variants × {ATtiny13a, tinyx5}, plus the analysis gates
(clang-tidy, cppcheck, MISRA). simavr is in Ubuntu 24.04's default repos. KLEE
can be a separate optional job (see Tier 3).

**Design-doc resource-utilization section.** The suite now *measures* the
numbers; they belong in the design doc as a resource table. Measured today:
ATtiny13a cd4053 ≈ 470 B flash (~46% of 1 KB), 8 B peak stack, 56 B SRAM margin
(64 B total); tinyx5 relay ≈ 524 B flash, 10 B peak stack, 246 B margin (256 B
total). For an engineer evaluating this as a reference base, headroom is a key
parameter. Pull flash from `avr-size` and stack from the HWM test output.

**Fix stale references / editorial note.** (a) This file and any prose still
referencing `attiny13_bypass.md` / `attiny13_bypass.c` must point at
`DESIGN_DOCUMENTATION.adoc` / `bypass_core.c` + drivers. (b) `DESIGN_
DOCUMENTATION.adoc` line ~464 still reads like an authoring note ("It shows the
counter climbing to 7…"); reword as a normal lead-in. Both are trivial.

---

## Tier 2 — closes verification / traceability gaps

**Datasheet citations in the design doc.** The sleep-wakeup §7.3 cite lives in
`bypass_core.c`; the *design doc itself* currently cites no datasheet sections.
Each load-bearing decision should trace to a page/section: WDT ~16 ms post-reset
window; WDTON always-on; internal-RC ±10%; Timer0 CTC formula; BOD level.

**Minimum-tap-interval regression test.** The design states the fastest
recognized tap interval is `PRESSED_THRESH + RELEASE_THRESH = 33 ms`. The 33 ms
figure appears only in test comments; there is no test that drives two presses
exactly 33 ms apart and asserts both register. This is the natural guard for any
future threshold change and maps directly to the "fast taps recognized" row of
the traceability matrix.

**`-fstack-usage` static bound.** Complements the runtime HWM test with a
compile-time structural upper bound that doesn't depend on a test exercising the
deepest path. Add the flag to the firmware build and optionally assert the
summed worst-case frame in `make test`.

**Flash-utilization budget assertion.** Capture `avr-size` output and fail the
build if flash exceeds a threshold (e.g. 90% of 1 KB). Firmware is ~46% today; a
future accidental bloat that eats the headroom would otherwise pass silently.
The firmware analog of a bundle-size budget.

---

## Tier 3 — platinum-level / nice-to-have

**Hardware-validation procedure.** The single largest residual verification gap
is structural: simavr cannot model the ATtiny13a watchdog system reset (only the
tinyx5 family), so the headline WDT-recovery guarantee on the *primary* part is
asserted by analogy, not direct simulation. Document a bench procedure: scope
PB1/PB2, artificially stop the ISR, confirm the device resets to BYPASS within
the WDT window; plus power-on glitch and BOD behavior. Bridges to the
manufacturing item below.

**CBMC formal analysis.** Prove properties of the *actual* C source rather than
a re-implementation: `cbmc --unwind 50` on the debounce path. Platinum-level
differentiator; CBMC ships as a Debian package.

**KLEE in CI.** `test_symbolic.c` already supports `-DUSE_KLEE` and there is a
`test-symbolic-klee` target; a CI job (klee/klee Docker image) would prove the
symbolic path is actually exercised, not merely compilable.

**tinyAVR 2-Series (ATtiny202) support.** The ATtiny202 is the natural
next-generation successor to the ATtiny13a: 8-pin SOT-23 or DFN-8, 2 KB flash,
256 B SRAM, capable at 3.3 V/5 V. However it is based on the AVR8X architecture
(tinyAVR 2-Series), a complete peripheral redesign — the ISA is
backward-compatible but virtually every register differs: GPIO is
`PORTA.DIR`/`PORTA.OUT`/`PORTA.IN` instead of `DDRB`/`PORTB`/`PINB`; the timer
is TCA0/TCB0 (different ISR vectors, different CTC setup); the WDT uses
`WDT.CTRLA`; the clock prescaler is `CLKCTRL.MCLKCTRLB`; sleep is
`SLPCTRL.CTRLA`. Programming uses UPDI (not ISP/SPI), requiring a different
avrdude programmer. Fuse bytes are a completely different layout
(`FUSE.WDTCFG`, `FUSE.BODCFG`, etc.).

The algorithm (`bypass_pure.c`) and all host-side tests are already fully
portable. The output abstraction (`bypass_hw_iface.h`) is partially complete —
effect state switching is already behind the interface — but the following
remain in `bypass_core.c` as classic-AVR code not yet abstracted: timer setup
and ISR vector, WDT arm/reset/clear, clock prescaler, ADC/analog-comparator
disable, power gating, sleep, interrupt controller setup, and footswitch pin
reading. The output drivers also use `DDRB` directly in `hw_init_ddrb_setup()`
and `hw_is_sanity_check_failed()`.

The clean implementation path is: (1) extend `bypass_hw_iface.h` with
primitives for footswitch read, pin direction, WDT reset, idle sleep, and MCU
init; (2) extract the classic-AVR implementations of those into a new
`bypass_mcu_attiny13a.c` (separating the currently monolithic `bypass_core.c`);
(3) update the output drivers to call `hw_pin_set_output(pin)` instead of
writing `DDRB` directly; (4) write `bypass_mcu_attiny202.c` implementing the
same interface with AVR8X registers; (5) add `attiny202` to the Makefile
(trivial given the existing template structure); (6) add ATtiny202 fuse config
and extend `test_fuses.c`.

The significant open gap is **simavr**: its AVR8X/tinyAVR-2-Series support is
limited to nonexistent, so the fault-injection and lock-step co-simulation tests
cannot automatically extend to ATtiny202. Options are: accept that the ATtiny202
build is validated by static analysis + CBMC + model check + real hardware (no
simulation layer); or evaluate QEMU's AVR plugin, which has a better AVR8X
trajectory.

---

## Tier 4 — out of scope for firmware (name only)

A manufacturer adopting this reference design additionally needs: a professional
schematic (KiCad), a BOM with manufacturer part numbers and approved
substitutes, a hardware production test procedure, and an FMEA. These are
outside the firmware scope; naming them in the design doc as "out of scope /
left to the implementer" is itself evidence of thoroughness.

---

## Priority summary

| Item                                         | Tier | Effort    | Impact                          |
|----------------------------------------------|------|-----------|---------------------------------|
| GitHub Actions CI (full variant/MCU matrix)  | 1    | 1–2 h     | Very high — credibility signal  |
| Design doc: resource-utilization section     | 1    | 1 h       | High — measured numbers exist   |
| Stale refs + editorial-note fix              | 1    | 15 min    | Medium — correctness of docs    |
| Design doc: datasheet citations              | 2    | 2 h       | High — completeness/rigor       |
| Minimum-tap-interval test                    | 2    | 1 h       | Medium — closes traceability    |
| `-fstack-usage` static bound                 | 2    | 30 min    | Medium — complements HWM test   |
| Flash-utilization budget assertion           | 2    | 20 min    | Medium — resource budget        |
| Hardware-validation procedure doc            | 3    | 2–3 h     | High — primary-part WDT gap     |
| CBMC formal analysis                         | 3    | 4–8 h     | Platinum-level credibility      |
| KLEE in CI                                   | 3    | 2 h       | Nice-to-have                    |
| tinyAVR 2-Series (ATtiny202) support         | 3    | 2–4 days  | Nice-to-have; simavr gap        |
| Manufacturing artifacts (name as scope)      | 4    | —         | Completeness signal             |
