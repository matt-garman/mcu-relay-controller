// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_HW_IFACE_H__
#define BYPASS_HW_IFACE_H__

#include <stdint.h>


// - set a GPIO pin high or low
// - assumes pin was previously configured as output
void hw_pin_set_high(uint8_t const pin);
void hw_pin_set_low(uint8_t const pin);


// LED_PIN high = status LED lit
// LED_PIN low = status LED dark
void hw_led_pin_set_high(void);
void hw_led_pin_set_low(void);


// - sets global effect state (ENGAGE/BYPASS)
// - lights or dims status LED
// - does implementation-specific audio routing device control (e.g. cd4053
//   switching, relay coil set/reset)
void hw_set_bypass_state(void);
void hw_set_engaged_state(void);


// - output-implementation-specific sanity check(s)
// - return 1 on sanity check failure: will force WDT timeout
// - return 0 on sanity check OK
uint8_t hw_is_sanity_check_failed(void);


void hw_init_ddrb_setup(void);


#endif // BYPASS_HW_IFACE_H__
