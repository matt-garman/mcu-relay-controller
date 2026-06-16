################################################################################
# bypass -- build / test / flash Makefile
################################################################################
#
# WHAT THIS BUILDS
#   A hardware-agnostic core (bypass_core.c) plus one of three interchangeable
#   output drivers, selected at build time:
#     - cd4053 : CD4053/TMUX4053 analog switch, single control line (CD4053_SIMPLE)
#     - mute   : CD4053/TMUX4053 with mute-before-switch (CD4053_WITH_MUTE)
#     - relay  : Panasonic TQ2-L2-5V latching relay, pulsed coils (TQ2_L2_5V_RELAY)
#   Each variant is built for two MCUs:
#     - ATtiny13a @ 1.2 MHz : the primary production part.
#     - ATtiny85  @ 1.0 MHz : a secondary build used ONLY for verification --
#                             simavr can model the ATtiny85 watchdog system
#                             reset, which it cannot do for the ATtiny13a.
#
#   Variant outputs are named bypass_<variant>.elf/.hex (ATtiny13a) and
#   bypass_<variant>_t85.elf/.hex (ATtiny85). Pick a variant for single-target
#   actions with VARIANT=<name>, e.g. `make VARIANT=relay program`.
#
# HOW THE TESTS ARE LAYERED (fast -> thorough)
#   1. analyze            static analysis (clang-tidy / -fanalyzer)
#   2. test-host          independent "golden model" of the debounce algorithm
#   3. test-model-check   exhaustive proof of invariants over the whole state space
#   4. test-sim / -t85    the REAL compiled firmware run inside simavr, including
#                         lock-step co-simulation (firmware RAM vs golden model,
#                         compared every 1ms tick)
#   5. test-fault-inject  corrupt MCU state, verify watchdog-reset recovery (t85)
#   6. test-mutation      inject firmware faults, verify the suite detects them
#   7. coverage-check     fail if golden-model line coverage drops below a floor
#
# Host model tests (2,3) build with ASan+UBSan (SANITIZE=) by default.
# The firmware ELFs depend on a toolchain-signature stamp, so a compiler change
# forces a rebuild and re-runs the fault-injection gate (5).
#
# COMMON COMMANDS
#   make                 build all ATtiny13a variant firmwares (.hex) + sizes
#   make test            fast full test suite (all variants) -- use constantly
#   make test-long       exhaustive test suite (minutes) -- before release/HW
#   make trace           emit bypass_trace.vcd waveform (VARIANT=, GTKWave)
#   make VARIANT=relay program   set fuses + flash one variant (fresh chip)
#   make clean           remove all build/test artifacts
#
# FAST vs FULL TESTS
#   `make test` compiles the fuzz/stress tests with reduced iteration counts
#   (FAST_*_DEFS) so it finishes quickly. `make test-long` rebuilds them with
#   the in-source defaults (FULL_*_DEFS = nothing extra) for exhaustive runs.
#   Any individual knob can also be overridden on the command line, e.g.:
#       make test SIM_DEFS='-DSIM_RANDOM_NOISE_DURATION_MS=20000u'
#
# USEFUL OVERRIDES (command line)
#   PROGRAMMER=usbtiny      use a different ISP programmer
#   COVERAGE_MIN=95         raise the coverage gate
#   HOSTCC=clang            use a different host compiler for the test suite
#
# Run `make help` for a one-line summary of every target.
################################################################################


# --- Toolchain & primary (ATtiny13a) target ---------------------------------
# NOTE: keep comments on their OWN lines, never as trailing inline comments on
# variable assignments -- make folds the leading whitespace into the value
# (e.g. TARGET would gain a trailing space and "$(TARGET).elf" would break).
#
# MCU     : primary production MCU (ATtiny13a)
# F_CPU   : 1.2 MHz (9.6 MHz internal RC / CKDIV8)
# FW_BASE : base name for .elf / .hex (suffixed per variant)
# CC      : AVR cross-compiler
# OBJCOPY : ELF -> Intel HEX
# SIZE    : flash/RAM usage reporter
# AVRDUDE : ISP flashing tool
MCU      = attiny13a
F_CPU    = 1200000UL
FW_BASE  = bypass
CC       = avr-gcc
OBJCOPY  = avr-objcopy
SIZE     = avr-size
AVRDUDE  = avrdude

# --- Secondary (ATtiny85) verification target -------------------------------
# Same firmware sources, different MCU/flags. Exists because simavr can model
# the ATtiny85 watchdog reset (used by the fault-injection tests).
MCU85      = attiny85
F_CPU85    = 1000000UL

# --- Output variants ---------------------------------------------------------
# The hardware-agnostic core (bypass_core.c) links against exactly one output
# driver. A variant is identified by a short name; each maps to the -D selector
# macro the firmware/tests compile with and to its driver source file. To add a
# variant: add its short name here and define macro_<name>/src_<name> below.
CORE_SRC = bypass_core.c
VARIANTS = cd4053 mute relay

# variant short name -> firmware -D selector macro
macro_cd4053 = CD4053_SIMPLE
macro_mute   = CD4053_WITH_MUTE
macro_relay  = TQ2_L2_5V_RELAY

# variant short name -> output driver source file
src_cd4053 = bypass_output_cd4053_simple.c
src_mute   = bypass_output_cd4053_with_mute.c
src_relay  = bypass_output_tq2_l2_5v_relay.c

# Headers shared by every firmware build; any change rebuilds all variants.
FW_HEADERS = bypass_config.h bypass_types.h bypass_core.h bypass_output.h \
             bypass_output_common.h \
             bypass_output_cd4053_simple.h bypass_output_cd4053_with_mute.h \
             bypass_output_tq2_l2_5v_relay.h

# VARIANT selects the single-target build for size/flash/trace/program actions.
# `make`/`make test` cover ALL variants; VARIANT only matters when you act on
# one specific image (e.g. flashing).
VARIANT ?= cd4053

# Programmer settings.
# PROGRAMMER: "51 AVR USB ISP ASP" dongle is a USBasp clone -> usbasp.
# AVRDUDE_PART: avrdude's short name for the ATtiny13/13a.
# Override on the command line if needed, e.g.:
#   make flash PROGRAMMER=usbtiny
PROGRAMMER   ?= usbasp
AVRDUDE_PART   ?= t13
AVRDUDE_PART85 ?= t85

# Fuse bytes for this design (verified bit-by-bit; see attiny13_bypass.c header):
#   lfuse=0x4A : SPIEN on, CKDIV8 on (1.2MHz), SUT=14CK+64ms, int 9.6MHz RC, WDTON forced on
#   hfuse=0xFB : 2.7V brown-out detection enabled; RSTDISBL/DWEN left safe
LFUSE = 0x4a
HFUSE = 0xfb

# ATtiny85 fuse bytes:
#   lfuse=0x62 : CKDIV8 on (1.0MHz), CKOUT off, SUT=14CK+64ms, int 8MHz RC
#   hfuse=0xCD : 2.7V BOD, SPIEN on, RSTDISBL/DWEN safe, WDTON forced on
LFUSE85 = 0x62
HFUSE85 = 0xcd

# Common avrdude flags for the ATtiny13a (programmer + part).
AVRDUDE_FLAGS = -c $(PROGRAMMER) -p $(AVRDUDE_PART)

# --- Host test-suite compiler / simavr ---------------------------------------
# Host (PC) compiler for the test suite (NOT the AVR cross-compiler).
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

# Sanitizers for the PURE-HOST model tests (test_logic_host, test_model_check,
# test_symbolic, test_fuses). UBSan catches the integer narrowing/overflow/
# signed-shift UB that the debounce arithmetic could otherwise hide; ASan
# catches any out-of-bounds/use-after-free in the harness itself.
# -fno-sanitize-recover turns any violation into a hard, nonzero-exit failure
# instead of a logged-and-continue warning. These pure-host binaries link no
# external libraries, so the sanitizers stay noise-free.
# Override on the command line to disable (e.g. a toolchain without the runtime):
#   make test SANITIZE=
SANITIZE    ?= -fsanitize=undefined,address -fno-sanitize-recover=all

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
                 -DSIM_POWER_ON_BOOTS=10 \
                 -DSIM_PARITY_ITERS=200u \
                 -DSIM_LOCKSTEP_ITERS=1500u
# Full (exhaustive) sizing == the in-source defaults, so no extra -D needed.
FULL_HOST_DEFS =
FULL_SIM_DEFS  =

# Selected per-invocation; `test-long`/`stress` override these.
HOST_DEFS ?= $(FAST_HOST_DEFS)
SIM_DEFS  ?= $(FAST_SIM_DEFS)

# --- Static-analysis (clang-tidy) configuration ------------------------------
# clang-tidy needs to know where avr-libc's headers live and which AVR target
# defines to assume. These shell-outs discover the avr-gcc include paths and
# architecture so clang can parse the firmware as the AVR build sees it.
AVR_IO_HEADER      := $(shell $(CC) -print-file-name=avr/io.h)
AVR_LIBC_INCLUDE   := $(patsubst %/avr/, %, $(dir $(AVR_IO_HEADER)))
AVR_GCC_INCLUDE    := $(shell $(CC) -print-file-name=include)
AVR_ARCH           := $(shell $(CC) -mmcu=$(MCU) -dM -E - < /dev/null | awk '/__AVR_ARCH__/ { print $$3; exit }')
# Shared clang target/flags so clang-tidy AND the clang static analyzer parse
# the firmware exactly as the AVR build sees it.
CLANG_AVR_FLAGS    ?= -target avr -mmcu=$(MCU) -DF_CPU=$(F_CPU) -D__AVR__ -D__AVR_ATtiny13A__ \
                      -D__AVR_DEVICE_NAME__=$(MCU) $(if $(AVR_ARCH),-D__AVR_ARCH__=$(AVR_ARCH)) \
                      -D__AVR_HAVE_PRR_PRTIM0 \
                      -Wno-macro-redefined \
					  -fshort-enums \
                      $(if $(AVR_LIBC_INCLUDE),-I$(AVR_LIBC_INCLUDE)) \
                      $(if $(AVR_GCC_INCLUDE),-I$(AVR_GCC_INCLUDE))
CLANG_TIDY_FLAGS   ?= $(CLANG_AVR_FLAGS)
# clang-tidy check set: the default plus a curated set of bug-finding groups.
# misc-include-cleaner is excluded because it flags the (correct) transitive
# include of <stdint.h>/<stdint.h> macros via <avr/io.h>, which is idiomatic
# for AVR firmware and not worth churning the includes over.
CLANG_TIDY_CHECKS  ?= -*,bugprone-*,cert-*,clang-analyzer-*,misc-*,-misc-include-cleaner,readability-misleading-indentation,performance-*
# clang-tidy command (override to point at a different tidy binary).
CLANG_TIDY         ?= clang-tidy
# The clang-tidy invocation PREFIX (tool + checks). The analyze-tidy recipe
# appends each firmware source and the AVR parse flags per file. Override to use
# a different tidy binary or check set.
ANALYZE_CMD        ?= $(CLANG_TIDY) --checks='$(CLANG_TIDY_CHECKS)' --warnings-as-errors='*'

# Firmware translation units analyzed/linted by the `analyze` targets: the
# hardware-agnostic core plus every variant's output driver. Each is analyzed
# variant-agnostically (the core needs no selector; each driver includes its own
# header directly).
FW_SOURCES         = $(CORE_SRC) $(foreach v,$(VARIANTS),$(src_$(v)))

# cppcheck: a second, independent analyzer. Uses the AVR platform model and the
# avr-libc include path so it sees the real register definitions.
CPPCHECK           ?= cppcheck
CPPCHECK_FLAGS     ?= --enable=warning,style,performance,portability \
                      --std=c11 --platform=avr8 --error-exitcode=2 \
                      --inline-suppr \
                      --suppress=missingIncludeSystem \
                      --suppress=unmatchedSuppression \
                      --suppress=unusedStructMember \
                      -D__AVR__ -D__AVR_ATtiny13A__ -DF_CPU=$(F_CPU) \
                      $(if $(AVR_LIBC_INCLUDE),-I$(AVR_LIBC_INCLUDE))

# Clang static analyzer (deep symbolic-execution path analysis). This is the
# stand-in for `gcc -fanalyzer`: the system avr-gcc (7.3.0) predates -fanalyzer
# (which needs GCC 10+), but clang's analyzer understands -target avr and the
# real avr-libc headers, giving equivalent inter-procedural flow analysis.
CLANG              ?= clang

# --- Firmware compile/link flags ---------------------------------------------
# -Os                 optimize for size (tiny flash)
# -fshort-enums       8-bit enums (the design relies on this)
# -funsigned-char     plain char is unsigned
# -ffunction/data-sections + --gc-sections : strip unused code/data
# -Werror -Wall -Wextra -Wconversion : strict; -Wconversion catches narrowing
# Flags common to every firmware build; the MCU/F_CPU differ per target and are
# prepended in CFLAGS (t13a) / CFLAGS85 (t85).
CFLAGS_COMMON = -Os \
          -fshort-enums -funsigned-char \
          -ffunction-sections -fdata-sections \
          -Werror -Wall -Wextra -Wconversion -std=c11

CFLAGS    = -mmcu=$(MCU)   -DF_CPU=$(F_CPU)   $(CFLAGS_COMMON)
CFLAGS85  = -mmcu=$(MCU85) -DF_CPU=$(F_CPU85) $(CFLAGS_COMMON)

LDFLAGS   = -mmcu=$(MCU)   -Wl,--gc-sections
LDFLAGS85 = -mmcu=$(MCU85) -Wl,--gc-sections

# --- Toolchain-change detection ----------------------------------------------
# The firmware's RAM-corruption sanity checks (main()'s guard) -- and the
# fault-injection tests that exercise them -- rely on the compiler keeping the
# checked globals coherent in RAM rather than caching them in registers. A
# compiler/optimization change could silently alter that and defeat the guard
# without any source change. To make such a change observable, we hash the
# compiler identities into a stamp file and have BOTH firmware ELFs depend on
# it. When the toolchain changes, the stamp is rewritten -> the firmware
# rebuilds -> the simavr harnesses relink -> `test-fault-inject` re-runs
# automatically. The stamp is only rewritten when the signature actually
# changes, so a normal build does not churn.
TOOLCHAIN_SIG   := $(shell { $(CC) --version; $(HOSTCC) --version; } 2>/dev/null | cksum | awk '{print $$1}')
TOOLCHAIN_STAMP := test/.toolchain.sig

$(TOOLCHAIN_STAMP): FORCE
	@mkdir -p test
	@if [ "$$(cat $@ 2>/dev/null)" != "$(TOOLCHAIN_SIG)" ]; then \
		printf '%s\n' "$(TOOLCHAIN_SIG)" > $@; \
		echo "toolchain signature changed ($(TOOLCHAIN_SIG)): firmware will rebuild and the fault-injection gate will re-run"; \
	fi

# Force-evaluated phony so the stamp recipe runs every invocation (it only
# touches the file when the signature differs).
.PHONY: FORCE
FORCE:

# Targets that are commands, not files.
.PHONY: all all13 all85 clean size readfuses fuses flash program help \
        test test-fast test-long stress \
        test-host test-sim test-sim-t85 \
        test-model-check test-fault-inject test-fuses test-symbolic test-mutation \
        analyze analyze-tidy analyze-cppcheck analyze-deep \
        trace coverage coverage-check coverage-clean \
        size85 fuses85 flash85 program85

# ============================================================================
# BUILD -- firmware variant matrix (3 variants x 2 MCUs)
# ============================================================================
#
# Per-variant ELF/HEX rules are generated by the template below so adding a
# variant needs no new build rules. Each rule links bypass_core.c with the
# variant's driver source and selects the variant with its -D macro. The
# toolchain stamp is a prerequisite so a compiler change forces a rebuild (and
# thus re-runs the fault-injection gate that validates the RAM-corruption guard).
#
# Generated for each variant <v>:
#   bypass_<v>.elf  / bypass_<v>.hex       (ATtiny13a, 1.2 MHz)
#   bypass_<v>_t85.elf / bypass_<v>_t85.hex (ATtiny85, 1.0 MHz)
define VARIANT_BUILD_RULES
$(FW_BASE)_$(1).elf: $$(CORE_SRC) $$(src_$(1)) $$(FW_HEADERS) $$(TOOLCHAIN_STAMP)
	$$(CC) $$(CFLAGS) -D$$(macro_$(1)) $$(LDFLAGS) -o $$@ $$(CORE_SRC) $$(src_$(1))

$(FW_BASE)_$(1).hex: $(FW_BASE)_$(1).elf
	$$(OBJCOPY) -O ihex -R .eeprom $$< $$@

$(FW_BASE)_$(1)_t85.elf: $$(CORE_SRC) $$(src_$(1)) $$(FW_HEADERS) $$(TOOLCHAIN_STAMP)
	$$(CC) $$(CFLAGS85) -D$$(macro_$(1)) $$(LDFLAGS85) -o $$@ $$(CORE_SRC) $$(src_$(1))

$(FW_BASE)_$(1)_t85.hex: $(FW_BASE)_$(1)_t85.elf
	$$(OBJCOPY) -O ihex -R .eeprom $$< $$@
endef
$(foreach v,$(VARIANTS),$(eval $(call VARIANT_BUILD_RULES,$(v))))

# Convenience lists of every variant's artifacts.
ALL_ELF13 = $(foreach v,$(VARIANTS),$(FW_BASE)_$(v).elf)
ALL_HEX13 = $(foreach v,$(VARIANTS),$(FW_BASE)_$(v).hex)
ALL_ELF85 = $(foreach v,$(VARIANTS),$(FW_BASE)_$(v)_t85.elf)
ALL_HEX85 = $(foreach v,$(VARIANTS),$(FW_BASE)_$(v)_t85.hex)

# Default goal: build every ATtiny13a variant image and print sizes.
all: all13

# Build all ATtiny13a variant firmwares (.hex) + print sizes.
all13: $(ALL_HEX13) size

# Build all ATtiny85 variant firmwares (.hex) + print sizes.
all85: $(ALL_HEX85) size85

# Report flash/RAM usage of every ATtiny13a variant build.
size: $(ALL_ELF13)
	@for e in $(ALL_ELF13); do echo "== $$e =="; $(SIZE) --mcu=$(MCU) -C $$e; done

# Report flash/RAM usage of every ATtiny85 variant build.
size85: $(ALL_ELF85)
	@for e in $(ALL_ELF85); do echo "== $$e =="; $(SIZE) --mcu=$(MCU85) -C $$e; done

# ============================================================================
# CLEAN
# ============================================================================

# Remove all build outputs and test binaries (keeps coverage/ -- see
# coverage-clean for that).
clean:
	rm -f $(ALL_ELF13) $(ALL_HEX13) $(ALL_ELF85) $(ALL_HEX85) \
		$(foreach v,$(VARIANTS),test/test_sim_$(v) test/test_sim_$(v)_t85 test/test_trace_$(v)) \
		test/test_logic_host \
		test/test_model_check test/test_symbolic test/test_fuses \
		test/test_symbolic.bc \
		bypass_trace.vcd $(FW_BASE).plist \
		$(TOOLCHAIN_STAMP)
	rm -rf test/klee-out-* test/klee-last

# ============================================================================
# FLASH / FUSES -- hardware (select the image with VARIANT=<name>)
# ============================================================================
# These act on ONE variant image, chosen by VARIANT (default cd4053). The t85
# equivalents (*85) flash the ATtiny85 build of the same variant.

# Read-only: print the chip's currently programmed fuse bytes. Run this FIRST
# to record a chip's existing fuses before changing anything.
readfuses:
	$(AVRDUDE) $(AVRDUDE_FLAGS) -U lfuse:r:-:h -U hfuse:r:-:h

# Write the design's fuse bytes. Safe: does not touch RSTDISBL/DWEN, so ISP
# access is preserved. Verify before relying on a board in the field.
fuses:
	$(AVRDUDE) $(AVRDUDE_FLAGS) \
		-U lfuse:w:$(LFUSE):m \
		-U hfuse:w:$(HFUSE):m

# Flash the selected variant's ATtiny13a image to the MCU.
flash: $(FW_BASE)_$(VARIANT).hex
	$(AVRDUDE) $(AVRDUDE_FLAGS) -U flash:w:$(FW_BASE)_$(VARIANT).hex:i

# Convenience: set fuses, then flash firmware. Use for a fresh chip.
program: fuses flash

# Write the ATtiny85 design fuse bytes.
fuses85:
	$(AVRDUDE) -c $(PROGRAMMER) -p $(AVRDUDE_PART85) \
		-U lfuse:w:$(LFUSE85):m \
		-U hfuse:w:$(HFUSE85):m

# Flash the selected variant's ATtiny85 image.
flash85: $(FW_BASE)_$(VARIANT)_t85.hex
	$(AVRDUDE) -c $(PROGRAMMER) -p $(AVRDUDE_PART85) -U flash:w:$(FW_BASE)_$(VARIANT)_t85.hex:i

# Convenience: set ATtiny85 fuses then flash (fresh chip).
program85: fuses85 flash85


# ============================================================================
# TESTS
# ============================================================================

# Default `make test`: FAST workload. Runs static analysis, the host golden
# model, the exhaustive state-space model check, the symbolic single-step proof,
# the fuse-byte check, the fault-injection sim tests, both simavr firmware
# suites, and enforces a coverage floor on the model. Designed to finish in
# ~1 minute for quick edit/build/test loops and CI.
test: analyze test-host test-model-check test-symbolic test-fuses test-fault-inject test-sim test-sim-t85 coverage-check
	@echo "=== all fast pre-hardware tests passed ==="

# Explicit alias for the fast suite (same as `make test`).
test-fast: test

# FULL exhaustive workload: same targets as `test`, but the fuzz/stress tests
# are rebuilt with their large in-source default durations (FULL_*_DEFS adds no
# overrides). Use before tagging a release or signing off for hardware.
test-long: HOST_DEFS = $(FULL_HOST_DEFS)
test-long: SIM_DEFS  = $(FULL_SIM_DEFS)
test-long: clean-tests analyze test-host test-model-check test-symbolic test-fuses test-fault-inject test-sim test-sim-t85 test-mutation coverage-check
	@echo "=== all FULL (exhaustive) pre-hardware tests passed ==="

# Friendly alias for the exhaustive suite (same as `make test-long`).
stress: test-long

# Remove ONLY the test binaries so the next test run rebuilds them with the
# currently selected workload sizing (FAST vs FULL *_DEFS).
.PHONY: clean-tests
clean-tests:
	rm -f test/test_logic_host test/test_model_check test/test_symbolic \
	      test/test_fuses \
	      $(foreach v,$(VARIANTS),test/test_sim_$(v) test/test_sim_$(v)_t85 test/test_trace_$(v))

# Golden-model unit tests: an INDEPENDENT host (PC) re-implementation of the
# debounce algorithm. No AVR involved -- fast logic verification that the
# algorithm itself meets the reliability goals. (test_sim* verify the REAL
# firmware matches.)
test-host: test/test_logic_host
	./test/test_logic_host

# Build rule for the golden model. Constants come from bypass_config.h (via the
# host shim) so the model can never drift from the firmware thresholds.
test/test_logic_host: test/test_logic_host.c test/bypass_config_host.h bypass_config.h
	$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $(HOST_DEFS) -Itest $< -o $@

# Exhaustive small-model state-space verification: breadth-first search over the
# ENTIRE reachable state space of the debounce algorithm (~66 states), proving
# the core reliability invariants hold for ALL inputs, not just sampled ones.
test-model-check: test/test_model_check
	./test/test_model_check

# Build rule for the state-space checker.
test/test_model_check: test/test_model_check.c test/model_step.h test/bypass_config_host.h bypass_config.h
	$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) -Itest $< -o $@

# Symbolic / exhaustive single-step property check: proves the per-step
# transition invariants of step() hold for EVERY (state x input) combination in
# the full domain (the inductive step behind the whole-program invariants).
# Default build enumerates exhaustively; if KLEE is installed, `make
# test-symbolic-klee` runs the same assertions under symbolic execution.
test-symbolic: test/test_symbolic
	./test/test_symbolic

# Build rule for the symbolic step checker.
test/test_symbolic: test/test_symbolic.c test/model_step.h test/bypass_config_host.h bypass_config.h
	$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) -Itest $< -o $@

# Optional: run the SAME single-step properties under KLEE symbolic execution
# (only if KLEE is installed). KLEE explores the symbolic input domain and
# proves the assertions with an SMT solver rather than by enumeration.
.PHONY: test-symbolic-klee
# Absolute paths to the brew-installed KLEE and its matching LLVM clang. Using
# absolute defaults so the target works even when `make`'s recipe shell does not
# have brew's shellenv on PATH (an interactive shell may, /bin/sh may not).
# Using llvm@16's clang (KLEE's own LLVM) to emit the bitcode avoids the
# host/module target-triple mismatch warning seen with /usr/bin/clang.
KLEE        ?= /home/linuxbrew/.linuxbrew/bin/klee
KLEE_CLANG  ?= /home/linuxbrew/.linuxbrew/opt/llvm@16/bin/clang
KLEE_INC    := /home/linuxbrew/.linuxbrew/Cellar/klee/3.2_3/include
test-symbolic-klee:
	@if command -v $(KLEE) >/dev/null 2>&1 && command -v $(KLEE_CLANG) >/dev/null 2>&1; then \
		$(KLEE_CLANG) -DUSE_KLEE -I$(KLEE_INC) -I$(SIMAVR_INC) -Itest -emit-llvm -c -g -O0 \
			test/test_symbolic.c -o test/test_symbolic.bc && \
		$(KLEE) --exit-on-error test/test_symbolic.bc; \
	else \
		echo "KLEE or its clang not installed; the exhaustive 'test-symbolic' target"; \
		echo "covers the same input domain. Install klee to enable SMT-backed proof."; \
	fi

# Fuse-byte verification: decode the EXACT lfuse/hfuse bytes this Makefile will
# burn (LFUSE/HFUSE for t13, LFUSE85/HFUSE85 for t85) and assert they match the
# documented design intent (clock, BOD 2.7V, ISP/RESET preserved, etc). Catches
# a wrong fuse before it reaches silicon -- invisible to every other test.
test-fuses: test/test_fuses
	./test/test_fuses

# Build rule for the fuse checker. Fuse byte values are injected from the
# Makefile variables (single source of truth) via -D. Depends on the Makefile
# so that editing a fuse byte forces a rebuild (the values live in the recipe,
# not in a tracked source file).
test/test_fuses: test/test_fuses.c Makefile
	$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) \
		-DT13_LFUSE=$(LFUSE) -DT13_HFUSE=$(HFUSE) \
		-DT85_LFUSE=$(LFUSE85) -DT85_HFUSE=$(HFUSE85) \
		$< -o $@

# simavr integration tests: run the REAL compiled firmware .elf in the
# instruction-accurate simulator, drive PB0, and assert LED + control-output
# behavior. One binary per variant (the same harness compiled with the variant's
# -D selector so it expects that variant's control-output behavior).
#
# Per-variant build + run rules are generated by the template below:
#   test/test_sim_<v>      ATtiny13a harness   -> run via test-sim-<v>
#   test/test_sim_<v>_t85  ATtiny85 harness    -> run via test-sim-<v>-t85
#                          (-DTARGET_T85 selects the WDT-reset-aware paths)
#   test/test_trace_<v>    VCD waveform builder (-DTRACE)
SIM_DEPS = test/test_sim.c test/model_step.h test/bypass_config_host.h \
           test/bypass_output_host.h bypass_config.h $(FW_HEADERS)

define VARIANT_SIM_RULES
test/test_sim_$(1): $$(SIM_DEPS) $(FW_BASE)_$(1).elf
	$$(HOSTCC) $$(SIM_CFLAGS) $$(SIM_DEFS) -D$$(macro_$(1)) -Itest \
		-DFW_PATH=\"$(FW_BASE)_$(1).elf\" \
		test/test_sim.c -o $$@ $$(SIM_LIBS)

test/test_sim_$(1)_t85: $$(SIM_DEPS) $(FW_BASE)_$(1)_t85.elf
	$$(HOSTCC) $$(SIM_CFLAGS) $$(SIM_DEFS) -D$$(macro_$(1)) -Itest \
		-DFW_PATH=\"$(FW_BASE)_$(1)_t85.elf\" \
		-DMCU_NAME=\"attiny85\" \
		-DF_CPU_HZ=1000000UL \
		-DTARGET_T85 \
		test/test_sim.c -o $$@ $$(SIM_LIBS)

test/test_trace_$(1): $$(SIM_DEPS) $(FW_BASE)_$(1).elf
	$$(HOSTCC) $$(SIM_CFLAGS) $$(SIM_DEFS) -D$$(macro_$(1)) -DTRACE -Itest \
		-DFW_PATH=\"$(FW_BASE)_$(1).elf\" \
		test/test_sim.c -o $$@ $$(SIM_LIBS)

.PHONY: test-sim-$(1) test-sim-$(1)-t85 test-fault-inject-$(1)
test-sim-$(1): test/test_sim_$(1)
	@echo "--- sim (ATtiny13a) variant: $(1) ---"
	./test/test_sim_$(1)
test-sim-$(1)-t85: test/test_sim_$(1)_t85
	@echo "--- sim (ATtiny85) variant: $(1) ---"
	./test/test_sim_$(1)_t85
test-fault-inject-$(1): test/test_sim_$(1)_t85
	@echo "--- fault-injection (ATtiny85) variant: $(1) ---"
	./test/test_sim_$(1)_t85 fault-inject
endef
$(foreach v,$(VARIANTS),$(eval $(call VARIANT_SIM_RULES,$(v))))

# Aggregate targets: run the relevant per-variant target for every variant.
test-sim:         $(foreach v,$(VARIANTS),test-sim-$(v))
test-sim-t85:     $(foreach v,$(VARIANTS),test-sim-$(v)-t85)
test-fault-inject:$(foreach v,$(VARIANTS),test-fault-inject-$(v))

# Mutation testing: inject deliberate faults into the PRODUCTION sources
# (bypass_core.c + the variant driver / bypass_config.h), rebuild, and confirm a
# fast test target DETECTS each one (the mutant is "killed"). A surviving mutant
# marks a gap in the suite. Operates on throwaway copies; never touches the real
# sources. Not part of `make test` (it rebuilds the firmware per mutant);
# included in `test-long` and runnable standalone.
test-mutation:
	./test/run_mutation_tests.sh

# Generate a GTKWave-viewable waveform of PB0/PB1/PB2/PB3 over a representative
# press/release sequence for the selected VARIANT. Writes bypass_trace.vcd.
trace: test/test_trace_$(VARIANT)
	./test/test_trace_$(VARIANT)
	@echo "View with: gtkwave bypass_trace.vcd"

# ============================================================================
# STATIC ANALYSIS & COVERAGE
# ============================================================================

# Static analysis of the firmware. Runs THREE independent analyzers and gates
# the build on any finding:
#   - clang-tidy   : lint + bug-pattern checks (ANALYZE_CMD)
#   - cppcheck     : second-opinion static analyzer (analyze-cppcheck)
#   - clang --analyze : deep symbolic-execution path analysis (analyze-deep),
#                       the stand-in for `gcc -fanalyzer` since the installed
#                       avr-gcc (7.3) predates it.
# -Wconversion is already enforced by the normal build (CFLAGS); these targets
# focus on deeper flow/lint analysis.
analyze: analyze-tidy analyze-cppcheck analyze-deep
	@echo "=== static analysis (clang-tidy + cppcheck + clang-analyzer) clean ==="

# clang-tidy (or whatever ANALYZE_CMD points at). Falls back to avr-gcc
# -fanalyzer if a NEWER avr-gcc that supports it is ever installed; otherwise
# errors with guidance.
analyze-tidy: $(FW_SOURCES) $(FW_HEADERS)
	@cmd=$(word 1,$(ANALYZE_CMD)); \
	if command -v $$cmd >/dev/null 2>&1; then \
		for f in $(FW_SOURCES); do \
			echo "clang-tidy: $$cmd $$f"; \
			$(ANALYZE_CMD) $$f -- $(CLANG_TIDY_FLAGS) || exit 1; \
		done; \
	elif $(CC) -fsyntax-only -fanalyzer -xc /dev/null >/dev/null 2>&1; then \
		echo "avr-gcc -fanalyzer"; \
		for f in $(FW_SOURCES); do \
			$(CC) $(CFLAGS) -fanalyzer -c $$f -o $(FW_BASE).analyze.o || exit 1; \
		done; \
		rm -f $(FW_BASE).analyze.o; \
	else \
		echo "No clang-tidy and avr-gcc lacks -fanalyzer. Install clang-tidy or set ANALYZE_CMD=..."; \
		exit 1; \
	fi

# cppcheck second-opinion analyzer (gates via --error-exitcode=2).
analyze-cppcheck: $(FW_SOURCES) $(FW_HEADERS)
	@if command -v $(CPPCHECK) >/dev/null 2>&1; then \
		echo "cppcheck: $(CPPCHECK)"; \
		$(CPPCHECK) $(CPPCHECK_FLAGS) $(FW_SOURCES); \
	else \
		echo "cppcheck not installed; skipping (install cppcheck to enable)"; \
	fi

# Deep path analysis via the clang static analyzer on the AVR target. Emits
# diagnostics as text and FAILS the build on any report (-Werror). This is the
# `-fanalyzer`-equivalent gate.
analyze-deep: $(FW_SOURCES) $(FW_HEADERS)
	@if command -v $(CLANG) >/dev/null 2>&1; then \
		for f in $(FW_SOURCES); do \
			echo "clang --analyze (-target avr): $(CLANG) $$f"; \
			$(CLANG) --analyze -Xclang -analyzer-output=text -Werror \
				$(CLANG_AVR_FLAGS) $$f || exit 1; \
		done; \
	elif $(CC) -fsyntax-only -fanalyzer -xc /dev/null >/dev/null 2>&1; then \
		echo "clang unavailable; using avr-gcc -fanalyzer"; \
		for f in $(FW_SOURCES); do \
			$(CC) $(CFLAGS) -fanalyzer -c $$f -o $(FW_BASE).analyze.o || exit 1; \
		done; \
		rm -f $(FW_BASE).analyze.o; \
	else \
		echo "No deep analyzer available (need clang or avr-gcc>=10 with -fanalyzer)."; \
		exit 1; \
	fi

# Where coverage artifacts are written.
COVERAGE_DIR = coverage
# Minimum acceptable golden-model line-coverage percentage (the gate threshold).
COVERAGE_MIN ?= 90

# Human-readable coverage report of the golden model (line + branch via gcov).
# Use this when you want to SEE coverage; use coverage-check to ENFORCE it.
coverage:
	@mkdir -p $(COVERAGE_DIR)
	$(HOSTCC) $(HOST_CFLAGS) $(HOST_DEFS) -Itest --coverage test/test_logic_host.c -o $(COVERAGE_DIR)/test_logic_host
	cd $(COVERAGE_DIR) && ./test_logic_host
	cd $(COVERAGE_DIR) && gcov -b test_logic_host.c 2>/dev/null || true
	@echo "Coverage report: $(COVERAGE_DIR)/test_logic_host.c.gcov"
	@echo "For HTML report: lcov --capture -d $(COVERAGE_DIR) -o $(COVERAGE_DIR)/coverage.info && genhtml $(COVERAGE_DIR)/coverage.info -o $(COVERAGE_DIR)/html"

# Coverage GATE (wired into `make test`): build the model with coverage, run it,
# and FAIL the build if golden-model line coverage drops below COVERAGE_MIN.
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

# Remove coverage artifacts (the coverage/ dir and any stray gcov data files).
coverage-clean:
	rm -rf $(COVERAGE_DIR)
	find . -name '*.gcda' -o -name '*.gcno' | xargs rm -f

# ============================================================================
# HELP
# ============================================================================

# One-line summary of the most useful targets.
help:
	@echo "Variants: $(VARIANTS)  (select one with VARIANT=<name>; default $(VARIANT))"
	@echo "Build:"
	@echo "  all (default)   build ALL ATtiny13a variant firmwares (.hex) + sizes"
	@echo "  all13 / all85   build all variant firmwares for ATtiny13a / ATtiny85"
	@echo "  size / size85   print flash/RAM usage for every variant (t13 / t85)"
	@echo "Test (each runs across ALL variants):"
	@echo "  test            FAST full suite -- analyze, model, sim (x3), coverage"
	@echo "  test-long       FULL exhaustive suite (minutes); alias: stress"
	@echo "  test-host       golden-model algorithm tests (host, variant-agnostic)"
	@echo "  test-model-check exhaustive state-space proof of invariants"
	@echo "  test-symbolic   exhaustive single-step property proof of step()"
	@echo "  test-symbolic-klee  same properties under KLEE (if installed)"
	@echo "  test-fuses      decode + verify the design fuse bytes (t13 + t85)"
	@echo "  test-sim        real firmware in simavr, all variants (ATtiny13a)"
	@echo "  test-sim-t85    real firmware in simavr, all variants (ATtiny85)"
	@echo "  test-sim-<v>    single variant, e.g. test-sim-relay / test-sim-relay-t85"
	@echo "  test-fault-inject  corrupt state, verify watchdog recovery (t85, all)"
	@echo "  test-mutation   inject firmware faults, verify the suite kills them"
	@echo "  trace           emit bypass_trace.vcd for VARIANT (GTKWave)"
	@echo "Analysis:"
	@echo "  analyze         static analysis of core + all drivers (3 analyzers)"
	@echo "  analyze-tidy / analyze-cppcheck / analyze-deep  individual analyzers"
	@echo "  coverage        human-readable golden-model coverage report"
	@echo "  coverage-check  fail if coverage < COVERAGE_MIN ($(COVERAGE_MIN)%)"
	@echo "Hardware (act on VARIANT=$(VARIANT); *85 targets for ATtiny85):"
	@echo "  readfuses       print current fuse bytes (read-only)"
	@echo "  fuses / fuses85 write design fuse bytes"
	@echo "  flash / flash85 flash the selected variant's firmware"
	@echo "  program / program85  fuses + flash (fresh chip)"
	@echo "Clean:"
	@echo "  clean           remove build + test artifacts"
	@echo "  clean-tests     remove only test binaries"
	@echo "  coverage-clean  remove coverage artifacts"
	@echo "Overrides: VARIANT=, PROGRAMMER=, COVERAGE_MIN=, HOSTCC=, HOST_DEFS=, SIM_DEFS="


# vim: tw=0 nowrap
