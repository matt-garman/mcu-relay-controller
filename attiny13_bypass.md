
= Electric Guitar Effect Engage/Bypass Scheme using ATtiny13a


== Why ATTiny13a

    - Readily available in SMD and through-hole
    - Low cost
    - avr tools - mature, cross-platform, fully-featured
      free/open-source tooling
    - Low-ish power by use of CPU IDLE mode for typically <1mA
      current draw
    - Trivial hardware implementation
    - Schmitt trigger GPIO pins


== High-Level Functional Description

This ATTiny13a will be used with a physical human-operated
footswitch (momentary, normally open), and an electric signal
switching IC (CD4053 or TMUX4053). These components comprise a
sub-circuit of a larger effect circuit, typically for use in a
pedal.

The user uses the footswitch to toggle between two states, "engaged"
and "bypass".  In the "engaged" state, the audio signal is routed
through some kind of effect; in "bypass", the effect is routed
straight through (as though the user removed the device entirely
from the signal chain).

This switching sub-circuit also has a status indicator LED; when the
effect is "engaged", the status LED should be lit, and when the
effect is in "bypass", the status LED should be dark.

Therefore, the ATTiny13a needs to:
    - maintain state (engage/bypass)
    - light or dark the status LED
    - respond to footswitch presses (including debounce)
    - control the actual signal switching mechanism (4053)

At power-on, the circuit should default to the bypass state with the
LED dark (no state persistence between power cycles).

The state change should feel immediate to the user, and be triggered
on press (not on release).

This sub-circuit should be as robust as possible and assume likely
adverse conditions:
    - high temperature (e.g. noon-time Death Valley performance)
    - EMI/RFI (e.g. cell phones, wifi, fluorescent lighting, AC motors,
      proximity to radio station, etc)

== Reliability Goals
    - Reference-quality
    - World-class touring musician grade
    - Suitable for use by established market-leading manufacturers
      (e.g. Boss, Ibanez)
    - Under assumed operating conditions, one physical press must
      generate exactly one state change
    - One physical release must generate zero state changes
    - Resiliency against EMI/RFI-induced false state changes in
      expected operating environments
    - Switch bounce must never generate multiple state changes
    - Holding the switch down for several seconds must not generate additional
      state changes
    - Fast, repeated taps should always be recognized (see Caveats
      and Limitations)
    - Latency should feel instantaneous as possible given switch
      age/quality and environmental noise: <10ms under ideal
      circumstances
    - Behavior must be deterministic and analyzable


== Caveats and Limitations
    - The footswitch polling loop is based on a 1ms timer, driven by
      the ATTiny13a's builtin oscillator; this oscillator can drift
      +/- 10% (e.g. voltage fluctuations, temperature), which means
      actual sampling is roughly 0.9-1.1ms.  The algorithm still
      holds, and timing variations of this magnitude will not be
      perceptible to the user and are therefore acceptable.
    - Pop/click suppression is out-of-scope for this design; a
      future revision will incorporate a temporary muting scheme
      during engaged-bypass transitions
    - The design could be made more battery-friendly by use of a
      DEEP_SLEEP state when waiting for a footswitch press; that may
      be considered for a future revision - but the current design
      bias is towards simplicity and verifiability
    - A mechanically stuck switch results in permanent active-mode
      power draw and no recovery; this is by design
    - "Fast" repeated taps: "fast" generally means at least
      33 milliseconds between presses under ideal conditions; time
      between recognized repeated taps will be longer in noisy
      environments, or when using old switches, etc
    - The design attempts to be EMI/RFI resilient; however, an
      old/low-quality/fouled-contact footswitch might be very
      "bouncy"; the design can't distinguish EMI/RFI noise from
      switch bounce.  The tradeoff is that increased noise/bounce
      immunity reduces responsiveness.  Therefore, the firmware
      design assumes the following:
        - High quality, modern footswitch
        - Short (<10cm) leads between footswitch and PCB
        - Footswitch leads are tightly twisted
        - PCB and footswitch wiring housed in a grounded metal
          enclosure


== Asymmetric Debounce Timing

The design deliberately uses asymmetric timing for debouncing the
switch press and release.  The goal is to use a shorter time window
for press-debounce, so the state change feels instantaneous to the
user.  The release-debounce, or lock-out period, is longer so as to
"hide" additional debounce latency from the user.

    - PRESSED_THRESH upper bound (latency constraint):
      At worst-case clock (+10% RC drift), one tick = 1.111ms. For
      <10ms press latency: PRESSED_THRESH = floor(10 / 1.111) = 9.
      Current value 8 has a 1-tick margin.  Note that changing to 10
      10 would violate the "<10ms press latency" goal.  Also note
      that a highly bouncy switch, or significant EMI/RFI *will*
      cause the press latency to exceed 10ms.

    - PRESSED_THRESH lower bound (EMI rejection):
      The RC filter (1k series + 22nF to ground + ~7.5k pullup
      effective) has a time constant 22nF × (1k || 7.5k) =
      ~18µs. EMI spikes shorter than a few RC time constants are
      attenuated below the Schmitt threshold. The filter provides
      hardware-level rejection of spikes shorter than ~100µs. The
      1ms timer tick plus PRESSED_THRESH=8 means a spurious low must
      persist for 8ms (contiguous or accumulating) to register
      roughly 80× the hardware filter's corner.

    - RELEASE_THRESH (minimum lockout before next press):
      The minimum tap interval is PRESSED_THRESH + RELEASE_THRESH =
      33ms at nominal clock.


== GPIO pin assignment

    - NOTE: PB5 serves multiple roles (e.g. physical RESET pin) in
      ATTiny13a; deliberately not used in this design
    - PB0: input; footswitch pin, normally held high; normally-open momentary
      footswitch pulls low when pressed
    - PB1: output; to status LED cathode mosfet: high when state is "engaged"
      (LED is lit), low when state is "bypass" (LED is dark)
    - PB2: output, used to control 4053 electrical switch
    - PB1, PB2 will have 100k pulldown resistors


== Footswitch-GPIO wiring

The GPIO pin tied to the footswitch (normally high) will have some
hardware-level EMI/RFI protections and also aid with debounce.
Wiring is as follows:

Footswitch lead 1: GND
Footswitch lead 2 ("FOOTSW_PIN"):
    - TVS diode to ground (cathode=signal, anode=GND)
    - series ferrite bead
    - series 1k resistor ("a" side to FB, "b" side to "node b")
    - 22nF capacitor "node b" to ground (close to MCU GPIO pin)
    - 10k pullup resistor "node b" to VCC

Wiring diagram:
```
                                     VCC(5v)
                                        |
                                      [10k]
                                        |
FOOTSW_PIN -----+-----[FB]-----[1k]-----+-----GPIO_PIN
                |                       |
             [TVS-K]                 [22nF]
             [TVS-A]                    |
                |                       |
               GND                     GND
```

Note this wiring is overbuilt for the intended purpose.  All parts
are recommended for best EMI/RFI resiliency, but for non-adverse
environmental conditions and/or prioritizing reduced BOM cost and
PCB space, the TVS diode and ferrite bead can be omitted.


== CD4053 Notes

    - the newer TMUX4053 switches can be controlled with logic
      levels lower than the voltage supply (e.g. CMOS, TTL)
    - the older CD4053 needs logic "true" to be the same level as
      the voltage supply
    - the ATTiny13a will run at 5v, but the 4053 will run at the
      effect voltage (9 to 18 volts)
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
    - assume 2n7000/2n7002 mosfets for CD4053 control and LED
      control


== Toolchain
    - avr tools: avr-gcc, avr-libc, avrdude toolchain
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


== Testing and Validation

This project features and extensive testing, validation and
simulation suite.  Specific tests verify specific requirements, as
show in the following table:

| Requirement                          | Test(s) that verify it                         │
|---                                   | ---
| One press → exactly one state change | model_check I2/I5; test_single_clean_press; test_lockstep_cosim
| Release → zero state changes         | model_check I3; test_single_clean_press; test_two_presses_round_trip
| Switch bounce → no extra changes     | test_bouncy_press; test_fuzz_extreme_bounce; test_extreme_bounce (sim)
| Hold seconds → no repeat             | test_long_hold; model_check I5 (liveness proof shows exactly 1)
| EMI/RFI resilience                   | test_sustained_noise; test_asymmetric_emi_bursts; test_random_noise
| Fast taps recognized                 | test_fast_repeated_taps; caveats section
| Latency <10ms                        | test_clean_press_latency; test_oscillator_drift_tolerance
| Deterministic/analyzable             | model_check exhaustive BFS; test_symbolic exhaustive enumeration
| Power-on → BYPASS                    | test_power_on_default; test_power_on_robustness



== ATTiny13a Program Flow

=== Functional Description

Define two program states: PRESS_DEBOUNCE_WAIT, RELEASE_DEBOUNCE_WAIT
Define two effect-circuit states: ENGAGED, BYPASS
Define constants (see "Asymmetric Debounce Timing" above for rationale)
    - RELEASE_THRESH = 25
    - PRESSED_THRESH = 8

The core mechanism is a saturating integrator: a counter that
increments by one per millisecond when the footswitch reads closed,
and decrements by one per millisecond when it reads open, bounded
between 0 and RELEASE_THRESH. A simple debounce timer resets on any
noise sample; the integrator tolerates noise, requiring only that
the majority of samples over a window indicate the intended state. A
spike shorter than PRESSED_THRESH milliseconds cannot reach the
threshold even if perfectly timed. A real press, even through
moderate contact bounce or EMI, accumulates reliably because each
clean low sample contributes persistently.

The state machine has two states. In PRESS_DEBOUNCE_WAIT, the
integrator climbs while the switch is held and falls while released.
When it reaches PRESSED_THRESH, the effect is toggled immediately
and the counter is loaded with RELEASE_THRESH, arming the lockout.
In RELEASE_DEBOUNCE_WAIT, the integrator can only drain (the switch
is being released). Only when it reaches zero does the machine
re-arm for the next press. This lockout prevents switch bounce on
release from generating a second event, and prevents a held switch
from re-triggering.

The thresholds are asymmetric by design. PRESSED_THRESH (8ms) is
kept low for fast response; it is the minimum sustained press
needed to register, and sets the best-case latency. RELEASE_THRESH
(25ms) is larger, providing robust lockout against release bounce
while still allowing fast repeated taps (minimum tap interval =
PRESSED_THRESH + RELEASE_THRESH = 33ms). See "Asymmetric Debounce
Timing" for details.

Additionally, the MCU's watchdog feature is used to reset the
device to the default state (bypass, status LED dark) in the event
of unexpected error, bit flip, etc.


=== State Machine

Saturating integrator (ISR, every 1ms, runs in both states):
  if footswitch pin low  (switch closed):  if counter < RELEASE_THRESH: counter++
  if footswitch pin high (switch open):    if counter > 0             : counter--

State machine (main loop, evaluated after each ISR wake):

| Current State        | Condition                   | Effect action           | Next State            |
| ---                  | ---                         | ---                     | ---                   |
| PRESS_DEBOUNCE_WAIT  | counter >= PRESSED_THRESH   | Toggle engaged/bypass;  | RELEASE_DEBOUNCE_WAIT |
|                      |                             | counter = RELEASE_THRESH|                       |
| PRESS_DEBOUNCE_WAIT  | counter <  PRESSED_THRESH   | (none)                  | PRESS_DEBOUNCE_WAIT   |
| RELEASE_DEBOUNCE_WAIT| counter == 0                | (none)                  | PRESS_DEBOUNCE_WAIT   |
| RELEASE_DEBOUNCE_WAIT| counter >  0                | (none)                  | RELEASE_DEBOUNCE_WAIT |



=== Timing Diagram

Diagram 1: Typical Case

                        |<-- PRESSED_THRESH -->|                  |
                        |        (8ms)         |                  |
   counter:  0  1  2  3  4  5  6  7 [8→25] 24 23 22 ... 2  1  0
                                          ↑ toggle fires;         ↑ re-arm
                                            counter jumps
                                            to RELEASE_THRESH

   PB0:      ‾‾‾‾|___________________________________|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
                   ← switch pressed →                 ← released →

   State:    |<-- PRESS_DEBOUNCE_WAIT -->|<-- RELEASE_DEBOUNCE_WAIT -->|<-- PRESS_DEBOUNCE_WAIT

   LED/4053: dark                        lit                                 (ready for next press)


Diagram 2: EMI/Noise Rejection Case

It shows the counter climbing to 7, then noise interrupting and the counter falling back, never reaching 8:

   counter:  0  1  2  3  4  5  6  7  6  5  4  3  2  1  0  0  0  0 ...
                                                                 (threshold never reached)

   PB0:      ‾‾‾‾|___________________________|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
                   ← spike / noise burst →

   State:    |<----------------- PRESS_DEBOUNCE_WAIT (no toggle) ----------------->|

   LED/4053: dark (unchanged)


