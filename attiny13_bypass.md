
= Electric Guitar Effect Engage/Bypass Scheme using ATtiny13a


== Why ATTiny13a

    - Readily available in SMD and through-hole
    - Low cost
    - avrtools - mature, fully-featured free/open-source tooling
    - Low power in sleep mode for battery-friendliness (note that
      the brownout detector (BOD) is active during sleep, which uses
      more power than a "pure" deep sleep: this is an accepted
      design compromise between resiliancy and power-savings)
    - Trivial hardware implementation
    - Schmitt trigger GPIO pins


== High-Level Functional Description

This ATTiny13a will be used with a physical human-operated
footswitch, and an electric signal switching IC (CD4053 or
TMUX4053). These components comprise a sub-circuit of a larger
effect circuit, typically for use in a pedal.

The user uses the footswitch to toggle between two states, "engaged"
and "bypass".  In the "engaged" state, the audio signal is routed
through some kind of effect; in "bypass", the effect is routed
straight through (as though the user removed the device entirely
from the signal chain).

This switching sub-circuit also has a status LED; when the effect is
"engaged", the status LED should be lit, and when the effect is in
"bypass", the status LED should be dark.

Therefore, the ATTiny13a needs to:
    - maintain state (engage/bypass)
    - light or dark the status LED
    - respond to footswitch presses (including debounce)
    - control the actual signal switching mechanism

At power-on, the circuit shoul default to the bypass state with the
LED dark (no state persistance between power cycles).

The state change should feel immediate to the user, and be triggered on press
(not on release).

This sub-circuit should be as robust as possible and assume likely adverse
conditions:
    - high temperature (e.g. noon-time Death Valley performance)
    - EMI/RFI (e.g. cell phones, wifi, flourescent lighting, AC motors,
      proximity to radio station, etc)

== Reliability Goals
    - Reference-quality
    - World-class touring musician grade
    - One physical press must generate exactly one state change
    - One physical release must generate zero state changes
    - Resiliancy against EMI/RFI-induced false state changes in
      expected operating environments
    - Switch bounce must never generate multiple state changes
    - Holding the switch down for several seconds must not generate additional
      state changes
    - Fast repeated taps should always be recognized (see Caveats
      and Limitations)
    - Latency should feel instantaneous as possible given switch
      age/quality and environmental noise: <10ms under ideal
      circumstances
    - Behavior must be deterministic and analyzable


== Caveats and Limitations
    - Pop/click suppression is out-of-scope for this design; a
      future revision will incorporate a temporary muting scheme
      during engaged-bypass transitions
    - A mechanically stuck switch results in permanent active-mode
      power draw and no recovery; this is by design
    - "Fast" repeated taps: "fast" generally means at least
      approximately 30 milliseconds between presses under ideal
      conditions; time between recognized repeated taps will longer
      in noisy environments, or when using old switches, etc


== Deep-sleep discussion
    - To save power, this design puts the MCU into DEEP_SLEEP mode
      between footswitch press events
    - Pin-change interrupt is used to wake the MCU from sleep and
      run the debounce routine
    - With the brownout detector (BOD) active, the MCU will draw
      roughly 20-25 micro-amps in deep sleep
    - Alternative, simpler designs could omit the sleep complexities
      at a significantly higher power cost:
        - naive always-active mode: likely 0.5 to 1.0 mA in idle
        - middle-ground CPU IDLE mode with 1ms timer active: roughly
          200-300 micro-amps while waiting for press
    - The added firmware complexity is an acceptable added cost for
      dramatically increased power savings


== GPIO pin assignment

    - NOTE: PB5 serves multiple roles (e.g. physical RESET pin) in
      ATTiny13a; deliberately not used in this design
    - PB0: input; footswitch pin, normally held high; normally-open momentary
      footswitch pulls low when pressed
    - PB1: output; to status LED cathode mosfet: high when state is "engaged"
      (LED is lit), low when state is "bypass" (LED is dark)
    - PB2: output, used to control 4053 electrical switch
    - PB1, PB2 will have 100k pulldown resistors


Note: the GPIO pin tied to the footswitch (normally high) will have some
hardware-level EMI/RFI protections and also aid with debounce:
    - 10k pullup resistor to ATTiny13a voltage supply (note: will be
      combined with internal AVR pullup resistor, so net is roughly
      7-8k)
    - 1k series resistor
    - 10nF MLCC capacitor to ground for EMI/RFI supression


CD4053 Note:
    - the newer TMUX4053 switches can be controlled with logic
      levels lower than the voltage supply
    - the older CD4053 needs a logic "true" to be the same level as
      the voltage supply
    - the ATTiny13a will run at 5v, but the 4053 will run at the
      effect voltage: 9 to 18 volts
    - 5v rail derived via LP2950 for through-hole implementation
      or AP7375 for SMD implementation
    - 5v rail only for MCU and footswitch - everything else
      (including status LED) is powered from the parent 9-18v rail
    - therefore, the 4053 control pins will be held to effect
      voltage via pullup resistor; the control pins will be pulled
      low (to ground) via a mosfet; that mosfet's gate will be
      controlled by ATTiny13a PB2
    - therefore, the PB2 pin value is actually inverted at the
      CD4053:
        - PB2 high -> mosfet on -> 4053 control pins low
        - PB2 low -> mosfet off -> 4053 control pins high
    - the 4053 is wired such that:
        - control pins high = bypass
        - control pins low = engaged


== Toolchain
    - avrtools (avr-gcc, avr-libc)
    - no Arduino

== Compilation Flags
    -fshort-enums // so that typedef'ed enums below are 8-bit ints
    -funsigned-char
    -ffunction-sections
    -fdata-sections
    -Wl,--gc-sections
    -Werror
    -Wall
    -Wextra


== ATTiny13a Program Flow/Psuedocode



////////
// types
////////


// the following enum describes the possible high-level states of
// the debounce/bypass scheme:
typedef enum {
    // the MCU is in it's lowest power state, essentially inactive
    // except for WDT, BOD, PBO/footswitch pin change detection
    DEEP_SLEEP,

    // 1ms PB0/footswitch pin sampling, waiting for footswitch to be
    // press-debounced (i.e. footswitch considered open/released in
    // this state)
    PRESS_DEBOUNCE_WAIT,

    // 1ms PB0/footswitch pin sampling, footswitch was previously
    // confirmed debounce-pressed, now waiting for footswitch to be
    // release-debounced (i.e. footswitch considered closed/pressed
    // in this state)
    RELEASE_DEBOUNCE_WAIT,

    // footswitch has been through a press-debounced and
    // release-debounced cycle, and the status LED plus actual
    // effect engage/bypass state has been updated
    PREPARE_SLEEP,
} program_state_t;

// a flag to keep track of the effect/bypass state
typedef enum {
    BYPASS,
    ENGAGED,
} effect_state_t;

// a flag to "multiplex" the WDT across the timer ISR and main()
// loop
typedef enum {
    TIMER_ISR_CALLED,
    TIMER_ISR_NOT_CALLED,
} timer_isr_called_t;


////////////
// constants
////////////

// see digital_read_majority_vote_pb0()
#define DIGITAL_READ_VOTERS (7)

// number of HIGH PB0/footswitch pin reads to be considered 
// release-debounced, i.e. the "lock-out" period
#define RELEASE_THRESH (25)

// number of LOW PB0/footswitch pin reads to be considered
// press-debounced
//
// trying to balance between "responds immediately" and "immune to
// spurious interrupts"
//
// the asymmetry between PRESSED_THRESH and RELEASE_THRESH is
// to bias the debounce time after the actual action (effect
// engage/bypass) to balance responsiveness with robust switch
// de-bouncing
#define PRESSED_THRESH (5)

// - maximum milliseconds we'll wait for the switch to be considered
//   press-debounced
// - this also acts as the back-to-sleep timeout on a spurious
//   wakeup due to e.g. EMI/RFI
#define MAX_PRESS_WAIT_MS (125)

// max milliseconds before WDT considers device hung/in bad state
#define WATCHDOG_TIMEOUT_MS (250)


///////////////////
// global variables
///////////////////

// uint8_t reads/writes are atomic on ATTiny13a

effect_state_t effect_state_; // not touched in ISR, don't need volatile
program_state_t program_state_; // not touched in ISR, don't need volatile
volatile timer_isr_called_t timer_isr_called_;
volatile uint8_t debounce_counter_;
volatile uint8_t press_debounce_timeout_;


////////////
// functions
////////////


uint8_t digital_read_majority_vote_pb0() function:
    // - "belt-and-suspenders" digital_read() wrapper (technically
    //   not necessary due to hardware RC filter and AVR Schmitt
    //   inputs)
    // - read PB0/footswitch pin DIGITAL_READ_VOTERS times rapidly:
    //   pin state is based on "majority vote"
    int8_t val = 0;
    for (uint8_t i=0; i<DIGITAL_READ_VOTERS; ++i) {
        val += digital_read(PB0) ? 1 : -1;
    }
    return val < 0 ? 0 : 1;


// initialization function: called when MCU is powered-on and out of
// RESET state
function init():

    // basic compile-time sanity checks on the #define constants
    - static_assert(1 == (DIGITAL_READ_VOTERS % 2)); // must be odd
    - static_assert(DIGITAL_READ_VOTERS >= 3);
    - static_assert(DIGITAL_READ_VOTERS < UINT8_MAX);
    - static_assert(RELEASE_THRESH < UINT8_MAX);
    - static_assert(RELEASE_THRESH > 0);
    - static_assert(RELEASE_THRESH > PRESSED_THRESH)
    - static_assert(PRESSED_THRESH < UINT8_MAX);
    - static_assert(PRESSED_THRESH > 0);
    - static_assert(MAX_PRESS_WAIT_MS < UINT8_MAX);
    - static_assert(MAX_PRESS_WAIT_MS > 0);
    - static_assert(MAX_PRESS_WAIT_MS > (RELEASE_THRESH+PRESSED_THRESH))
    - static_assert(WATCHDOG_TIMEOUT_MS > MAX_PRESS_WAIT_MS)

    - disable interrupts
    - clear WDT flag, reset WDT (init() may be called due to watchdog event)
    - enable WDT with WATCHDOG_TIMEOUT_MS timeout
    - disable unused features: ADC, analog comparator, etc
    - set MCU to 1.2MHz clock (lowest practical speed)
    - set PB0 as input (footswich), enable internal pullup (note:
      this will be in parallel with external 10k pullup resistor,
      lowering effective pullup value to approximately 7-8k)
    - set PB1, PB2 as output (LED, 4053, respectively)
    - set unused GPIO pins (excluding PB5) to output low (will be not-connected at hardware level)
    - enable brownout detection (BOD) at 2.7v

    - call set_bypass_state() function // note: sets effect_state_ = BYPASS

    // special case: footswitch pressed during power-on: keep in
    // bypass state, but use timer + interrupt function to wait
    // for release
    if PB0 is low { // footswitch is pressed
        - timer_isr_called_ = TIMER_ISR_CALLED;
        - program_state_ = RELEASE_DEBOUNCE_WAIT;
        - debounce_counter_ = RELEASE_THRESH;
        - press_debounce_timeout_ = 0; // this isn't necessary, since press_debounce_timeout_ only matters in PRESS_DEBOUNCE_WAIT
        - enable WDT with WATCHDOG_TIMEOUT_MS timeout
        - set 1ms timer interrupt to call timer_interrupt() periodically (wait for swtich to be released)
    }
    else { // standard case
        program_state_ = PREPARE_SLEEP;
    }


on_pin_change() function:
    // - this should only be called when the MCU is in DEEP_SLEEP 
    //   state
    // - DEEP_SLEEP state should only be possible when the
    //   footswitch has been release-debounced
    // - use PCINT0 on PB0 (wakes from power-down, edge-agnostic,
    //   but logic structure means it will wake on falling-edge)
    - wakeup MCU

set_bypass_state() function:
    - effect_state_ = BYPASS;
    - dark status LED
    - PB2 low (4053 control pins high)

set_engaged_state() function:
    - effect_state_ = ENGAGED;
    - light status LED
    - PB2 high (4053 control pins low)

timer_interrupt() function:
    // - called from timer interrupt every 1ms
    // - read PB0/footswitch pin, increment/decrement saturating
    //   accordingly
    // - use a saturating integrator to have some tolerance to noisy
    //   switches/environments

    timer_isr_called_ = TIMER_ISR_CALLED; // used by main() to reset WDC

    if (press_debounce_timeout_ < UINT8_MAX) { ++press_debounce_timeout_; }

    // saturating integrator update
    // PBO0 zero (low) == switch closed
    // PBO0 one (high) == switch open
    if (0 == digital_read(PB0)) {
        if (debounce_counter_ < RELEASE_THRESH) { ++debounce_counter_: }
    } else { // PB0 is high -> switch open
        if (debounce_counter_ > 0) { --debounce_counter_; }
    }



// program entry point/main loop
main() function:

    init();

    while (1) {

        // - the intent is to make sure both main() is running AND
        //   the timer ISR is being invoked
        // - if main() loop fails or timer ISR stops running,
        //   watchdog timeout will expire
        if (TIMER_ISR_CALLED == timer_isr_called_) {
            timer_isr_called_ = TIMER_ISR_NOT_CALLED;
            wdt_reset(); // reset WDT, aka "pet the dog"
        }

        switch (program_state_) {

            // we were woken from sleep via on_pin_change()
            case DEEP_SLEEP: {
                disable pin change interrupt
                clear/reset WDT counters/timer
                enable WDT with WATCHDOG_TIMEOUT_MS timeout
                timer_isr_called_ = TIMER_ISR_NOT_CALLED;
                debounce_counter_ = digital_read_majority_vote_pb0();
                program_state_ = PRESS_DEBOUNCE_WAIT;
                press_debounce_timeout_ = 0;
                set 1ms timer interrupt to call timer_interrupt() periodically
                break;
            }

            // NOTE: reading the volatile globals here (e.g.
            // debounce_counter_) is a potential race condition,
            // since they are subject to writing in the ISR.
            // uint8_t operations in AVR are atomic, so we won't
            // read garbage, but we might read an "old" value in the
            // precise instant before it is updated.  We could
            // consider pausing interrupts to read the values into
            // local copy variables first
            
            // waiting for the footswitch to be press-debounced
            case PRESS_DEBOUNCE_WAIT: {
                // check for press-debounced condition
                if (debounce_counter_ >= PRESSED_THRESH) {
                    debounce_counter_ = RELEASE_THRESH;
                    program_state_ = RELEASE_DEBOUNCE_WAIT;
                    if (BYPASS == effect_state_) { set_engaged_state(); }
                    else { set_bypass_state(); }
                }
            
                // timeout spurious interrupt/false trigger
                else if (press_debounce_timeout_ >= MAX_PRESS_WAIT_MS) {
                    program_state_ = PREPARE_SLEEP;
                }

                // pause this loop until the 1ms switch poll timer wakes it
                else {
                    set cpu to SLEEP_MODE_IDLE 
                }
                break;
            }
            
            // waiting for the footswitch to be release-debounced
            // note: holding the switch closed, or mechanical
            //       failure (e.g. switch welded shut) causes this
            //       state to exist indefinitely: this is the design
            //       intent (software is "helpless", need physical
            //       human resolution)
            case RELEASE_DEBOUNCE_WAIT: {
                if (0 == debounce_counter_) {
                    program_state_ = PREPARE_SLEEP;
                }

                // pause this loop until the 1ms switch poll timer wakes it
                else {
                    set cpu to SLEEP_MODE_IDLE 
                }
                break;
            }
            
            case PREPARE_SLEEP: {

                // PCINT flag latches edges even when interrupt
                // disabled; we deliberately do not clear it before
                // sleep so a press during the sleep-transition
                // window is serviced immediately on wake.

                disable 1ms timer
                disable watchdog timer // must disable watchdog timer in deep sleep mode, otherwise WDT will trigger and reset the device
                program_state_ = DEEP_SLEEP;
                enable pin change interrupt // step x
                put MCU to deep sleep state // step y
                break;
            }
            
            default: {
                // invalid state, should be impossible (cosmic rays, massive EMI pulse, etc)
                disable all interrupts
                set watchdog to shortest timeout (16ms)
                while (1) { } // infinite loop to force WDT-reset
                break;
            }

    }

