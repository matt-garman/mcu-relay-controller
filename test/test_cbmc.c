// CBMC formal verification of the bypass debounce core (bypass_pure.c).
//
// WHY THIS EXISTS
// ---------------
// test_model_check.c (exhaustive BFS) and test_symbolic.c (exhaustive
// enumeration / KLEE) already prove the design's invariants -- but the former
// two enumerate a finite domain on the *host* C semantics, and the KLEE path is
// optional. This file adds a third, independent proof engine: CBMC, a bounded
// model checker with a SAT/SMT backend. It proves the SAME safety/liveness
// properties, but it does so:
//
//   1. Over the symbolic input domain (not by enumeration), via a SAT solver --
//      a mathematically distinct argument from the explicit BFS/enumeration, so
//      a defect that slipped past one engine's encoding is unlikely to slip past
//      all three.
//   2. On the ACTUAL firmware functions -- debounce_integrate(), debounce_step()
//      and debounce_init_context() from bypass_pure.c, the exact code that ships
//      (the functional-core/imperative-shell split is what makes the core
//      hardware-free and therefore model-checkable). This is TODO.md Tier 3:
//      "Prove properties of the actual C source rather than a re-implementation."
//   3. With CBMC's automatic instrumentation enabled (--bounds-check,
//      --conversion-check, --signed/unsigned-overflow-check, --unwinding-
//      assertions, ...), which additionally proves the debounce path is free of
//      undefined behaviour -- arithmetic overflow, out-of-range conversions,
//      out-of-bounds access -- something the assertion-based tests cannot see.
//
// STRUCTURE
// ---------
// Each property is a separate top-level "proof harness" function; the Makefile
// invokes cbmc once per harness with `--function <name>`. CBMC supplies
// nondeterministic (universally-quantified) inputs via the undefined nondet_*()
// functions; __CPROVER_assume() restricts them to the valid domain and
// __CPROVER_assert() states the property. A SUCCESSFUL run is a proof the
// assertion holds for EVERY admitted input.
//
// The straight-line harnesses (integrate / step / init) need no unwinding. The
// two liveness harnesses contain bounded loops and are run with `--unwind 50`
// (> every loop's fixed bound) plus `--unwinding-assertions`, so CBMC proves the
// loops are fully unrolled and the bound is a real (not assumed) horizon.

#include <stdint.h>

// Real firmware functions + step() + thresholds + enum constants, via the host
// config shim. Single source of truth shared with the other model tests.
#include "model_step.h"

// CBMC nondeterministic value sources (undefined => universally quantified).
uint8_t nondet_uint8(void);
int     nondet_int(void);

//////////////////////////////////////////////////////////////////////////////
// (C1) debounce_integrate(): the ISR saturating integrator.
//
// Proved over the FULL uint8_t counter domain (0..255), not just the valid
// 0..RELEASE_THRESH range -- so this is also a defense-in-depth proof that a
// corrupted counter cannot make the integrator overflow/underflow or step by
// more than 1. CBMC's overflow/conversion checks run on the real ++/-- too.
//////////////////////////////////////////////////////////////////////////////
void prove_integrate(void) {
    int     pin_low = nondet_int();
    uint8_t dc      = nondet_uint8();
    __CPROVER_assume(pin_low == 0 || pin_low == 1);

    pin_state_t const pin = (pin_low != 0) ? PIN_STATE_LOW : PIN_STATE_HIGH;
    uint8_t const out = debounce_integrate(pin, dc);

    // Moves by at most 1 in either direction.
    int const d = (int)out - (int)dc;
    __CPROVER_assert(d >= -1 && d <= 1, "(C1) integrator moved by more than 1");

    // Pressed (LOW) increments but saturates at RELEASE_THRESH; never exceeds it
    // when starting at or below it. Released (HIGH) decrements but never below 0.
    if (pin == PIN_STATE_LOW) {
        __CPROVER_assert(out >= dc, "(C1) pressed sample decreased the counter");
        __CPROVER_assert(dc >= RELEASE_THRESH ? (out == dc)
                                              : (out == (uint8_t)(dc + 1U)),
                         "(C1) pressed sample did not saturate/increment correctly");
    } else {
        __CPROVER_assert(out <= dc, "(C1) released sample increased the counter");
        __CPROVER_assert(dc == 0U ? (out == 0U) : (out == (uint8_t)(dc - 1U)),
                         "(C1) released sample did not floor/decrement correctly");
    }

    // In-range input keeps the result in range (the closure property the
    // whole-program invariant relies on).
    if (dc <= RELEASE_THRESH) {
        __CPROVER_assert(out <= RELEASE_THRESH,
                         "(C1) in-range counter left valid range");
    }
}

//////////////////////////////////////////////////////////////////////////////
// (C2) debounce_step(): the main-loop state machine, proved directly on its
// raw result struct -- including the fields step() hides (toggled, fault,
// reload_lockout, lockout_value). This is strictly more than the step()-level
// proofs in test_symbolic.c can observe.
//////////////////////////////////////////////////////////////////////////////
void prove_debounce_step(void) {
    uint8_t ps = nondet_uint8();
    uint8_t es = nondet_uint8();
    uint8_t dc = nondet_uint8();
    __CPROVER_assume(ps <= RELEASE_DEBOUNCE_WAIT);
    __CPROVER_assume(es <= ENGAGED);
    __CPROVER_assume(dc <= RELEASE_THRESH);

    debounce_context_t ctx;
    ctx.program_state    = (program_state_t)ps;
    ctx.effect_state     = (effect_state_t)es;
    ctx.debounce_counter = dc;
    debounce_step_result_t const r = debounce_step(ctx);

    // (C2a) result states stay in range.
    __CPROVER_assert(r.program_state <= RELEASE_DEBOUNCE_WAIT,
                     "(C2a) result program_state out of range");
    __CPROVER_assert(r.effect_state <= ENGAGED,
                     "(C2a) result effect_state out of range");

    // (C2b) a valid (in-range) program_state never faults.
    __CPROVER_assert(!r.fault, "(C2b) fault raised for an in-range program_state");

    // (C2c) toggle iff a press just completed; it flips the effect, enters the
    //       release lock-out, and requests the full RELEASE_THRESH reload.
    if (r.toggled) {
        __CPROVER_assert(ctx.program_state == PRESS_DEBOUNCE_WAIT,
                         "(C2c) toggle from a non-press-wait state");
        __CPROVER_assert(r.program_state == RELEASE_DEBOUNCE_WAIT,
                         "(C2c) toggle did not enter release-wait");
        __CPROVER_assert(r.effect_state != ctx.effect_state,
                         "(C2c) toggle did not change the effect state");
        __CPROVER_assert(r.reload_lockout && r.lockout_value == RELEASE_THRESH,
                         "(C2c) toggle did not request a full lock-out reload");
    }

    // (C2d) the lock-out (RELEASE_DEBOUNCE_WAIT) never changes the effect state
    //       and never toggles -- the "one press = one change" guarantee.
    if (ctx.program_state == RELEASE_DEBOUNCE_WAIT) {
        __CPROVER_assert(r.effect_state == ctx.effect_state,
                         "(C2d) effect state changed during lock-out");
        __CPROVER_assert(!r.toggled, "(C2d) toggle occurred during lock-out");
        __CPROVER_assert(!r.reload_lockout,
                         "(C2d) spurious lock-out reload during release-wait");
    }

    // (C2e) effect state changes ONLY together with a toggle.
    if (r.effect_state != ctx.effect_state) {
        __CPROVER_assert(r.toggled, "(C2e) effect changed without a toggle flag");
    }

    // (C2f) a reload is only ever requested with the documented value.
    if (r.reload_lockout) {
        __CPROVER_assert(r.lockout_value == RELEASE_THRESH,
                         "(C2f) reload requested with a value other than RELEASE_THRESH");
    }
}

//////////////////////////////////////////////////////////////////////////////
// (C2x) fault path: a CORRUPTED program_state (outside the enum's 0..1 range,
// e.g. an SEU-flipped byte) MUST raise the fault flag so the shell can force a
// watchdog reset. This proves the defensive default: case is reachable and
// behaves as designed.
//////////////////////////////////////////////////////////////////////////////
void prove_corrupt_state_faults(void) {
    uint8_t ps = nondet_uint8();
    uint8_t es = nondet_uint8();
    uint8_t dc = nondet_uint8();
    __CPROVER_assume(ps > RELEASE_DEBOUNCE_WAIT); // corrupted: not a valid enum

    debounce_context_t ctx;
    ctx.program_state    = (program_state_t)ps;
    ctx.effect_state     = (effect_state_t)es;
    ctx.debounce_counter = dc;
    debounce_step_result_t const r = debounce_step(ctx);

    __CPROVER_assert(r.fault, "(C2x) corrupted program_state did not raise fault");
    __CPROVER_assert(!r.toggled, "(C2x) corrupted program_state toggled the effect");
}

//////////////////////////////////////////////////////////////////////////////
// (C3) debounce_init_context(): power-on state from the sampled footswitch.
//////////////////////////////////////////////////////////////////////////////
void prove_init_context(void) {
    int pin_low = nondet_int();
    __CPROVER_assume(pin_low == 0 || pin_low == 1);
    pin_state_t const pin = (pin_low != 0) ? PIN_STATE_LOW : PIN_STATE_HIGH;

    debounce_context_t const c = debounce_init_context(pin);

    // Always powers on in BYPASS (the effect must be OFF at power-up).
    __CPROVER_assert(c.effect_state == BYPASS, "(C3) did not power on in BYPASS");

    if (pin == PIN_STATE_LOW) {
        // Footswitch held at power-on: arm the release lock-out so the held
        // press is debounced out before any toggle is possible.
        __CPROVER_assert(c.program_state == RELEASE_DEBOUNCE_WAIT,
                         "(C3) power-on-pressed did not enter release-wait");
        __CPROVER_assert(c.debounce_counter == RELEASE_THRESH,
                         "(C3) power-on-pressed did not load the full lock-out");
    } else {
        // Normal power-on: ready to recognise the first press.
        __CPROVER_assert(c.program_state == PRESS_DEBOUNCE_WAIT,
                         "(C3) normal power-on not in press-wait");
        __CPROVER_assert(c.debounce_counter == 0U,
                         "(C3) normal power-on counter not zero");
    }
}

//////////////////////////////////////////////////////////////////////////////
// (C4) Composed single step (ISR integrate + main-loop state machine), the
// SAME step() the other model tests use. Mirrors the P1..P6 bundle of
// test_symbolic.c, but discharged by CBMC's SAT backend over the symbolic
// domain. This is the inductive step that, with valid initial states, implies
// the whole-program invariants.
//////////////////////////////////////////////////////////////////////////////
void prove_step_transition(void) {
    uint8_t ps = nondet_uint8();
    uint8_t es = nondet_uint8();
    uint8_t dc = nondet_uint8();
    int     in = nondet_int();
    __CPROVER_assume(ps <= RELEASE_DEBOUNCE_WAIT);
    __CPROVER_assume(es <= ENGAGED);
    __CPROVER_assume(dc <= RELEASE_THRESH);
    __CPROVER_assume(in == 0 || in == 1);

    state_t s = { ps, es, dc };
    step_result_t const r = step(s, in);

    // (P1) range safety
    __CPROVER_assert(r.next.program_state <= RELEASE_DEBOUNCE_WAIT,
                     "(P1) program_state out of range");
    __CPROVER_assert(r.next.effect_state <= ENGAGED,
                     "(P1) effect_state out of range");
    __CPROVER_assert(r.next.debounce_counter <= RELEASE_THRESH,
                     "(P1) debounce_counter out of range");

    // (P2) toggle => out of press-wait, into release-wait, lock-out loaded, flip
    if (r.toggled) {
        __CPROVER_assert(s.program_state == PRESS_DEBOUNCE_WAIT,
                         "(P2) toggle from non-press-wait");
        __CPROVER_assert(r.next.program_state == RELEASE_DEBOUNCE_WAIT,
                         "(P2) toggle did not enter release-wait");
        __CPROVER_assert(r.next.debounce_counter == RELEASE_THRESH,
                         "(P2) toggle did not load full lock-out");
        __CPROVER_assert(r.next.effect_state != s.effect_state,
                         "(P2) toggle did not change effect_state");
    }

    // (P3) release-wait never changes the effect / never toggles
    if (s.program_state == RELEASE_DEBOUNCE_WAIT) {
        __CPROVER_assert(r.next.effect_state == s.effect_state,
                         "(P3) effect_state changed during lock-out");
        __CPROVER_assert(r.toggled == 0, "(P3) toggle during lock-out");
    }

    // (P4) effect_state changes only with the toggle flag
    if (r.next.effect_state != s.effect_state) {
        __CPROVER_assert(r.toggled == 1, "(P4) effect changed without toggle flag");
    }

    // (P5) re-arm to press-wait only when the counter reached 0
    if (s.program_state == RELEASE_DEBOUNCE_WAIT &&
        r.next.program_state == PRESS_DEBOUNCE_WAIT) {
        __CPROVER_assert(r.next.debounce_counter == 0,
                         "(P5) re-armed with a nonzero counter");
    }

    // (P6) counter moves by at most 1 except the deterministic lock-out reload
    if (!r.toggled) {
        int const d = (int)r.next.debounce_counter - (int)s.debounce_counter;
        __CPROVER_assert(d >= -1 && d <= 1,
                         "(P6) counter moved by more than 1 without a toggle");
    }
}

//////////////////////////////////////////////////////////////////////////////
// (C5) Bounded liveness of PRESS: from PRESS_DEBOUNCE_WAIT with counter 0,
// holding the switch pressed produces EXACTLY ONE toggle, at EXACTLY
// PRESSED_THRESH ticks, with no second toggle for the rest of the horizon.
// Run with --unwind 50 + --unwinding-assertions (horizon < 50).
//////////////////////////////////////////////////////////////////////////////
#define PRESS_HORIZON ((int)PRESSED_THRESH + (int)RELEASE_THRESH + 4)
void prove_press_liveness(void) {
    uint8_t es = nondet_uint8();
    __CPROVER_assume(es <= ENGAGED);

    state_t s = { PRESS_DEBOUNCE_WAIT, es, 0 };
    int toggles = 0;
    int first   = -1;
    for (int t = 0; t < PRESS_HORIZON; ++t) {
        step_result_t const r = step(s, 1 /* pressed */);
        if (r.toggled) { toggles++; if (first < 0) { first = t + 1; } }
        s = r.next;
    }
    __CPROVER_assert(toggles == 1, "(C5) held press did not produce exactly one toggle");
    __CPROVER_assert(first == (int)PRESSED_THRESH,
                     "(C5) clean-press toggle latency was not PRESSED_THRESH");
}

//////////////////////////////////////////////////////////////////////////////
// (C6) Bounded liveness of RELEASE: from EVERY valid state, holding the switch
// released returns the machine to PRESS_DEBOUNCE_WAIT / counter 0 within a
// bounded horizon (no stuck lock-out for a switch that is actually released),
// producing at most one toggle on the way. The intentional held-forever
// deadlock is NOT claimed -- it only applies while the input stays low.
//////////////////////////////////////////////////////////////////////////////
#define RELEASE_HORIZON ((int)RELEASE_THRESH + 4)
void prove_release_liveness(void) {
    uint8_t ps = nondet_uint8();
    uint8_t es = nondet_uint8();
    uint8_t dc = nondet_uint8();
    __CPROVER_assume(ps <= RELEASE_DEBOUNCE_WAIT);
    __CPROVER_assume(es <= ENGAGED);
    __CPROVER_assume(dc <= RELEASE_THRESH);

    state_t s = { ps, es, dc };
    int reached = 0;
    int toggles = 0;
    for (int t = 0; t < RELEASE_HORIZON; ++t) {
        step_result_t const r = step(s, 0 /* released */);
        toggles += r.toggled;
        s = r.next;
        if (s.program_state == PRESS_DEBOUNCE_WAIT && s.debounce_counter == 0) {
            reached = 1;
            break;
        }
    }
    __CPROVER_assert(reached, "(C6) released hold did not re-arm within the horizon");
    __CPROVER_assert(toggles <= 1, "(C6) released hold produced more than one toggle");
}
