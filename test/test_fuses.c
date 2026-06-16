// Host-compiled fuse-byte decoder / verifier.
//
// WHY THIS EXISTS
// ---------------
// The firmware's correctness depends on fuse bytes that are configured OUTSIDE
// the C source (in the Makefile, written by avrdude). A wrong fuse byte does
// not show up in any simavr or golden-model test -- it only bites on real
// silicon (wrong clock => wrong debounce timing; BOD off => brown-out glitches;
// RSTDISBL set => bricked ISP). This test decodes the EXACT fuse bytes the
// Makefile will burn and asserts they match the documented design intent, so a
// fat-fingered fuse edit fails CI instead of a bench session.
//
// The fuse byte values are injected by the Makefile via -D so there is a single
// source of truth (the Makefile's LFUSE/HFUSE/LFUSE85/HFUSE85 variables):
//   -DT13_LFUSE=0x6a -DT13_HFUSE=0xfb -DT85_LFUSE=0x62 -DT85_HFUSE=0xdd
//
// Datasheet references:
//   ATtiny13A  rev. 8126F, "Fuse Bytes" (low/high byte bit maps)
//   ATtiny25/45/85 rev. 2586Q, "Fuse Bytes"
//
// AVR fuses are ACTIVE-LOW: a programmed (enabled) bit reads 0.

#include <stdint.h>
#include <stdio.h>

#ifndef T13_LFUSE
#  define T13_LFUSE 0x6a
#endif
#ifndef T13_HFUSE
#  define T13_HFUSE 0xfb
#endif
#ifndef T85_LFUSE
#  define T85_LFUSE 0x62
#endif
#ifndef T85_HFUSE
#  define T85_HFUSE 0xdd
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do {                                  \
    g_checks++;                                                \
    if (!(cond)) {                                             \
        g_failures++;                                          \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
        fprintf(stderr, __VA_ARGS__);                          \
        fprintf(stderr, "\n");                                 \
    }                                                          \
} while (0)

// Extract a contiguous bit field [lsb .. lsb+width-1] from a byte.
static unsigned field(unsigned byte, unsigned lsb, unsigned width) {
    return (byte >> lsb) & ((1u << width) - 1u);
}

//////////////////////////////////////////////////////////////////////////////
// ATtiny13A fuse map (datasheet 8126F; cross-checked against avr-libc
// iotn13a.h FUSE_* macros).
//
// LOW byte:
//   bit7 SPIEN      (0=enabled)      -- ISP programming
//   bit6 EESAVE     (0=preserve EEPROM on chip erase)
//   bit5 WDTON      (0=WDT always on)
//   bit4 CKDIV8     (0=enabled -> /8)
//   bit3 SUT1
//   bit2 SUT0
//   bit1 CKSEL1
//   bit0 CKSEL0
//   (CKSEL[1:0]=10 -> 9.6 MHz internal RC; SUT[1:0]=10 -> 14CK + 64ms)
//
// HIGH byte (NOTE: layout differs from the ATtiny85!):
//   bit7..bit5 = 1 (reserved, read 1)
//   bit4 SELFPRGEN (1=disabled)
//   bit3 DWEN      (1=disabled -> PB5 stays RESET/ISP)
//   bit2 BODLEVEL1
//   bit1 BODLEVEL0
//   bit0 RSTDISBL  (1=external RESET enabled -> PB5 stays RESET/ISP)
//   BODLEVEL[1:0] (bit2,bit1): 11=BOD off, 10=1.8V, 01=2.7V, 00=4.3V
//////////////////////////////////////////////////////////////////////////////
static void verify_t13(void) {
    unsigned lo = (unsigned)T13_LFUSE;
    unsigned hi = (unsigned)T13_HFUSE;

    printf("  ATtiny13a: lfuse=0x%02x hfuse=0x%02x\n", lo, hi);

    // --- LOW byte ---
    CHECK(field(lo, 7, 1) == 0, "t13 SPIEN must be enabled (0) to keep ISP; lfuse bit7=%u", field(lo,7,1));
    CHECK(field(lo, 5, 1) == 0, "t13 WDTON must be 0 (WDT forced always-on, cannot be disabled by software); lfuse bit5=%u", field(lo,5,1));
    CHECK(field(lo, 4, 1) == 0, "t13 CKDIV8 must be enabled (0) for 1.2MHz; lfuse bit4=%u", field(lo,4,1));
    CHECK(field(lo, 0, 2) == 0x2, "t13 CKSEL[1:0] must be 0b10 (9.6MHz int RC); got 0b%u%u",
          field(lo,1,1), field(lo,0,1));
    CHECK(field(lo, 2, 2) == 0x2, "t13 SUT[1:0] must be 0b10 (14CK+64ms, stable LDO ramp); got 0b%u%u",
          field(lo,3,1), field(lo,2,1));

    // --- HIGH byte ---
    CHECK(field(hi, 5, 3) == 0x7, "t13 hfuse bits 7:5 reserved, should read 1; got 0x%x", field(hi,5,3));
    CHECK(field(hi, 4, 1) == 1, "t13 SELFPRGEN must be disabled (1); hfuse bit4=%u", field(hi,4,1));
    CHECK(field(hi, 3, 1) == 1, "t13 DWEN must be disabled (1) so PB5 stays RESET/ISP; hfuse bit3=%u", field(hi,3,1));
    CHECK(field(hi, 0, 1) == 1, "t13 RSTDISBL must be 1 (external RESET kept, ISP preserved); hfuse bit0=%u", field(hi,0,1));
    // BODLEVEL[1:0] = (bit2,bit1). 0b01 == 2.7V on the ATtiny13A.
    CHECK(field(hi, 1, 2) == 0x1, "t13 BODLEVEL[1:0] must be 0b01 (2.7V); got 0b%u%u",
          field(hi,2,1), field(hi,1,1));
}

//////////////////////////////////////////////////////////////////////////////
// ATtiny85 fuse map (2586Q)
//
// LOW byte:
//   bit7 CKDIV8 (0=enabled -> /8)
//   bit6 CKOUT  (1=disabled)
//   bit5 SUT1
//   bit4 SUT0
//   bit3..bit0 CKSEL[3:0]  (0010 -> 8 MHz internal RC)
//   (SUT[1:0]=10 -> 14CK + 64ms with the int-RC range)
//
// HIGH byte:
//   bit7 RSTDISBL  (1=disabled -> PB5 stays RESET)
//   bit6 DWEN      (1=disabled)
//   bit5 SPIEN     (0=enabled)
//   bit4 WDTON     (1=WDT not forced on)
//   bit3 EESAVE    (1=don't preserve EEPROM)
//   bit2 BODLEVEL2
//   bit1 BODLEVEL1
//   bit0 BODLEVEL0
//   (BODLEVEL[2:0]=101 -> 2.7V)
//////////////////////////////////////////////////////////////////////////////
static void verify_t85(void) {
    unsigned lo = (unsigned)T85_LFUSE;
    unsigned hi = (unsigned)T85_HFUSE;

    printf("  ATtiny85:  lfuse=0x%02x hfuse=0x%02x\n", lo, hi);

    // --- LOW byte ---
    CHECK(field(lo, 7, 1) == 0, "t85 CKDIV8 must be enabled (0) for 1.0MHz; lfuse bit7=%u", field(lo,7,1));
    CHECK(field(lo, 6, 1) == 1, "t85 CKOUT should be disabled (1); lfuse bit6=%u", field(lo,6,1));
    CHECK(field(lo, 0, 4) == 0x2, "t85 CKSEL[3:0] must be 0b0010 (8MHz int RC); got 0x%x", field(lo,0,4));
    CHECK(field(lo, 4, 2) == 0x2, "t85 SUT[1:0] must be 0b10 (14CK+64ms); got 0b%u%u",
          field(lo,5,1), field(lo,4,1));

    // --- HIGH byte ---
    CHECK(field(hi, 7, 1) == 1, "t85 RSTDISBL must be 1 (PB5 stays RESET, keep ISP); hfuse bit7=%u", field(hi,7,1));
    CHECK(field(hi, 6, 1) == 1, "t85 DWEN must be disabled (1); hfuse bit6=%u", field(hi,6,1));
    CHECK(field(hi, 5, 1) == 0, "t85 SPIEN must be enabled (0) to keep ISP; hfuse bit5=%u", field(hi,5,1));
    CHECK(field(hi, 4, 1) == 0, "t85 WDTON must be 0 (WDT forced always-on, cannot be disabled by software); hfuse bit4=%u", field(hi,4,1));
    CHECK(field(hi, 0, 3) == 0x5, "t85 BODLEVEL[2:0] must be 0b101 (2.7V); got 0x%x", field(hi,0,3));
}

int main(void) {
    printf("fuse-byte verification:\n");
    verify_t13();
    verify_t85();

    // -------------------------------------------------------------------------
    // CRITICAL CROSS-CHECK: the design spec (the design doc / bypass_core.c header)
    // states "enable brown-out detection (BOD) at 2.7V". Verify BOTH parts
    // actually encode 2.7V BOD, since a wrong BODLEVEL is invisible to every
    // other test (it only bites as brown-out glitches on real silicon).
    //
    //   ATtiny13a: hfuse BODLEVEL[1:0] = (bit2,bit1); 0b01 == 2.7V.
    //   ATtiny85:  hfuse BODLEVEL[2:0] = (bit2,bit1,bit0); 0b101 == 2.7V.
    // -------------------------------------------------------------------------
    {
        unsigned t13_bodlevel = field((unsigned)T13_HFUSE, 1, 2);
        CHECK(t13_bodlevel == 0x1,
              "DESIGN INTENT: ATtiny13a BOD must be 2.7V (BODLEVEL=0b01). "
              "Configured hfuse=0x%02x has BODLEVEL=0b%u%u",
              (unsigned)T13_HFUSE, field((unsigned)T13_HFUSE,2,1), field((unsigned)T13_HFUSE,1,1));

        unsigned t85_bodlevel = field((unsigned)T85_HFUSE, 0, 3);
        CHECK(t85_bodlevel == 0x5,
              "DESIGN INTENT: ATtiny85 BOD must be 2.7V (BODLEVEL=0b101). "
              "Configured hfuse=0x%02x has BODLEVEL=0x%x",
              (unsigned)T85_HFUSE, t85_bodlevel);
    }

    printf("fuse checks: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
