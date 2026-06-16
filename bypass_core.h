#ifndef BYPASS_CORE_H__
#define BYPASS_CORE_H__


//////////////////////////////////////////////////////////////////////////////
// PROGRAM GLOBALS
//////////////////////////////////////////////////////////////////////////////

extern volatile effect_state_t effect_state_;
extern volatile program_state_t program_state_;
extern volatile timer_isr_called_t timer_isr_called_;
extern volatile uint8_t debounce_counter_;


//////////////////////////////////////////////////////////////////////////////
// FUNCTIONS
//////////////////////////////////////////////////////////////////////////////

// - set a GPIO pin high or low
// - assumes pin was previously configured as output
void pin_set_high(uint8_t const pin);
void pin_set_low(uint8_t const pin);


// - sets global effect state (ENGAGE/BYPASS)
// - lights or dims status LED
// - does implementation-specific audio routing device control (e.g. cd4053
//   switching, relay coil set/reset)
void set_bypass_state(void);
void set_engaged_state(void);

// - output-implementation-specific sanity check(s)
// - return 1 on sanity check failure: will force WDT timeout
// - return 0 on sanity check OK
uint8_t is_sanity_check_failed(void);

#endif // BYPASS_CORE_H__
