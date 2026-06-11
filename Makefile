
MCU      = attiny13a
F_CPU    = 1200000UL
TARGET   = attiny13_bypass
CC       = avr-gcc
OBJCOPY  = avr-objcopy
SIZE     = avr-size
AVRDUDE  = avrdude

# Programmer settings.
# PROGRAMMER: "51 AVR USB ISP ASP" dongle is a USBasp clone -> usbasp.
# AVRDUDE_PART: avrdude's short name for the ATtiny13/13a.
# Override on the command line if needed, e.g.:
#   make flash PROGRAMMER=usbtiny
PROGRAMMER   ?= usbasp
AVRDUDE_PART ?= t13

# Fuse bytes for this design (verified bit-by-bit; see attiny13_bypass.c header):
#   lfuse=0x6A : SPIEN on, CKDIV8 on (1.2MHz), SUT=14CK+64ms, int 9.6MHz RC
#   hfuse=0xFB : 2.7V brown-out detection enabled; RSTDISBL/DWEN left safe
LFUSE = 0x6a
HFUSE = 0xfb

AVRDUDE_FLAGS = -c $(PROGRAMMER) -p $(AVRDUDE_PART)

CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os \
          -fshort-enums -funsigned-char \
          -ffunction-sections -fdata-sections \
          -Werror -Wall -Wextra -std=c11

LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

.PHONY: all clean size readfuses fuses flash program

all: $(TARGET).hex size

$(TARGET).elf: $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(TARGET).elf
	$(SIZE) --mcu=$(MCU) -C $<

clean:
	rm -f $(TARGET).elf $(TARGET).hex

# Read and print the currently programmed fuse bytes (safe, read-only).
# Run this first to record the chip's existing fuses before changing them.
readfuses:
	$(AVRDUDE) $(AVRDUDE_FLAGS) -U lfuse:r:-:h -U hfuse:r:-:h

# Write the design's fuse bytes. Safe: does not touch RSTDISBL/DWEN, so
# ISP access is preserved. Verify before relying on a board in the field.
fuses:
	$(AVRDUDE) $(AVRDUDE_FLAGS) \
		-U lfuse:w:$(LFUSE):m \
		-U hfuse:w:$(HFUSE):m

# Flash the firmware image to the MCU.
flash: $(TARGET).hex
	$(AVRDUDE) $(AVRDUDE_FLAGS) -U flash:w:$(TARGET).hex:i

# Convenience: set fuses, then flash firmware. Use for a fresh chip.
program: fuses flash


# vim: tw=0 nowrap
