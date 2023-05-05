// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.


/*
 * dummy interface implementation - simple stubs that do nothing, except act
 * as a rudimentary test for compilation
 * test with:
 *    gcc -Wall -ggdb3 -Os mcu-relay-controller.c dummy.c
 */

#include "mcu-relay-controller-iface.h"


void MRC_hardware_init() { }
void MRC_disable_interrupts() { }
void MRC_disable_sleep() { }
void MRC_enable_interrupts() { }
void MRC_enter_sleep_mode() { }
void MRC_sleep_millisecs(uint16_t const ms) { }
void MRC_led_pin_high() { }
void MRC_led_pin_low() { }
void MRC_led_toggle() { }
void MRC_relay_coil_pin1_set_high() { }
void MRC_relay_coil_pin1_set_low() { }
void MRC_relay_coil_pin2_set_high() { }
void MRC_relay_coil_pin2_set_low() { }
uint8_t MRC_get_switch_pin_state() { return 0; }

