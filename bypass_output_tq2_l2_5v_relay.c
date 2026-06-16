#include "bypass_core.h"
#include "bypass_output.h"
#include "bypass_output_tq2-l2-5v_relay.h"


// FIXME
uint8_t is_sanity_check_failed(void) {
    return
        ((DDRB & ((1 << LED_PIN) | (1 << RELAY_SET_PIN) | (1 << RELAY_RESET_PIN))) !=
         ((1 << LED_PIN) | (1 << RELAY_SET_PIN) | (1 << RELAY_RESET_PIN)))
}


void init_ddrb_setup(void) {
    DDRB = (1 << LED_PIN) | (1 << RELAY_SET_PIN) | (1 << RELAY_RESET_PIN) | (1 << PB4);
}


// force both coils low
static void set_relay_coils_low(void) {
    pin_set_low(RELAY_RESET_PIN);
    pin_set_low(RELAY_SET_PIN);
}

void set_bypass_state(void) {
    set_relay_coils_low();

    effect_state_ = BYPASS;   // set effect state to BYPASS
    led_pin_set_low();        // dark status LED

    pin_set_high(RELAY_RESET_PIN); // pulse reset coil
    _delay_ms(TQ2_L2_5V_PULSE_MS); // busy sleep for coil pulse time

    set_relay_coils_low();
}

void set_engaged_state(void) {
    set_relay_coils_low();

    effect_state_ = ENGAGED;  // set effect state to ENGAGED
    led_pin_set_high();       // light status LED

    pin_set_high(RELAY_SET_PIN);   // pulse set coil
    _delay_ms(TQ2_L2_5V_PULSE_MS); // busy sleep for coil pulse time

    set_relay_coils_low();
}

