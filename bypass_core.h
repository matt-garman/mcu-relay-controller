// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_CORE_H__
#define BYPASS_CORE_H__

#include "bypass_types.h"

#include <stdint.h>


//////////////////////////////////////////////////////////////////////////////
// PROGRAM GLOBALS
//////////////////////////////////////////////////////////////////////////////

extern volatile effect_state_t effect_state_;
extern volatile program_state_t program_state_;
extern volatile timer_isr_called_t timer_isr_called_;
extern volatile uint8_t debounce_counter_;


//////////////////////////////////////////////////////////////////////////////
// FUNCTIONS
//////////////////////////////////////////////////////////////////////////////

// - set a GPIO pin high or low
// - assumes pin was previously configured as output
void pin_set_high(uint8_t const pin);
void pin_set_low(uint8_t const pin);


// LED_PIN high = status LED lit
// LED_PIN low = status LED dark
void led_pin_set_high(void);
void led_pin_set_low(void);


#endif // BYPASS_CORE_H__
