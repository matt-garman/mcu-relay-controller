#ifndef BYPASS_OUTPUT_TQ2_L2_5V_RELAY_H__
#define BYPASS_OUTPUT_TQ2_L2_5V_RELAY_H__

#include <avr/io.h>


#define RELAY_RESET_PIN (PB2)
#define RELAY_SET_PIN   (PB3)

// Panasonic TQ-L2-5V specifies a 4ms minimum current pulse for the set/reset
// coils; multiple by a factor of three for a safety margin
#define TQ2_L2_5V_PULSE_MS (12) 


#endif // BYPASS_OUTPUT_TQ2_L2_5V_RELAY_H__

