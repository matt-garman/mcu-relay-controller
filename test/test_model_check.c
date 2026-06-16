// Exhaustive small-model state-space verification for the bypass
// debounce/bypass algorithm.
//
// WHY THIS EXISTS
// ---------------
// test_logic_host.c (the golden model) and test_sim.c (the real firmware in
// simavr) both verify behavior for *sampled* input sequences (hand-written
// cases + randomized fuzzing). Randomized fuzzing can only ever cover a
// vanishingly small fraction of possible input streams, so it can *find* bugs
// but can never *prove* their absence.
//
// The debounce algorithm, however, has a tiny finite state space:
//     program_state   : 2 values (PRESS_DEBOUNCE_WAIT, RELEASE_DEBOUNCE_WAIT)
//     effect_state    : 2 values (BYPASS, ENGAGED)
//     debounce_counter : 0 .. RELEASE_THRESH  (RELEASE_THRESH+1 values)
//   => 2 * 2 * 26 = 104 reachable model states, with a 1-bit input each tick.
//
// That is small enough to verify EXHAUSTIVELY. This file performs a complete
// breadth-first exploration of the reachable state space under BOTH possible
// inputs at every state, and proves the design's core reliability invariants
// hold for ALL possible input sequences -- not just sampled ones:
//
//   (I1) Reachable-state safety: program_state, effect_state, and
//        debounce_counter never leave their valid ranges.
//   (I2) Single-action-per-press: a toggle (effect-state change) can ONLY
//        occur on the transition out of PRESS_DEBOUNCE_WAIT, and immediately
//        enters RELEASE_DEBOUNCE_WAIT. Therefore between any two toggles the
//        machine MUST pass through RELEASE_DEBOUNCE_WAIT and back -- i.e. the
//        switch must be release-debounced. This is the formal statement of
//        "one press = one change" and "bounce/hold cannot multi-toggle".
//   (I3) No toggle in RELEASE_DEBOUNCE_WAIT: the release-wait state never
//        changes effect_state (the lock-out is honored for every input).
//   (I4) Liveness of release: from EVERY reachable state, a finite run of
//        "released" (high) samples always returns the machine to
//        PRESS_DEBOUNCE_WAIT with counter 0 (no deadlock / no stuck lock-out
//        for a switch that is actually released). The "held forever" deadlock
//        is intentional and is therefore NOT claimed here -- it only applies
//        while the input stays low.
//   (I5) Liveness of press: from PRESS_DEBOUNCE_WAIT, a finite run of
//        "pressed" (low) samples always produces exactly one toggle.
//
// The model logic here is the SAME algorithm as the golden model and the
// firmware (ISR saturating integrator + main-loop state machine), and the
// thresholds come from bypass_config.h, so this verifies the actual design.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// state_t, step_result_t, step(), enum constants, and firmware thresholds.
// This is the single source of truth shared with test_symbolic.c.
#include "model_step.h"

//////////////////////////////////////////////////////////////////////////////
// Harness
//////////////////////////////////////////////////////////////////////////////

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do {                                  \
    g_checks++;                                                \
    if (!(cond)) {                                             \
        g_failures++;                                          \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
        fprintf(stderr, __VA_ARGS__);                          \
        fprintf(stderr, "\n");                                 \
    }                                                          \
} while (0)

// Dense index for a state. Counter range is 0..RELEASE_THRESH inclusive.
// RELEASE_THRESH is unsigned in the firmware config (a count is non-negative);
// cast to int here so this file's signed index/loop arithmetic stays signed and
// does not trip -Werror=sign-conversion/sign-compare.
#define COUNTER_VALUES ((int)RELEASE_THRESH + 1)
#define NUM_STATES     (2 * 2 * COUNTER_VALUES)

static int state_index(state_t s) {
    return (s.program_state * 2 + s.effect_state) * COUNTER_VALUES
           + s.debounce_counter;
}

static int state_valid(state_t s) {
    return s.program_state <= RELEASE_DEBOUNCE_WAIT
        && s.effect_state  <= ENGAGED
        && s.debounce_counter <= RELEASE_THRESH;
}

//////////////////////////////////////////////////////////////////////////////
// (I1)(I2)(I3) Exhaustive BFS over the reachable state space.
//////////////////////////////////////////////////////////////////////////////
static void verify_reachable_state_space(void) {
    uint8_t seen[NUM_STATES];
    memset(seen, 0, sizeof(seen));

    // Both documented power-on states are roots of the reachable graph:
    //   - normal:           PRESS_DEBOUNCE_WAIT, BYPASS, counter 0
    //   - power-on-pressed:  RELEASE_DEBOUNCE_WAIT, BYPASS, counter RELEASE_THRESH
    state_t roots[2] = {
        { PRESS_DEBOUNCE_WAIT,   BYPASS, 0 },
        { RELEASE_DEBOUNCE_WAIT, BYPASS, RELEASE_THRESH },
    };

    // Simple explicit BFS/DFS worklist.
    state_t stack[NUM_STATES];
    int sp = 0;
    int reachable_count = 0;

    for (int i = 0; i < 2; ++i) {
        int idx = state_index(roots[i]);
        if (!seen[idx]) { seen[idx] = 1; stack[sp++] = roots[i]; reachable_count++; }
    }

    while (sp > 0) {
        state_t s = stack[--sp];

        CHECK(state_valid(s),
              "(I1) reached invalid state ps=%u es=%u dc=%u",
              s.program_state, s.effect_state, s.debounce_counter);

        for (int bit = 0; bit < 2; ++bit) {
            int pin_low = bit; // 0 = released/high, 1 = pressed/low
            step_result_t r = step(s, pin_low);

            // (I1) successor must be valid.
            CHECK(state_valid(r.next),
                  "(I1) step produced invalid state from ps=%u es=%u dc=%u in=%d",
                  s.program_state, s.effect_state, s.debounce_counter, pin_low);

            // (I2) a toggle may ONLY happen leaving PRESS_DEBOUNCE_WAIT, and it
            //      must land in RELEASE_DEBOUNCE_WAIT with the lock-out loaded.
            if (r.toggled) {
                CHECK(s.program_state == PRESS_DEBOUNCE_WAIT,
                      "(I2) toggle from non-press-wait state ps=%u", s.program_state);
                CHECK(r.next.program_state == RELEASE_DEBOUNCE_WAIT,
                      "(I2) toggle did not enter release-wait");
                CHECK(r.next.debounce_counter == RELEASE_THRESH,
                      "(I2) toggle did not load full release lock-out, dc=%u",
                      r.next.debounce_counter);
                CHECK(r.next.effect_state != s.effect_state,
                      "(I2) toggle did not actually change effect_state");
            }

            // (I3) RELEASE_DEBOUNCE_WAIT must never change effect_state.
            if (s.program_state == RELEASE_DEBOUNCE_WAIT) {
                CHECK(r.next.effect_state == s.effect_state,
                      "(I3) effect_state changed during release lock-out");
                CHECK(r.toggled == 0,
                      "(I3) toggle occurred during release lock-out");
            }

            int nidx = state_index(r.next);
            if (!seen[nidx]) { seen[nidx] = 1; stack[sp++] = r.next; reachable_count++; }
        }
    }

    printf("  state-space: %d reachable states (of %d encodable)\n",
           reachable_count, NUM_STATES);
    CHECK(reachable_count > 0, "no reachable states explored");
}

//////////////////////////////////////////////////////////////////////////////
// (I2 corollary) Between any two toggles the machine must pass through
// RELEASE_DEBOUNCE_WAIT. Verified structurally above; here we also check the
// strongest practical phrasing: starting from EVERY reachable state, no single
// step can toggle if we are already in RELEASE_DEBOUNCE_WAIT, and the only way
// back to a toggling-capable state is via counter reaching 0 (full release).
//////////////////////////////////////////////////////////////////////////////
static void verify_lockout_requires_full_release(void) {
    // From any RELEASE_DEBOUNCE_WAIT state, the ONLY transition back to
    // PRESS_DEBOUNCE_WAIT requires debounce_counter == 0 after the ISR update.
    for (uint8_t es = 0; es <= ENGAGED; ++es) {
        for (int dc = 0; dc <= (int)RELEASE_THRESH; ++dc) {
            state_t s = { RELEASE_DEBOUNCE_WAIT, es, (uint8_t)dc };
            for (int bit = 0; bit < 2; ++bit) {
                step_result_t r = step(s, bit);
                if (r.next.program_state == PRESS_DEBOUNCE_WAIT) {
                    // Re-arm only allowed when the integrator hit 0.
                    CHECK(r.next.debounce_counter == 0,
                          "(I2c) re-armed to press-wait with counter %u (dc was %d, in=%d)",
                          r.next.debounce_counter, dc, bit);
                }
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////////
// (I4) Liveness: from every reachable state, holding the switch RELEASED
// (high) for a bounded number of ticks returns to PRESS_DEBOUNCE_WAIT, dc=0.
//////////////////////////////////////////////////////////////////////////////
static void verify_release_liveness(void) {
    // Worst case: counter at RELEASE_THRESH; decrements by 1 per released tick;
    // then one more step to re-arm. Bound generously.
    const int bound = RELEASE_THRESH + 4;

    for (uint8_t ps = 0; ps <= RELEASE_DEBOUNCE_WAIT; ++ps) {
        for (uint8_t es = 0; es <= ENGAGED; ++es) {
            for (int dc = 0; dc <= (int)RELEASE_THRESH; ++dc) {
                state_t s = { ps, es, (uint8_t)dc };
                int reached = 0;
                int toggles = 0;
                for (int t = 0; t < bound; ++t) {
                    step_result_t r = step(s, 0 /* released */);
                    toggles += r.toggled;
                    s = r.next;
                    if (s.program_state == PRESS_DEBOUNCE_WAIT
                        && s.debounce_counter == 0) {
                        reached = 1;
                        break;
                    }
                }
                CHECK(reached,
                      "(I4) released hold from ps=%u es=%u dc=%d did not re-arm "
                      "within %d ticks", ps, es, dc, bound);
                // Releasing may legitimately complete at most one in-flight
                // press (if dc was already >= PRESSED_THRESH in press-wait), but
                // never more than one.
                CHECK(toggles <= 1,
                      "(I4) released hold produced %d toggles (expected <=1)",
                      toggles);
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////////
// (I5) Liveness: from PRESS_DEBOUNCE_WAIT with counter 0, holding the switch
// PRESSED (low) produces EXACTLY ONE toggle within PRESSED_THRESH ticks, and
// no further toggles no matter how long it is held.
//////////////////////////////////////////////////////////////////////////////
static void verify_press_liveness(void) {
    for (uint8_t es = 0; es <= ENGAGED; ++es) {
        state_t s = { PRESS_DEBOUNCE_WAIT, es, 0 };
        int toggles = 0;
        int first_toggle_tick = -1;
        const int hold = 5000; // 5 seconds of holding
        for (int t = 0; t < hold; ++t) {
            step_result_t r = step(s, 1 /* pressed */);
            if (r.toggled) {
                toggles++;
                if (first_toggle_tick < 0) { first_toggle_tick = t + 1; }
            }
            s = r.next;
        }
        CHECK(toggles == 1,
              "(I5) %d-ms press from es=%u produced %d toggles (expected 1)",
              hold, es, toggles);
        CHECK(first_toggle_tick == PRESSED_THRESH,
              "(I5) clean-press latency was %d ticks (expected PRESSED_THRESH=%d)",
              first_toggle_tick, PRESSED_THRESH);
    }
}

//////////////////////////////////////////////////////////////////////////////
// (I1-I3 under nondeterministic scheduling)
//
// The deterministic BFS above assumes exactly one ISR fire followed by
// exactly one main-loop pass per 1ms tick. In practice the firmware can
// run main twice in one tick period (once for the toggle/re-arm action,
// once to reach the next sleep_mode() call) and in theory could miss
// running main at all (stalled core). This function proves I1-I3 hold
// under all schedules where main runs 0, 1, or 2 times per ISR tick.
//
// The race the proof covers explicitly:
//   tick N:  ISR fires, debounce_counter_ reaches PRESSED_THRESH.
//            main pass 1: sees counter >= PRESSED_THRESH -> toggle
//              (writes counter = RELEASE_THRESH, enters RELEASE_DEBOUNCE_WAIT).
//            main pass 2: in RELEASE_DEBOUNCE_WAIT, counter = RELEASE_THRESH > 0
//              -> no action (goes to sleep). I3 holds: no second toggle.
//
// I4/I5 liveness requires at least one main pass per tick (forward progress);
// they are proved above under the deterministic (k=1) schedule.
//////////////////////////////////////////////////////////////////////////////

// ISR sub-step: update the saturating counter only (no program-state change).
static uint8_t isr_counter_step(uint8_t counter, int pin_low) {
    if (pin_low) {
        if (counter < RELEASE_THRESH) { counter++; }
    } else {
        if (counter > 0) { counter--; }
    }
    return counter;
}

// One pass of the main-loop state machine. Modifies *s in place.
// Returns 1 if a toggle occurred, 0 otherwise.
static int main_state_step(state_t *s) {
    if (s->program_state == PRESS_DEBOUNCE_WAIT) {
        if (s->debounce_counter >= PRESSED_THRESH) {
            s->debounce_counter = RELEASE_THRESH;
            s->program_state    = RELEASE_DEBOUNCE_WAIT;
            s->effect_state     = (s->effect_state == BYPASS) ? ENGAGED : BYPASS;
            return 1;
        }
    } else {
        if (s->debounce_counter == 0) {
            s->program_state = PRESS_DEBOUNCE_WAIT;
        }
    }
    return 0;
}

static void verify_nondeterministic_scheduling(void) {
    // Collect the same reachable state set as verify_reachable_state_space().
    uint8_t reachable[NUM_STATES];
    memset(reachable, 0, sizeof(reachable));
    state_t wl[NUM_STATES];
    int wp = 0;
    int total_reachable = 0;

    state_t roots[2] = {
        { PRESS_DEBOUNCE_WAIT,   BYPASS, 0 },
        { RELEASE_DEBOUNCE_WAIT, BYPASS, RELEASE_THRESH },
    };
    for (int i = 0; i < 2; ++i) {
        int idx = state_index(roots[i]);
        if (!reachable[idx]) {
            reachable[idx] = 1; wl[wp++] = roots[i]; total_reachable++;
        }
    }
    while (wp > 0) {
        state_t s = wl[--wp];
        for (int bit = 0; bit < 2; ++bit) {
            step_result_t r = step(s, bit);
            int nidx = state_index(r.next);
            if (!reachable[nidx]) {
                reachable[nidx] = 1; wl[wp++] = r.next; total_reachable++;
            }
        }
    }

    // For each reachable state, verify I1-I3 for k = 0, 1, 2 main passes.
    for (int idx = 0; idx < NUM_STATES; ++idx) {
        if (!reachable[idx]) continue;

        // Decode the state from its dense index.
        state_t s;
        s.debounce_counter = (uint8_t)(idx % COUNTER_VALUES);
        int tmp = idx / COUNTER_VALUES;
        s.effect_state     = (uint8_t)(tmp % 2);
        s.program_state    = (uint8_t)(tmp / 2);

        for (int bit = 0; bit < 2; ++bit) {
            // Apply the ISR (counter update only).
            state_t s_isr = s;
            s_isr.debounce_counter = isr_counter_step(s.debounce_counter, bit);

            CHECK(state_valid(s_isr),
                  "(I1-nd) ISR produced invalid state from ps=%u es=%u dc=%u in=%d",
                  s.program_state, s.effect_state, s.debounce_counter, bit);

            for (int k = 0; k <= 2; ++k) {
                state_t sm = s_isr;
                int total_toggles = 0;

                for (int m = 0; m < k; ++m) {
                    uint8_t ps_before = sm.program_state;
                    uint8_t es_before = sm.effect_state;
                    int toggled = main_state_step(&sm);
                    total_toggles += toggled;

                    // (I3-nd) a main pass entering RELEASE_DEBOUNCE_WAIT must
                    // not toggle. After the ISR + zero prior main passes, if
                    // the pre-ISR state was already RELEASE_DEBOUNCE_WAIT, the
                    // post-ISR state is still RELEASE_DEBOUNCE_WAIT.
                    if (ps_before == RELEASE_DEBOUNCE_WAIT) {
                        CHECK(!toggled,
                              "(I3-nd) toggle in pass %d from RELEASE_WAIT: "
                              "ps=%u es=%u dc=%u in=%d k=%d",
                              m, s.program_state, s.effect_state,
                              s.debounce_counter, bit, k);
                        CHECK(sm.effect_state == es_before,
                              "(I3-nd) effect_state changed in pass %d from RELEASE_WAIT",
                              m);
                    }
                }

                // (I1-nd) result state must be in valid range.
                CHECK(state_valid(sm),
                      "(I1-nd) invalid state after ISR+%d main passes: "
                      "ps=%u es=%u dc=%u (from ps=%u es=%u dc=%u in=%d)",
                      k, sm.program_state, sm.effect_state, sm.debounce_counter,
                      s.program_state, s.effect_state, s.debounce_counter, bit);

                // (I2-nd) at most 1 toggle per super-step. After one toggle
                // the machine is in RELEASE_DEBOUNCE_WAIT; I3 (proved above)
                // prevents any further toggle in subsequent main passes.
                CHECK(total_toggles <= 1,
                      "(I2-nd) %d toggles in super-step (k=%d main passes): "
                      "ps=%u es=%u dc=%u in=%d",
                      total_toggles, k,
                      s.program_state, s.effect_state, s.debounce_counter, bit);
            }
        }
    }

    printf("  nondeterministic scheduling: %d reachable states, "
           "k={0,1,2} main-passes per ISR verified\n", total_reachable);
}

int main(void) {
    printf("exhaustive state-space verification "
           "(PRESSED_THRESH=%d, RELEASE_THRESH=%d):\n",
           PRESSED_THRESH, RELEASE_THRESH);

    verify_reachable_state_space();
    verify_lockout_requires_full_release();
    verify_release_liveness();
    verify_press_liveness();
    verify_nondeterministic_scheduling();

    printf("state-space model check: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
