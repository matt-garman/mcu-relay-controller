// Shared single-step model for test_model_check.c and test_symbolic.c.
//
// Both files re-implement the debounce algorithm to prove invariants; keeping
// them in sync by hand is a maintenance hazard. This header provides the ONE
// canonical copy of the step() function and its supporting types. Including it
// in two translation units is safe because all definitions are static.
//
// The algorithm here must remain byte-identical to the ISR+main-loop in
// attiny13_bypass.c. If the firmware changes, update this file first, then
// re-run the full test suite to confirm the proofs still hold on the new
// algorithm.

#ifndef MODEL_STEP_H__
#define MODEL_STEP_H__

#include <stdint.h>

#include "bypass_config_host.h" // RELEASE_THRESH, PRESSED_THRESH (firmware truth)

// Mirror attiny13_bypass.c's program_state_t / effect_state_t enum values.
enum { PRESS_DEBOUNCE_WAIT = 0, RELEASE_DEBOUNCE_WAIT = 1 };
enum { BYPASS = 0, ENGAGED = 1 };

typedef struct {
    uint8_t program_state;    // PRESS_DEBOUNCE_WAIT or RELEASE_DEBOUNCE_WAIT
    uint8_t effect_state;     // BYPASS or ENGAGED
    uint8_t debounce_counter; // 0 .. RELEASE_THRESH
} state_t;

// Result of a single 1ms step: the successor state plus a flag indicating
// whether a toggle (effect-state change) occurred during the step.
typedef struct {
    state_t next;
    int     toggled;
} step_result_t;

// One 1ms step: ISR saturating integrator, then one main-loop state-machine
// pass. Mirrors attiny13_bypass.c exactly.
// pin_low != 0  =>  PB0 read low  =>  switch pressed.
static step_result_t step(state_t s, int pin_low) {
    step_result_t r;
    r.toggled = 0;

    // --- ISR: saturating integrator ---
    if (pin_low) {
        if (s.debounce_counter < RELEASE_THRESH) { s.debounce_counter++; }
    } else {
        if (s.debounce_counter > 0) { s.debounce_counter--; }
    }

    // --- main loop state machine ---
    if (s.program_state == PRESS_DEBOUNCE_WAIT) {
        if (s.debounce_counter >= PRESSED_THRESH) {
            s.debounce_counter = RELEASE_THRESH;
            s.program_state    = RELEASE_DEBOUNCE_WAIT;
            s.effect_state     = (s.effect_state == BYPASS) ? ENGAGED : BYPASS;
            r.toggled = 1;
        }
    } else { // RELEASE_DEBOUNCE_WAIT
        if (s.debounce_counter == 0) {
            s.program_state = PRESS_DEBOUNCE_WAIT;
        }
    }

    r.next = s;
    return r;
}

#endif // MODEL_STEP_H__
