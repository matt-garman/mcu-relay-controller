#ifndef BYPASS_OUTPUT_H__
#define BYPASS_OUTPUT_H__

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


#endif // BYPASS_OUTPUT_H__
