// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.


/*
 * interface
 *   - these functions need to be defined per-device that is to be supported
 *   - "MRC" prefix is for mcu-relay-controller (poor man's namespace)
 */


#include <stdint.h>


// called at program startup
// generally, this should do things such as:
//    - any startup delay
//    - setup IO pins
//    - set initial state of device (currently assumed to be OFF)
//    - set appropriate wake-on-pin-change interrupts
//    - any other hardware-specific stuff
void MRC_hardware_init();

// abstractions for hardware-specific functionality
void MRC_disable_interrupts();
void MRC_disable_sleep();
void MRC_enable_interrupts();
void MRC_enter_sleep_mode();
#undef MRC_sleep_millisecs


// status indicator LED functionality, toggle state of the pin connected to
// the status LED; convention used here:
//   OFF state => LED pin held low
//   ON  state => LED pin held high
void MRC_led_pin_set_high(); // i.e. turn LED on
void MRC_led_pin_set_low();  // i.e. turn LED off
void MRC_led_toggle(); // flip state (typically can be done with a single NOR expression)

// relay control: two pins are used for relay coil control of a *latching*
// relay (this could be a single-coil latching relay, or a dual-coil latching
// relay); call these pins "pin1" and "pin2", then:
//   activate   => pin1 held high for RELAY_SETTLE_TIME_MS, then set low
//                 pin2 kept low
//   deactivate => pin1 kept low
//                 pin2 held high for RELAY_SETTLE_TIME_MS, then set low
void MRC_relay_coil_pin1_set_high();
void MRC_relay_coil_pin1_set_low();
void MRC_relay_coil_pin2_set_high();
void MRC_relay_coil_pin2_set_low();

// momentary-switch connected pin: is it high or low?
// convention used here: the momentary switch pin will normally be kept high
// (e.g. via pullup resistor); with the switch is pressed, it will force the
// pin low
// should return 1 for HIGH or 0 for LOW
uint8_t MRC_switch_pin_get_state();

