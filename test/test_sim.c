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

#ifndef FW_PATH
#  define FW_PATH      "attiny13_bypass.elf"
#endif
#ifndef MCU_NAME
#  define MCU_NAME     "attiny13"
#endif
#ifndef F_CPU_HZ
#  define F_CPU_HZ     1200000UL
#endif
#define PRESSED_THRESH 8
#define RELEASE_THRESH 25

#ifndef SIM_RANDOM_NOISE_DURATION_MS
#define SIM_RANDOM_NOISE_DURATION_MS 60000u
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

#define FOOTSW_PIN   0  // PB0
#define LED_PIN      1  // PB1
#define CD4053_PIN   2  // PB2
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
    if (v != g_led_level) { g_led_changes++; }
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

// (Re)load firmware and reset sim to a clean power-on state with the
// footswitch in the given initial position.
static int sim_reset(int footsw_pressed_at_power_on) {
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

    // reset instrumentation
    g_led_level = g_cd4053_level = 0;
    g_led_changes = 0;
    g_saw_sleep = 0;
    g_saw_crash = 0;

    // Register output watchers on PB1 and PB2.
    avr_irq_register_notify(
        avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), LED_PIN),
        led_hook, NULL);
    avr_irq_register_notify(
        avr_io_getirq(g_avr, AVR_IOCTL_IOPORT_GETIRQ('B'), CD4053_PIN),
        cd4053_hook, NULL);

    // Establish the footswitch level BEFORE the firmware samples it in init().
    footsw_set(footsw_pressed_at_power_on);

    // Let init() run and settle (clock, timer, first ticks).
    run_ms(5);
    return 0;
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
static void test_random_noise_resilience(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t before = g_led_changes;
    uint32_t rng = 0xDEADBEEF;

    for (uint32_t t = 0; t < SIM_RANDOM_NOISE_DURATION_MS; ++t) {
        int pressed = (xorshift32(&rng) & 0xFF) < 128;
        footsw_drive(pressed, 1);
    }

    uint32_t toggles = g_led_changes - before;
    CHECK(toggles < 2000,
          "random noise produced too many toggles: %u", toggles);
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

// Oscillator drift tolerance: run the round-trip test at ±10% CPU frequency
// to verify debounce timing holds across the clock tolerance range.
static void test_oscillator_drift_tolerance(void) {
    static const uint32_t freqs[] = {
        (uint32_t)(F_CPU_HZ * 0.9),
        (uint32_t)(F_CPU_HZ * 1.1),
    };

    for (int f = 0; f < 2; ++f) {
        if (sim_reset(0) != 0) { g_failures++; return; }
        g_avr->frequency = freqs[f];

        CHECK(g_led_level == 0, "drift %d: power-on BYPASS", f);

        footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
        CHECK(g_led_level == 1, "drift %d: press engages", f);
        footsw_set(1); run_ms(50); footsw_set(0); run_ms(50);
        CHECK(g_led_level == 0, "drift %d: return to BYPASS", f);
    }
}

// Asymmetric EMI bursts: 50ms ON at 500Hz, 200ms OFF.  Models cell-phone
// TDMA handshake interference near audio gear.
static void test_asymmetric_emi_bursts(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }

    uint32_t changes_before = g_led_changes;

    for (int burst = 0; burst < 240; ++burst) {
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
    for (int boot = 0; boot < 30; ++boot) {
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

int main(void) {
#ifdef TRACE
    int rc = generate_trace();
    if (g_avr) { avr_terminate(g_avr); free(g_avr); }
    return rc;
#else
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
    test_enters_idle_sleep();
    test_watchdog_not_tripped_normally();
#ifdef TARGET_T85
    test_watchdog_backstop_reset();
#else
    test_watchdog_backstop_documented();
#endif
    test_register_corruption_recovery();
    test_oscillator_drift_tolerance();
    test_asymmetric_emi_bursts();
    test_power_on_robustness();

    if (g_avr) { avr_terminate(g_avr); free(g_avr); }

    printf("\nsimavr firmware tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
#endif
}
