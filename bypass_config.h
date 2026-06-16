// Copyright (c) Matthew Garman.  All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for
// license information.

#ifndef BYPASS_CONFIG_H__
#define BYPASS_CONFIG_H__

// The ATtiny25/45/85 ("tinyx5" family) are register- and clock-compatible with
// one another: same 8MHz internal RC (1.0MHz via CKDIV8), same Timer0, and the
// same TIMSK/TIFR register names. They differ only in flash/RAM size, which the
// firmware does not depend on. So they are handled identically here, distinct
// only from the ATtiny13/a (1.2MHz, TIMSK0/TIFR0).
#if defined(__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
#  define BYPASS_MCU_TINYX5 1
#endif

// Timer0 CTC config for a 1ms tick:
//   ATtiny13/a    (F_CPU=1.2MHz): 8 * (149 + 1) / 1200000  -> exactly 1ms
//   ATtiny25/45/85 (F_CPU=1.0MHz): 8 * (124 + 1) / 1000000  -> exactly 1ms
#if defined(BYPASS_MCU_TINYX5)
#  define TIMER0_OCR0A_1MS (124)
#else // ATtiny13a
#  define TIMER0_OCR0A_1MS (149)
#endif


// - core design assumes a 1ms timer interrupt (within the bounds of the MCU's
//   built-in RC oscillator precision)
// - timing is therefore dependent on MCU operating speed; timer math based on
//   the CPU clock rate (1.2MHz for ATtiny13/a, 1.0MHz for ATtiny25/45/85)
#if defined(BYPASS_MCU_TINYX5)
#  if !defined(F_CPU)
#    error "F_CPU must be defined via build flags, e.g. -DF_CPU=1000000UL"
#  endif
#  if (F_CPU != 1000000UL)
#    error "F_CPU must be 1000000 for ATtiny25/45/85 (CKDIV8 enabled, 8MHz internal RC)"
#  endif
// the tinyx5 family uses TIMSK, not TIMSK0; alias for shared code (in C file)
#  define TIMSK0 TIMSK
// the tinyx5 family uses TIFR, not TIFR0
#  define TIFR0 TIFR
#else
#  if !defined(F_CPU)
#    error "F_CPU must be defined via build flags, e.g. -DF_CPU=1200000UL"
#  endif
#  if (F_CPU != 1200000UL)
#    error "F_CPU must be 1200000 for ATtiny13/a"
#  endif
#endif



// number of HIGH PB0/footswitch pin reads to be considered 
// release-debounced, i.e. the "lock-out" period
#define RELEASE_THRESH (25)

// number of LOW PB0/footswitch pin reads to be considered
// press-debounced
//
// trying to balance between "responds immediately" and "immune to
// spurious interrupts": we can't readily distinguish between
// environmental noise (that we want to filter/ignore as entirely)
// versus noise from an old/bouncy switch.
//
// note also that PRESSED_THRESH is the *minimum* time to confirmed
// press-debounce: a noisy switch or environmental EMI/RFI could
// increase the time to confirmed press-debounce
//
// the asymmetry between PRESSED_THRESH and RELEASE_THRESH is
// to bias the debounce time after the actual action (effect
// engage/bypass) to balance responsiveness with robust switch
// de-bouncing
#define PRESSED_THRESH (8)


#endif // BYPASS_CONFIG_H__
