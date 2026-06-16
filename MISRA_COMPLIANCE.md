# MISRA-C:2012 Compliance Summary

This firmware is checked against the **MISRA-C:2012** guidelines for the C
language subset used in critical and embedded systems. This document records
the compliance posture, the analysis method, and every deviation — each MISRA
rule the project knowingly does not satisfy, with its justification.

The intent is a *compliant-with-documented-deviations* posture: the analysis
runs clean except for a small, explicitly enumerated set of deviations that are
inherent to bare-metal AVR programming, each justified below and waived through
a per-file entry in [`test/misra_suppressions.txt`](test/misra_suppressions.txt).

> **Note on rule wording.** The official MISRA rule texts are copyrighted by the
> MISRA Consortium and are not reproduced here. The summaries below are our own
> paraphrases for orientation only; consult the published MISRA-C:2012 standard
> for the authoritative text, rationale, exceptions, and amplification.

## How it is checked

| | |
|---|---|
| Analyzer | `cppcheck` 2.13.0, MISRA addon (`misra.py`) |
| Target model | `--platform=avr8`, `--std=c11` |
| Compiler / headers | `avr-gcc` 7.3.0 (avr-libc register definitions) |
| Build target | `make analyze-misra` |
| Supporting files | [`test/misra.json`](test/misra.json) (addon config), [`test/misra_rules.txt`](test/misra_rules.txt) (rule-text paraphrases), [`test/misra_suppressions.txt`](test/misra_suppressions.txt) (deviation waivers) |

`make analyze-misra` runs the addon over every firmware translation unit — the
hardware-agnostic core plus each output-driver variant — and **gates** on any
finding not covered by a documented deviation below. It is part of the `analyze`
aggregate and therefore of `make test`.

To review the *full* inventory including the waived deviations (e.g. when
maintaining this document), run `make analyze-misra-report`.

## Compliance boundary

The compliance boundary is **this project's own source** — `bypass_core.c` and
the `bypass_output_*` driver/header set. The **avr-libc / avr-gcc system
headers** are outside the boundary: they are adopted toolchain code, not authored
by this project, and are excluded from the analysis (by include-path suppression
in the Makefile). This is the standard treatment of library/toolchain code under
MISRA Directive 4.1's "adopted code" provisions.

## Deviations

All deviations fall into two classes. Each is waived per-file in the
suppressions list (not project-wide), so a new occurrence in a *new* file still
fails the gate and forces a conscious review.

### D-1 — Hardware register access

| | |
|---|---|
| **Rules** | 11.4 (pointer ↔ integer conversion, Advisory); 10.1 (inappropriate essential type, Required); 10.8 (composite-expression cast, Required) |
| **Files** | `bypass_core.c`, `bypass_output_cd4053_simple.c`, `bypass_output_cd4053_with_mute.c`, `bypass_output_tq2_l2_5v_relay.c` |
| **Instances** | 11.4 ×28, 10.1 ×26, 10.8 ×6 |

**Rationale.** Direct manipulation of AVR I/O registers is unavoidable in
bare-metal firmware, and avr-libc exposes every register through the `_SFR_*`
macros, which expand to a dereference of an integer address cast to a
`volatile`-qualified pointer. This makes three rules structurally unsatisfiable
for any register access:

- **Rule 11.4** fires on the integer-to-pointer conversion inside every register
  read or write, e.g.

  ```c
  ADCSRA = 0;                 // _SFR_IO8(0x06) -> *(volatile uint8_t *)(0x26)
  TCCR0A = (1 << WGM01);
  ```

- **Rule 10.1** fires on the bit-manipulation idioms used in register
  read-modify-write, e.g.

  ```c
  PORTB |=  (1 << LED_PIN);
  PORTB &= (uint8_t)~(1 << LED_PIN);
  ```

- **Rule 10.8** fires on the `(uint8_t)` casts of those composite bit
  expressions, which are themselves present *to keep the result in `uint8_t`*
  and silence `-Wconversion`.

There is no portable, register-correct way to express these operations without
the underlying integer-to-pointer conversion and bit arithmetic. The accesses
are confined to the pin-helper functions and `init()`; the debounce algorithm
itself contains no such code. These rules are widely deviated for this exact
reason in professional embedded MISRA projects.

**Scope control.** Waived per-file. A register access introduced in a new
translation unit will not be silently covered — it must be reviewed and the file
added here explicitly.

### D-2 — Cross-translation-unit shared pin macros

| | |
|---|---|
| **Rule** | 2.5 (unused macro definition, Advisory) |
| **File** | `bypass_output_common.h` |
| **Instances** | 3 |

**Rationale.** `FOOTSW_PIN` and `LED_PIN` are the two pins common to every
output variant, so they are defined once in the shared `bypass_output_common.h`:

```c
#define FOOTSW_PIN (PB0)
#define LED_PIN    (PB1)
```

They are referenced by the **core** (which reads the footswitch and drives the
LED) but not by the **output-driver** translation units, which only drive their
own control pins. Because cppcheck analyzes each translation unit independently,
it reports these macros as "unused" when checking a driver TU that includes the
header but does not reference them. They are **not dead code** — they are used by
the core, and centralizing them avoids three-way duplication with no compiler
check that the copies stay in sync. The finding is an artifact of per-TU
analysis of a deliberately shared definition.

## Maintenance

When changing the firmware:

1. Run `make analyze-misra`. If it fails, a finding is **not** covered by a
   deviation above.
2. Prefer to **fix** the finding (most essential-type and precedence issues are
   genuine and fixable — 12 such were fixed when this analysis was first
   established).
3. Only if the finding is genuinely unavoidable (e.g. a new register access in a
   new file under D-1), add a per-file entry to
   [`test/misra_suppressions.txt`](test/misra_suppressions.txt) **and** record it
   against the relevant deviation here. A suppression without a documented
   rationale is itself a compliance defect.
