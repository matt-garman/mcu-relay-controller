#ifndef BYPASS_TYPES_H__
#define BYPASS_TYPES_H__



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


// a flag to "multiplex" the WDT across the timer ISR and main() loop
typedef enum {
    TIMER_ISR_CALLED = 0,
    TIMER_ISR_NOT_CALLED,
} timer_isr_called_t;



#endif // BYPASS_TYPES_H__
