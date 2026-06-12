
MCU      = attiny13a
F_CPU    = 1200000UL
TARGET   = attiny13_bypass
CC       = avr-gcc
OBJCOPY  = avr-objcopy
SIZE     = avr-size
AVRDUDE  = avrdude

# ATtiny85 variant: same firmware source, different MCU/flags
MCU85      = attiny85
F_CPU85    = 1000000UL
TARGET85   = attiny13_bypass_t85

# Programmer settings.
# PROGRAMMER: "51 AVR USB ISP ASP" dongle is a USBasp clone -> usbasp.
# AVRDUDE_PART: avrdude's short name for the ATtiny13/13a.
# Override on the command line if needed, e.g.:
#   make flash PROGRAMMER=usbtiny
PROGRAMMER   ?= usbasp
AVRDUDE_PART   ?= t13
AVRDUDE_PART85 ?= t85

# Fuse bytes for this design (verified bit-by-bit; see attiny13_bypass.c header):
#   lfuse=0x6A : SPIEN on, CKDIV8 on (1.2MHz), SUT=14CK+64ms, int 9.6MHz RC
#   hfuse=0xFB : 2.7V brown-out detection enabled; RSTDISBL/DWEN left safe
LFUSE = 0x6a
HFUSE = 0xfb

# ATtiny85 fuse bytes:
#   lfuse=0x62 : CKDIV8 on (1.0MHz), CKOUT off, SUT=14CK+64ms, int 8MHz RC
#   hfuse=0xDA : 2.7V BOD, SPIEN on, RSTDISBL/DWEN/WDTON/EESAVE safe
LFUSE85 = 0x62
HFUSE85 = 0xdd

AVRDUDE_FLAGS = -c $(PROGRAMMER) -p $(AVRDUDE_PART)

# Host (PC) compiler for the test suite.
HOSTCC      ?= cc
HOST_CFLAGS  = -std=c11 -Wall -Wextra -Werror
SIMAVR_INC  ?= /usr/include/simavr
SIM_CFLAGS   = -std=c11 -Wall -Wextra -I$(SIMAVR_INC)
SIM_LIBS     = -lsimavr -lelf

AVR_IO_HEADER      := $(shell $(CC) -print-file-name=avr/io.h)
AVR_LIBC_INCLUDE   := $(patsubst %/avr/, %, $(dir $(AVR_IO_HEADER)))
AVR_GCC_INCLUDE    := $(shell $(CC) -print-file-name=include)
AVR_ARCH           := $(shell $(CC) -mmcu=$(MCU) -dM -E - < /dev/null | awk '/__AVR_ARCH__/ { print $$3; exit }')
CLANG_TIDY_FLAGS   ?= -target avr -mmcu=$(MCU) -DF_CPU=$(F_CPU) -D__AVR__ -D__AVR_ATtiny13A__ \
                      -D__AVR_DEVICE_NAME__=$(MCU) $(if $(AVR_ARCH),-D__AVR_ARCH__=$(AVR_ARCH)) \
                      -D__AVR_HAVE_PRR_PRTIM0 \
                      -Wno-macro-redefined \
                      $(if $(AVR_LIBC_INCLUDE),-I$(AVR_LIBC_INCLUDE)) \
                      $(if $(AVR_GCC_INCLUDE),-I$(AVR_GCC_INCLUDE))
ANALYZE_CMD        ?= clang-tidy $(TARGET).c -- $(CLANG_TIDY_FLAGS)

CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os \
          -fshort-enums -funsigned-char \
          -ffunction-sections -fdata-sections \
          -Werror -Wall -Wextra -std=c11

LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

.PHONY: all clean size readfuses fuses flash program test test-host test-sim test-sim-t85 trace analyze \
        size85 fuses85 flash85 program85

all: $(TARGET).hex size

$(TARGET).elf: $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(TARGET).elf
	$(SIZE) --mcu=$(MCU) -C $<

# --- ATtiny85 variant --------------------------------------------------------

$(TARGET85).elf: $(TARGET).c
	$(CC) -mmcu=$(MCU85) -DF_CPU=$(F_CPU85) -Os \
		-fshort-enums -funsigned-char \
		-ffunction-sections -fdata-sections \
		-Werror -Wall -Wextra -std=c11 \
		-Wl,--gc-sections -o $@ $<

$(TARGET85).hex: $(TARGET85).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size85: $(TARGET85).elf
	$(SIZE) --mcu=$(MCU85) -C $<

fuses85:
	$(AVRDUDE) -c $(PROGRAMMER) -p $(AVRDUDE_PART85) \
		-U lfuse:w:$(LFUSE85):m \
		-U hfuse:w:$(HFUSE85):m

flash85: $(TARGET85).hex
	$(AVRDUDE) -c $(PROGRAMMER) -p $(AVRDUDE_PART85) -U flash:w:$(TARGET85).hex:i

program85: fuses85 flash85

# --- Clean ------------------------------------------------------------------

clean:
	rm -f $(TARGET).elf $(TARGET).hex \
		$(TARGET85).elf $(TARGET85).hex \
		test/test_logic_host test/test_sim test/test_sim_t85 test/test_trace \
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

# Run all pre-hardware tests: host golden-model + simavr firmware sim (both MCUs).
test: test-host test-sim test-sim-t85

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

# ATtiny85 simavr integration tests: same test harness, different MCU and firmware.
test-sim-t85: test/test_sim_t85
	./test/test_sim_t85

test/test_sim_t85: test/test_sim.c $(TARGET85).elf
	$(HOSTCC) $(SIM_CFLAGS) \
		-DFW_PATH=\"$(TARGET85).elf\" \
		-DMCU_NAME=\"attiny85\" \
		-DF_CPU_HZ=1000000UL \
		-DTARGET_T85 \
		$< -o $@ $(SIM_LIBS)

# Generate a GTKWave-viewable waveform of PB0/PB1/PB2 (bypass_trace.vcd).
# Build a TRACE-enabled variant of the sim harness and run it.
trace: test/test_trace
	./test/test_trace
	@echo "View with: gtkwave bypass_trace.vcd"

test/test_trace: test/test_sim.c $(TARGET).elf
	$(HOSTCC) $(SIM_CFLAGS) -DTRACE $< -o $@ $(SIM_LIBS)

# Static analysis (default: avr-gcc -fanalyzer). Override ANALYZE_CC / ANALYZE_FLAGS as needed.
analyze: $(TARGET).c
	@cmd=$(word 1,$(ANALYZE_CMD)); \
	if command -v $$cmd >/dev/null 2>&1; then \
		$(ANALYZE_CMD); \
	elif $(CC) -fsyntax-only -fanalyzer -xc /dev/null >/dev/null 2>&1; then \
		$(CC) $(CFLAGS) -fanalyzer -c $< -o $(TARGET).analyze.o; \
		rm -f $(TARGET).analyze.o; \
	else \
		echo "No static analysis tool available. Set ANALYZE_CMD=... or install clang-tidy / avr-gcc with -fanalyzer."; \
		exit 1; \
	fi


# vim: tw=0 nowrap
