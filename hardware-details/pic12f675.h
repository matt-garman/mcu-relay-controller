// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef PIC12F657_H__
#define PIC12F657_H__

#include <xc.h>

// configuration bits
#pragma config FOSC = INTRCIO
#pragma config WDTE = OFF
#pragma config PWRTE = OFF
#pragma config MCLRE = OFF
#pragma config BOREN = OFF
#pragma config CP = OFF
#pragma config CPD = OFF

// Does not appear to be possible to change the pic12f675's operating
// frequency if using the internal oscillator; default internal oscillator
// frequncy is 4MHz.
// See Microchip Community Forum post "Changing operating frequency of
// pic10f320 and pic12f675 using internal oscillator":
// https://forum.microchip.com/s/topic/a5C3l000000BoWVEA0/t390718?comment=P-2919727
#define _XTAL_FREQ 4000000


#define MRC_sleep_millisecs(n) __delay_ms(n);

#endif // PIC12F657_H__

