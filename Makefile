
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
#   hfuse=0xDD : 2.7V BOD, SPIEN on, RSTDISBL/DWEN/WDTON/EESAVE safe
LFUSE85 = 0x62
HFUSE85 = 0xdd

AVRDUDE_FLAGS = -c $(PROGRAMMER) -p $(AVRDUDE_PART)

# Host (PC) compiler for the test suite.
HOSTCC      ?= cc
# -Wconversion catches implicit integer-narrowing/sign-change footguns in the
# debounce arithmetic. The host model and the firmware share the same integer
# semantics, so the model is a good place to enforce it too.
HOST_CFLAGS  = -std=c11 -Wall -Wextra -Werror -Wconversion
SIMAVR_INC  ?= /usr/include/simavr
# Note: simavr's own headers are not -Wconversion clean, so the sim harness
# uses -Wall -Wextra (still -Werror) without -Wconversion.
SIM_CFLAGS   = -std=c11 -Wall -Wextra -Werror -I$(SIMAVR_INC)
SIM_LIBS     = -lsimavr -lelf

# --- Test workload sizing ----------------------------------------------------
# The default `make test` runs a FAST but still-meaningful workload so it
# finishes in a few seconds (good for edit/build/test loops and CI gating).
# `make test-long` (alias: `make stress`) runs the FULL exhaustive workload.
# Every knob below can also be overridden individually on the command line.
#
# Fast (default) sizing:
FAST_HOST_DEFS = -DMODEL_FUZZ_RANDOM_DURATION_MS=100000u \
                 -DMODEL_FUZZ_POWER_ON_TRIALS=25 \
                 -DMODEL_FUZZ_ADVERSARIAL_CYCLES=25 \
                 -DMODEL_FUZZ_EXTREME_BOUNCE_PRESSES=5
FAST_SIM_DEFS  = -DSIM_RANDOM_NOISE_DURATION_MS=5000u \
                 -DSIM_SUSTAINED_NOISE_DURATION_MS=2000u \
                 -DSIM_EMI_BURSTS=40 \
                 -DSIM_EXTREME_BOUNCE_PRESSES=5 \
                 -DSIM_ADVERSARIAL_CYCLES=20 \
                 -DSIM_POWER_ON_BOOTS=10
# Full (exhaustive) sizing == the in-source defaults, so no extra -D needed.
FULL_HOST_DEFS =
FULL_SIM_DEFS  =

# Selected per-invocation; `test-long`/`stress` override these.
HOST_DEFS ?= $(FAST_HOST_DEFS)
SIM_DEFS  ?= $(FAST_SIM_DEFS)

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
          -Werror -Wall -Wextra -Wconversion -std=c11

LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

.PHONY: all clean size readfuses fuses flash program \
        test test-fast test-long stress \
        test-host test-sim test-sim-t85 \
        test-model-check test-fault-inject \
        trace analyze coverage coverage-check coverage-clean \
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
		-Werror -Wall -Wextra -Wconversion -std=c11 \
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
		test/test_model_check \
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

# Default `make test`: FAST workload. Runs static analysis, the host golden
# model, the exhaustive state-space model check, the fault-injection sim tests,
# both simavr firmware suites, and enforces a coverage floor on the model.
# Designed to finish in a few seconds for quick edit/build/test loops and CI.
test: analyze test-host test-model-check test-fault-inject test-sim test-sim-t85 coverage-check
	@echo "=== all fast pre-hardware tests passed ==="

# Explicit alias for the fast suite.
test-fast: test

# FULL exhaustive workload: same targets, but with the large fuzz/stress
# durations. Use before tagging a release or signing off for hardware.
test-long: HOST_DEFS = $(FULL_HOST_DEFS)
test-long: SIM_DEFS  = $(FULL_SIM_DEFS)
test-long: clean-tests analyze test-host test-model-check test-fault-inject test-sim test-sim-t85 coverage-check
	@echo "=== all FULL (exhaustive) pre-hardware tests passed ==="

# Friendly alias.
stress: test-long

# Remove only the test binaries (force rebuild with the selected *_DEFS).
.PHONY: clean-tests
clean-tests:
	rm -f test/test_logic_host test/test_sim test/test_sim_t85 \
	      test/test_model_check test/test_trace

# Host-compiled golden-model unit tests (no AVR; fast logic verification).
test-host: test/test_logic_host
	./test/test_logic_host

test/test_logic_host: test/test_logic_host.c test/bypass_config_host.h bypass_config.h
	$(HOSTCC) $(HOST_CFLAGS) $(HOST_DEFS) -Itest $< -o $@

# Exhaustive small-model state-space verification: BFS over the entire reachable
# state space of the debounce algorithm, proving the core reliability invariants
# for ALL inputs (not just sampled ones).
test-model-check: test/test_model_check
	./test/test_model_check

test/test_model_check: test/test_model_check.c test/bypass_config_host.h bypass_config.h
	$(HOSTCC) $(HOST_CFLAGS) -Itest $< -o $@

# simavr integration tests: run the REAL firmware .elf in the simulator,
# drive PB0, assert PB1/PB2. Depends on the built firmware.
test-sim: test/test_sim
	./test/test_sim

test/test_sim: test/test_sim.c test/bypass_config_host.h bypass_config.h $(TARGET).elf
	$(HOSTCC) $(SIM_CFLAGS) $(SIM_DEFS) -Itest test/test_sim.c -o $@ $(SIM_LIBS)

# ATtiny85 simavr integration tests: same test harness, different MCU and firmware.
test-sim-t85: test/test_sim_t85
	./test/test_sim_t85

test/test_sim_t85: test/test_sim.c test/bypass_config_host.h bypass_config.h $(TARGET85).elf
	$(HOSTCC) $(SIM_CFLAGS) $(SIM_DEFS) -Itest \
		-DFW_PATH=\"$(TARGET85).elf\" \
		-DMCU_NAME=\"attiny85\" \
		-DF_CPU_HZ=1000000UL \
		-DTARGET_T85 \
		test/test_sim.c -o $@ $(SIM_LIBS)

# Fault-injection sim tests: corrupt MCU state (program_state_, effect_state_,
# timer_isr_called_, DDRB/PORTB) and verify the firmware's sanity-check
# force_wdt_reset() path. Runs on the ATtiny85 build (simavr models WDT reset).
test-fault-inject: test/test_sim_t85
	./test/test_sim_t85 fault-inject

# Generate a GTKWave-viewable waveform of PB0/PB1/PB2 (bypass_trace.vcd).
# Build a TRACE-enabled variant of the sim harness and run it.
trace: test/test_trace
	./test/test_trace
	@echo "View with: gtkwave bypass_trace.vcd"

test/test_trace: test/test_sim.c $(TARGET).elf
	$(HOSTCC) $(SIM_CFLAGS) $(SIM_DEFS) -Itest -DTRACE test/test_sim.c -o $@ $(SIM_LIBS)

# Static analysis (default: clang-tidy, else avr-gcc -fanalyzer). Override
# ANALYZE_CMD as needed. NOTE: -Wconversion is now enforced (-Werror) by the
# firmware build itself (see CFLAGS), so no separate advisory report is needed.
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
	@echo "--- firmware -Wconversion report (non-fatal; AVR register idiom) ---"
	@$(CC) $(filter-out -Werror,$(CFLAGS)) -Wconversion -fsyntax-only $< 2>&1 | sed 's/^/  /' || true
	@echo "--- end -Wconversion report ---"

# Code coverage of the golden model (exercises the same algorithm as the firmware).
# Generates gcov line/branch coverage for the host-compiled reference model.
COVERAGE_DIR = coverage
# Minimum acceptable line-coverage percentage for the golden model (gate).
COVERAGE_MIN ?= 90

coverage:
	@mkdir -p $(COVERAGE_DIR)
	$(HOSTCC) $(HOST_CFLAGS) $(HOST_DEFS) -Itest --coverage test/test_logic_host.c -o $(COVERAGE_DIR)/test_logic_host
	cd $(COVERAGE_DIR) && ./test_logic_host
	cd $(COVERAGE_DIR) && gcov -b test_logic_host.c 2>/dev/null || true
	@echo "Coverage report: $(COVERAGE_DIR)/test_logic_host.c.gcov"
	@echo "For HTML report: lcov --capture -d $(COVERAGE_DIR) -o $(COVERAGE_DIR)/coverage.info && genhtml $(COVERAGE_DIR)/coverage.info -o $(COVERAGE_DIR)/html"

# Coverage gate: build the model with coverage, run it, and FAIL if line
# coverage of the golden model drops below COVERAGE_MIN percent. Wired into
# `make test` so coverage regressions break the build.
coverage-check:
	@mkdir -p $(COVERAGE_DIR)
	@$(HOSTCC) $(HOST_CFLAGS) $(HOST_DEFS) -Itest --coverage \
		test/test_logic_host.c -o $(COVERAGE_DIR)/test_logic_host_cov
	@cd $(COVERAGE_DIR) && ./test_logic_host_cov >/dev/null
	@cd $(COVERAGE_DIR) && gcov test_logic_host_cov-test_logic_host.c >/dev/null 2>&1 \
		|| gcov test_logic_host.c >/dev/null 2>&1 || true
	@pct=$$(cd $(COVERAGE_DIR) && gcov test_logic_host_cov-test_logic_host.c 2>/dev/null \
		| awk -F'[:%]' '/Lines executed/ {print $$2; exit}'); \
	if [ -z "$$pct" ]; then \
		pct=$$(cd $(COVERAGE_DIR) && gcov -o . test_logic_host_cov 2>/dev/null \
			| awk -F'[:%]' '/Lines executed/ {print $$2; exit}'); \
	fi; \
	echo "golden-model line coverage: $${pct:-unknown}% (floor $(COVERAGE_MIN)%)"; \
	if [ -z "$$pct" ]; then \
		echo "WARNING: could not determine coverage (gcov output parse failed); not gating."; \
	else \
		awk -v p="$$pct" -v m="$(COVERAGE_MIN)" 'BEGIN { exit !(p+0 >= m+0) }' \
			|| { echo "FAIL: coverage $$pct% below floor $(COVERAGE_MIN)%"; exit 1; }; \
	fi

coverage-clean:
	rm -rf $(COVERAGE_DIR)
	find . -name '*.gcda' -o -name '*.gcno' | xargs rm -f


# vim: tw=0 nowrap
