// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#include "bypass_output_tq2_l2_5v_relay.h"
#include "bypass_output_common.h"
#include "bypass_config.h"
#include "bypass_hw_iface.h"

#include <avr/io.h>     // Defines register and bit names
#include <util/delay.h> // _delay_ms()
#include <assert.h>     // For static_assert()



uint8_t hw_is_sanity_check_failed(void) {

    static_assert(TQ2_L2_5V_PULSE_MS < RELEASE_THRESH,
            "relay coil pulse must be shorter than the release-lockout window, "
            "or the re-arm point can be missed during the blocking actuation");

    return
        ((DDRB & ((1 << LED_PIN) | (1 << RELAY_SET_PIN) | (1 << RELAY_RESET_PIN))) !=
         ((1 << LED_PIN) | (1 << RELAY_SET_PIN) | (1 << RELAY_RESET_PIN)))
        ;
}


void hw_init_ddrb_setup(void) {
    DDRB = (1 << LED_PIN) | (1 << RELAY_SET_PIN) | (1 << RELAY_RESET_PIN) | (1 << PB4);
}


// force both coils low
static void set_relay_coils_low(void) {
    hw_pin_set_low(RELAY_RESET_PIN);
    hw_pin_set_low(RELAY_SET_PIN);
}

void hw_set_bypass_state(void) {
    set_relay_coils_low();

    hw_led_pin_set_low();        // dark status LED

    hw_pin_set_high(RELAY_RESET_PIN); // pulse reset coil
    _delay_ms(TQ2_L2_5V_PULSE_MS); // busy sleep for coil pulse time

    set_relay_coils_low();
}

void hw_set_engaged_state(void) {
    set_relay_coils_low();

    hw_led_pin_set_high();       // light status LED

    hw_pin_set_high(RELAY_SET_PIN);   // pulse set coil
    _delay_ms(TQ2_L2_5V_PULSE_MS); // busy sleep for coil pulse time

    set_relay_coils_low();
}

