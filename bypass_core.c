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

#include "bypass_core.h"
#include "bypass_config.h"
#include "bypass_output.h"
#include "bypass_output_common.h"
#include "bypass_types.h"

#include <assert.h>        // For static_assert()
#include <avr/io.h>        // Defines register and bit names
#include <avr/wdt.h>       // watchdog timer: wdt_enable(), wdt_reset(), WDTO_* timeouts
#include <avr/power.h>     // clock_prescale_set(), power_all_disable()
#include <avr/sleep.h>     // sleep states
#include <avr/interrupt.h> // ISR() interrupt service routine macro


//////////////////////////////////////////////////////////////////////////////
// PROGRAM GLOBALS
//////////////////////////////////////////////////////////////////////////////

// note: effect_state_ and program_state_ are not touched in the ISRs, so
// technically don't need to be volatile; however, without volatile, these
// variables might be local stack copies from memory depending on how the
// compiler generates code, which would make the the memory-corruption
// sanity check in the main loop worthless.  So we make them volatile for the
// purposes of preventing compiler output that would render the sanity check
// useless.
volatile effect_state_t effect_state_;
volatile program_state_t program_state_;
volatile timer_isr_called_t timer_isr_called_;
volatile uint8_t debounce_counter_;


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
__attribute__((noreturn)) static void force_wdt_reset(void) {
    cli();
    while (1) {}
}


// LED_PIN high = status LED lit
// LED_PIN low = status LED dark
void led_pin_set_high(void) { PORTB |=  (1 << LED_PIN); }
void led_pin_set_low(void)  { PORTB &= (uint8_t)~(1 << LED_PIN); }


// - set a GPIO pin high or low
// - assumes pin was previously configured as output
void pin_set_high(uint8_t const pin) { PORTB |=  (uint8_t)(1 << pin); }
void pin_set_low(uint8_t const pin)  { PORTB &= (uint8_t)~(1 << pin); }


// read FOOTSW_PIN to determine if it's high or low
// returns: 0 (low) or 1 (high)
// note: "!!" converts any non-zero value to 1
static uint8_t digital_read_footswitch_pin(void) { return !!(PINB & (1 << FOOTSW_PIN)); }


// Timer0 Compare-Match A interrupt; fires every 1ms (see init()).
// - read footswitch pin, increment/decrement saturating accordingly
// - use a saturating integrator to have some tolerance to noisy
//   switches/environments
ISR(TIM0_COMPA_vect) {

    timer_isr_called_ = TIMER_ISR_CALLED; // used by main() to reset WDT

    // saturating integrator update
    // footswitch pin zero (low) == switch closed
    // footswitch pin one (high) == switch open
    if (0 == digital_read_footswitch_pin()) {
        if (debounce_counter_ < RELEASE_THRESH) { ++debounce_counter_; }
    }
    else { // footswitch pin is high -> switch open
        if (debounce_counter_ > 0) { --debounce_counter_; }
    }
}


// high-level initialization
// called at power-on, and after RESET (e.g. due to watchdog timeout)
static void init(void) {

    // compile-time sanity checks
    static_assert(RELEASE_THRESH < UINT8_MAX,                   "RELEASE_THRESH >= UINT8_MAX");
    static_assert(RELEASE_THRESH > 0,                           "RELEASE_THRESH <= 0");
    static_assert(RELEASE_THRESH > PRESSED_THRESH,              "RELEASE_THRESH <= PRESSED_THRESH");
    static_assert(PRESSED_THRESH < UINT8_MAX,                   "PRESSED_THRESH >= UINT8_MAX");
    static_assert(PRESSED_THRESH > 0,                           "PRESSED_THRESH <= 0");
    static_assert(1 == sizeof(effect_state_t),                  "sizeof(effect_state_t) != 1, use -fshort-enums");
    static_assert(1 == sizeof(program_state_t),                 "sizeof(program_state_t) != 1, use -fshort-enums");
    static_assert(1 == sizeof(timer_isr_called_t),              "sizeof(timer_isr_called_t) != 1, use -fshort-enums");
    static_assert(1000 == (F_CPU / 8 / (TIMER0_OCR0A_1MS + 1)), "OCR0A/F_CPU mismatch, ISR won't be on 1ms timer");

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
    init_ddrb_setup();


    // enable the input pullup for FOOTSW_PIN
    // note additional external 10k pullup
    // FOOTSW_PIN high = switch open/released
    // FOOTSW_PIN low = switch closed/pressed
    // this also sets unused pins low
    PORTB = (1 << FOOTSW_PIN);

    GIMSK = 0; // pin change interrupts: not needed
    PCMSK = 0; // external interrupts: not used


    // always start in bypass
    set_bypass_state(); // note: sets effect_state_ = BYPASS

    // special case: footswitch pressed during power-on: keep in bypass state,
    // but use timer + interrupt function to wait for release
    if (0 == digital_read_footswitch_pin()) {
        program_state_ = RELEASE_DEBOUNCE_WAIT;
        debounce_counter_ = RELEASE_THRESH;
    }
    // typical startup case: assume switch is not pressed
    else {
        program_state_ = PRESS_DEBOUNCE_WAIT;
        debounce_counter_ = 0;
    }

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
 
     init();
 
     while (1) {
 
         // basic sanity checks against outlier events (cosmic rays, extreme
         // EMI)
        // always called, regardless of state
        // force WDT timeout if fail
        if ( (program_state_ > RELEASE_DEBOUNCE_WAIT) ||
                (effect_state_ > ENGAGED) ||
                (timer_isr_called_ > TIMER_ISR_NOT_CALLED) ||
                // assert footswitch pullup still enabled
                ((PORTB & (1 << FOOTSW_PIN)) == 0) ||
                // config-specific runtime sanity checks
                is_sanity_check_failed()
           ) {
            force_wdt_reset();
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
        }

        switch (program_state_) {

            // NOTE: reading the volatile globals here (e.g.
            // debounce_counter_) is a potential race condition,
            // since they are subject to writing in the ISR.
            // uint8_t operations in AVR are atomic, so we won't
            // read garbage, but we might read an "old" value in the
            // precise instant before it is updated.  We could
            // consider pausing interrupts to read the values into
            // local copy variables first - but that adds complexity
            // when the worst-case issue is an extra 1ms delay -
            // imperceptible and therefore acceptable for this
            // design

            // waiting for the footswitch to be press-debounced
            case PRESS_DEBOUNCE_WAIT:
                {
                    // check for press-debounced condition
                    if (debounce_counter_ >= PRESSED_THRESH) {
                        // note: logical race here, ISR could increment
                        // debounce_counter_ between above read and below
                        // write: ok, does not violate design intent
                        debounce_counter_ = RELEASE_THRESH;
                        program_state_ = RELEASE_DEBOUNCE_WAIT;
                        if (BYPASS == effect_state_) { set_engaged_state(); }
                        else { set_bypass_state(); }
                    }

                    // Pause until the next 1ms Timer0 compare-match ISR wakes
                    // the core. Lost-wakeup is impossible on AVR IDLE sleep:
                    // if the ISR fires in the window between the counter check
                    // above and the SLEEP instruction, the hardware aborts SLEEP
                    // immediately and services the interrupt before the next
                    // instruction (ATtiny13A datasheet §7.3, Sleep Modes).
                    // No tick is ever missed even without disabling interrupts
                    // around the check-then-sleep sequence.
                    else {
                        sleep_mode();
                    }
                }
                break;

            // waiting for the footswitch to be release-debounced
            // note: holding the switch closed, or mechanical
            //       failure (e.g. switch welded shut) causes this
            //       state to exist indefinitely: this is the design
            //       intent (software is "helpless", need physical
            //       human resolution)
            case RELEASE_DEBOUNCE_WAIT:
                {
                    if (0 == debounce_counter_) {
                        program_state_ = PRESS_DEBOUNCE_WAIT;
                    }
                    // Same AVR lost-wakeup guarantee: pending ISR aborts SLEEP
                    // immediately, no tick missed (ATtiny13A datasheet §7.3).
                    else {
                        sleep_mode();
                    }
                }
                break;

            // invalid state, should be impossible (cosmic rays, massive EMI pulse, etc)
            default:
                force_wdt_reset();
                break;

        }
    }

}

