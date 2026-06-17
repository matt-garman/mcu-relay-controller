// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#include "bypass_output_cd4053_with_mute.h"
#include "bypass_output_common.h"
#include "bypass_config.h"
#include "bypass_hw_iface.h"

#include <avr/io.h>     // Defines register and bit names
#include <util/delay.h> // _delay_ms()
#include <assert.h>     // For static_assert()


uint8_t hw_is_sanity_check_failed(void) {

    static_assert(CD4053_MUTE_DELAY_MS < RELEASE_THRESH,
            "CD4053 mute delay must be shorter than the release-lockout window, "
            "or the re-arm point can be missed during the blocking actuation");

    return 
        ((DDRB & ((1 << LED_PIN) | (1 << CD4053_CTL1) | (1 << CD4053_CTL2))) !=
         ((1 << LED_PIN) | (1 << CD4053_CTL1) | (1 << CD4053_CTL2)))
        ;
}


void hw_init_ddrb_setup(void) {
    DDRB = (1 << LED_PIN) | (1 << CD4053_CTL1) | (1 << CD4053_CTL2) | (1 << PB4);
}


// See "Improved Scheme With Muting" in DESIGN_DOCUMENTATION.adoc
void hw_set_bypass_state(void) {
    hw_pin_set_high(CD4053_CTL1); // re-assert previous ENGAGED state
    hw_pin_set_high(CD4053_CTL2);

    hw_led_pin_set_low();        // dark status LED

    hw_pin_set_low(CD4053_CTL1); // MUTE
    _delay_ms(CD4053_MUTE_DELAY_MS); // busy sleep for pre-switch mute time

    hw_pin_set_low(CD4053_CTL2); // un-mute in BYPASS state
}

void hw_set_engaged_state(void) {
    hw_pin_set_low(CD4053_CTL1); // re-assert previous BYPASS state
    hw_pin_set_low(CD4053_CTL2);

    hw_led_pin_set_high();       // light status LED

    hw_pin_set_high(CD4053_CTL2); // MUTE
    _delay_ms(CD4053_MUTE_DELAY_MS); // busy sleep for pre-switch mute time

    hw_pin_set_high(CD4053_CTL1); // un-mute in ENGAGED state
}


