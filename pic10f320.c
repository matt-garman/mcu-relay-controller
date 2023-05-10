

// pic10f320 pinout
//
//                       +----+
// N/C              pin1-|    |-pin8 RA3/~MCLR/VPP/IOC3
// VDD              pin2-|    |-pin7 VSS
// RA2/INT/IOC2     pin3-|    |-pin6 N/C
// ICSPCLK/RA1/IOC1 pin4-|    |-pin5 RA0/ICSPDAT/IOC0
//                       +----+
//
// Pin assignment:
//    pin1 => n/c
//    pin2 => +5v
//    pin3/RA2 => one side of relay coil
//    pin4/RA1 => other side of relay coil
//    pin5/RA0 => to gate of NPN which closes LED cathode to GND
//    pin6 => n/c
//    pin7 => GND
//    pin8/RA3 => to momentary normally-open SPST switch (switch press ties pin to GND)
//
// *** use pin4/RA1 for other side of relay coil
// *** RA3 is read-only
//
// Helpful series of blog posts on pic10f320 here:
//     https://jamiestarling.com/pic10f322-xc8-code-wpua-weak-pull-ups/
//
// Building:
// xc8-cc -Os -mcpu=10F320 sleepy-bypass.c

#include "mcu-relay-controller-iface.h"

#include "pic10f320.h"

#define STARTUP_DELAY_MS 72 // startup delay - FIXME - where 72ms comes from?


void MRC_hardware_init(void)
{
    __delay_ms(STARTUP_DELAY_MS);

    // option register
    // 7     6      5    4    3   2   1   0
    // ~GPPU INTEDG T0CS T0SE PSA PS2 PS1 PS0
    OPTION_REG = 0;

    // interrupt control register
    // 7   6    5      4    3     2      1    0
    // GIE PEIE RMR0IE INTE IOCIE TMR0IF INTF IOCIF   [10f320]
    // GIE PEIE T0IE   INTE GPIE  T0IF   INTF GPIF    [12f675]
    // GIE = global interrupt enable
    // IOCIE = interrupt-on-change interrupt enable
    INTCON = 0; // disable for now, enable in main loop

    CWG1CON0 = 0; // disable Complementary Waveform Generator (CWG)
    ANSELA = 0; // disable analog GPIO
    PORTA = 0b00001111; // PORTA registers - configure RA[0-3] as digital I/O - I think
    TRISA = 0; // PORTA tri-state register - TRISA[0-3]/RA[0-3] = output driver enabled
    LATA = 0b00000001; // PORTA DATA LATCH REGISTER - disable all output on GPIO pins
    WPUA = 0b00001000; // enable weak pull-up register for WPUA3/RA3
    IOCAP = 0; // disable interrupt-on-change (IOC) PORTA positive edge resistors for all pins
    IOCAN = 0b00001000; // IOCAN3 (RA3) OC PORTA negative edge resistor
    
    //OSCCON = 0b00110000; // 1 MHz
}

#define pic10f320_enable_interrupts() do { IOCAP=0; IOCAN=0b00001000; INTCON=0b10001000; } while(0)


void MRC_disable_interrupts(void) {  }
void MRC_disable_sleep(void) { }
void MRC_enable_interrupts(void) {  } // use ei() instead?

void MRC_enter_sleep_mode(void) { pic10f320_enable_interrupts(); SLEEP(); }

void MRC_led_pin_set_high(void) { RA0 = ON;   }
void MRC_led_pin_set_low(void)  { RA0 = OFF;  }
void MRC_led_toggle(void)       { RA0 = !RA0; }

void MRC_relay_coil_pin1_set_high(void) { RA2 = 1; } // RA2 == pin1
void MRC_relay_coil_pin1_set_low(void)  { RA2 = 0; }
void MRC_relay_coil_pin2_set_high(void) { RA1 = 1; } // RA1 == pin2
void MRC_relay_coil_pin2_set_low(void)  { RA1 = 0; }

uint8_t MRC_switch_pin_get_state(void)
{ 
    return ( (INTCON & 0b1) && (IOCAF & 0b00001000) && (0 == RA3) ) ? 0 : 1;
}

void MRC_switch_pin_clear_int_flags(void) { IOCAF = 0; }

void __interrupt() ISR(void)
{
    INTCON = 0;
}

