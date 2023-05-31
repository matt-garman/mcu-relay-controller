// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef ATTINY_H__
#define ATTINY_H__

#include <util/delay.h>    // Defines _delay_ms

#define MRC_sleep_millisecs(n) _delay_ms(n);
#define MRC_sleep_microsecs(n) _delay_us(n);

#endif // ATTINY_H__

