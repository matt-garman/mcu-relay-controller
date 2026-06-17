// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.


// designed for avrtools (standard avr-gcc, avr-libc, avrdude toolchain)
//
// compile with:
//     -fshort-enums // so that typedef'ed enums below are 8-bit ints
//     -funsigned-char
//     -ffunction-sections
//     -fdata-sections
//     -Wl,--gc-sections
//     -Werror
//     -Wall
//     -Wextra
//
// Fuse configuration:
// Fuse      | Value                                     | Rationale
// ----------+-------------------------------------------+----------------------------------------------------------------
// CKSEL     | 0b0010 (Internal 9.6MHz)                  | Required for 1.2MHz operation with CKDIV8
// SUT       | 0b00 (14 CK + 4ms) or 0b10 (14 CK + 64ms) | 64ms recommended for stable power-on with LDO regulator ramp-up
// CKDIV8    | 0 (enabled, i.e., divide by 8)            | Yields 1.2MHz system clock
// WDTON     | 0 (enabled, i.e., WDT always on)          | Silicon-level guarantee: WDT cannot be disabled by software;
//           |                                           | resilient against stray WDTCR writes (EMI, cosmic rays)
// BODEN     | 0 (enabled)                               | Required for brown-out protection
// BODLEVEL  | 0b10 (2.7V)                               | Matches your design spec
// RSTDISBL  | 1 (disabled, i.e., PB5 remains RESET)     | Critical: clearing this disables ISP programming
// SELFPRGEN | 1 (disabled)                              | No self-programming needed
// DWEN      | 1 (disabled)                              | debugWIRE not needed in production; consumes PB5
//
// avrdude fuse targets: -U lfuse:w:0x4a:m -U hfuse:w:0xfb:m
// 
// Note: useful fuse tool here: https://www.engbedded.com/fusecalc/
//

#include "bypass_config.h"
#include "bypass_output_common.h"
#include "bypass_types.h"
#include "bypass_pure.h"
#include "bypass_hw_iface.h"

#include <assert.h>        // For static_assert()
#include <avr/io.h>        // Defines register and bit names
#include <avr/wdt.h>       // watchdog timer: wdt_enable(), wdt_reset(), WDTO_* timeouts
#include <avr/power.h>     // clock_prescale_set(), power_all_disable()
#include <avr/sleep.h>     // sleep states
#include <avr/interrupt.h> // ISR() interrupt service routine macro


// Upper bound for values stored in the uint8_t debounce counter, as an
// UNSIGNED constant. We deliberately do NOT use <stdint.h>'s UINT8_MAX: by C
// integer-promotion rules a uint8_t promotes to (signed) int, so UINT8_MAX
// itself has type int. Comparing it to our unsigned thresholds is an
// essential-type-category mix (MISRA 10.4), and its expansion (0x7f*2+1) also
// trips MISRA 12.1. A plain unsigned literal means the same thing and avoids
// both -- see MISRA_COMPLIANCE.md.
//
// Loosely speaking: MISRA-C compliant UINT8_MAX
#define DEBOUNCE_COUNTER_MAX (255U)


//////////////////////////////////////////////////////////////////////////////
// FILE-SCOPED TYPES
//////////////////////////////////////////////////////////////////////////////

// a flag to "multiplex" the WDT across the timer ISR and main() loop
typedef enum {
    TIMER_ISR_CALLED = 0,
    TIMER_ISR_NOT_CALLED,
} timer_isr_called_t;



//////////////////////////////////////////////////////////////////////////////
// PROGRAM GLOBALS
//////////////////////////////////////////////////////////////////////////////

// a single volatile global variable, shared between main() and the timer ISR
static volatile timer_isr_called_t timer_isr_called_;

// overall debounce context
static volatile debounce_context_t ctx_;


//////////////////////////////////////////////////////////////////////////////
// FUNCTIONS
//////////////////////////////////////////////////////////////////////////////

// infinite-loop function to force watchdog reset
//
// this function is designed for critical, unrecoverable errors (presumably by
// ultra-rare events, e.g. cosmic rays, extreme EMI)
//
// IMPORTANT: this function relies on the watchdog being active; calling this
// without an active WDT will lock up the MCU
__attribute__((noreturn)) static void hw_force_wdt_reset(void) {
    cli();
    while (1) {}
}


// LED_PIN high = status LED lit
// LED_PIN low = status LED dark
void hw_led_pin_set_high(void) { PORTB |=  (1 << LED_PIN); }
void hw_led_pin_set_low(void)  { PORTB &= (uint8_t)~(1 << LED_PIN); }


// - set a GPIO pin high or low
// - assumes pin was previously configured as output
void hw_pin_set_high(uint8_t const pin) { PORTB |=  (uint8_t)(1 << pin); }
void hw_pin_set_low(uint8_t const pin)  { PORTB &= (uint8_t)~(1 << pin); }


// read FOOTSW_PIN to determine if it's high or low
// returns: PIN_STATE_HIGH or PIN_STATE_LOW
static pin_state_t hw_digital_read_footswitch_pin(void) { 
    return (0U == (PINB & (1 << FOOTSW_PIN))) ?
        PIN_STATE_LOW :
        PIN_STATE_HIGH;
}


// set AVR to IDLE SLEEP mode: halts main() loop, but ISRs continue to run
static void hw_set_idle_sleep_mode(void) { sleep_mode(); }


// Timer ISR
// Timer0 Compare-Match A interrupt; fires every 1ms (see init()).
ISR(TIM0_COMPA_vect) {
    timer_isr_called_ = TIMER_ISR_CALLED; // used by main() to reset WDT
    ctx_.debounce_counter = debounce_integrate(
            hw_digital_read_footswitch_pin(),
            ctx_.debounce_counter);
}


// high-level initialization
// called at power-on, and after RESET (e.g. due to watchdog timeout)
static void init(void) {


    // compile-time sanity checks
    static_assert(RELEASE_THRESH < DEBOUNCE_COUNTER_MAX,           "RELEASE_THRESH >= UINT8_MAX");
    static_assert(RELEASE_THRESH > 0U,                             "RELEASE_THRESH <= 0");
    static_assert(RELEASE_THRESH > PRESSED_THRESH,                 "RELEASE_THRESH <= PRESSED_THRESH");
    static_assert(PRESSED_THRESH < DEBOUNCE_COUNTER_MAX,           "PRESSED_THRESH >= UINT8_MAX");
    static_assert(PRESSED_THRESH > 0U,                             "PRESSED_THRESH <= 0");
    static_assert(1U == sizeof(effect_state_t),                    "sizeof(effect_state_t) != 1, use -fshort-enums");
    static_assert(1U == sizeof(program_state_t),                   "sizeof(program_state_t) != 1, use -fshort-enums");
    static_assert(1U == sizeof(timer_isr_called_t),                "sizeof(timer_isr_called_t) != 1, use -fshort-enums");
    static_assert(1000U == (F_CPU / 8U / (TIMER0_OCR0A_1MS + 1U)), "OCR0A/F_CPU mismatch, ISR won't be on 1ms timer");

    // disable interrupts (don't want init() to be interrupted); will
    // re-enable at end of function
    cli();

    // Watchdog: ~250ms timeout in system-reset mode. wdt_enable() sets WDE
    // (reset mode) for us. WDTO_250MS is the nearest standard step.
    //
    // NOTE: the AVR watchdog timer uses a separate oscillator that is
    // independent of the system clock; it has *very* loose tolerance.  We
    // should expect our 250ms watchdog timeout to be 100-350ms in practice.
    //
    // Also note: after a watchdog-triggered reset, WDTCR resets to 0 with WDE
    // forced on by WDRF, so the effective timeout is ~16ms until wdt_enable()
    // runs.  With a 50% margin, this could be as low as 7-8ms.
    //
    // We need to ensure that we don't create a WDT reset loop by making
    // init() so long that the WDT bites.  Hence, one of the first things we
    // do in init() is reset the WDT and then set the timeout to 250ms.  After
    // RESET, we expect a few dozen instructions, therefore a few dozen
    // microseconds until wdt_reset()/wdt_enable() is called.
    //
    // Therefore, there is a few milliseconds of margin against WDT reset
    // loop; but the WDT reset and re-arm should remain at the very start of
    // this function.
    wdt_reset(); // pet the dog (init() could be called from previous WDT timeout)
    MCUSR &= (uint8_t)~(1 << WDRF); // must clear WDRF before WDE can be cleared
    wdt_enable(WDTO_250MS); // (re-)arm the WDT

    // make the 1.2MHz system clock explicit at runtime
    // (9.6MHz internal RC / 8). The CKDIV8 fuse already does this at
    // power-on; setting it here is belt-and-suspenders and survives any prior
    // prescaler change.
    clock_prescale_set(clock_div_8);

    // disable unused components (save power, reduce chance of spurious
    // activity)
    ADCSRA = 0; // disable ADC (analog to digital converter)
    ACSR = (1 << ACD); // disable analog comparator

    // gate clocks to unused modules, explicitly re-enable Timer0, used for
    // 1ms footswitch pin polling
    power_all_disable();
    power_timer0_enable();


    // GPIO setup:
    //   PB5 = RESET, leave as input (do not touch)
    //   FOOTSW_PIN is always an input
    //   All other pins are configured as output, even if unused
    //   Unused pins are driven low
    hw_init_ddrb_setup();


    // enable the input pullup for FOOTSW_PIN
    // note additional external 10k pullup
    // FOOTSW_PIN high = switch open/released
    // FOOTSW_PIN low = switch closed/pressed
    // this also sets unused pins low
    PORTB = (1 << FOOTSW_PIN);

    GIMSK = 0; // pin change interrupts: not needed
    PCMSK = 0; // external interrupts: not used


    // always start in bypass
    hw_set_bypass_state();


    pin_state_t const pin_state = hw_digital_read_footswitch_pin();
    ctx_ = debounce_init_context(pin_state);


    // ISR-main() WDT handshake: let ISR set this to called when timer is
    // activated
    timer_isr_called_ = TIMER_ISR_NOT_CALLED;


    // Timer0: CTC mode (WGM01=1), prescaler /8, compare match every 1ms.
    TCCR0A = (1 << WGM01);     // CTC: clear timer on compare A
    TCCR0B = (1 << CS01);      // prescaler = clk/8
    OCR0A  = TIMER0_OCR0A_1MS; // 149 -> 1ms tick at 1.2MHz/8
    TCNT0  = 0;                // start count from 0
    TIMSK0 = (1 << OCIE0A);    // enable Compare Match A interrupt
    TIFR0  = (1 << OCF0A);     // explicitly clear TIFR0's OCF0A (prevent ISR firing immediately below after sei() from state compare-match flag from WDT reset)

    // CPU sleeps in IDLE between 1ms ticks: core halts, but Timer0 keeps
    // running so the tick ISR still wakes us. (Deeper modes would stop
    // Timer0)
    // NOTE: set_sleep_mode() is an avr-libc macro whose ~mask expansion trips
    // -Wconversion; suppress locally since we cannot cast inside the macro.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
    set_sleep_mode(SLEEP_MODE_IDLE);
#pragma GCC diagnostic pop


    // init done, now re-enable interrupts
    sei();
    __asm__ __volatile__("" ::: "memory"); // belt-and-suspenders to prevent compiler reordering across sei()
}


// program entry point
__attribute__((OS_main)) int main(void) {

    init(); // note: initializes ctx_ via debounce_init_context()

    while (1) {

        // basic sanity checks against outlier events (cosmic rays, extreme
        // EMI)
        // always called, regardless of state
        // force WDT timeout if fail
        if ( (ctx_.program_state > RELEASE_DEBOUNCE_WAIT) ||
                (ctx_.effect_state > ENGAGED) ||
                (timer_isr_called_ > TIMER_ISR_NOT_CALLED) ||
                // assert footswitch pullup still enabled
                ((PORTB & (1 << FOOTSW_PIN)) == 0) ||
                // config-specific runtime sanity checks
                hw_is_sanity_check_failed()
           ) {
            hw_force_wdt_reset();
        }

        // - the intent is to make sure both main() is running AND
        //   the timer ISR is being invoked
        // - if main() loop fails or timer ISR stops running,
        //   watchdog timeout will expire
        // - potential logical race here with timer ISR - could possibly miss
        //   one timer ISR update, but will be correct on next loop iteration,
        //   so will not trigger WDT timeout
        if (TIMER_ISR_CALLED == timer_isr_called_) {
            timer_isr_called_ = TIMER_ISR_NOT_CALLED;
            wdt_reset(); // "pet the dog"

            debounce_step_result_t const res = debounce_step(ctx_);

            ctx_.program_state = res.program_state;
            ctx_.effect_state = res.effect_state;
            if (res.reload_lockout)
            {
                ctx_.debounce_counter = res.lockout_value;
            }

            // note: the fault condition is
            // defense-in-depth/belt-and-suspenders with the sanity checks
            // above
            if (res.fault) {
                hw_force_wdt_reset();
            }
            else if (res.toggled) {
                if (BYPASS == res.effect_state) { hw_set_bypass_state(); }
                else /*ENGAGED == res.effect_state*/ { hw_set_engaged_state(); }
            }
            else {
                // state advanced this tick with no toggle and no fault: nothing to do
            }
        }

        // Pause until the next 1ms Timer0 compare-match ISR wakes the core.
        // Lost-wakeup is impossible on AVR IDLE sleep: if the ISR fires in
        // the window between clearing timer_isr_called_ and the SLEEP
        // instruction, the hardware aborts SLEEP immediately and services the
        // interrupt before the next instruction (ATtiny13A datasheet §7.3,
        // Sleep Modes). No tick is ever missed even without disabling
        // interrupts around the check-then-sleep sequence.
        hw_set_idle_sleep_mode();
    }

}

