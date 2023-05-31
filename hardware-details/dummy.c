// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.


/*
 * dummy interface implementation - simple stubs that do nothing, except act
 * as a rudimentary test for compilation
 * test with (for example):
 *    gcc -Wall -ggdb3 -Os mcu-relay-controller.c dummy.c
 */

#include "dummy.h"

#include "../mcu-relay-controller-iface.h"

void MRC_hardware_init(void) { }
void MRC_disable_interrupts(void) { }
void MRC_disable_sleep(void) { }
void MRC_enable_interrupts(void) { }
void MRC_enter_sleep_mode(void) { }
void MRC_led_pin_set_high(void) { }
void MRC_led_pin_set_low(void) { }
void MRC_led_toggle(void) { }
void MRC_relay_coil_pin1_set_high(void) { }
void MRC_relay_coil_pin1_set_low(void) { }
void MRC_relay_coil_pin2_set_high(void) { }
void MRC_relay_coil_pin2_set_low(void) { }
uint8_t MRC_switch_pin_get_state(void) { return 0; }
void MRC_switch_pin_clear_int_flags(void) { }

