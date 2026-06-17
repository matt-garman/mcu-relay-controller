// Shared single-step model for test_model_check.c, test_symbolic.c and
// test_sim.c.
//
// SINGLE SOURCE OF TRUTH
// ----------------------
// This header used to RE-IMPLEMENT the debounce algorithm so the proofs had a
// model to check. That re-implementation was a maintenance hazard: it had to be
// kept byte-identical to the firmware by hand. It no longer exists. step() now
// DELEGATES directly to the firmware's own pure functions -- debounce_integrate()
// and debounce_step() from bypass_pure.c -- so the model checker, the symbolic
// property test, and the simavr oracle all exercise the EXACT code that ships.
// There is nothing left to drift.
//
// The pure firmware logic is host-compilable (no hardware, no globals, no side
// effects), which is the whole point of the functional-core/imperative-shell
// split in bypass_pure.c. Each test that includes this header must therefore
// link bypass_pure.c (the Makefile recipes do this; the firmware config shim is
// force-included so bypass_config.h's thresholds resolve on the host).
//
// The test-facing types (state_t / step_result_t) are kept deliberately stable
// and uint8_t-based so the three consuming test files need no changes; step()
// bridges them to the firmware's debounce_context_t.

#ifndef MODEL_STEP_H__
#define MODEL_STEP_H__

#include <stdint.h>

#include "bypass_config_host.h" // RELEASE_THRESH, PRESSED_THRESH (firmware truth)
#include "../bypass_pure.h"     // debounce_integrate(), debounce_step(), and the
                                // program_state_t / effect_state_t enums
                                // (PRESS_DEBOUNCE_WAIT, BYPASS, ENGAGED, ...)
                                // shared with the firmware via bypass_types.h.

// Test-facing state. Field names/order match what the three test files already
// use; the firmware's debounce_context_t carries the same fields with enum
// types, which step() converts to/from below.
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

// One 1ms step: the firmware's ISR saturating integrator followed by one
// main-loop state-machine pass -- i.e. exactly the per-tick work bypass_core.c's
// main() does (debounce_integrate() inside the timer-tick block, then
// debounce_step()). Delegates to the real firmware functions so this verifies
// the shipping algorithm, not a copy.
// pin_low != 0  =>  PB0 read low  =>  switch pressed.
static step_result_t step(state_t s, int pin_low) {
    // ISR (firmware: TIM0_COMPA_vect): saturating integrator over the live pin.
    // The test convention is pin_low != 0 => switch PRESSED. The firmware models
    // a pressed switch as the footswitch pin reading LOW (PIN_STATE_LOW), which
    // is what debounce_integrate() increments on. The integrated counter is the
    // ISR-owned value; the state machine reads it but only the caller may write
    // it back.
    pin_state_t const pin       = (pin_low != 0) ? PIN_STATE_LOW : PIN_STATE_HIGH;
    uint8_t     const integrated = debounce_integrate(pin, s.debounce_counter);

    // main-loop state machine (firmware: debounce_step()).
    debounce_context_t ctx;
    ctx.program_state    = (program_state_t)s.program_state;
    ctx.effect_state     = (effect_state_t)s.effect_state;
    ctx.debounce_counter = integrated;
    debounce_step_result_t const dr = debounce_step(ctx);

    // Apply exactly as bypass_core.c main() does: caller-owned fields always,
    // and the counter ONLY on an explicit lockout reload -- otherwise it keeps
    // the ISR-integrated value (the caller never clobbers the ISR's update).
    step_result_t r;
    r.next.program_state    = (uint8_t)dr.program_state;
    r.next.effect_state     = (uint8_t)dr.effect_state;
    r.next.debounce_counter = dr.reload_lockout ? dr.lockout_value : integrated;
    r.toggled               = (int)dr.toggled;
    return r;
}

#endif // MODEL_STEP_H__
