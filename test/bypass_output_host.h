// Host-test shim for including the firmware's output (pin-assignment) headers
// on a native (non-AVR) compiler.
//
// The firmware output headers (bypass_output_common.h and the per-variant
// bypass_output_*.h) define the pin assignments (FOOTSW_PIN, LED_PIN, and the
// variant-specific control pins) in terms of the AVR register-bit names
// PB0..PB5. On the AVR target those names come from <avr/io.h>; the headers
// guard that include behind #if defined(__AVR__), so on the host we must define
// the names ourselves BEFORE including them.
//
// The whole point: the sim tests pull the pin numbers from the SAME firmware
// headers the firmware compiles against, so a pin reassignment can never
// silently diverge between firmware and test. This mirrors how
// bypass_config_host.h shares the debounce thresholds.
//
// The variant is selected by the SAME macro the firmware build uses
// (CD4053_SIMPLE / CD4053_WITH_MUTE / TQ2_L2_5V_RELAY), so the Makefile passes
// one -D to both the firmware and the test.

#ifndef BYPASS_OUTPUT_HOST_H__
#define BYPASS_OUTPUT_HOST_H__

// --- AVR register-bit-name shims --------------------------------------------
// PORTB has 6 bits (PB0..PB5) on both the ATtiny13a and ATtiny85. Define them
// as their bit positions, matching <avr/io.h>.
#ifndef PB0
#  define PB0 0
#endif
#ifndef PB1
#  define PB1 1
#endif
#ifndef PB2
#  define PB2 2
#endif
#ifndef PB3
#  define PB3 3
#endif
#ifndef PB4
#  define PB4 4
#endif
#ifndef PB5
#  define PB5 5
#endif

// Pins common to every variant (footswitch + status LED).
#include "../bypass_output_common.h"

// Variant-specific control pins. Default to the CD4053 simple variant when no
// selector is defined, matching the firmware's behavior.
#if defined(CD4053_WITH_MUTE)
#  include "../bypass_output_cd4053_with_mute.h"
#elif defined(TQ2_L2_5V_RELAY)
#  include "../bypass_output_tq2_l2_5v_relay.h"
#else
#  include "../bypass_output_cd4053_simple.h"
#endif

#endif // BYPASS_OUTPUT_HOST_H__
