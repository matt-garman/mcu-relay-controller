// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef MCU_RELAY_CONTROLLER_IFACE_H__
#define MCU_RELAY_CONTROLLER_IFACE_H__

/*
 * interface
 *   - these functions need to be defined per-device that is to be supported
 *   - "MRC" prefix is for mcu-relay-controller (poor man's namespace)
 */


#include <stdint.h>


/*
 * global constants
 */

// - how long must a current be applied to the relay coil in order for it to
//   reliably change state?
// - this is relay-specific, consult the relay's datasheet; for the Takamisawa
//   AL5WN-K and Panasonic TQ2-L-5V, 3ms is specified, but we want to add a
//   few millisecs for margin
// - read of the Kemet EC2-3TNU (which has very similar specs as Panasonic
//   TQ2-L-5V) datasheet suggests a current pulse time of at least 10ms to
//   account for relay bounce time
#define RELAY_SETTLE_TIME_MS 15

// - the reference implementation circuit includes an RF filter on the wire
//   between MCU and momentary switch, which should help eliminate spurious
//   pin-change interrupts; it also acts as a hardware debouncer
// - we will still do some software-level debouncing: in addition to checking
//   switch state multiple times, we'll also add a hard delay after a
//   confirmed (i.e. debounced) switch press
#define SWITCH_DEBOUNCE_TIME_MS 100

// - how many times we'll poll the switch's state during the debounce routine
// - note the debounce routine 
#define MAX_N_SWITCH_DEBOUNCE_READS 80

// - this number is used in the switch debounce routine; it defines how many
//   consecutive desired state reads we need from the switch to consider it
//   pressed
// - 0xFF is hex, corresponding binary (uint8_t) is: 0b11111111
//   in other words, we want eight consecutive, consistent reads of the switch
//   state to consider it pressed and de-bounced
#define SWITCH_DEBOUNCE_TARGET 0xFF

// hopefully these are self-explanatory :)
#define OFF (0)
#define ON (1)

#define LOW (0)
#define HIGH (1)

#ifndef FALSE
#  define FALSE (0)
#  define TRUE (!FALSE)
#endif

/*
 * abstractions for hardware-specific functionality
 *
 * see example implementations: some of these functions may have empty bodies
 */

// called at program startup
// generally, this should do things such as:
//    - any startup delay
//    - setup IO pins
//    - set initial state of device (currently assumed to be OFF)
//    - set appropriate wake-on-pin-change interrupts
//    - any other hardware-specific stuff
void MRC_hardware_init(void);

// sleep and interrupt related
// note that MRC_sleep_millisecs() is a macro, rather than a function
void MRC_disable_interrupts(void);
void MRC_disable_sleep(void);
void MRC_enable_interrupts(void);
void MRC_enter_sleep_mode(void);
#undef MRC_sleep_millisecs

// status indicator LED functionality, toggle state of the pin connected to
// the status LED; convention used here:
//   OFF state => LED pin held low
//   ON  state => LED pin held high
void MRC_led_pin_set_high(void); // i.e. turn LED on
void MRC_led_pin_set_low(void);  // i.e. turn LED off
void MRC_led_toggle(void); // flip state (typically can be done with a single NOR expression)

// relay control: two pins are used for relay coil control of a *latching*
// relay (this could be a single-coil latching relay, or a dual-coil latching
// relay); call these pins "pin1" and "pin2", then:
//   activate   => pin1 held high for RELAY_SETTLE_TIME_MS, then set low
//                 pin2 kept low
//   deactivate => pin1 kept low
//                 pin2 held high for RELAY_SETTLE_TIME_MS, then set low
void MRC_relay_coil_pin1_set_high(void);
void MRC_relay_coil_pin1_set_low(void);
void MRC_relay_coil_pin2_set_high(void);
void MRC_relay_coil_pin2_set_low(void);

// momentary-switch connected pin: is it high or low?
// convention used here: the momentary switch pin will normally be kept high
// (e.g. via pullup resistor); with the switch is pressed, it will force the
// pin low
// should return HIGH or LOW
uint8_t MRC_switch_pin_get_state(void);

// some micro-controllers (e.g. pic10f320) set a flag when an interrupt is
// triggered, and that flag needs to be cleared after it is read.
void MRC_switch_pin_clear_int_flags(void);

#endif // MCU_RELAY_CONTROLLER_IFACE_H__

