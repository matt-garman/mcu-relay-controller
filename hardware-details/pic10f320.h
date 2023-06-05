// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef PIC10F320_H__
#define PIC10F320_H__

#include <xc.h>


/*
 * configuration bits
 */
#pragma config FOSC = INTOSC // Oscillator Selection bits (INTOSC oscillator: CLKIN function disabled)
#pragma config BOREN = OFF   // Brown-out Reset Enable (Brown-out Reset disabled)
#pragma config WDTE = OFF    // Watchdog Timer Enable (WDT disabled)
#pragma config PWRTE = OFF   // Power-up Timer Enable bit (PWRT disabled)
#pragma config MCLRE = OFF   // MCLR Pin Function Select bit (MCLR pin function is digital input, MCLR internally tied to VDD)
#pragma config CP = OFF      // Code Protection bit (Program memory code protection is disabled)
#pragma config LVP = OFF     // Low-Voltage Programming Disable (Low-voltage programming enabled)
#pragma config LPBOR = OFF   // Brown-out Reset Selection bits (BOR disabled)
#pragma config BORV = LO     // Brown-out Reset Voltage Selection (Brown-out Reset Voltage (Vbor), low trip point selected.)
#pragma config WRT = OFF     // Flash Memory Self-Write Protection (Write protection off)


// - _XTAL_FREQ is used only by the delay macros; it does not actually affect
//   the operating frequency
// - the operating frequency can be changed by setting the IRCF bits of the
//   OSCCON register
// - See Microchip Community Forum post "Changing operating frequency of
//   pic10f320 and pic12f675 using internal oscillator":
//   https://forum.microchip.com/s/topic/a5C3l000000BoWVEA0/t390718?comment=P-2919727
// - Note: default frequency after reset is 8MHz
#define _XTAL_FREQ 1000000  // OSCCON = 0b00110000; // 1 MHz

#define MRC_sleep_millisecs(n) __delay_ms(n);

#endif // PIC10F320_H__

