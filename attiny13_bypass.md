
= Electric Guitar Effect Engage/Bypass Scheme using ATtiny13a


== Why ATTiny13a

    - Readily available in SMD and through-hole
    - Low cost
    - avrtools - mature, fully-featured free/open-source tooling
    - Low power in sleep mode for battery-friendliness
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
    - light or dim the status LED
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
    - EMI/RFI must never generate a false state change
    - Switch bounce must never generate multiple state changes
    - Holding the switch down for several seconds must not generate additional
      state changes
    - Fast repeated taps should always be recognized
    - Latency should feel instantaneous (<10 ms ideally)
    - Behavior must be deterministic and analyzable


== GPIO pin assignment

    - NOTE: PB5 serves multiple roles (e.g. physical RESET pin) in
      ATTiny13a; deliberately not used in this design
    - PB0: input; footswitch pin, normally held high; normally-open momentary
      footswitch pulls low when pressed
    - PB1: output; to status LED cathode mosfet: high when state is "engaged"
      (LED is lit), low when state is "bypass" (LED is dark)
    - PB2: output, used to control 4053 electrical switch


Note: the GPIO pin tied to the footswitch (normally high) will have some
hardware-level EMI/RFI protections and also aid with debounce:
    - 10k pullup resistor to ATTiny13a voltage supply
    - 470-ohm series resistor
    - 10nF MLCC capacitor to ground for debounce

CD4053 Note:
    - the newer TMUX4053 switches can be controlled with logic
      levels lower than the voltage supply
    - the older CD4053 needs a logic "true" to be the same level as
      the voltage supply
    - the ATTiny13a will run at 5v, but the 4053 will run at the
      effect voltage: 9 to 18 volts
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


== ATTiny13a Program Flow/Psuedocode

Initialization:
    - boilerplate ATTiny13a init
        - disable unused features: ADC, analog comparator, etc
        - set MCU to 1.2MHz clock (lowest practical speed)
        - set PB0 as input (footswich)
        - set PB1, PB2 as output (LED, 4053, respectively)
        - set unused GPIO pins to output low (will be not-connected
          at hardware level)
    - enable brownout detection (BOD) - what does this mean/what does this do???
    - global typedefs:
        - uint8_t = boolean
    - global constants:
        - boolean FALSE = 0
        - boolean TRUE = (!FALSE)
        - boolean BYPASS = FALSE // global effect_state
        - boolean ENGAGED = TRUE // global effect_state
        - boolean PRESSED = TRUE // global switch_state, set when switch considered press-debounced
        - boolean RELEASED = FALSE // global switch_state, set when switch considered release-debounced
        - boolean LOW = FALSE // GPIO read value
        - boolean HIGH = TRUE // GPIO read value
        - uint8_t RELEASE_THRESH = 25 // number of consecutive HIGH switch pin reads to be considered release-debounced
        - uint8_t PRESSED_THRESH = 5 // number of consecutive LOW switch pin reads to be considered press-debounced
        - uint8_t MAX_PRESS_WAIT_MS = 80 // maximum milliseconds we'll wait for the switch to be considered press-debounced
        - uint8_t WATCHDOG_TIMEOUT_MS = 128 // max milliseconds before WDT considers device hung/in bad state
    - global variables init (shared between main() and ISRs - must be "volatile")
        - boolean "effect_state" = BYPASS
        - boolean "switch_state" = RELEASED
        - uint8_t "debounce_counter" = 0
        - uint8_t "press_debounce_timeout" = 0
    - static_assert(RELEASE_THRESH > PRESSED_THRESH)
    - static_assert(MAX_PRESS_WAIT_MS > (RELEASE_THRESH+PRESSED_THRESH))
    - static_assert(WATCHDOG_TIMEOUT_MS > MAX_PRESS_WAIT_MS)
    - call set_bypass_state() function
    - if PB0 is low (i.e. footswitch is pressed):
        // special case: footswitch pressed during power-on: keep in
        // bypass state, but use timer + interrupt function to wait
        // for release
        - switch_state = PRESSED
        - debounce_counter = RELEASE_THRESH
        - set 1ms timer interrupt to call timer_interrupt() periodically (wait for swtich to be released)
      else call go_to_sleep() function


on_pin_change() function:
    - disable pin change interrupt
    - wakeup MCU // run main() block

go_to_sleep() function:
    - disable watchdog timer
    - disable 1ms timer
    - enable pin change interrupt
    - put MCU to deep sleep state

set_bypass_state() function:
    - state set to BYPASS
    - set status LED dark
    - PB2 low -> 4053 control pins high

set_engaged_state() function:
    - state set to engaged
    - light status LED
    - PB2 high -> 4053 control pins low

timer_interrupt() function:
    // - called from timer interrupt every 1ms
    // - read footswitch pin, looking for consecutive reads of the same value
    // - use a saturating integrator to have some tolerance to noisy
    //   switches/environments
    // - after PRESSED_THRESH sampled LOW reads, we consider the
    //   switch press-debounced
    // - after RELEASE_THRESH sampled HIGH reads, we consider
    //   the switch release-debounced
    // - the asymmetry between PRESSED_THRESH and RELEASE_THRESH is
    //   to bias the debounce time after the actual action (effect
    //   engage/bypass) to balance responsiveness with robust switch
    //   de-bouncing

    - reset WDT // watchdog timer ("pet the dog")

    // timeout on noise/spurious interrupt
    - if (press_debounce_timeout < UINT8_MAX) { ++press_debounce_timeout; }
    - if ((RELEASED == switch_state) && (press_debounce_timeout > MAX_PRESS_WAIT_MS))
        - call go_to_sleep() function

    // saturating integrator update
    - boolean raw_press = read(PB0)
    - if (LOW == raw_press)
        - if (debounce_counter < RELEASE_THRESH) { ++debounce_counter }
      else // HIGH == raw_press
        - if (debounce_counter > 0) { --debounce_counter }

    // switch state update
    - if ((PRESSED == switch_state) && (0 == debounce_counter))
        - switch_state = RELEASED
    - if ((RELEASED == switch_state) && (debounce_counter >= PRESSED_THRESH))
        - switch_state = PRESSED
        - debounce_counter = RELEASE_THRESH

    // change effect state only once per wakeup (i.e. once per
    // switch press)
    - if ((PRESSED == switch_state) && (!did_effect_state_change)
        - if (ENGAGED == effect_state)
            - call set_bypass_state() function
          else // BYPASS == effect_state
            - call set_engaged_state() function
        - did_effect_state_change = TRUE

    // after we've changed state due to a debounced footswitch
    // press, now we wait for a debounced footswitch release
    - if ((RELEASED == switch_state) && (did_effect_state_change))
        - call go_to_sleep() function


main():
    // inifinite loop
    // due to use of IDLE_MODE, this block should fire exactly once
    // on every wakeup (due to pin change interrupt/on_pin_change())
    - clear/reset WDT counters/timer
    - enable WDT with WATCHDOG_TIMEOUT_MS timeout
    - press_debounce_timeout = 0
    - boolean did_effect_state_change = FALSE
    - debounce_counter = (PB0 == LOW)
    - set 1ms timer interrupt to call timer_interrupt() periodically
    - set IDLE_MODE sleep state // timers active but CPU otherwise halted

