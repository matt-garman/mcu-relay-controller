// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_TYPES_H__
#define BYPASS_TYPES_H__

#include <stdint.h>


// possible high-level states of the debounce/bypass scheme
typedef enum {
    // 1ms footswitch pin sampling, waiting for footswitch to be
    // press-debounced (i.e. footswitch considered open/released in this
    // state)
    PRESS_DEBOUNCE_WAIT = 0,

    // 1ms footswitch pin sampling, footswitch was previously confirmed
    // debounce-pressed, now waiting for footswitch to be release-debounced
    // (i.e. footswitch considered closed/pressed in this state)
    RELEASE_DEBOUNCE_WAIT,
} program_state_t;


// a flag to keep track of the effect/bypass state
typedef enum {
    BYPASS = 0,
    ENGAGED,
} effect_state_t;


typedef enum {
    PIN_STATE_LOW = 0,
    PIN_STATE_HIGH
} pin_state_t;


// wrap up the three global variables that comprise the runtime context of the
// debounce-bypass algorithm
typedef struct {
    program_state_t program_state;
    effect_state_t  effect_state;
    uint8_t         debounce_counter;
} debounce_context_t;




#endif // BYPASS_TYPES_H__
