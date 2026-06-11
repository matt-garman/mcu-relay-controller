// Host-compiled "golden model" unit tests for the attiny13_bypass debounce
// state machine.
//
// IMPORTANT: this file does NOT include the firmware. It re-implements the
// documented algorithm (the saturating integrator + the PRESS/RELEASE state
// machine) as an independent reference ("golden model"), then verifies that
// the algorithm itself satisfies the design's reliability goals. The simavr
// integration tests (test_sim.c) verify that the *real* firmware matches.
//
// Build & run (see Makefile target `test-host`):
//   cc -std=c11 -Wall -Wextra -Werror test/test_logic_host.c -o test/test_logic_host
//   ./test/test_logic_host
//
// The thresholds below MUST be kept in sync with attiny13_bypass.c.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

//////////////////////////////////////////////////////////////////////////////
// Golden model: mirrors attiny13_bypass.c constants and logic exactly.
//////////////////////////////////////////////////////////////////////////////

#define RELEASE_THRESH 25
#define PRESSED_THRESH 8

typedef enum { PRESS_DEBOUNCE_WAIT = 0, RELEASE_DEBOUNCE_WAIT } program_state_t;
typedef enum { BYPASS = 0, ENGAGED } effect_state_t;

typedef struct {
    program_state_t program_state;
    effect_state_t  effect_state;
    uint8_t         debounce_counter;
    // instrumentation for tests:
    uint32_t        toggle_count; // number of effect-state changes
} model_t;

// Initialize the model the way init() does.
// pressed_at_power_on mirrors the "footswitch held during power-on" case.
static void model_init(model_t *m, int pressed_at_power_on) {
    memset(m, 0, sizeof(*m));
    m->effect_state = BYPASS; // set_bypass_state()
    if (pressed_at_power_on) {
        m->program_state    = RELEASE_DEBOUNCE_WAIT;
        m->debounce_counter = RELEASE_THRESH;
    } else {
        m->program_state    = PRESS_DEBOUNCE_WAIT;
        m->debounce_counter = 0;
    }
}

// One 1ms timer tick: the saturating integrator update from the ISR.
// pin_low != 0 means PB0 read low == switch closed/pressed.
static void model_tick_isr(model_t *m, int pin_low) {
    if (pin_low) {
        if (m->debounce_counter < RELEASE_THRESH) { ++m->debounce_counter; }
    } else {
        if (m->debounce_counter > 0) { --m->debounce_counter; }
    }
}

// One pass of the main loop state machine (the part that consumes the
// integrator result and toggles effect state).
static void model_main_step(model_t *m) {
    switch (m->program_state) {
    case PRESS_DEBOUNCE_WAIT:
        if (m->debounce_counter >= PRESSED_THRESH) {
            m->debounce_counter = RELEASE_THRESH;
            m->program_state    = RELEASE_DEBOUNCE_WAIT;
            m->effect_state     = (m->effect_state == BYPASS) ? ENGAGED : BYPASS;
            m->toggle_count++;
        }
        break;
    case RELEASE_DEBOUNCE_WAIT:
        if (m->debounce_counter == 0) {
            m->program_state = PRESS_DEBOUNCE_WAIT;
        }
        break;
    }
}

// Advance the model by one millisecond: ISR runs, then main loop reacts.
// (In the firmware main() may run many times per ms while sleeping between
// ticks, but the state machine is idempotent within a tick, so once per ms
// is a faithful model.)
static void model_step_ms(model_t *m, int pin_low) {
    model_tick_isr(m, pin_low);
    model_main_step(m);
}

//////////////////////////////////////////////////////////////////////////////
// Test harness
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

// Helper: drive the model with a level (1=pressed/low, 0=released/high) for
// `ms` milliseconds.
static void drive(model_t *m, int pin_low, int ms) {
    for (int i = 0; i < ms; ++i) { model_step_ms(m, pin_low); }
}

//////////////////////////////////////////////////////////////////////////////
// Tests
//////////////////////////////////////////////////////////////////////////////

// Reliability goal: one clean press generates exactly one state change.
static void test_single_clean_press(void) {
    model_t m; model_init(&m, 0);
    CHECK(m.effect_state == BYPASS, "should power on in BYPASS");

    drive(&m, 1, 50);  // hold pressed well past PRESSED_THRESH
    CHECK(m.effect_state == ENGAGED, "one press should engage");
    CHECK(m.toggle_count == 1, "exactly one toggle, got %u", m.toggle_count);

    drive(&m, 0, 50);  // release, well past RELEASE_THRESH
    CHECK(m.toggle_count == 1, "release must NOT toggle, got %u", m.toggle_count);
    CHECK(m.program_state == PRESS_DEBOUNCE_WAIT, "should be ready for next press");
}

// Reliability goal: one release generates zero state changes.
// And two presses toggle back to BYPASS.
static void test_two_presses_round_trip(void) {
    model_t m; model_init(&m, 0);
    drive(&m, 1, 50); drive(&m, 0, 50);  // press 1
    CHECK(m.effect_state == ENGAGED, "after press 1: ENGAGED");
    drive(&m, 1, 50); drive(&m, 0, 50);  // press 2
    CHECK(m.effect_state == BYPASS, "after press 2: back to BYPASS");
    CHECK(m.toggle_count == 2, "exactly two toggles, got %u", m.toggle_count);
}

// Reliability goal: holding the switch for seconds must not repeat-toggle.
static void test_long_hold_single_toggle(void) {
    model_t m; model_init(&m, 0);
    drive(&m, 1, 5000);  // hold for 5 seconds
    CHECK(m.toggle_count == 1, "5s hold = single toggle, got %u", m.toggle_count);
    CHECK(m.effect_state == ENGAGED, "still engaged during hold");
}

// Reliability goal: switch bounce must never generate multiple state changes.
// Model a bouncy press: rapid low/high chatter that nets to a press.
static void test_bouncy_press_single_toggle(void) {
    model_t m; model_init(&m, 0);
    // 40ms of chatter: alternating 1ms low / 1ms high. Net integrator drift
    // is +0 per pair until contact settles, then settle low.
    for (int i = 0; i < 20; ++i) { model_step_ms(&m, 1); model_step_ms(&m, 0); }
    // then settle pressed
    drive(&m, 1, 50);
    drive(&m, 0, 50);
    CHECK(m.toggle_count == 1, "bouncy press = single toggle, got %u", m.toggle_count);
}

// Reliability goal: short EMI/RFI spikes shorter than PRESSED_THRESH must be
// rejected (no toggle).
static void test_short_spike_rejected(void) {
    model_t m; model_init(&m, 0);
    // Spike: PRESSED_THRESH-1 ms low, then released. Must not reach threshold.
    drive(&m, 1, PRESSED_THRESH - 1);
    CHECK(m.toggle_count == 0, "sub-threshold spike must not toggle, got %u", m.toggle_count);
    drive(&m, 0, 50);
    CHECK(m.toggle_count == 0, "still no toggle after spike clears, got %u", m.toggle_count);
    CHECK(m.effect_state == BYPASS, "remain in BYPASS after spike");
}

// Exactly-at-threshold press should toggle (boundary test).
static void test_exact_threshold_toggles(void) {
    model_t m; model_init(&m, 0);
    drive(&m, 1, PRESSED_THRESH); // exactly threshold ticks low
    CHECK(m.toggle_count == 1, "exactly PRESSED_THRESH should toggle, got %u", m.toggle_count);
}

// Power-on-with-switch-held: stay BYPASS, no toggle, become ready only after
// release.
static void test_power_on_pressed(void) {
    model_t m; model_init(&m, 1); // held at power-on
    CHECK(m.effect_state == BYPASS, "power-on-pressed stays BYPASS");
    CHECK(m.program_state == RELEASE_DEBOUNCE_WAIT, "armed for release");

    drive(&m, 1, 1000);  // keep holding 1s
    CHECK(m.toggle_count == 0, "no toggle while held at power-on, got %u", m.toggle_count);

    drive(&m, 0, 50);    // release
    CHECK(m.program_state == PRESS_DEBOUNCE_WAIT, "ready after release");
    CHECK(m.toggle_count == 0, "release after power-on-hold must not toggle, got %u", m.toggle_count);

    // now a real press should work
    drive(&m, 1, 50);
    CHECK(m.effect_state == ENGAGED, "first real press engages after power-on-hold");
    CHECK(m.toggle_count == 1, "exactly one toggle after release+press, got %u", m.toggle_count);
}

// Lock-out: during RELEASE_DEBOUNCE_WAIT, a brief re-press (bounce) should not
// produce a second toggle and should not get "stuck".
static void test_release_lockout(void) {
    model_t m; model_init(&m, 0);
    drive(&m, 1, 20);          // press -> engaged, enters RELEASE_DEBOUNCE_WAIT
    CHECK(m.toggle_count == 1, "engaged once");
    // bounce during release: a few low blips
    for (int i = 0; i < 5; ++i) { model_step_ms(&m, 0); model_step_ms(&m, 1); }
    drive(&m, 0, 50);          // finally release cleanly
    CHECK(m.toggle_count == 1, "no extra toggle from release bounce, got %u", m.toggle_count);
    CHECK(m.program_state == PRESS_DEBOUNCE_WAIT, "recovered to press-wait");
}

// Fast repeated taps (>=~30ms apart per design) are each recognized.
static void test_fast_repeated_taps(void) {
    model_t m; model_init(&m, 0);
    int expected = 0;
    for (int tap = 0; tap < 5; ++tap) {
        drive(&m, 1, 15);  // press (>= PRESSED_THRESH)
        drive(&m, 0, 30);  // release (>= RELEASE_THRESH worth of decrement)
        expected++;
        CHECK(m.toggle_count == (uint32_t)expected,
              "tap %d should yield %d toggles, got %u", tap, expected, m.toggle_count);
    }
    CHECK(m.effect_state == ENGAGED, "odd number of taps -> ENGAGED");
}

int main(void) {
    test_single_clean_press();
    test_two_presses_round_trip();
    test_long_hold_single_toggle();
    test_bouncy_press_single_toggle();
    test_short_spike_rejected();
    test_exact_threshold_toggles();
    test_power_on_pressed();
    test_release_lockout();
    test_fast_repeated_taps();

    printf("\nhost golden-model tests: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
