# Remaining work toward textbook reference quality

## 1. Project infrastructure (highest-impact, easiest)

**README.md is absent.** Every GitHub project needs one, and for a project at
this quality level it's conspicuous to lack it. Should cover: what it is,
hardware required, build/test instructions (`make test`), fuse programming
command, and a link to the design doc. This is the first thing any
manufacturer's engineer sees.

**No CI/CD pipeline.** A `.github/workflows/test.yml` that runs `make test` on
push gives you the "build passing" badge and proves the suite is reproducible
without the developer's machine. For a project making claims of Boss-grade
verification, a badge-less repo undercuts credibility. The simavr package is
available in Ubuntu 24.04's default repos, so the workflow is a straightforward
6-line apt install + make test.

**Design doc format mismatch.** `attiny13_bypass.md` uses AsciiDoc syntax
(`=` headings, `==` subheadings) but has a `.md` extension. GitHub renders
`.md` as Markdown, where `=` has no special meaning — the headings currently
display as plain text with leading equals signs. Options:
- Rename to `attiny13_bypass.adoc` — GitHub renders AsciiDoc natively, no
  content changes needed.
- Convert to Markdown — change `=` to `#` and `==` to `##` throughout.

Either works; AsciiDoc rename is the lower-effort path.

---

## 2. Firmware correctness: two small but real gaps

**Timer formula has no compile-time guard.** The design spec says "1ms tick at
1.2MHz via prescaler /8 and OCR0A=149." That formula is:

    F_CPU / 8 / (TIMER0_OCR0A_1MS + 1) == 1000

If anyone ports to a different clock speed and gets OCR0A wrong, the debounce
timing silently breaks — wrong thresholds, wrong latency. A `static_assert` on
this expression would catch it at compile time. The test suite would eventually
catch it too (latency test), but a compile-time error with a clear message is
far better. This is a one-liner.

**`__attribute__((OS_main))` on `main()`.** avr-gcc's `OS_main` attribute tells
the compiler that `main()` is the top-level function of an embedded OS and will
never return, so it should not generate register save/restore
prologue/epilogue for the call frame. Without it, the compiler generates a
slightly larger, slightly slower entry. For a textbook firmware this is the
correct annotation; it also documents intent.

---

## 3. Testing: two meaningful holes

**No minimum-tap-interval test.** The design doc states the fastest recognized
tap interval is PRESSED_THRESH + RELEASE_THRESH = 33ms. There is no sim test
that drives two presses with exactly 33ms between edges and asserts both are
detected. The adversarial test comes close but does not specifically target this
boundary. A "fastest double-press" test is the natural regression guard for any
future threshold change and directly maps to the "fast taps recognized"
requirement row in the traceability matrix.

**`-fstack-usage` is unused.** avr-gcc's `-fstack-usage` flag emits a `.su`
file with per-function stack frame sizes at compile time. The runtime canary
HWM test measures the empirical high-water mark; the static analysis would give
a structural upper bound without depending on whether a test happened to
exercise the deepest path. For a tiny MCU with 64 bytes of SRAM, knowing the
theoretical worst-case stack depth is more convincing than measuring a subset
of paths. Adding `-fstack-usage` to the Makefile and optionally asserting the
total via a grep/awk check in `make test` would close this.

---

## 4. Code quality: MISRA-C

`cppcheck --addon=misra --suppressions-list=...` will run a MISRA-C 2012
compliance check. MISRA compliance is expected for automotive/industrial
embedded code. The project will likely have a small number of justified
deviations (anonymous enums, some use of implementation-defined behavior for
AVR types) — documenting these explicitly as "required deviation D-1: ..." is
itself evidence of rigor. Boss uses MISRA-compliant toolchains internally;
seeing this analysis in a reference design carries weight.

---

## 5. Documentation: three gaps

**Design doc editorial note at line 324.** The sentence "A second diagram
showing the EMI/noise rejection case is also worth including — it shows..." is
an authoring note left in the document. The diagram that follows it is already
there; the sentence just needs to be rewritten as a normal lead-in ("The
following diagram shows the noise-rejection case...").

**Datasheet citations are incomplete.** The sleep wakeup now cites §7.3. Other
datasheet-dependent design decisions are still un-cited:
- WDT 16ms post-reset window → the WDT section
- WDTON fuse always-on behavior → Fuse Bits chapter
- Internal oscillator ±10% accuracy → Electrical Characteristics
- Timer0 CTC formula → Timer/Counter0 section

For a reference design, each load-bearing design decision should trace to a
datasheet page or section the way a journal paper cites sources.

**Memory layout is measured but not documented.** The stack HWM test now proves
56 bytes of margin on ATtiny13a (64 bytes total SRAM, 8 bytes stack, ~4 bytes
BSS). This is a remarkable fact — the firmware occupies 12 of 64 bytes of SRAM
and has 52 bytes of headroom. It should be in the design doc as a resource
utilization section, alongside flash utilization from `avr-size`. For a Boss
engineer evaluating whether to base a design on this reference, resource
headroom is a key parameter.

---

## 6. Build system: flash utilization assertion

`avr-size --mcu=attiny13a --format=avr attiny13_bypass.elf` gives flash and
SRAM utilization. The Makefile should capture this and assert it stays under a
threshold (e.g., 90% of 1KB flash). Right now the firmware uses roughly 400
bytes (~40% of flash); a future accidental bloat that consumes the remaining
headroom would silently succeed. This is the firmware analog of a bundle-size
budget in web development.

---

## 7. Formal verification: nice-to-have

**CBMC or Frama-C** would prove properties of the actual C source, not a
re-implementation. The model checker verifies a re-implementation of the
algorithm; CBMC (`cbmc --unwind 50 attiny13_bypass.c`) would operate on the
compiler's actual input. This is platinum-level and requires some setup, but it
would make this one of the most rigorously verified hobbyist AVR firmwares in
existence — a genuine differentiator for a reference design. CBMC is available
as a Debian package.

**KLEE in CI** is possible via a Docker image (klee/klee on DockerHub) and
`test_symbolic.c` already supports it with `-DUSE_KLEE`. A GitHub Actions job
that builds and runs KLEE proves the symbolic execution path actually gets
exercised, not just compiled.

---

## 8. Manufacturing gap (out of scope for firmware, but worth naming)

A Boss engineer adopting this reference design would additionally need: a
professional schematic (KiCad or similar), a BOM with manufacturer part numbers
and approved substitutes, a hardware production test procedure (what to measure
with a scope and DMM on each unit), and an FMEA. These are outside the firmware
scope but naming them in the design doc as "out of scope / left to the
implementer" is itself evidence of thoroughness.

---

## Priority order if implementing

| Item                                        | Effort    | Impact                        |
|---------------------------------------------|-----------|-------------------------------|
| README.md                                   | 30 min    | Very high — first impression  |
| GitHub Actions CI                           | 30 min    | Very high — credibility signal|
| Design doc format fix (.md → .adoc or Markdown) | 15 min | High — renders correctly on GitHub |
| Timer formula `static_assert`               | 5 min     | High — prevents silent porting bug |
| `__attribute__((OS_main))`                  | 2 min     | Medium — correctness annotation |
| Minimum-tap-interval sim test               | 1 hour    | Medium — closes traceability gap |
| `-fstack-usage` in Makefile                 | 30 min    | Medium — complements HWM test |
| Flash utilization assertion                 | 20 min    | Medium — resource budget      |
| MISRA-C check + deviation doc               | 2–4 hours | High for industrial credibility |
| Design doc: editorial + citations + memory layout | 2 hours | High — completeness      |
| CBMC formal analysis                        | 4–8 hours | Platinum-level credibility    |
| KLEE in CI                                  | 2 hours   | Nice-to-have                  |
