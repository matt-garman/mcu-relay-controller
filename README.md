
# README

## Overview

mcu-relay-bypass is intended to be a very simple framework and reference
implementation of suitable code for programming microcontrollers to respond to
momentary footswitch activity, and in turn power a relay coil.  The presumed
application is to toggle a musical effect (particularly guitar effect pedals)
between bypass and engaged modes.  As a reference implementation, the goal is
to provide a plug-and-play program for DIY effects enthusiasts, for a handful
of cheap and readily-available microcontrollers and relays.  As a framework,
the goal is to make it easy to add features and support for more
microcontrollers and relays.

This code attempts to expand on examples available on the web by having low
power consumption as a top-priority goal.  There exist examples of using the
NE555 timer (instead of an MCU) to respond to momentary footswitch action and
drive a relay.  These are simple and reliable circuits, but the NE555 uses a
non-trivial amount of current.  (There exist CMOS versions of the NE555, e.g.
LMC555, TLC555, TS555, but these are still relatively high-current compared to
an MCU in sleep mode.)


## Building and Programming

For the AVR MCUs, I am using a "usbtiny" programmer, for example,
[PGM-11801](https://www.sparkfun.com/products/11801).  I have added
pre-built hex image files to the "images" folder, there's no need to
compile if you just want to program your MCU.

- ATtiny85
```
avr-gcc -Os -std=gnu99 -DF_CPU=1000000UL -DIMPL_ATTINY -mmcu=attiny85 -o attiny85.elf mcu-relay-controller.c attiny.c
avr-objcopy -j .text -j .data -O ihex attiny85.elf attiny85.hex
avrdude -c usbtiny -p attiny85 -v -P usb -U flash:w:attiny85.hex
```
- ATtiny13A - essentially the same as ATtiny85, but with the obvious
  tool option changes and the additional define -DATTINY13

```
avr-gcc -Os -std=gnu99 -DF_CPU=1000000UL -DIMPL_ATTINY -DATTINY13 -mmcu=attiny13 -o attiny13.elf mcu-relay-controller.c attiny.c
avr-objcopy -j .text -j .data -O ihex attiny13.elf attiny13.hex
avrdude -c usbtiny -p attiny13 -v -P usb -U flash:w:attiny13.hex
```


## Current State

Currently the project has, as a starting point, basic relay bypass
implementations for a few microcontrollers.  These compile, and work
in basic testing.

Currently supported microcontrollers:

- ATmel ATTiny85
- ATmel ATTiny13A
- Microchip PIC12F675
- Microchip PIC10320

Microcontrollers that *probably* work with little-to-no modification of the
code:

- ATTiny13 - appears to be same as ATTiny13 but older manufacturing
  process
- ATTiny45 - appears to be same as ATTiny85 but with less memory
- PIC10F322 - essentially same as 10F320
- PIC12F629 - appears to be same as PIC12F675 but without 10-bit A/D
  converter
- PIC12F508, PIC12F609 - other possible alternatives to PIC12F675

Currently supported relays:

- Takamisawa AL5WN-K
- Panasonic TQ2-L-5V


## Which MCU Should I Choose?

As always, "it depends".  Ultimately, for this particular application, it
doesn't really matter, because the program is so simple.  Obviously, if you
are familiar with microcontroller development and programming, then choose the
one that you are familiar with, or the one that is closest to your familiar
MCU.

If this is your first introduction to microcontrollers, then I think the two
main considerations are (1) cost and (2) ease-of-use.  As for cost, at the
time of this writing, the PIC10320 wins, being $0.82 each.  The next-cheapest
is the ATTiny13A, at $1.13 each.  (These are through-hole prices,
surface-mount versions tend to be cheaper.)  However, I personally prefer the
ATtiny family because:

- there exists a great open-source toolchain,
  [avr tools](https://www.nongnu.org/avr-libc/user-manual/overview.html)
  for the ATmel AVR family of microcontrollers
- a free compiler exists for PIC microcontrollers, but it is proprietary,
  closed-source, and feature-limited (unless a license is purchased)
- programmers for ATmel AVR devices appear to be cheaper
- though not used in this project, the ATtiny MCUs can use the Arduino
  libraries (which should speak for itself in terms of community support
  and enthusiasm)


## Program Concept

While there are many examples of using an MCU to drive a relay, I was unable
to find any examples that took power consumption into consideration.  With low
current draw as a goal, the program is thefore designed around a *latching*
relay, as non-latching relays require a constant current draw in one of the
states (latching relays only require active power for state change).
Furthermore, this code assumes a latching relay with a single coil, that is,
one where current direction is reveresed to change relay state (there also
exist relays that have two coils, one for each state).

In pseudocode, the program works like this:

```
    1. initialize hardware
       a. disable unused portions of the MCU
       b. configure pins for digital I/O and read or write as appropriate
       c. configure sleep mode
       d. other device-specific hardware initialization
    2. set initial relay and status LED state
    3. for the momentary-switch connected pin,
       instruct MCU to generate an interrupt on pin change
    4. put MCU in sleep mode (MCU will wake on pin change interrupt)
    5. when MCU is awakened by pin change interrupt:
       a. disable interrupts
       b. flip change relay state
       c. flip status LED state
       d. re-enable interrupts
       e. put MCU in sleep mode
```

In practice, the actual code almost matches the pseudocode 1:1, and (without
comments) is shorter than this README!


## Implementation Considerations

In addition to low-power consumption, another primary goal of this project is
a *simple*, plug-and-play implementation.  The goal is to be as simple as
possible, but without sacrificing reliability or longevity.  Some open
questions/considerations therefore:

1. Should we better isolate the MCU from the relay coil?  The concerns here are (1) using the MCU as a current
   source/sink when powering the coil, and (2) damage to the MCU pins from the
   coil's collapsing field after activation.  *Answer: almost certainly, yes;
   see [this](https://electronics.stackexchange.com/questions/666328/should-i-protect-against-collapsing-field-effects-of-a-microcontroller-driven-sm).*
2. Are the internal pullup/pulldown resistors sufficient for our purposes?
3. The reference PCB implements a basic RF filter on the line between switch
   and MCU; is this neccessary?  If so, is it sufficient?
4. Should we worry about de-bouncing the switch?  Or is the RF filter plus
   wake-on-interrupt sufficient for our purposes?
5. 4.5v relays are probably better suited to this project (as opposed to 5v
   relays), as most (likely all) MCUs will experience a voltage drop at the
   current required to drive a relay.
6. Consider using two-coil latching relays, as they may be more easily
   decoupled from the MCU.


## Future Goals

1. Verified predictability and reliabilty from extensive field-testing
2. Peer-review of the code base
3. Add support for muting during relay state transition (see BYOC relay bypass
   or [this post](https://www.diystompboxes.com/smfforum/index.php?topic=118021.msg1263909#msg1263909))
4. Add support for two-coil relays (note this allows for simpler de-coupling
   of MCU and relay; see BYOC relay bypass)
5. Add support for non-latching relays
6. Generalize PCB with jumpers to be MCU agnostic
7. Add support for fancier UI features, such as momentary-on, double-tap
   support, etc
8. Implement scheme for having guaranteed user-defined power-off state (e.g.
   device always reverts to bypass on power loss, maybe use MCU watchdog or
   brownout detector)


## Effect Pedal Bypass Manifesto

In my opinion, the following is a list of requirements for an effect pedal
bypass switching scheme:

1.  Reliable
2.  Predictable/deterministic
3.  Low power consumption
4.  Wide input voltage (9-18VDC)
5.  Low cost
6.  Low part count
7.  Easy implementation
8.  Uses commodity, current-production, readily available parts
9.  No unwanted noise (e.g. pops/clicks) when switching
10. Easily adaptable to true bypass or buffered bypass
11. Easy integration into effect PCB to minimize signal wire
    length
12. Uses engineering best practices: no exploiting of undefined
    behavior, all logic and operation should be deterministic,
    all components should be used within their design tolerances


## Credits and Inspiration

1. My favorite guitar effect PCB suppliers, for providing great PCBs and
   fostering my continued love of DIY electronics:
    - [PedalPCB](https://www.pedalpcb.com/)
    - [Aion](https://www.aionfx.com/)
    - [Madbean](https://www.madbeanpedals.com/)
    - [Build Your Own Clone](https://buildyourownclone.myshopify.com/) - note BYOC sells a 
      [relay bypass board](https://buildyourownclone.myshopify.com/products/relay-bypass-board)
      that uses a two-coil relay, decouples the MCU (PIC12F609) from the relay
      via NPN transistors, and uses a pair of BS170 MOSFETs for muting during
      relay state change
2. This classic article:
   [Coda Effect: Relay Bypass: Conception and Relay Bypass Code](https://www.coda-effects.com/2016/04/relay-bypass-conception-and-relay.html)
3. This wonderful series from
   [Amplified Parts](https://www.amplifiedparts.com)
   on Relay True Bypass Switching:
    - [Relay True Bypass Switching Part 1: Relay Basics and Stereo Guitar Pedals](https://www.amplifiedparts.com/tech-articles/relay-true-bypass-switching-1)
    - [Relay True Bypass Switching Part 2: Momentary and Soft Touch Switches](https://www.amplifiedparts.com/tech-articles/relay-true-bypass-switching-2)
    - [Relay True Bypass Switching Part 3: Microcontrollers and Current Savings](https://www.amplifiedparts.com/tech-articles/relay-true-bypass-switching-3)
    - [Relay True Bypass Switching Part 4: Smart Switching](https://www.amplifiedparts.com/tech-articles/relay-true-bypass-switching-4)
4. [mstratman's relay-bypass project on GitHub](https://github.com/mstratman/relay-bypass)
    - This code is available as a product for MAS Effects, see 
      [MAS Effects Relay Bypass](https://mas-effects.com/relay-bypass/)
5. [ToyKeeper's Anduril Flashlight Firmware](https://launchpad.net/flashlight-firmware)
    - The Anduril project is everything this project aims to be:
      well-engineered, open-source, and above all incredibly useful
      microcontroller software.
6. mictester's circuit and discussion thread on freestompboxes:
   [A Switching Scheme](https://www.freestompboxes.org/viewtopic.php?t=31684)
7. [Latching relay driver with CMOS - difference between these two circuits?](https://www.diystompboxes.com/smfforum/index.php?topic=118021.0)
   discussion on DIYstompboxes
8. [diyAudio](https://www.diyaudio.com/) - where I got my start in DIY audio electronics
9. [The Best Switch Debounce Routine Ever](https://drmarty.blogspot.com/2009/05/best-switch-debounce-routine-ever.html)
10. Electrical Engineering StackExchange [Should I protect against collapsing field effects of a microcontroller-driven small-signal relay coil?](https://electronics.stackexchange.com/questions/666328/should-i-protect-against-collapsing-field-effects-of-a-microcontroller-driven-sm) - suggests 

