// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_PURE_H__
#define BYPASS_PURE_H__

#include "bypass_types.h"

#include <stdint.h>
#include <stdbool.h>


// saturating integrator update
//   footswitch pin zero (low) == switch closed
//   footswitch pin one (high) == switch open
//
// returns: debounce_counter: same as input value, or incremented or
//          decremented by one, depending on pin_low and current
//          debounce_counter value
uint8_t debounce_integrate(
        pin_state_t const pin_state,
        uint8_t           debounce_counter);



// sets initial debounce state by looking at the current footswitch state
// (open/closed); designed to be called at program start-up/ater a RESET
//
// return a debounce context that corresponds to the footswitch state
debounce_context_t debounce_init_context(pin_state_t const pin_state);


typedef struct {
      program_state_t program_state;   // caller-owned: safe to write
      effect_state_t  effect_state;    // caller-owned: safe to write
      bool            toggled;         // informs caller of effect-state toggle
      bool            fault;           // informs caller of fault condition
      bool            reload_lockout;  // flag to instruct caller whether or
                                       // not to apply lockout_value
      uint8_t         lockout_value;   // set caller-owned global context
                                       // debounce_counter to this value if
                                       // reload_lockout is true
} debounce_step_result_t;


// run from main loop, once per tick, after debounce_integrate()
// looks at the debounce counter, and returns an updated state if state
// transition conditions are met
//
// returns:
//   a result struct which informs the caller if the state was toggled, if a
//   fault was detected, and a (possibly updated) debounce context
debounce_step_result_t debounce_step(debounce_context_t const ctx);


#endif // BYPASS_PURE_H__
