// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#include <xc.h>

// configuration bits
#pragma config FOSC = INTRCIO
#pragma config WDTE = OFF
#pragma config PWRTE = OFF
#pragma config MCLRE = OFF
#pragma config BOREN = OFF
#pragma config CP = OFF
#pragma config CPD = OFF

//#define _XTAL_FREQ 1000000
#define _XTAL_FREQ 4000000



#define MRC_sleep_millisecs(n) __delay_ms(n);


