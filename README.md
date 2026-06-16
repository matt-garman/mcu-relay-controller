
# MCU Firmware for Switch Debounce and Electric Instrument Effects Switching

The project contains firmware for the ATtiny13a and ATtiny85
AVR-family microcontrollers.  The firmware is intended to be used
for electric instrument effects (e.g. guitar effect pedals) bypass
switching.  The firmware has four responsibilities:

    - Maintain state (engage/bypass)
    - Light or dark a status indicator LED
    - Respond to footswitch presses, *including debounce*
    - Control the actual signal switching mechanism

Fundamentally, the algorithm uses a saturating integrator to
debounce the footswitch and offer some EMI/RFI protection.

The firmware is bundled with an extensive test and validation suite.
the project's overall goal is to be reference-quality, suitable for
use in professional, touring-grade effects.

See the [Design Documentation](DESIGN_DOCUMENTATION.adoc) for the
complete firmware description and design details.


# Circuit-switching Hardware Support

The firmware currently supports circuit-switching via:

    - CD4053 or TMUX4053 electrical analog switches
    - Panasonic TQ-L2-5v mechanical relay

See the [Design Documentation](DESIGN_DOCUMENTATION.adoc) for the
control line specifics.  Note that it should be possible to use
other analog switches (e.g. DG413) or relays (e.g. Kemet EC2-3TNU).


# Quickstart

Requres avrtools, assumes a USBasp programmer, and a fresh ATtiny13a
chip:

```
make
make program
```

See TOOLCHAIN.md for full environmental details.  

