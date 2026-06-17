// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#include "bypass_pure.h"
#include "bypass_config.h"


// saturating integrator update
// footswitch pin zero (low) == switch closed
// footswitch pin one (high) == switch open
uint8_t debounce_integrate(
        pin_state_t const pin_state, // PIN_STATE_LOW means footswitch is closed
        uint8_t           debounce_counter) { // not explicitly const, but not modified

    uint8_t counter = debounce_counter;

    if (PIN_STATE_LOW == pin_state) { // switch closed
        if (debounce_counter < RELEASE_THRESH) { ++counter; }
    }
    else { // footswitch pin is high -> switch open
        if (debounce_counter > 0U) { --counter; }
    }

    return counter;
}


debounce_step_result_t debounce_step(debounce_context_t const ctx) {

    debounce_step_result_t res = {
        .program_state    = ctx.program_state,
        .effect_state     = ctx.effect_state,
        .toggled          = false,
        .fault            = false,
        .reload_lockout   = false,
        .lockout_value    = 0U
    };

    switch (ctx.program_state) {

        // waiting for the footswitch to be press-debounced
        case PRESS_DEBOUNCE_WAIT:
            {
                // check for press-debounced condition
                if (ctx.debounce_counter >= PRESSED_THRESH) {
                    res.reload_lockout = true;
                    res.lockout_value = RELEASE_THRESH;
                    res.program_state = RELEASE_DEBOUNCE_WAIT;
                    res.toggled = true;
                    if (BYPASS == ctx.effect_state)
                    { 
                        res.effect_state = ENGAGED;
                    } else { // ENGAGED == res.ctx.effect_state
                        res.effect_state = BYPASS;
                    }
                }
            }
            break;

            // waiting for the footswitch to be release-debounced
            // note: holding the switch closed, or mechanical
            //       failure (e.g. switch welded shut) causes this
            //       state to exist indefinitely: this is the design
            //       intent (software is "helpless", need physical
            //       human resolution)
            case RELEASE_DEBOUNCE_WAIT:
                {
                    if (0U == ctx.debounce_counter) {
                        res.program_state = PRESS_DEBOUNCE_WAIT;
                    }
                }
                break;

            default: // should be impossible (but let caller know)
                res.fault = true;
                break;
        }

    return res;
}


debounce_context_t debounce_init_context(pin_state_t const pin_state)
{
    // typical startup case: assume switch is not pressed
    debounce_context_t ctx = {
        .program_state    = PRESS_DEBOUNCE_WAIT,
        .effect_state     = BYPASS,
        .debounce_counter = 0U,
    };

    // special case: footswitch pressed during power-on: keep in bypass state,
    // but use timer + interrupt function to wait for release
    if (PIN_STATE_LOW == pin_state) {
        ctx.program_state = RELEASE_DEBOUNCE_WAIT;
        ctx.debounce_counter = RELEASE_THRESH;
    }

    return ctx;
}
