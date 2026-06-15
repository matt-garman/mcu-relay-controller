// simavr integration tests for attiny13_bypass.
//
// This runs the ACTUAL compiled firmware (attiny13_bypass.elf) inside the
// simavr instruction-accurate simulator, drives the footswitch pin (PB0)
// over simulated time, and asserts on the LED (PB1) and CD4053 (PB2) outputs.
//
// Unlike test_logic_host.c (a golden model), this exercises the real ISR,
// Timer0 configuration, sleep/wake, and main-loop state machine as compiled
// for the AVR.
//
// Build & run: see Makefile target `test-sim`.
//
// Pin mapping (from the firmware):
//   PB0 = footswitch input  : LOW = pressed, HIGH = released
//   PB1 = status LED output  : HIGH = engaged/lit, LOW = bypass/dark
//   PB2 = CD4053 ctrl output : HIGH when engaged, LOW when bypass

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sim_avr.h"
#include "sim_elf.h"
#include "avr_ioport.h"
#include "sim_irq.h"
#include "sim_vcd_file.h"

// Pull PRESSED_THRESH / RELEASE_THRESH (and the pin numbers) directly from the
// firmware's bypass_config.h, via the host shim, so the sim tests can never
// drift from the real firmware thresholds. The shim defines FOOTSW_PIN /
// LED_PIN / CD4053_PIN as PB0/PB1/PB2 == 0/1/2, which are exactly the IRQ
// indices this harness uses below.
#include "bypass_config_host.h"

#ifndef FW_PATH
#  define FW_PATH      "attiny13_bypass.elf"
#endif
#ifndef MCU_NAME
#  define MCU_NAME     "attiny13"
#endif
#ifndef F_CPU_HZ
#  define F_CPU_HZ     1200000UL
#endif

#ifndef SIM_RANDOM_NOISE_DURATION_MS
#define SIM_RANDOM_NOISE_DURATION_MS 60000u
#endif

// Expected toggle count for the fixed-seed (0xDEADBEEF) random-noise stream.
// This is duration-dependent and empirically measured against BOTH the real
// firmware (simavr) and the golden model -- they agree exactly. When changing
// SIM_RANDOM_NOISE_DURATION_MS, set a matching expected count (or 0 to disable
// the exact check and rely only on the physical ceiling).
//   5000 ms  -> 10 toggles
//   10000 ms -> 16 toggles
//   60000 ms -> 77 toggles
#ifndef SIM_NOISE_EXPECTED_TOGGLES
#  if (SIM_RANDOM_NOISE_DURATION_MS == 60000u)
#    define SIM_NOISE_EXPECTED_TOGGLES 77u
#  elif (SIM_RANDOM_NOISE_DURATION_MS == 5000u)
#    define SIM_NOISE_EXPECTED_TOGGLES 10u
#  else
#    define SIM_NOISE_EXPECTED_TOGGLES 0u /* 0 => skip exact check */
#  endif
#endif

#ifndef SIM_ADVERSARIAL_CYCLES
#define SIM_ADVERSARIAL_CYCLES 50
#endif

#ifndef SIM_EXTREME_BOUNCE_PRESSES
#define SIM_EXTREME_BOUNCE_PRESSES 10
#endif

#ifndef SIM_EXTREME_BOUNCE_CHATTER_MS
#define SIM_EXTREME_BOUNCE_CHATTER_MS 50
#endif

#ifndef SIM_SUSTAINED_NOISE_DURATION_MS
#define SIM_SUSTAINED_NOISE_DURATION_MS 10000u
#endif

// Number of asymmetric-EMI bursts (each burst = 50ms noise + 200ms silence,
// i.e. 250ms of simulated time). 240 bursts == 60s.
#ifndef SIM_EMI_BURSTS
#define SIM_EMI_BURSTS 240
#endif

// Number of simulated power-on cycles for the power-on robustness test.
#ifndef SIM_POWER_ON_BOOTS
#define SIM_POWER_ON_BOOTS 30
#endif

// Iterations for the odd/even toggle-parity invariant stream. Each iteration
// holds a random level for up to ~12ms of *real-firmware* simulated time, so
// this is the dominant cost of the sim suite; keep modest for `make test` and
// crank it up via -D for `make test-long`.
#ifndef SIM_PARITY_ITERS
#define SIM_PARITY_ITERS 400u
#endif

// Number of 1ms ticks driven through the lock-step co-simulation (firmware vs
// golden model, internal-state comparison every tick). Each tick is one full
// wake/process/sleep cycle of the real firmware, so this is comparable in cost
// to the parity stream; keep modest for `make test`, crank via -D for
// `make test-long`.
#ifndef SIM_LOCKSTEP_ITERS
#define SIM_LOCKSTEP_ITERS 5000u
#endif

// PB0/PB1/PB2 IRQ indices (these match FOOTSW_PIN/LED_PIN/CD4053_PIN from
// bypass_config.h, but the simavr IRQ API wants plain integers).
#define TIMSK_MEM_ADDR 0x59  // TIMSK0/TIMSK I/O addr 0x39 + SFR offset 0x20
#define DDRB_MEM_ADDR  0x37  // DDRB I/O addr 0x17 + SFR offset 0x20
#define PORTB_MEM_ADDR 0x38  // PORTB I/O addr 0x18 + SFR offset 0x20

// --- global sim state shared with output-watch callbacks -------------------
static avr_t      *g_avr = NULL;
static int         g_led_level    = 0; // current PB1 level
static int         g_cd4053_level = 0; // current PB2 level
static uint32_t    g_led_changes  = 0; // count of PB1 transitions
static int         g_saw_sleep    = 0; // set if CPU ever entered cpu_Sleeping
static int         g_saw_crash    = 0; // set if CPU ever hit cpu_Crashed (WDT)
// cycle timestamp of the most recent PB1 (LED) transition; used by the latency
// test to measure press->toggle time against the real firmware.
static avr_cycle_count_t g_last_led_change_cycle = 0;

// Resolved SRAM addresses of firmware globals (looked up from the ELF symbol
// table in sim_reset()). 0 == not found. Used by the fault-injection tests to
// corrupt firmware RAM and exercise the main-loop sanity-check path.
static uint32_t    g_addr_program_state = 0;
static uint32_t    g_addr_effect_state  = 0;
static uint32_t    g_addr_timer_isr     = 0;
static uint32_t    g_addr_debounce      = 0;

// --- test bookkeeping ------------------------------------------------------
// (unused in the TRACE build, which only generates a VCD waveform)
static int g_failures __attribute__((unused)) = 0;
static int g_checks   __attribute__((unused)) = 0;

#define CHECK(cond, ...) do {                                  \
    g_checks++;                                                \
    if (!(cond)) {                                             \
        g_failures++;                                          \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
        fprintf(stderr, __VA_ARGS__);                          \
        fprintf(stderr, "\n");                                 \
    }                                                          \
} while (0)

// Called by simavr whenever PB1 (LED) changes level.
static void led_hook(struct avr_irq_t *irq, uint32_t value, void *param) {
    (void)irq; (void)param;
    int v = value ? 1 : 0;
    if (v != g_led_level) {
        g_led_changes++;
        if (g_avr) { g_last_led_change_cycle = g_avr->cycle; }
    }
    g_led_level = v;
}

// Called by simavr whenever PB2 (CD4053) changes level.
static void cd4053_hook(struct avr_irq_t *irq, uint32_t value, void *param) {
    (void)irq; (void)param;
    g_cd4053_level = value ? 1 : 0;
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// Drive the footswitch pin. pressed != 0 => drive LOW (pressed).
static void footsw_set(int pressed) {
    avr_irq_t *pin = avr_io_getirq(g_avr,
                                   AVR_IOCTL_IOPORT_GETIRQ('B'),
                                   FOOTSW_PIN);
    // pin level: 1 = high (released), 0 = low (pressed)
    avr_raise_irq(pin, pressed ? 0 : 1);
}

// Run the simulation for `ms` milliseconds of simulated time.
// Tracks whether the CPU ever enters sleep (cpu_Sleeping) or crashes/
// watchdog-resets (cpu_Crashed) during the interval.
static void run_ms(unsigned ms) {
    // cycles per ms = F_CPU / 1000
    avr_cycle_count_t target = g_avr->cycle + (F_CPU_HZ / 1000UL) * (avr_cycle_count_t)ms;
    while (g_avr->cycle < target) {
        int st = avr_run(g_avr);
        if (st == cpu_Sleeping)  { g_saw_sleep = 1; }
        if (st == cpu_Crashed)   { g_saw_crash = 1; }
        if (st == cpu_Done || st == cpu_Crashed) {
            // cpu_Crashed is how simavr reports a watchdog reset; some tests
            // expect this, so don't spam unless it's a surprise Done.
            break;
        }
    }
}

static inline void footsw_drive(int pressed, unsigned ms) {
    footsw_set(pressed);
    run_ms(ms);
}

// Run until the CPU first enters IDLE sleep (which only happens in the main
// loop AFTER init() completes and the state machine has nothing to do), or
// until `cycle_budget` cycles elapse. Returns the cycle count at which sleep
// was first observed, or 0 if it never slept within the budget.
//
// This is the cleanest "init() finished and the main loop is live" signal:
// init() runs with interrupts disabled and never sleeps, so the first
// cpu_Sleeping marks the transition into steady-state operation.
static avr_cycle_count_t run_until_first_sleep(avr_cycle_count_t cycle_budget) {
    avr_cycle_count_t start  = g_avr->cycle;
    avr_cycle_count_t target = start + cycle_budget;
    while (g_avr->cycle < target) {
        int st = avr_run(g_avr);
        if (st == cpu_Sleeping) {
            g_saw_sleep = 1;
            return g_avr->cycle;
        }
        if (st == cpu_Crashed) { g_saw_crash = 1; return 0; }
        if (st == cpu_Done)    { return 0; }
    }
    return 0;
}

// (Re)load firmware and reset sim to a clean power-on state with the
// footswitch in the given initial position.
//
// `settle` controls whether we advance 5ms so init() finishes and the first
// ticks land before returning. Most tests want that (sim_reset()). The init()
// timing test wants the sim positioned EXACTLY at reset so it can measure how
// long init() takes, so it calls sim_reset_raw(..., 0).
static int sim_reset_raw(int footsw_pressed_at_power_on, int settle) {
    static elf_firmware_t fw; // persistent: avr keeps pointers into it
    memset(&fw, 0, sizeof(fw));

    if (elf_read_firmware(FW_PATH, &fw) != 0) {
        fprintf(stderr, "ERROR: cannot read firmware '%s'\n", FW_PATH);
        return -1;
    }
    fw.frequency = F_CPU_HZ;

    if (g_avr) { avr_terminate(g_avr); free(g_avr); g_avr = NULL; }

    g_avr = avr_make_mcu_by_name(MCU_NAME);
    if (!g_avr) {
        fprintf(stderr, "ERROR: unknown MCU '%s'\n", MCU_NAME);
        return -1;
    }
    avr_init(g_avr);
    avr_load_firmware(g_avr, &fw);
    g_avr->frequency = F_CPU_HZ;

    // Resolve firmware-global SRAM addresses from the ELF symbol table so the
    // fault-injection tests can poke them without hardcoding addresses. Symbol
    // addresses for the data space carry the 0x800000 marker; mask to the raw
    // SRAM index used by g_avr->data[].
    g_addr_program_state = g_addr_effect_state = 0;
    g_addr_timer_isr = g_addr_debounce = 0;
#if defined(ELF_SYMBOLS) && ELF_SYMBOLS
    for (uint32_t i = 0; i < fw.symbolcount; ++i) {
        const char *name = fw.symbol[i]->symbol;
        uint32_t    a    = fw.symbol[i]->addr & 0xFFFFu;
        if      (strcmp(name, "program_state_")    == 0) g_addr_program_state = a;
        else if (strcmp(name, "effect_state_")     == 0) g_addr_effect_state  = a;
        else if (strcmp(name, "timer_isr_called_") == 0) g_addr_timer_isr     = a;
        else if (strcmp(name, "debounce_counter_") == 0) g_addr_debounce      = a;
    }
#endif

    // reset instrumentation
    g_led_level = g_cd4053_level = 0;
    g_led_changes = 0;
    g_saw_sleep = 0;
    g_saw_crash = 0;
    g_last_led_change_cycle = 0;

    // Register output watchers on PB1 and PB2.
    avr_irq_register_notify(
        avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), LED_PIN),
        led_hook, NULL);
    avr_irq_register_notify(
        avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), CD4053_PIN),
        cd4053_hook, NULL);

    // Establish the footswitch level BEFORE the firmware samples it in init().
    footsw_set(footsw_pressed_at_power_on);

    // Let init() run and settle (clock, timer, first ticks) unless the caller
    // wants the sim left exactly at reset (init() timing measurement).
    if (settle) { run_ms(5); }
    return 0;
}

// Convenience wrapper: reset + 5ms settle (the behavior every existing test
// relies on).
static int sim_reset(int footsw_pressed_at_power_on) {
    return sim_reset_raw(footsw_pressed_at_power_on, 1);
}

//////////////////////////////////////////////////////////////////////////////
// Tests against the REAL firmware
//////////////////////////////////////////////////////////////////////////////
#ifndef TRACE

// Power-on default: BYPASS -> LED dark (PB1 low), PB2 low.
static void test_power_on_default(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    CHECK(g_led_level == 0, "power-on LED should be dark, got %d", g_led_level);
    CHECK(g_cd4053_level == 0, "power-on CD4053 should be low, got %d", g_cd4053_level);
}

// One clean press engages: LED lit, PB2 high, exactly one LED transition.
static void test_single_press_engages(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    uint32_t before = g_led_changes;
    footsw_set(1); run_ms(50);   // press & hold past threshold
    footsw_set(0); run_ms(50);   // release
    CHECK(g_led_level == 1, "after press LED should be lit, got %d", g_led_level);
    CHECK(g_cd4053_level == 1, "after press CD4053 high, got %d", g_cd4053_level);
    CHECK((g_led_changes - before) == 1,
          "exactly one LED transition, got %u", g_led_changes - before);
}

// Two presses return to BYPASS.
static void test_two_presses_round_trip(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50); // press 1
    CHECK(g_led_level == 1, "press1 -> lit");
    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50); // press 2
    CHECK(g_led_level == 0, "press2 -> dark");
}

// Holding for seconds does not repeat-toggle.
static void test_long_hold_single_toggle(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    uint32_t before = g_led_changes;
    footsw_set(1); run_ms(3000); // hold 3s
    CHECK((g_led_changes - before) == 1,
          "3s hold = single LED change, got %u", g_led_changes - before);
    CHECK(g_led_level == 1, "still engaged during hold");
    footsw_set(0); run_ms(50);
}

// Sub-threshold spike rejected (< PRESSED_THRESH ms ~ 8ms). Use 3ms.
static void test_short_spike_rejected(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    uint32_t before = g_led_changes;
    footsw_set(1); run_ms(3);     // brief spike
    footsw_set(0); run_ms(50);
    CHECK((g_led_changes - before) == 0,
          "short spike must not toggle, LED changes=%u", g_led_changes - before);
    CHECK(g_led_level == 0, "remain dark after spike");
}

// Power-on with footswitch held: stays BYPASS, no toggle while held; first
// real press only counts after release.
static void test_power_on_pressed(void) {
    if (sim_reset(1) != 0) { g_failures++; return; }
    CHECK(g_led_level == 0, "power-on-pressed stays dark");
    uint32_t before = g_led_changes;
    run_ms(500);                  // keep holding
    CHECK((g_led_changes - before) == 0,
          "no toggle while held at power-on, changes=%u", g_led_changes - before);
    footsw_set(0); run_ms(50);    // release
    footsw_set(1); run_ms(50);    // first real press
    CHECK(g_led_level == 1, "first real press engages after power-on hold");
    footsw_set(0); run_ms(50);
}

// Fast repeated taps each register.
static void test_fast_repeated_taps(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    uint32_t before = g_led_changes;
    int taps = 4;
    for (int i = 0; i < taps; ++i) {
        footsw_drive(1, 20);
        footsw_drive(0, 40);
    }
    CHECK((g_led_changes - before) == (uint32_t)taps,
          "%d taps -> %d LED changes, got %u", taps, taps, g_led_changes - before);
    CHECK(g_led_level == 0, "even taps -> back to dark");
}

// Random noise fuzz: 60s of random chatter should not spam toggles.
//
// This drives PB0 with a fixed-seed 50%-duty random stream. 50% duty is the
// integrator's worst case: the saturating counter random-walks around its
// midpoint and occasionally crosses PRESSED_THRESH, so a *handful* of toggles
// is expected and correct -- but the count must stay far below the physical
// ceiling.
//
// Bounds (for the default 60s / seed 0xDEADBEEF run):
//   - Hard physical ceiling: a real toggle needs at least
//     (PRESSED_THRESH + RELEASE_THRESH) = 33 ms, so 60000/33 ~= 1818 is the
//     absolute maximum any correct implementation could ever produce.
//   - Empirically, the real firmware (and the golden model, byte-for-byte)
//     produce EXACTLY SIM_NOISE_EXPECTED_TOGGLES for this seed/duration. We
//     assert that exact value as a tight regression lock, and also re-check
//     the physical ceiling as a defense-in-depth invariant.
//
// The old `< 2000` bound was nearly meaningless: it would pass even if the
// firmware toggled on essentially every threshold crossing.
static void test_random_noise_resilience(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t before = g_led_changes;
    uint32_t rng = 0xDEADBEEF;

    for (uint32_t t = 0; t < SIM_RANDOM_NOISE_DURATION_MS; ++t) {
        int pressed = (xorshift32(&rng) & 0xFF) < 128;
        footsw_drive(pressed, 1);
    }

    uint32_t toggles = g_led_changes - before;

    // Physical ceiling: at most one toggle per (PRESSED_THRESH+RELEASE_THRESH)
    // milliseconds, regardless of input. This invariant scales with duration
    // and seed, so it stays valid under -D overrides.
    uint32_t physical_max =
        SIM_RANDOM_NOISE_DURATION_MS / (PRESSED_THRESH + RELEASE_THRESH) + 1u;
    CHECK(toggles <= physical_max,
          "random noise exceeded physical toggle ceiling: %u > %u",
          toggles, physical_max);

    // Tight regression lock for the fixed seed + duration. SIM_NOISE_EXPECTED_
    // TOGGLES == 0 means "duration not a known calibration point, skip".
    if (SIM_NOISE_EXPECTED_TOGGLES != 0u) {
        CHECK(toggles == SIM_NOISE_EXPECTED_TOGGLES,
              "random noise toggle count drifted: got %u, expected %u "
              "(firmware/algorithm change? re-measure and update)",
              toggles, (unsigned)SIM_NOISE_EXPECTED_TOGGLES);
    }
}

// Adversarial thresholds: oscillate just below and just above PRESSED_THRESH.
static void test_adversarial_patterns(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t before = g_led_changes;
    for (int cycle = 0; cycle < SIM_ADVERSARIAL_CYCLES; ++cycle) {
        footsw_drive(1, PRESSED_THRESH - 1);
        footsw_drive(0, PRESSED_THRESH);
    }
    CHECK((g_led_changes - before) == 0,
          "sub-threshold oscillation should not toggle, got %u",
          g_led_changes - before);

    before = g_led_changes;
    for (int cycle = 0; cycle < SIM_ADVERSARIAL_CYCLES; ++cycle) {
        footsw_drive(1, PRESSED_THRESH + 1);
        footsw_drive(0, RELEASE_THRESH + 5);
    }
    CHECK((g_led_changes - before) == (uint32_t)SIM_ADVERSARIAL_CYCLES,
          "just-past-threshold presses should toggle %u times, got %u",
          (uint32_t)SIM_ADVERSARIAL_CYCLES, g_led_changes - before);
    CHECK(g_led_level == 0,
          "after even toggles LED should be dark, got %d", g_led_level);
}

// Extreme bounce: random chatter before each press should still yield one toggle.
static void test_extreme_bounce(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t before = g_led_changes;

    for (int press = 0; press < SIM_EXTREME_BOUNCE_PRESSES; ++press) {
        uint32_t rng = 0x12345678u + (uint32_t)press;
        for (int i = 0; i < SIM_EXTREME_BOUNCE_CHATTER_MS; ++i) {
            int pressed = xorshift32(&rng) & 1;
            footsw_drive(pressed, 1);
        }
        footsw_drive(1, 20);
        footsw_drive(0, 40);
    }

    CHECK((g_led_changes - before) == (uint32_t)SIM_EXTREME_BOUNCE_PRESSES,
          "extreme bounce should yield %u toggles, got %u",
          (uint32_t)SIM_EXTREME_BOUNCE_PRESSES, g_led_changes - before);
}

// Sustained 1kHz chatter should never reach the threshold.
static void test_sustained_noise(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t before = g_led_changes;
    for (uint32_t t = 0; t < SIM_SUSTAINED_NOISE_DURATION_MS; ++t) {
        int pressed = (t & 1) ? 1 : 0;
        footsw_drive(pressed, 1);
    }

    CHECK((g_led_changes - before) == 0,
          "sustained 1ms square wave should not toggle, got %u",
          g_led_changes - before);
    CHECK(g_led_level == 0, "square wave should leave LED dark, got %d", g_led_level);
}


//////////////////////////////////////////////////////////////////////////////
// Timing verification against the REAL firmware
//////////////////////////////////////////////////////////////////////////////

// Cycles per simulated millisecond at the configured CPU frequency.
#define CYCLES_PER_MS (F_CPU_HZ / 1000UL)

// (#2) init() / power-on timing: init() runs with interrupts disabled and never
// sleeps, so the first cpu_Sleeping marks "init() finished and main loop is
// idle". Measure how long that takes from reset and assert it completes WELL
// within the WDT window (~250ms nominal, but as low as ~100ms with the WDT's
// loose oscillator). The firmware header claims "100s of microseconds"; we
// require a generous <50ms ceiling so a future init() bloat that risks a
// WDT-reset loop fails here instead of bricking a board.
static void test_init_completes_before_wdt(void) {
    if (sim_reset_raw(0, 0) != 0) { g_failures++; return; }

    avr_cycle_count_t start = g_avr->cycle;
    // Give it up to 100ms of budget; we EXPECT it far sooner.
    avr_cycle_count_t slept_at =
        run_until_first_sleep((avr_cycle_count_t)(100UL * CYCLES_PER_MS));

    CHECK(slept_at != 0,
          "init() never reached idle sleep within 100ms (init() too long / hung?)");
    if (slept_at == 0) return;

    double init_ms = (double)(slept_at - start) / (double)CYCLES_PER_MS;
    printf("  init()->first-idle: %.3f ms (must be << WDT ~100-250ms)\n", init_ms);
    // The first sleep happens after init() AND one main-loop pass; the first
    // tick may need up to ~1ms. Require comfortably under the worst-case WDT.
    CHECK(init_ms < 50.0,
          "init() to first idle took %.3f ms; too close to WDT window", init_ms);
}

// (#2) Power-on sampling order: the footswitch level present at reset must be
// the one init() acts on. If held at power-on, the firmware enters
// RELEASE_DEBOUNCE_WAIT and must NOT toggle when later released (verified
// functionally elsewhere); here we assert the converse race direction -- a
// switch that is RELEASED at power-on must leave the device immediately ready
// so that a press arriving very soon after boot is honored.
static void test_power_on_sampling_race(void) {
    // Released at power-on, then press almost immediately after init settles.
    if (sim_reset(0) != 0) { g_failures++; return; }
    CHECK(g_led_level == 0, "released-at-power-on must boot dark");
    uint32_t before = g_led_changes;
    footsw_set(1); run_ms(50);
    CHECK((g_led_changes - before) == 1,
          "press right after boot should engage exactly once, got %u",
          g_led_changes - before);
    footsw_set(0); run_ms(50);
}

// (#2) Tick period: measure the real Timer0 ISR period by timing two
// consecutive LED-relevant events. We do this indirectly: drive a clean press
// and measure press->toggle latency, which is exactly PRESSED_THRESH ticks; the
// implied tick period must be ~1ms (the whole debounce-timing contract).
//
// (#6) Latency assertion on the REAL ELF: a clean press must toggle the LED in
// PRESSED_THRESH ticks, and the wall-clock latency must satisfy the <10ms
// design goal under nominal clock.
static void test_clean_press_latency(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t before = g_led_changes;
    footsw_set(1);                       // press
    avr_cycle_count_t press_cycle = g_avr->cycle;
    run_ms(50);                          // hold past threshold
    CHECK((g_led_changes - before) == 1, "clean press should toggle once");
    CHECK(g_led_level == 1, "clean press should engage (LED lit)");

    double latency_ms =
        (double)(g_last_led_change_cycle - press_cycle) / (double)CYCLES_PER_MS;
    printf("  clean-press latency (real ELF): %.3f ms "
           "(PRESSED_THRESH=%d ticks)\n", latency_ms, PRESSED_THRESH);

    // Implied tick period from the measured latency.
    double tick_ms = latency_ms / (double)PRESSED_THRESH;

    // The tick must be ~1ms (allow the +/-10% RC tolerance plus a little
    // measurement slack for when within the tick the press was sampled).
    CHECK(tick_ms >= 0.8 && tick_ms <= 1.25,
          "implied tick period %.4f ms is outside the ~1ms contract", tick_ms);

    // The headline latency goal: <10ms under nominal clock.
    CHECK(latency_ms <= 10.0,
          "clean-press latency %.3f ms exceeds the 10ms design goal", latency_ms);

    footsw_set(0); run_ms(50);
}

// (#6) Odd/even toggle PARITY invariant across a long random stream against the
// REAL firmware: the LED level must always equal (toggle_count is odd). i.e.
// engaged iff an odd number of state changes have occurred. This catches any
// firmware path that could change the LED without going through the single
// toggle point (e.g. a stray PORTB write, a missed inversion, a glitch).
static void test_toggle_parity_invariant(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t rng = 0x5A17C0DEu;
    uint32_t base = g_led_changes; // should be 0 at boot, but be robust

    // Drive a long, moderately-correlated random stream so we actually get a
    // healthy mix of real toggles (not just noise). Hold each level for a few
    // ms so presses can cross PRESSED_THRESH.
    const unsigned iters = SIM_PARITY_ITERS;
    for (unsigned i = 0; i < iters; ++i) {
        int pressed = (xorshift32(&rng) & 0xFF) < 96; // ~37% pressed
        unsigned hold = 1u + (xorshift32(&rng) % 12u);
        footsw_drive(pressed, hold);

        uint32_t toggles = g_led_changes - base;
        int expect_lit = (int)(toggles & 1u); // odd toggles => engaged/lit
        CHECK(g_led_level == expect_lit,
              "parity broken at i=%u: toggles=%u led=%d expected=%d",
              i, toggles, g_led_level, expect_lit);
        // CD4053 must track the LED exactly (both reflect engaged/bypass).
        CHECK(g_cd4053_level == g_led_level,
              "CD4053 (PB2=%d) diverged from LED (PB1=%d) at i=%u",
              g_cd4053_level, g_led_level, i);
    }
}


//////////////////////////////////////////////////////////////////////////////
// Lock-step co-simulation: firmware internal state vs golden model, EVERY tick.
//
// The output-only tests above prove the LED/CD4053 *transitions* match
// expectations. This test goes deeper: it drives the SAME input stream into the
// real firmware (simavr) and an independent golden model, and after every 1ms
// tick compares the firmware's internal RAM (debounce_counter_, program_state_,
// effect_state_) against the model's. That closes the binary<->model gap left
// open by the proofs in test_model_check.c / test_symbolic.c, which verify a
// re-implementation of the algorithm rather than the compiled binary: here the
// compiled firmware's full state trajectory must match the proven model tick
// for tick, not merely agree on the final toggle count.
//
// The model below is byte-identical to the golden model in test_logic_host.c
// and the step() in test_model_check.c / test_symbolic.c, and pulls its
// thresholds from bypass_config.h via the host shim.
//////////////////////////////////////////////////////////////////////////////

enum { LS_PRESS_WAIT = 0, LS_RELEASE_WAIT = 1 };
enum { LS_BYPASS = 0, LS_ENGAGED = 1 };

typedef struct {
    uint8_t program_state;
    uint8_t effect_state;
    uint8_t debounce_counter;
} ls_model_t;

static void ls_model_init(ls_model_t *m, int pressed_at_power_on) {
    m->effect_state = LS_BYPASS;
    if (pressed_at_power_on) {
        m->program_state    = LS_RELEASE_WAIT;
        m->debounce_counter = RELEASE_THRESH;
    } else {
        m->program_state    = LS_PRESS_WAIT;
        m->debounce_counter = 0;
    }
}

// One 1ms tick: ISR saturating integrator, then one main-loop state-machine
// pass. pin_low != 0 means PB0 reads low == switch pressed.
static void ls_model_step(ls_model_t *m, int pin_low) {
    if (pin_low) {
        if (m->debounce_counter < RELEASE_THRESH) { m->debounce_counter++; }
    } else {
        if (m->debounce_counter > 0) { m->debounce_counter--; }
    }
    if (m->program_state == LS_PRESS_WAIT) {
        if (m->debounce_counter >= PRESSED_THRESH) {
            m->debounce_counter = RELEASE_THRESH;
            m->program_state    = LS_RELEASE_WAIT;
            m->effect_state     = (m->effect_state == LS_BYPASS) ? LS_ENGAGED : LS_BYPASS;
        }
    } else {
        if (m->debounce_counter == 0) { m->program_state = LS_PRESS_WAIT; }
    }
}

// Advance the firmware by EXACTLY one 1ms Timer0 tick and leave it settled:
// wait for the compare-match ISR to wake the core, then run until it returns to
// IDLE sleep (main has fully reacted to this tick -- including the extra,
// non-sleeping main-loop pass on a toggle or re-arm). This is a phase- and
// drift-free tick boundary: the firmware disables pin-change interrupts, so
// changing PB0 never wakes the core -- only the timer does -- and the input set
// before this call is the one this single tick integrates.
//
// Returns 0 on success, -1 if the expected wake/sleep cycle did not occur
// within a safety budget (a crash or a stuck core -- itself a failure).
static int run_one_tick_settled(void) {
    const avr_cycle_count_t budget = (avr_cycle_count_t)(5UL * (F_CPU_HZ / 1000UL));
    avr_cycle_count_t start = g_avr->cycle;

    // 1. Wait for the core to WAKE (timer ISR fired). While idle, avr_run
    //    returns cpu_Sleeping and fast-forwards to the next timer event.
    for (;;) {
        int st = avr_run(g_avr);
        if (st == cpu_Crashed) { g_saw_crash = 1; return -1; }
        if (st == cpu_Done) return -1;
        if (st != cpu_Sleeping) break;
        if (g_avr->cycle - start > budget) return -1;
    }
    // 2. Wait until it SLEEPS again (main finished processing this tick).
    for (;;) {
        int st = avr_run(g_avr);
        if (st == cpu_Crashed) { g_saw_crash = 1; return -1; }
        if (st == cpu_Done) return -1;
        if (st == cpu_Sleeping) { g_saw_sleep = 1; break; }
        if (g_avr->cycle - start > budget) return -1;
    }
    return 0;
}

static void test_lockstep_cosim(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    CHECK(g_addr_debounce != 0 && g_addr_program_state != 0 && g_addr_effect_state != 0,
          "lock-step: could not resolve firmware global addresses (need ELF symbols)");
    if (!g_addr_debounce || !g_addr_program_state || !g_addr_effect_state) return;

    ls_model_t m;
    ls_model_init(&m, 0); // released at power-on, matching sim_reset(0)

    // Establish the sleeping precondition and verify the anchor: after the 5ms
    // settle with the switch released, firmware and model must already agree.
    run_until_first_sleep((avr_cycle_count_t)(10UL * (F_CPU_HZ / 1000UL)));
    CHECK(g_avr->data[g_addr_program_state] == m.program_state &&
          g_avr->data[g_addr_effect_state]  == m.effect_state  &&
          g_avr->data[g_addr_debounce]      == m.debounce_counter,
          "lock-step anchor mismatch: fw(ps=%u es=%u dc=%u) model(ps=%u es=%u dc=%u)",
          g_avr->data[g_addr_program_state], g_avr->data[g_addr_effect_state],
          g_avr->data[g_addr_debounce], m.program_state, m.effect_state, m.debounce_counter);

    uint32_t rng = 0xC051A1EDu;
    unsigned ticks = 0;
    unsigned mismatches = 0;
    unsigned toggles = 0; // sanity: confirm the stream actually exercises toggles

    // Outer loop picks a level and a hold duration; holds long enough to cross
    // PRESSED_THRESH (toggle) and RELEASE_THRESH (full re-arm), so every code
    // path -- press, lock-out, release, re-arm -- is exercised in lock-step.
    while (ticks < SIM_LOCKSTEP_ITERS && mismatches < 5) {
        int pin_low = ((xorshift32(&rng) & 0xFF) < 128); // ~50% pressed
        unsigned hold = 1u + (xorshift32(&rng) % 30u);   // up to 30 ticks
        for (unsigned h = 0; h < hold && ticks < SIM_LOCKSTEP_ITERS; ++h, ++ticks) {
            uint8_t es_before = m.effect_state;

            footsw_set(pin_low); // pressed => drive LOW
            if (run_one_tick_settled() != 0) {
                CHECK(0, "lock-step: firmware failed to complete tick %u "
                         "(crash or stuck core)", ticks);
                return;
            }
            ls_model_step(&m, pin_low);
            if (m.effect_state != es_before) { toggles++; }

            uint8_t fw_ps = g_avr->data[g_addr_program_state];
            uint8_t fw_es = g_avr->data[g_addr_effect_state];
            uint8_t fw_dc = g_avr->data[g_addr_debounce];

            int ok = (fw_ps == m.program_state)
                  && (fw_es == m.effect_state)
                  && (fw_dc == m.debounce_counter);
            CHECK(ok,
                  "lock-step divergence at tick %u (in=%d): "
                  "fw(ps=%u es=%u dc=%u) != model(ps=%u es=%u dc=%u)",
                  ticks, pin_low, fw_ps, fw_es, fw_dc,
                  m.program_state, m.effect_state, m.debounce_counter);
            if (!ok) { mismatches++; }

            // The firmware outputs must also track the model's effect state
            // exactly (LED lit / CD4053 high == ENGAGED).
            CHECK(g_led_level == (int)m.effect_state,
                  "lock-step: LED (PB1=%d) disagrees with model effect_state=%u at tick %u",
                  g_led_level, m.effect_state, ticks);
            CHECK(g_cd4053_level == (int)m.effect_state,
                  "lock-step: CD4053 (PB2=%d) disagrees with model effect_state=%u at tick %u",
                  g_cd4053_level, m.effect_state, ticks);
        }
    }

    CHECK(toggles >= 5,
          "lock-step: stream exercised only %u toggles (expected >=5); "
          "input not stimulating the toggle path", toggles);
    printf("  lock-step: %u ticks compared, %u toggles, %u mismatches\n",
           ticks, toggles, mismatches);
}


//////////////////////////////////////////////////////////////////////////////
// Sleep + watchdog behavior
//////////////////////////////////////////////////////////////////////////////

// The firmware should sleep (SLEEP_MODE_IDLE) between 1ms ticks while idle.
// simavr reports cpu_Sleeping when the core executes SLEEP and is idle.
static void test_enters_idle_sleep(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    g_saw_sleep = 0;
    footsw_set(0);      // released/idle
    run_ms(50);         // let it idle across many ticks
    CHECK(g_saw_sleep == 1, "firmware should enter IDLE sleep while waiting");
}

// Watchdog must NOT fire during normal operation: the timer ISR pets the dog
// every tick. Run a long idle period and confirm no crash/reset.
static void test_watchdog_not_tripped_normally(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    g_saw_crash = 0;
    footsw_set(0);
    run_ms(1000);       // 1s, well beyond the 250ms WDT window
    CHECK(g_saw_crash == 0, "watchdog must not reset during normal idle operation");
    // and the device should still respond afterwards
    uint32_t before = g_led_changes;
    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK((g_led_changes - before) == 1, "still responsive after long idle");
}

#ifdef TARGET_T85
// Watchdog BACKSTOP: verify WDT system reset on ATtiny85 (simavr ATtiny85
// model supports WDT reset).
//
// If the timer ISR stops running, the main loop never pets the dog, and the
// WDT (~250ms) performs a SYSTEM RESET.  The firmware must reinitialize in
// BYPASS and be fully responsive.
static void test_watchdog_backstop_reset(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    // Verify normal operation first
    CHECK(g_led_level == 0, "WDT test: power-on LED dark");
    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK(g_led_level == 1, "WDT test: press engages");
    uint32_t changes_before = g_led_changes;

    // Kill the timer interrupt: no more ticks -> main loop never pets the dog.
    avr_core_watch_write(g_avr, TIMSK_MEM_ADDR, 0x00);

    // Wait well past WDT timeout (250ms) for the system reset to fire.
    run_ms(600);

    // After WDT reset, firmware should reinit in BYPASS (LED dark, CD4053 low).
    // The fact that LED returned to dark proves the MCU was reset and init()
    // ran again correctly.
    CHECK(g_led_level == 0,
          "WDT test: after reset LED should be dark (reinit to BYPASS)");

    // Post-reset responsiveness is a hardware verification item: simavr resets
    // PINB to 0x00 on WDT reset, which puts the footswitch model in an
    // inconsistent state with respect to the external IRQ drive level.  The
    // firmware's power-on-pressed path handles this gracefully (LED stays
    // dark), but subsequent pin transitions in the sim do not match hardware
    // behavior.
    (void)changes_before;
}

// (#3) Watchdog timeout BOUND: not only must the WDT eventually reset after the
// ISR dies, it must do so within the part's WDT window. The AVR WDT oscillator
// is loose (~100-350ms for a nominal 250ms setting), so we assert the reset
// lands inside a generous [50ms, 500ms] envelope -- catching both a WDT that
// never fires AND one mis-configured to an absurdly long timeout.
static void test_watchdog_timeout_within_bound(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    // Engage so the LED is LIT; the post-reset reinit to BYPASS (LED dark) is
    // our reset timestamp.
    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK(g_led_level == 1, "WDT-bound: press engages before we break the ISR");
    if (g_led_level != 1) return;

    // Kill the timer ISR; record the moment.
    avr_core_watch_write(g_avr, TIMSK_MEM_ADDR, 0x00);
    avr_cycle_count_t kill_cycle = g_avr->cycle;

    // Run up to 500ms, stopping as soon as the LED goes dark (reset reinit).
    avr_cycle_count_t deadline =
        kill_cycle + (avr_cycle_count_t)(500UL * CYCLES_PER_MS);
    avr_cycle_count_t reset_cycle = 0;
    while (g_avr->cycle < deadline) {
        int st = avr_run(g_avr);
        if (st == cpu_Crashed) { g_saw_crash = 1; }
        if (g_led_level == 0) { reset_cycle = g_avr->cycle; break; }
        if (st == cpu_Done) break;
    }

    CHECK(reset_cycle != 0,
          "WDT-bound: device did not reset to BYPASS within 500ms of ISR death");
    if (reset_cycle == 0) return;

    double wdt_ms = (double)(reset_cycle - kill_cycle) / (double)CYCLES_PER_MS;
    printf("  WDT reset fired %.1f ms after ISR death "
           "(nominal 250ms, RC tolerance ~100-350ms)\n", wdt_ms);
    CHECK(wdt_ms >= 50.0 && wdt_ms <= 500.0,
          "WDT reset latency %.1f ms outside expected [50,500] ms envelope",
          wdt_ms);
}
#else
// Watchdog BACKSTOP (documented simavr limitation for ATtiny13).
//
// On real hardware, if the timer ISR stops running the main loop never pets
// the dog and the WDT (~250ms) performs a SYSTEM RESET.
//
// However, simavr 1.6's ATtiny13 model does NOT emulate the watchdog system
// reset: the WDT timer expires but the core is not reset (it just keeps
// sleeping/waking). simavr only reports cpu_Crashed for a CPU stuck asleep
// with interrupts globally disabled -- not for a watchdog timeout.
//
// What we CAN assert here is the weaker property that the firmware does not
// lock the CPU in a way that even simavr would flag. The real backstop check
// must be validated on hardware (e.g. scope PB1/PB2 and confirm the device
// resets to BYPASS ~250ms after the ISR is artificially stopped).
static void test_watchdog_backstop_documented(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    g_saw_crash = 0;
    footsw_set(0);
    run_ms(20);                       // confirm running normally first
    CHECK(g_saw_crash == 0, "should be healthy before we break the ISR");

    // Kill the timer interrupt: no more ticks -> main loop never pets the dog.
    avr_core_watch_write(g_avr, TIMSK_MEM_ADDR, 0x00);
    run_ms(600);

    // We do NOT assert a reset here (simavr can't model it). We only document
    // that the firmware keeps the global interrupt path alive (does not wedge
    // the CPU into simavr's crash detector). Hardware validates the true reset.
    CHECK(g_saw_crash == 0,
          "simavr cannot model WDT reset; verify backstop on hardware (see comment)");
}
#endif

// Register corruption recovery: corrupt DDRB/PORTB to trigger the firmware's
// sanity-check force_wdt_reset() path.
//
// On ATtiny85, simavr models WDT reset, so the firmware recovers and
// reinitializes in BYPASS.  On ATtiny13, the WDT is not fully modeled, so the
// weaker property (CPU enters stuck state without wedging simavr) is checked.
static void test_register_corruption_recovery(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK(g_led_level == 1, "corruption test: normal press engages");

    // Corrupt DDRB: clear the LED output bit, making PB1 an input.
    // This violates the main-loop sanity check.
    avr_core_watch_write(g_avr, DDRB_MEM_ADDR,
                         g_avr->data[DDRB_MEM_ADDR] & ~(1 << LED_PIN));

#ifdef TARGET_T85
    // ATtiny85: WDT reset is emulated.  Firmware recovers via reset.
    // The LED returning to dark proves the MCU reset and init() ran.
    run_ms(500);
    CHECK(g_led_level == 0,
          "corruption test: WDT reset recovered, LED dark (reinit BYPASS)");
    // Post-reset responsiveness for the corruption path has the same
    // PINB-reset-to-0x00 limitation documented in the WDT backstop test.
#else
    // ATtiny13: simavr does not emulate WDT reset.  After corruption, the
    // firmware hits the sanity check, calls force_wdt_reset() (cli + busy
    // loop).  Verify the CPU enters the stuck state without wedging simavr.
    g_saw_sleep = 0;
    run_ms(200);
    CHECK(g_saw_sleep == 0,
          "corruption test: ATtiny13 in stuck force_wdt_reset loop "
          "(no sleep with cli active)");
#endif
}

//////////////////////////////////////////////////////////////////////////////
// Fault injection: corrupt each variable checked by the main-loop sanity
// guard and confirm the firmware detects it and takes the force_wdt_reset()
// path. These exercise the sanity-check branches that the existing
// DDRB-corruption test does not reach:
//
//   firmware main() guard (attiny13_bypass.c):
//     program_state_ > RELEASE_DEBOUNCE_WAIT   -> invalid program state
//     effect_state_  > ENGAGED                 -> invalid effect state
//     timer_isr_called_ > TIMER_ISR_NOT_CALLED -> invalid handshake flag
//     footswitch pullup bit cleared in PORTB   -> lost input pullup
//   plus the switch() default: (program_state_ out of enum range).
//
// On ATtiny85 simavr models the WDT system reset, so we assert full recovery
// to BYPASS. On ATtiny13 the WDT reset is not modeled, so we assert the weaker
// property that the firmware wedges into the cli()+busy-loop (no further sleep)
// -- the same approach the existing register-corruption test uses.

// Shared helper: after injecting a fault, verify the firmware reacts.
//   t85: WDT reset fires -> firmware reinits -> LED dark (BYPASS).
//   t13: firmware enters force_wdt_reset() cli/busy loop -> no more sleeps.
static void expect_fault_response(const char *what) {
#ifdef TARGET_T85
    run_ms(500); // > WDT 250ms timeout
    CHECK(g_led_level == 0,
          "fault-inject [%s]: WDT reset recovered, LED dark (reinit BYPASS)",
          what);
#else
    g_saw_sleep = 0;
    run_ms(200);
    CHECK(g_saw_sleep == 0,
          "fault-inject [%s]: ATtiny13 stuck in force_wdt_reset loop "
          "(no sleep with cli active)", what);
#endif
}

// Corrupt program_state_ to an out-of-range value (hits both the explicit
// `program_state_ > RELEASE_DEBOUNCE_WAIT` guard and the switch() default).
static void test_fault_inject_program_state(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    CHECK(g_addr_program_state != 0,
          "fault-inject: could not resolve program_state_ address");
    if (g_addr_program_state == 0) return;

    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK(g_led_level == 1, "fault-inject [program_state_]: normal press engages");

    // 0xFF is far outside {PRESS_DEBOUNCE_WAIT, RELEASE_DEBOUNCE_WAIT}.
    avr_core_watch_write(g_avr, g_addr_program_state, 0xFF);
    expect_fault_response("program_state_");
}

// Corrupt effect_state_ to an out-of-range value (> ENGAGED).
static void test_fault_inject_effect_state(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    CHECK(g_addr_effect_state != 0,
          "fault-inject: could not resolve effect_state_ address");
    if (g_addr_effect_state == 0) return;

    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK(g_led_level == 1, "fault-inject [effect_state_]: normal press engages");

    avr_core_watch_write(g_avr, g_addr_effect_state, 0x7F);
    expect_fault_response("effect_state_");
}

// Corrupt timer_isr_called_ to an out-of-range value
// (> TIMER_ISR_NOT_CALLED).
//
// NOTE: the timer ISR rewrites this flag to TIMER_ISR_CALLED every 1ms, so a
// corrupted value only survives long enough to be seen by main() if main()
// happens to read it within that window. On the ATtiny85 build the WDT reset
// gives us a deterministic signal once main() catches it, so we retry across
// several ticks. On the ATtiny13 build (no modeled WDT reset) the catch is a
// tight, non-deterministic race that we cannot reliably win from the test
// harness, so this particular injection is t85-only.
#ifdef TARGET_T85
static void test_fault_inject_timer_isr_flag(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    CHECK(g_addr_timer_isr != 0,
          "fault-inject: could not resolve timer_isr_called_ address");
    if (g_addr_timer_isr == 0) return;

    footsw_set(0); run_ms(20);

    // Poke the corrupted value once per tick over a window; the WDT reset will
    // fire the first time main() observes a >1 value before being overwritten.
    for (int i = 0; i < 300; ++i) {
        avr_core_watch_write(g_avr, g_addr_timer_isr, 0x55);
        run_ms(1);
        if (g_led_level == 0 && i > 5) {
            break; // already reset; stop poking
        }
    }
    // > WDT timeout window has elapsed within the loop above; confirm BYPASS.
    CHECK(g_led_level == 0,
          "fault-inject [timer_isr_called_]: WDT reset recovered, LED dark");
}
#endif

// Clear the footswitch pullup bit in PORTB: the firmware's sanity check
// requires PORTB & (1<<FOOTSW_PIN) to remain set.
static void test_fault_inject_lost_pullup(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK(g_led_level == 1, "fault-inject [pullup]: normal press engages");

    avr_core_watch_write(g_avr, PORTB_MEM_ADDR,
                         g_avr->data[PORTB_MEM_ADDR] & (uint8_t)~(1 << FOOTSW_PIN));
    expect_fault_response("PORTB pullup");
}

// Clear the CD4053 output direction bit in DDRB (companion to the existing LED
// DDRB test; together they cover both required output-direction bits).
static void test_fault_inject_cd4053_ddr(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
    CHECK(g_led_level == 1, "fault-inject [cd4053 DDR]: normal press engages");

    avr_core_watch_write(g_avr, DDRB_MEM_ADDR,
                         g_avr->data[DDRB_MEM_ADDR] & (uint8_t)~(1 << CD4053_PIN));
    expect_fault_response("CD4053 DDR");
}

static void run_fault_injection_suite(void) {
    test_fault_inject_program_state();
    test_fault_inject_effect_state();
#ifdef TARGET_T85
    test_fault_inject_timer_isr_flag();
#endif
    test_fault_inject_lost_pullup();
    test_fault_inject_cd4053_ddr();
}

// Oscillator drift tolerance: verify the <10ms press-latency goal holds across
// the ±10% RC oscillator tolerance documented in the design spec.
//
// The simavr firmware is cycle-accurate, so drift cannot be modeled by changing
// g_avr->frequency (run_ms uses compile-time F_CPU_HZ and the timer fires on
// cycle counts regardless). Instead we measure cycle latency to first toggle,
// convert to ticks (cycles_per_tick = F_CPU_HZ/1000 is exact for both MCUs),
// then interpret those ticks at the drifted real-world frequency. A +10% clock
// means each tick completes in 1ms/1.1 ≈ 0.909ms of real time; the design goal
// of <10ms must hold for the worst case (-10%: 1 tick = 1.111ms, so
// PRESSED_THRESH=8 ticks × 1.111ms = 8.89ms).
static void test_oscillator_drift_tolerance(void) {
    static const double drift_factors[] = { 0.9, 1.1 };
    // Exact cycles per 1ms tick for both MCUs (f/prescaler/(OCR0A+1) = 1000 Hz).
    const double cycles_per_tick = (double)(F_CPU_HZ / 1000UL);

    for (int f = 0; f < 2; ++f) {
        if (sim_reset(0) != 0) { g_failures++; return; }

        CHECK(g_led_level == 0, "drift %.1fx: power-on BYPASS", drift_factors[f]);

        uint32_t before = g_led_changes;
        footsw_set(1);
        avr_cycle_count_t press_cycle = g_avr->cycle;
        // Hold for 20ms (16+ ticks) -- well past PRESSED_THRESH even at worst drift.
        run_ms(20);

        CHECK((g_led_changes - before) == 1,
              "drift %.1fx: press should toggle exactly once, got %u",
              drift_factors[f], g_led_changes - before);

        if ((g_led_changes - before) == 1) {
            // Convert cycle latency to real wall-clock ms at the drifted frequency.
            // At drift d: 1 tick = (cycles_per_tick / (F_CPU_HZ * d)) seconds
            //           = 1ms / d real time.
            double ticks = (double)(g_last_led_change_cycle - press_cycle) / cycles_per_tick;
            double latency_ms = ticks / drift_factors[f];
            printf("  drift %.1fx: latency %.2f ms "
                   "(PRESSED_THRESH=%d ticks, goal <10ms)\n",
                   drift_factors[f], latency_ms, PRESSED_THRESH);
            CHECK(latency_ms <= 10.0,
                  "drift %.1fx: latency %.2f ms exceeds the 10ms design goal",
                  drift_factors[f], latency_ms);
        }

        footsw_set(0); run_ms(50);
        before = g_led_changes;
        footsw_set(1); run_ms(20); footsw_set(0); run_ms(50);
        CHECK((g_led_changes - before) == 1,
              "drift %.1fx: second press should toggle once, got %u",
              drift_factors[f], g_led_changes - before);
        CHECK(g_led_level == 0, "drift %.1fx: round trip returns to BYPASS", drift_factors[f]);
    }
}

// Asymmetric EMI bursts: 50ms ON at 500Hz, 200ms OFF.  Models cell-phone
// TDMA handshake interference near audio gear.
static void test_asymmetric_emi_bursts(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t changes_before = g_led_changes;

    for (int burst = 0; burst < SIM_EMI_BURSTS; ++burst) {
        for (int i = 0; i < 50; ++i) { footsw_drive(i & 1, 1); }
        footsw_drive(0, 200);
    }

    uint32_t toggles = g_led_changes - changes_before;
    CHECK(toggles == 0, "asymmetric EMI: no toggle from bursty interference");
    CHECK(g_led_level == 0, "asymmetric EMI: remained dark (BYPASS)");
}

// Power-on robustness: simulate multiple power cycles, verify consistent
// BYPASS initialization and responsiveness.
static void test_power_on_robustness(void) {
    for (int boot = 0; boot < SIM_POWER_ON_BOOTS; ++boot) {
        if (sim_reset(0) != 0) { g_failures++; return; }

        CHECK(g_led_level == 0, "power-on boot %d: always BYPASS", boot);

        run_ms(50);
        CHECK(g_led_level == 0, "power-on boot %d: stays dark idle", boot);

        footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
        CHECK(g_led_level == 1, "power-on boot %d: press engages", boot);
        footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
        CHECK(g_led_level == 0, "power-on boot %d: return to BYPASS", boot);
    }
}
#endif // !TRACE

//////////////////////////////////////////////////////////////////////////////
// VCD waveform trace (opt-in; built when TRACE is defined)
//////////////////////////////////////////////////////////////////////////////
#ifdef TRACE
// Produce a GTKWave-viewable trace of PB0/PB1/PB2 through a representative
// press/release sequence. Writes bypass_trace.vcd in the CWD.
static int generate_trace(void) {
    if (sim_reset(0) != 0) { return 1; }

    avr_vcd_t vcd;
    if (avr_vcd_init(g_avr, "bypass_trace.vcd", &vcd, 1000 /*usec flush*/) != 0) {
        fprintf(stderr, "ERROR: avr_vcd_init failed\n");
        return 1;
    }
    avr_vcd_add_signal(&vcd,
        avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), FOOTSW_PIN),
        1, "PB0_footswitch");
    avr_vcd_add_signal(&vcd,
        avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), LED_PIN),
        1, "PB1_LED");
    avr_vcd_add_signal(&vcd,
        avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), CD4053_PIN),
        1, "PB2_CD4053");

    avr_vcd_start(&vcd);

    // Scenario: idle, press (engage), release, press (bypass), release.
    footsw_set(0); run_ms(30);
    footsw_set(1); run_ms(40);
    footsw_set(0); run_ms(40);
    footsw_set(1); run_ms(40);
    footsw_set(0); run_ms(40);

    avr_vcd_stop(&vcd);
    avr_vcd_close(&vcd);
    printf("wrote bypass_trace.vcd (open with: gtkwave bypass_trace.vcd)\n");
    return 0;
}
#endif

int main(int argc, char **argv) {
#ifdef TRACE
    (void)argc; (void)argv;
    int rc = generate_trace();
    if (g_avr) { avr_terminate(g_avr); free(g_avr); }
    return rc;
#else
    // `test_sim fault-inject` runs ONLY the fault-injection suite (used by the
    // Makefile `test-fault-inject` target against the ATtiny85 build). With no
    // argument, run the full suite, which includes fault injection.
    int fault_only = (argc > 1 && strcmp(argv[1], "fault-inject") == 0);

    if (fault_only) {
        run_fault_injection_suite();
        if (g_avr) { avr_terminate(g_avr); free(g_avr); }
        printf("\nsimavr fault-injection tests: %d checks, %d failures\n",
               g_checks, g_failures);
        return g_failures ? 1 : 0;
    }

    test_power_on_default();
    test_single_press_engages();
    test_two_presses_round_trip();
    test_long_hold_single_toggle();
    test_short_spike_rejected();
    test_power_on_pressed();
    test_fast_repeated_taps();
    test_random_noise_resilience();
    test_adversarial_patterns();
    test_extreme_bounce();
    test_sustained_noise();
    test_init_completes_before_wdt();
    test_power_on_sampling_race();
    test_clean_press_latency();
    test_toggle_parity_invariant();
    test_lockstep_cosim();
    test_enters_idle_sleep();
    test_watchdog_not_tripped_normally();
#ifdef TARGET_T85
    test_watchdog_backstop_reset();
    test_watchdog_timeout_within_bound();
#else
    test_watchdog_backstop_documented();
#endif
    test_register_corruption_recovery();
    run_fault_injection_suite();
    test_oscillator_drift_tolerance();
    test_asymmetric_emi_bursts();
    test_power_on_robustness();

    if (g_avr) { avr_terminate(g_avr); free(g_avr); }

    printf("\nsimavr firmware tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
#endif
}
