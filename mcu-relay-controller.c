// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.


#include "mcu-relay-controller-iface.h"


// hardware design:
// the microcontroller should have at least four general purpose IO pins for
// the folloing:
//     - status indicator LED
//     - momentary switch
//     - relay coil 1
//     - relay coil 2


// include hardware-specific implementation file(s)
#ifdef IMPL_DUMMY
#  include "dummy.h"
#endif
#ifdef IMPL_ATTINY
#  include "attiny.h"
#endif
#ifdef IMPL_PIC12F675
#  include "pic12f675.h"
#endif


// the relay has two states, which we'll call ON or OFF
volatile uint8_t relay_state = OFF;

void relay_activate(void)
{
    MRC_relay_coil_pin1_set_high();
    MRC_relay_coil_pin2_set_low(); // should already be low
    MRC_sleep_millisecs(RELAY_SETTLE_TIME_MS);
    MRC_relay_coil_pin1_set_low();
    relay_state = ON;
}

void relay_deactivate(void)
{
    MRC_relay_coil_pin2_set_high();
    MRC_relay_coil_pin1_set_low(); // should already be low
    MRC_sleep_millisecs(RELAY_SETTLE_TIME_MS);
    MRC_relay_coil_pin2_set_low();
    relay_state = OFF;
}

void relay_toggle(void)
{
    if (relay_state == OFF) { relay_activate();  }
    else                    { relay_deactivate(); }
}

int main(int argc, char* argv[])
{
    // initialize hardware
    MRC_hardware_init();

    // make relay and status indicator LED consistent consistent with the
    // default relay_state = OFF
    relay_deactivate();
    MRC_led_pin_set_low();

    while (1)
    {
        MRC_disable_interrupts();
        MRC_disable_sleep();

        if (0 == MRC_switch_pin_get_state())
        {
            relay_toggle();
            MRC_led_toggle();
            MRC_sleep_millisecs(SWITCH_DEBOUNCE_TIME_MS);
        }

        MRC_enable_interrupts();
        MRC_enter_sleep_mode();
    }

    return 0;
}
