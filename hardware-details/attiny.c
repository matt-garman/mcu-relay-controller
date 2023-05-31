// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.


// ATtiny85 pinout
//                                         +----+
// (PCINT5/~RESET/ADC0/dW)        PB5 pin1-|    |-pin8 VCC
// (PCINT3/XTAL1/CLKI/~OC1B/ADC3) PB3 pin2-|    |-pin7 PB2 (SCK/USCK/SCL/ADC1/T0/INT0/PCINT2)
// (PCINT4/XTAL2/CLKO/OC1B/ADC2)  PB4 pin3-|    |-pin6 PB1 (MISO/DO/AIN1/OC0B/OC1A/PCINT1)
//                                GND pin4-|    |-pin5 PB0 (MOSI/DI/SDA/AIN0/OC0A/~OC1A/AREF/PCINT0)
//                                         +----+
//
// PB0 => momentary switch
// PB1 => status indicator LED
// PB3 => relay coil pin1 (goes high for set/activate)
// PB2 => relay coil pin2 (goes high for reset/deactivate)

#include "../mcu-relay-controller-iface.h"

#include "attiny.h"

#include <avr/io.h>        // Defines register and bit names
#include <avr/power.h>     // power_all_disable();
#include <avr/sleep.h>     // sleep states
#include <avr/interrupt.h> // ISR() interrupt service routine macro

#define STARTUP_DELAY_MS 5


void MRC_hardware_init(void)
{
    // not sure if this is necessary - just want to give the mcu and
    // surrounding circuitry little time to "settle"
    _delay_ms(STARTUP_DELAY_MS);

    // set data direction register so that PB0 is an input,
    // PB1-3 are outputs
    DDRB = 0b00001110;

    // enable the input pullup for PB0
    // keeps PB0 high, will go low when switch is pressed
    PORTB = 0b00000001;

    // disable ADC (analog to digital converter)
    ADCSRA = 0;

    // disable all other built-in modules
    // valid for ATtiny85 (and presumably 45)
#ifndef ATTINY13
    power_all_disable();
#endif // ATTINY13

    // turn on pin change interrupts
    GIMSK = 0b00100000;

    // set physical pin5 aka PB0 to be the pin watched for pin changes
    // bits 5:0 - PCINT[5:0]
    // PCINT0 = bit0 of PCMSK, aka physical pin5 aka PB0
    // see page 52, section 9.3.4 of datasheet
    PCMSK = 0b00000001; 

    // MCU Control Register
    // Bits 1:0 - ISC0[1:0]: Interrupt Sense Control 0 Bit 1 and Bit 0
    //MCUCR = 0b00000010; // falling edge of INT0 generates an interrupt request

    _delay_ms(STARTUP_DELAY_MS);

    // SLEEP_MODE_PWR_DOWN is the lowest power state for the ATtiny45/85
    // for our purposes, we generally want only the pin-change detection
    // circuitry active
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);

}

void MRC_disable_interrupts(void) { cli(); }
void MRC_disable_sleep(void) { sleep_disable(); }
void MRC_enable_interrupts(void) { sei(); }

void MRC_enter_sleep_mode(void)
{
    sleep_enable(); // enable sleeping
    sleep_mode();   // go to sleep
}

void MRC_led_pin_set_high(void) { PORTB |=  (1 << PB1); }
void MRC_led_pin_set_low(void)  { PORTB &= ~(1 << PB1); }
void MRC_led_toggle(void)       { PORTB ^=  (1 << PB1); }

void MRC_relay_coil_pin1_set_high(void) { PORTB |=  (1 << PB3); } // PB3 == pin1
void MRC_relay_coil_pin1_set_low(void)  { PORTB &= ~(1 << PB3); }
void MRC_relay_coil_pin2_set_high(void) { PORTB |=  (1 << PB2); } // PB2 == pin2
void MRC_relay_coil_pin2_set_low(void)  { PORTB &= ~(1 << PB2); }

uint8_t MRC_switch_pin_get_state(void) { return (PINB & 0b1); }
void MRC_switch_pin_clear_int_flags(void) { }


// https://www.nongnu.org/avr-libc/user-manual/group__avr__interrupts.html
// don't actually do anything in the ISR (except disable interrupts), we just
// want to wake the mcu from sleep; actual work takes place in the main loop
ISR(PCINT0_vect)
{
    cli(); // disable interrupts
}

