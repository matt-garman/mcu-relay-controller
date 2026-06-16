#include "bypass_core.h"
#include "bypass_output.h"
#include "bypass_output_common.h"
#include "bypass_output_cd4053_with_mute.h"


uint8_t is_sanity_check_failed(void) {
    return 
        ((DDRB & ((1 << LED_PIN) | (1 << CD4053_CTL1) | (1 << CD4053_CTL2))) !=
         ((1 << LED_PIN) | (1 << CD4053_CTL1) | (1 << CD4053_CTL2)))
        ;
}


void init_ddrb_setup(void) {
    DDRB = (1 << LED_PIN) | (1 << CD4053_CTL1) | (1 << CD4053_CTL2) | (1 << PB4);
}


// See "Improved Scheme With Muting" in DESIGN_DOCUMENTATION.adoc
void set_bypass_state(void) {
    pin_set_high(CD4053_CTL1); // re-assert previous ENGAGED state
    pin_set_high(CD4053_CTL2);

    effect_state_ = BYPASS;   // set effect state to BYPASS
    led_pin_set_low();        // dark status LED

    pin_set_low(CD4053_CTL1); // MUTE
    _delay_ms(CD4053_MUTE_DELAY_MS); // busy sleep for pre-switch mute time

    pin_set_low(CD4053_CTL2); // un-mute in BYPASS state
}

void set_engaged_state(void) {
    pin_set_low(CD4053_CTL1); // re-assert previous BYPASS state
    pin_set_low(CD4053_CTL2);

    effect_state_ = ENGAGED;  // set effect state to ENGAGED
    led_pin_set_high();       // light status LED

    pin_set_high(CD4053_CTL2); // MUTE
    _delay_ms(CD4053_MUTE_DELAY_MS); // busy sleep for pre-switch mute time

    pin_set_high(CD4053_CTL1); // un-mute in ENGAGED state
}


