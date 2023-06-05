// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.


// pic12f675 pinout
//
//             +----+
// VDD    pin1-|    |-pin8 GND
// GPIO.5 pin2-|    |-pin7 GPIO.0
// GPIO.4 pin3-|    |-pin6 GPIO.1
// GPIO.3 pin4-|    |-pin5 GPIO.2
//             +----+
//
// pin2/GP5 and pin3/GP4 => to relay coil
// pin7/GP0 => to LED anode (go high when effect on)
// pin6/GP1 => to switch, pulled high, switch closed = pulled to 0v
// pin4, pin5 = NC
//
// https://embeddedlaboratory.blogspot.com/2016/10/how-to-solve-target-device-has-invalid.html
// How to solve "Target Device has Invalid Calibration Data (0x00)"
// In MPLAB X:
//     File -> Project Properties
//          Conf -> [programmer, i.e. PICkit 3]
//          Option Categories -> Program Options
//          Select "Program calibration memory" and apply
//     File -> Project Properties
//          XC8 Global Options -> XC8 Linker
//          Option Categories -> Runtime
//          Select "Calibrate oscillator" and apply
//


#include "../mcu-relay-controller-iface.h"

#include "pic12f675.h"


#define STARTUP_DELAY_MS 72 // startup delay - FIXME - where 72ms comes from?


void MRC_hardware_init(void)
{
    __delay_ms(STARTUP_DELAY_MS);

    // option register
    // 7     6      5    4    3   2   1   0
    // ~GPPU INTEDG T0CS T0SE PSA PS2 PS1 PS0
    OPTION_REG = 0;

    // set up pins
    ANSEL = 0; // no analog GPIO
    CMCON = 0x07; // comparator off
    ADCON0 = 0; // ADC and DAC converters off
    VRCON = 0; // turn off voltage reference
    TRISIO0 = 0; // GPIO 0 is an output ("0" like "Output")
    TRISIO4 = 0;
    TRISIO5 = 0;
    TRISIO1 = 1; // GPIO 1 is an input (1 like "Input") 
    GPIO = 0; // Initially, all GPIOs are in a low state

    // interrupt control register
    // 7   6    5    4    3    2    1    0
    // GIE PEIE T0IE INTE GPIE T0IF INTF GPIF
    // GIE = global interrupt enable
    // GPIE = port change interrupt enable
    //INTCON = 0b10001000;
    INTCON = 0; // disable for now, enable in main loop

    // interrupt-on-change GPIO register
    IOC = 0b00000010; // should be IOC1 = GP1 = switch

    // set weak pull-up resistor for switch???
    // https://ww1.microchip.com/downloads/en/devicedoc/41190c.pdf
    // page 19

    // enable interrupts
    // https://ww1.microchip.com/downloads/en/DeviceDoc/50002737C%20XC8%20C%20Compiler%20UG%20for%20PIC.pdf
    //ei();
}


void MRC_disable_interrupts(void) { }
void MRC_disable_sleep(void) { }
void MRC_enable_interrupts(void) { INTCON = 0b10001000; } // use ei() instead?

// this is a bit of a hack - even though the main function does
//    MRC_enable_interrupts()
//    MRC_enter_sleep_mode()
// something was happening that would result in the mcu working as expected a
// few times, then get "stuck" in sleep mode; my hunch is that code was being
// re-ordered, and the interrupt wasn't truly being enabled before going to
// sleep
void MRC_enter_sleep_mode(void) { INTCON = 0b10001000; SLEEP(); }

void MRC_led_pin_set_high(void) { GP0 = ON;   }
void MRC_led_pin_set_low(void)  { GP0 = OFF;  }
void MRC_led_toggle(void)       { GP0 = !GP0; }

void MRC_relay_coil_pin1_set_high(void) { GP5 = 1; } // GP5 == pin1
void MRC_relay_coil_pin1_set_low(void)  { GP5 = 0; }
void MRC_relay_coil_pin2_set_high(void) { GP4 = 1; } // GP4 == pin2
void MRC_relay_coil_pin2_set_low(void)  { GP4 = 0; }

uint8_t MRC_switch_pin_get_state(void) { return 0 == GP1 ? LOW : HIGH; }
void MRC_switch_pin_clear_int_flags(void) { }

// https://www.microforum.cc/topic/38-help-with-this-error-error-variable-has-incomplete-type-void/
// http://picforum.ric323.com/viewtopic.php?f=44&t=701
void __interrupt() ISR(void)
{
    INTCON = 0; // disable interrupts and clear interrupt flags - should we
                // use di() instead?
}

