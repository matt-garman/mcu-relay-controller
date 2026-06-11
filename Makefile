
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

# Host (PC) compiler for the test suite.
HOSTCC      ?= cc
HOST_CFLAGS  = -std=c11 -Wall -Wextra -Werror
SIMAVR_INC  ?= /usr/include/simavr
SIM_CFLAGS   = -std=c11 -Wall -Wextra -I$(SIMAVR_INC)
SIM_LIBS     = -lsimavr -lelf

CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os \
          -fshort-enums -funsigned-char \
          -ffunction-sections -fdata-sections \
          -Werror -Wall -Wextra -std=c11

LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

.PHONY: all clean size readfuses fuses flash program test test-host test-sim trace

all: $(TARGET).hex size

$(TARGET).elf: $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(TARGET).elf
	$(SIZE) --mcu=$(MCU) -C $<

clean:
	rm -f $(TARGET).elf $(TARGET).hex \
		test/test_logic_host test/test_sim test/test_trace \
		bypass_trace.vcd

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


# --- Tests -----------------------------------------------------------------

# Run all pre-hardware tests: host golden-model + simavr firmware sim.
test: test-host test-sim

# Host-compiled golden-model unit tests (no AVR; fast logic verification).
test-host: test/test_logic_host
	./test/test_logic_host

test/test_logic_host: test/test_logic_host.c
	$(HOSTCC) $(HOST_CFLAGS) $< -o $@

# simavr integration tests: run the REAL firmware .elf in the simulator,
# drive PB0, assert PB1/PB2. Depends on the built firmware.
test-sim: test/test_sim
	./test/test_sim

test/test_sim: test/test_sim.c $(TARGET).elf
	$(HOSTCC) $(SIM_CFLAGS) $< -o $@ $(SIM_LIBS)

# Generate a GTKWave-viewable waveform of PB0/PB1/PB2 (bypass_trace.vcd).
# Build a TRACE-enabled variant of the sim harness and run it.
trace: test/test_trace
	./test/test_trace
	@echo "View with: gtkwave bypass_trace.vcd"

test/test_trace: test/test_sim.c $(TARGET).elf
	$(HOSTCC) $(SIM_CFLAGS) -DTRACE $< -o $@ $(SIM_LIBS)


# vim: tw=0 nowrap
