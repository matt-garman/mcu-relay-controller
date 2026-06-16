// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_OUTPUT_TQ2_L2_5V_RELAY_H__
#define BYPASS_OUTPUT_TQ2_L2_5V_RELAY_H__

#if defined(__AVR__)
#  include <avr/io.h>
#endif


#define RELAY_RESET_PIN (PB2)
#define RELAY_SET_PIN   (PB3)

// Panasonic TQ-L2-5V specifies a 4ms minimum current pulse for the set/reset
// coils; multiple by a factor of three for a safety margin
#define TQ2_L2_5V_PULSE_MS (12) 


#endif // BYPASS_OUTPUT_TQ2_L2_5V_RELAY_H__

