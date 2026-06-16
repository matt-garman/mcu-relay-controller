
#include "bypass_core.h"
#include "bypass_output.h"
#include "bypass_output_common.h"
#include "bypass_output_cd4053_simple.h"

// assert critical pin directions hold: LED & CD4053 outputs, footswitch input
uint8_t is_sanity_check_failed(void) {
    return ((DDRB & ((1 << LED_PIN) | (1 << CD4053_PIN))) !=
            ((1 << LED_PIN) | (1 << CD4053_PIN)));
}


void init_ddrb_setup(void) {
    DDRB = (1 << LED_PIN) | (1 << CD4053_PIN) | (1 << PB3) | (1 << PB4);
}


// CD4053_PIN high -> mosfet on  -> 4053 control pins low
// CD4053_PIN low  -> mosfet off -> 4053 control pins high
void set_bypass_state(void) {
    effect_state_ = BYPASS;   // set effect state to BYPASS
    led_pin_set_low();        // dark status LED
    pin_set_low(CD4053_PIN);  // set CD4053 pin low
}

void set_engaged_state(void) {
    effect_state_ = ENGAGED;  // set effect state to ENGAGED
    led_pin_set_high();       // light status LED
    pin_set_high(CD4053_PIN); // set CD4053 pin high
}

