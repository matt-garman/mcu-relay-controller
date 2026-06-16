// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_OUTPUT_COMMON_H__
#define BYPASS_OUTPUT_COMMON_H__

// Pin names (PB0/PB1/...) come from <avr/io.h> on the AVR target. Host test
// code (see test/bypass_output_host.h) defines those names itself before
// including this header, so it can read the SAME pin assignments the firmware
// uses without pulling in the AVR-only <avr/io.h>.
#if defined(__AVR__)
#  include <avr/io.h>
#endif


// footswitch and status LED pins are common across all output variants
#define FOOTSW_PIN (PB0)
#define LED_PIN    (PB1)

#endif // BYPASS_OUTPUT_COMMON_H__
