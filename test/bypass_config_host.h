// Host-test shim for including the firmware's bypass_config.h on a native
// (non-AVR) compiler.
//
// bypass_config.h is written for the AVR target: it references AVR register
// names (PB0/PB1/PB2, TIMSK) from <avr/io.h> and enforces target-specific
// F_CPU values via #error guards. The host-compiled tests (test_logic_host.c
// and test_sim.c) do not include <avr/io.h>, so this shim provides the minimum
// definitions required to satisfy those references, then includes the REAL
// firmware config header.
//
// The whole point: the tests now pull RELEASE_THRESH / PRESSED_THRESH (and the
// pin numbers / timer reload) directly from the single source of truth in
// bypass_config.h, so they can never silently drift from the firmware.

#ifndef BYPASS_CONFIG_HOST_H__
#define BYPASS_CONFIG_HOST_H__

// --- AVR register-name shims -------------------------------------------------
// bypass_config.h defines FOOTSW_PIN/LED_PIN/CD4053_PIN in terms of these.
// On the AVR target these come from <avr/io.h>; here we mirror the documented
// pin assignment (PB0=footswitch, PB1=LED, PB2=CD4053).
#ifndef PB0
#  define PB0 0
#endif
#ifndef PB1
#  define PB1 1
#endif
#ifndef PB2
#  define PB2 2
#endif

// --- F_CPU shim --------------------------------------------------------------
// bypass_config.h's F_CPU #error guards only fire for the AVR targets it
// recognizes. On host (__AVR_ATtiny85__ undefined) it takes the "ATtiny13/a"
// branch and requires F_CPU == 1200000UL. Provide that if the test did not
// already set one, purely to satisfy the guard; the host tests do not depend
// on F_CPU for any logic.
#ifndef F_CPU
#  define F_CPU 1200000UL
#endif

#include "../bypass_config.h"

#endif // BYPASS_CONFIG_HOST_H__
