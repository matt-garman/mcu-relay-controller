// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#include "bypass_output_cd4053_simple.h"
#include "bypass_output_common.h"
#include "bypass_hw_iface.h"

#include <avr/io.h> // Defines register and bit names


// assert critical pin directions hold: LED & CD4053 outputs, footswitch input
uint8_t hw_is_sanity_check_failed(void) {
    return ((DDRB & ((1 << LED_PIN) | (1 << CD4053_PIN))) !=
            ((1 << LED_PIN) | (1 << CD4053_PIN)));
}


void hw_init_ddrb_setup(void) {
    DDRB = (1 << LED_PIN) | (1 << CD4053_PIN) | (1 << PB3) | (1 << PB4);
}


// CD4053_PIN high -> mosfet on  -> 4053 control pins low
// CD4053_PIN low  -> mosfet off -> 4053 control pins high
void hw_set_bypass_state(void) {
    hw_led_pin_set_low();        // dark status LED
    hw_pin_set_low(CD4053_PIN);  // set CD4053 pin low
}

void hw_set_engaged_state(void) {
    hw_led_pin_set_high();       // light status LED
    hw_pin_set_high(CD4053_PIN); // set CD4053 pin high
}

