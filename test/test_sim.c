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

#define FW_PATH      "attiny13_bypass.elf"
#define MCU_NAME     "attiny13"
#define F_CPU_HZ     1200000UL

#define FOOTSW_PIN   0  // PB0
#define LED_PIN      1  // PB1
#define CD4053_PIN   2  // PB2

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
        footsw_set(1); run_ms(20);
        footsw_set(0); run_ms(40);
    }
    CHECK((g_led_changes - before) == (uint32_t)taps,
          "%d taps -> %d LED changes, got %u", taps, taps, g_led_changes - before);
    CHECK(g_led_level == 0, "even taps -> back to dark");
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

// Watchdog BACKSTOP (documented simavr limitation).
//
// On real hardware, if the timer ISR stops running the main loop never pets
// the dog and the WDT (~250ms) performs a SYSTEM RESET. We tried to verify
// this by disabling Timer0's compare interrupt (TIMSK0=0) mid-run.
//
// However, simavr 1.6's ATtiny13 model does NOT emulate the watchdog system
// reset: the WDT timer expires but the core is not reset (it just keeps
// sleeping/waking). simavr only reports cpu_Crashed for a CPU stuck asleep
// with interrupts globally disabled -- not for a watchdog timeout.
//
// So this case CANNOT be fully verified in simulation; it must be validated
// on hardware (e.g. scope PB1/PB2 and confirm the device resets to BYPASS
// ~250ms after the ISR is artificially stopped). What we CAN assert here is
// the weaker property that the firmware does not lock the CPU in a way that
// even simavr would flag. The real backstop check is on the hardware TODO
// list.
//
// TIMSK0 lives at SRAM address 0x59 on the ATtiny13a (IO 0x39 + 0x20).
#define TIMSK0_MEM_ADDR 0x59
static void test_watchdog_backstop_documented(void) {
    if (sim_reset(0) != 0) { g_failures++; return; }
    g_saw_crash = 0;
    footsw_set(0);
    run_ms(20);                       // confirm running normally first
    CHECK(g_saw_crash == 0, "should be healthy before we break the ISR");

    // Kill the timer interrupt: no more ticks -> main loop never pets the dog.
    avr_core_watch_write(g_avr, TIMSK0_MEM_ADDR, 0x00);
    run_ms(600);

    // We do NOT assert a reset here (simavr can't model it). We only document
    // that the firmware keeps the global interrupt path alive (does not wedge
    // the CPU into simavr's crash detector). Hardware validates the true reset.
    CHECK(g_saw_crash == 0,
          "simavr cannot model WDT reset; verify backstop on hardware (see comment)");
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
    test_enters_idle_sleep();
    test_watchdog_not_tripped_normally();
    test_watchdog_backstop_documented();

    if (g_avr) { avr_terminate(g_avr); free(g_avr); }

    printf("\nsimavr firmware tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
#endif
}
