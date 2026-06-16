#ifndef BYPASS_CONFIG_H__
#define BYPASS_CONFIG_H__

// Timer0 CTC config for a 1ms tick:
//   ATtiny13/a (F_CPU=1.2MHz): 8 * (149 + 1) / 1200000  -> exactly 1ms
//   ATtiny85   (F_CPU=1.0MHz): 8 * (124 + 1) / 1000000  -> exactly 1ms
#if defined(__AVR_ATtiny85__)
#  define TIMER0_OCR0A_1MS (124)
#else // ATtiny13a
#  define TIMER0_OCR0A_1MS (149)
#endif


// - core design assumes a 1ms timer interrupt (within the bounds of the MCU's
//   built-in RC oscillator precision)
// - timing is therefore dependent on MCU operating speed; timer math based on
//   the CPU clock rate (1.2MHz for ATtiny13/a, 1.0MHz for ATtiny85)
#if defined(__AVR_ATtiny85__)
#  if !defined(F_CPU)
#    error "F_CPU must be defined via build flags, e.g. -DF_CPU=1000000UL"
#  endif
#  if (F_CPU != 1000000UL)
#    error "F_CPU must be 1000000 for ATtiny85 (CKDIV8 enabled, 8MHz internal RC)"
#  endif
// ATtiny85 uses TIMSK, not TIMSK0; alias for shared code (in C file)
#  define TIMSK0 TIMSK
// ATtiny85 uses TIFR, not TIFR0
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
