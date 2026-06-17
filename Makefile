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
#   Each variant is built for:
#     - ATtiny13a @ 1.2 MHz : the primary part (distinct core).
#     - tinyx5 family @ 1.0 MHz : ATtiny85 and ATtiny45 (and trivially the t25).
#                             These are core-identical to each other; simavr can
#                             model their watchdog system reset (which it cannot
#                             do for the ATtiny13a), so they also carry the
#                             WDT-reset / fault-injection coverage.
#
#   Variant outputs are named bypass_<variant>.elf/.hex (ATtiny13a) and
#   bypass_<variant>_t<n>.elf/.hex (tinyx5, n in {85,45}). Pick a variant for
#   single-target actions with VARIANT=<name>, e.g. `make VARIANT=relay program`
#   (ATtiny13a) or `make VARIANT=relay program45` (ATtiny45).
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

# --- Secondary targets: the tinyx5 family (ATtiny25/45/85) ------------------
# These parts are core-identical to one another: same 1.0 MHz config, same
# registers, same fuse bytes -- they differ ONLY in flash/RAM size, the -mmcu
# name, and the avrdude part. simavr models their watchdog system reset (which
# it cannot do for the ATtiny13a), so they also carry the WDT-reset and
# fault-injection coverage for the whole family. Suffix <n> names the artifacts
# (bypass_<variant>_t<n>.elf, targets size<n>/flash<n>/...). To add a sibling
# (e.g. the ATtiny25), append its number here and define mmcu_<n>/part_<n>.
TINYX5     = 85 45
mmcu_85    = attiny85
mmcu_45    = attiny45
part_85    = t85
part_45    = t45
F_CPU_X5   = 1000000UL

# --- Output variants ---------------------------------------------------------
# The hardware-agnostic core (bypass_core.c) links against exactly one output
# driver. A variant is identified by a short name; each maps to the -D selector
# macro the firmware/tests compile with and to its driver source file. To add a
# variant: add its short name here and define macro_<name>/src_<name> below.
CORE_SRC = bypass_core.c bypass_pure.c
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
FW_HEADERS = bypass_config.h bypass_types.h bypass_hw_iface.h \
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

# Fuse bytes for this design (verified bit-by-bit; see bypass_core.c header):
#   lfuse=0x4A : SPIEN on, CKDIV8 on (1.2MHz), SUT=14CK+64ms, int 9.6MHz RC, WDTON forced on
#   hfuse=0xFB : 2.7V brown-out detection enabled; RSTDISBL/DWEN left safe
LFUSE = 0x4a
HFUSE = 0xfb

# tinyx5 family fuse bytes (identical across ATtiny25/45/85):
#   lfuse=0x62 : CKDIV8 on (1.0MHz), CKOUT off, SUT=14CK+64ms, int 8MHz RC
#   hfuse=0xCD : 2.7V BOD, SPIEN on, RSTDISBL/DWEN safe, WDTON forced on
LFUSE_X5 = 0x62
HFUSE_X5 = 0xcd

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

# Host-compiled copy of the firmware's PURE logic (bypass_pure.c), linked into
# every test that includes model_step.h. Since the convergence, model_step.h's
# step() delegates to the real debounce_integrate()/debounce_step() instead of a
# re-implementation, so those tests must link the firmware functions directly --
# the model can no longer drift from what ships. bypass_pure.c is AVR-targeted
# but hardware-free; force-including the config shim lets it compile natively so
# its only firmware dependency (the RELEASE_THRESH/PRESSED_THRESH thresholds in
# bypass_config.h) resolves on the host. The shim has an include guard, so
# force-including it into the test TU as well (which already pulls it in via
# model_step.h) is harmless.
PURE_HOST_SRC    = bypass_pure.c
PURE_HOST_DEP    = bypass_pure.c bypass_pure.h bypass_types.h
PURE_HOST_CFLAGS = -include test/bypass_config_host.h

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

# --- MISRA-C:2012 analysis (cppcheck misra addon) ----------------------------
# Same cppcheck binary, driven by its bundled misra.py addon. Three committed
# support files make the run readable and reproducible:
#   test/misra.json           - addon config; points misra.py at the rule texts
#   test/misra_rules.txt      - SHORT PARAPHRASES of each rule (cppcheck ships
#                               no rule texts -- they are copyrighted -- so
#                               without this every finding is an opaque number)
#   test/misra_suppressions.txt - documented per-file deviations (each maps to a
#                               "D-n" record in MISRA_COMPLIANCE.md)
# Notes:
#   - PYTHONWARNINGS=ignore silences a DeprecationWarning from misra.py under
#     Python 3.12+; cppcheck treats ANY addon stderr as a hard failure.
#   - avr-libc / avr-gcc system headers are outside the compliance boundary, so
#     their violations are suppressed by path (the '*:DIR/*' globs are quoted in
#     the recipe to keep the shell from expanding them).
#   - cppcheck must run from the project root so the relative addon/rule paths
#     resolve in the addon subprocess; `make` already does.
MISRA_ADDON        ?= test/misra.json
MISRA_RULES        ?= test/misra_rules.txt
MISRA_SUPPRESS     ?= test/misra_suppressions.txt

# Robust avr-libc include discovery for the MISRA run. The shared
# AVR_LIBC_INCLUDE (above) is derived from `$(CC) -print-file-name=avr/io.h`,
# which on this toolchain returns a bare name -- avr-libc's headers live outside
# avr-gcc's own dirs -- so it can resolve to a non-path. MISRA's value rules
# (10.x essential type, 11.x pointer/integer) are meaningless without the real
# register headers, so we discover the directory from the preprocessor's actual
# search path and fall back to the shared variable only if that fails.
MISRA_AVR_INCLUDE  := $(shell echo | $(CC) -xc -E -Wp,-v - 2>&1 | grep -oE '^ /[^ ]+' | tr -d ' ' | while read d; do if [ -f "$$d/avr/io.h" ]; then realpath "$$d" 2>/dev/null || echo "$$d"; break; fi; done)
ifeq ($(MISRA_AVR_INCLUDE),)
MISRA_AVR_INCLUDE  := $(AVR_LIBC_INCLUDE)
endif

# Base flags shared by the gating (analyze-misra) and report (analyze-misra-
# report) targets. The documented-deviation waiver (--suppressions-list) is
# deliberately NOT here: the gating target adds it (plus --error-exitcode) to
# fail on un-waived findings, while the report target omits it to show the full
# inventory including the waived deviations.
MISRA_CPPCHECK_FLAGS ?= --addon=$(MISRA_ADDON) --std=c11 --platform=avr8 \
                      --enable=style --inline-suppr \
                      --suppress=missingIncludeSystem \
                      --suppress=unmatchedSuppression \
                      $(if $(MISRA_AVR_INCLUDE),'--suppress=*:$(MISRA_AVR_INCLUDE)/*' -I$(MISRA_AVR_INCLUDE)) \
                      $(if $(AVR_GCC_INCLUDE),'--suppress=*:$(AVR_GCC_INCLUDE)/*' -I$(AVR_GCC_INCLUDE)) \
                      -D__AVR__ -D__AVR_ATtiny13A__ -DF_CPU=$(F_CPU)

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

# Primary (ATtiny13a). The tinyx5 family's per-chip flags are computed inline in
# the build/sim templates from mmcu_<n> + F_CPU_X5 + CFLAGS_COMMON.
CFLAGS    = -mmcu=$(MCU)   -DF_CPU=$(F_CPU)   $(CFLAGS_COMMON)
LDFLAGS   = -mmcu=$(MCU)   -Wl,--gc-sections

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
# Targets that are commands, not files. Per-chip tinyx5 targets (all85/size85/
# fuses85/flash85/program85, *45, test-sim-t85, ...) are declared .PHONY by the
# templates that generate them.
.PHONY: all all13 clean size readfuses fuses flash program help \
        test test-fast test-long stress \
        test-host test-sim test-sim-secondary \
        test-model-check test-fault-inject test-fuses test-symbolic test-cbmc test-mutation \
        analyze analyze-tidy analyze-cppcheck analyze-deep \
        trace coverage coverage-check coverage-clean

# ============================================================================
# BUILD -- firmware matrix (3 variants x {ATtiny13a, tinyx5 family})
# ============================================================================
#
# ELF/HEX rules are generated by templates so adding a variant OR a tinyx5
# sibling needs no new build rules. Each rule links bypass_core.c with the
# variant's driver source and selects the variant with its -D macro. The
# toolchain stamp is a prerequisite so a compiler change forces a rebuild (and
# thus re-runs the fault-injection gate that validates the RAM-corruption guard).
#
# Generated per variant <v> (ATtiny13a, 1.2 MHz):
#   bypass_<v>.elf / bypass_<v>.hex
# Generated per variant <v> x tinyx5 chip <n> (1.0 MHz):
#   bypass_<v>_t<n>.elf / bypass_<v>_t<n>.hex

# $(call VARIANT_BUILD_T13,variant)
define VARIANT_BUILD_T13
$(FW_BASE)_$(1).elf: $$(CORE_SRC) $$(src_$(1)) $$(FW_HEADERS) $$(TOOLCHAIN_STAMP)
	$$(CC) $$(CFLAGS) -D$$(macro_$(1)) $$(LDFLAGS) -o $$@ $$(CORE_SRC) $$(src_$(1))

$(FW_BASE)_$(1).hex: $(FW_BASE)_$(1).elf
	$$(OBJCOPY) -O ihex -R .eeprom $$< $$@
endef
$(foreach v,$(VARIANTS),$(eval $(call VARIANT_BUILD_T13,$(v))))

# $(call VARIANT_BUILD_X5,variant,chip-number) -- one tinyx5 chip
define VARIANT_BUILD_X5
$(FW_BASE)_$(1)_t$(2).elf: $$(CORE_SRC) $$(src_$(1)) $$(FW_HEADERS) $$(TOOLCHAIN_STAMP)
	$$(CC) -mmcu=$$(mmcu_$(2)) -DF_CPU=$$(F_CPU_X5) $$(CFLAGS_COMMON) -Wl,--gc-sections \
		-D$$(macro_$(1)) -o $$@ $$(CORE_SRC) $$(src_$(1))

$(FW_BASE)_$(1)_t$(2).hex: $(FW_BASE)_$(1)_t$(2).elf
	$$(OBJCOPY) -O ihex -R .eeprom $$< $$@
endef
$(foreach v,$(VARIANTS),$(foreach n,$(TINYX5),$(eval $(call VARIANT_BUILD_X5,$(v),$(n)))))

# Convenience lists of every variant's artifacts (t13a + each tinyx5 chip).
ALL_ELF13 = $(foreach v,$(VARIANTS),$(FW_BASE)_$(v).elf)
ALL_HEX13 = $(foreach v,$(VARIANTS),$(FW_BASE)_$(v).hex)
ALL_ELFX5 = $(foreach v,$(VARIANTS),$(foreach n,$(TINYX5),$(FW_BASE)_$(v)_t$(n).elf))
ALL_HEXX5 = $(foreach v,$(VARIANTS),$(foreach n,$(TINYX5),$(FW_BASE)_$(v)_t$(n).hex))
# Per-chip ELF/HEX lists (for the size<n>/all<n> targets).
$(foreach n,$(TINYX5),$(eval ELF_t$(n) := $(foreach v,$(VARIANTS),$(FW_BASE)_$(v)_t$(n).elf)))
$(foreach n,$(TINYX5),$(eval HEX_t$(n) := $(foreach v,$(VARIANTS),$(FW_BASE)_$(v)_t$(n).hex)))

# Default goal: build every ATtiny13a variant image and print sizes.
all: all13

# Build all ATtiny13a variant firmwares (.hex) + print sizes.
all13: $(ALL_HEX13) size

# Report flash/RAM usage of every ATtiny13a variant build.
size: $(ALL_ELF13)
	@for e in $(ALL_ELF13); do echo "== $$e =="; $(SIZE) --mcu=$(MCU) -C $$e; done

# Per-tinyx5-chip build + size targets: all85/size85, all45/size45, ...
# $(call MCU_X5_BUILD_TARGETS,chip-number)
define MCU_X5_BUILD_TARGETS
.PHONY: all$(1) size$(1)
all$(1): $$(HEX_t$(1)) size$(1)
size$(1): $$(ELF_t$(1))
	@for e in $$(ELF_t$(1)); do echo "== $$$$e =="; $$(SIZE) --mcu=$$(mmcu_$(1)) -C $$$$e; done
endef
$(foreach n,$(TINYX5),$(eval $(call MCU_X5_BUILD_TARGETS,$(n))))

# ============================================================================
# CLEAN
# ============================================================================

# Remove all build outputs and test binaries (keeps coverage/ -- see
# coverage-clean for that).
clean:
	rm -f $(ALL_ELF13) $(ALL_HEX13) $(ALL_ELFX5) $(ALL_HEXX5) \
		$(foreach v,$(VARIANTS),test/test_sim_$(v) test/test_trace_$(v)) \
		$(foreach v,$(VARIANTS),$(foreach n,$(TINYX5),test/test_sim_$(v)_t$(n))) \
		test/test_logic_host \
		test/test_model_check test/test_symbolic test/test_fuses \
		test/test_symbolic.bc \
		bypass_trace.vcd $(FW_BASE).plist \
		$(TOOLCHAIN_STAMP)
	rm -f *.dump *.ctu-info cppcheck-addon-ctu-file-list*
	rm -rf test/klee-out-* test/klee-last

# ============================================================================
# FLASH / FUSES -- hardware (select the image with VARIANT=<name>)
# ============================================================================
# These act on ONE variant image, chosen by VARIANT (default cd4053). The
# per-chip tinyx5 equivalents (fuses85/flash85/program85, fuses45/...) act on
# the corresponding ATtiny85/ATtiny45 build of the selected variant.

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

# Per-tinyx5-chip fuses/flash/program targets: fuses85/flash85/program85,
# fuses45/flash45/program45, ... All share the tinyx5 fuse bytes and differ only
# in the avrdude part. flash<n>/program<n> act on the VARIANT-selected image.
# $(call MCU_X5_FLASH_TARGETS,chip-number)
define MCU_X5_FLASH_TARGETS
.PHONY: fuses$(1) flash$(1) program$(1)
fuses$(1):
	$$(AVRDUDE) -c $$(PROGRAMMER) -p $$(part_$(1)) \
		-U lfuse:w:$$(LFUSE_X5):m \
		-U hfuse:w:$$(HFUSE_X5):m
flash$(1): $(FW_BASE)_$$(VARIANT)_t$(1).hex
	$$(AVRDUDE) -c $$(PROGRAMMER) -p $$(part_$(1)) -U flash:w:$(FW_BASE)_$$(VARIANT)_t$(1).hex:i
program$(1): fuses$(1) flash$(1)
endef
$(foreach n,$(TINYX5),$(eval $(call MCU_X5_FLASH_TARGETS,$(n))))


# ============================================================================
# TESTS
# ============================================================================

# Default `make test`: FAST workload. Runs static analysis, the host golden
# model, the exhaustive state-space model check, the symbolic single-step proof,
# the fuse-byte check, the fault-injection sim tests, both simavr firmware
# suites, and enforces a coverage floor on the model. Designed to finish in
# ~1 minute for quick edit/build/test loops and CI.
test: analyze test-host test-model-check test-symbolic test-cbmc test-fuses test-fault-inject test-sim test-sim-secondary coverage-check
	@echo "=== all fast pre-hardware tests passed ==="

# Explicit alias for the fast suite (same as `make test`).
test-fast: test

# FULL exhaustive workload: same targets as `test`, but the fuzz/stress tests
# are rebuilt with their large in-source default durations (FULL_*_DEFS adds no
# overrides). Use before tagging a release or signing off for hardware.
test-long: HOST_DEFS = $(FULL_HOST_DEFS)
test-long: SIM_DEFS  = $(FULL_SIM_DEFS)
test-long: clean-tests analyze test-host test-model-check test-symbolic test-cbmc test-fuses test-fault-inject test-sim test-sim-secondary test-mutation coverage-check
	@echo "=== all FULL (exhaustive) pre-hardware tests passed ==="

# Friendly alias for the exhaustive suite (same as `make test-long`).
stress: test-long

# Remove ONLY the test binaries so the next test run rebuilds them with the
# currently selected workload sizing (FAST vs FULL *_DEFS).
.PHONY: clean-tests
clean-tests:
	rm -f test/test_logic_host test/test_model_check test/test_symbolic \
	      test/test_fuses \
	      $(foreach v,$(VARIANTS),test/test_sim_$(v) test/test_trace_$(v)) \
	      $(foreach v,$(VARIANTS),$(foreach n,$(TINYX5),test/test_sim_$(v)_t$(n)))

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

# Build rule for the state-space checker. Links bypass_pure.c so step() exercises
# the real firmware functions (see model_step.h / PURE_HOST_SRC).
test/test_model_check: test/test_model_check.c test/model_step.h test/bypass_config_host.h bypass_config.h $(PURE_HOST_DEP)
	$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $(PURE_HOST_CFLAGS) -Itest $< $(PURE_HOST_SRC) -o $@

# Symbolic / exhaustive single-step property check: proves the per-step
# transition invariants of step() hold for EVERY (state x input) combination in
# the full domain (the inductive step behind the whole-program invariants).
# Default build enumerates exhaustively; if KLEE is installed, `make
# test-symbolic-klee` runs the same assertions under symbolic execution.
test-symbolic: test/test_symbolic
	./test/test_symbolic

# Build rule for the symbolic step checker. Links bypass_pure.c so step()
# exercises the real firmware functions (see model_step.h / PURE_HOST_SRC).
test/test_symbolic: test/test_symbolic.c test/model_step.h test/bypass_config_host.h bypass_config.h $(PURE_HOST_DEP)
	$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $(PURE_HOST_CFLAGS) -Itest $< $(PURE_HOST_SRC) -o $@

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

# Optional: CBMC bounded-model-checking of the REAL pure core (bypass_pure.c).
# A third, independent proof engine (SAT/SMT) for the same safety + liveness
# invariants, run on the actual firmware functions rather than a re-model -- plus
# CBMC's automatic instrumentation proving the debounce path is free of integer
# overflow / out-of-range conversion / out-of-bounds undefined behaviour. See
# test/test_cbmc.c. Only runs if cbmc is installed; otherwise the exhaustive
# test-model-check / test-symbolic targets already cover the same properties.
.PHONY: test-cbmc
CBMC        ?= cbmc
# bypass_pure.c includes the AVR-targeted bypass_config.h directly; supply the
# same minimal target macros the host shim provides (F_CPU + the PBx pin numbers)
# so it parses natively, exactly as PURE_HOST_CFLAGS does for the other tests.
CBMC_DEFS   = -DF_CPU=1200000UL -DPB0=0 -DPB1=1 -DPB2=2
# Turn on the full automatic-property instrumentation: any UB on the debounce
# path becomes a proof obligation, not a silent assumption.
CBMC_CHECKS = --bounds-check --pointer-check --div-by-zero-check \
              --signed-overflow-check --unsigned-overflow-check \
              --conversion-check --undefined-shift-check
# Straight-line proofs (no loops) and the two bounded-liveness proofs (loops
# fully unrolled at --unwind 50, > every harness's fixed horizon; the unwinding
# assertion proves the bound is real, not assumed). Matches TODO.md's
# `cbmc --unwind 50` on the debounce path.
CBMC_PROOFS      = prove_integrate prove_debounce_step prove_corrupt_state_faults \
                   prove_init_context prove_step_transition
CBMC_PROOFS_LOOP = prove_press_liveness prove_release_liveness
test-cbmc:
	@if command -v $(CBMC) >/dev/null 2>&1; then \
		for p in $(CBMC_PROOFS); do \
			echo "cbmc: $$p"; \
			$(CBMC) test/test_cbmc.c $(PURE_HOST_SRC) -Itest $(CBMC_DEFS) \
				--function $$p $(CBMC_CHECKS) || exit 1; \
		done; \
		for p in $(CBMC_PROOFS_LOOP); do \
			echo "cbmc: $$p (--unwind 50)"; \
			$(CBMC) test/test_cbmc.c $(PURE_HOST_SRC) -Itest $(CBMC_DEFS) \
				--function $$p --unwind 50 --unwinding-assertions $(CBMC_CHECKS) || exit 1; \
		done; \
		echo "=== CBMC: all debounce-core proofs SUCCESSFUL ==="; \
	else \
		echo "cbmc not installed; the exhaustive 'test-model-check' and 'test-symbolic'"; \
		echo "targets cover the same properties. Install cbmc (apt-get install cbmc) to"; \
		echo "enable SAT/SMT proof of the real bypass_pure.c source."; \
	fi

# Fuse-byte verification: decode the EXACT lfuse/hfuse bytes this Makefile will
# burn (LFUSE/HFUSE for t13a, LFUSE_X5/HFUSE_X5 for the tinyx5 family) and assert
# they match the documented design intent (clock, BOD 2.7V, ISP/RESET preserved,
# etc). Catches a wrong fuse before it reaches silicon -- invisible to every
# other test. The tinyx5 fuse bytes are identical across ATtiny25/45/85, so the
# checker's T85_* bytes cover the whole family.
test-fuses: test/test_fuses
	./test/test_fuses

# Build rule for the fuse checker. Fuse byte values are injected from the
# Makefile variables (single source of truth) via -D. Depends on the Makefile
# so that editing a fuse byte forces a rebuild (the values live in the recipe,
# not in a tracked source file).
test/test_fuses: test/test_fuses.c Makefile
	$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) \
		-DT13_LFUSE=$(LFUSE) -DT13_HFUSE=$(HFUSE) \
		-DT85_LFUSE=$(LFUSE_X5) -DT85_HFUSE=$(HFUSE_X5) \
		$< -o $@

# simavr integration tests: run the REAL compiled firmware .elf in the
# instruction-accurate simulator, drive PB0, and assert LED + control-output
# behavior. One binary per (variant x MCU): the same harness compiled with the
# variant's -D selector (so it expects that variant's control output) and the
# MCU's parameters. tinyx5 builds add -DTARGET_TINYX5 to enable the
# WDT-reset-aware paths simavr can model for that family.
#
# Generated rules:
#   test/test_sim_<v>         ATtiny13a   -> run via test-sim-<v>
#   test/test_sim_<v>_t<n>    tinyx5 chip -> run via test-sim-<v>-t<n>
#   test/test_trace_<v>       VCD waveform builder (-DTRACE, ATtiny13a)
SIM_DEPS = test/test_sim.c test/model_step.h test/bypass_config_host.h \
           test/bypass_output_host.h bypass_config.h $(FW_HEADERS) $(PURE_HOST_DEP)

# $(call VARIANT_SIM_T13,variant)
define VARIANT_SIM_T13
test/test_sim_$(1): $$(SIM_DEPS) $(FW_BASE)_$(1).elf
	$$(HOSTCC) $$(SIM_CFLAGS) $$(SIM_DEFS) $$(PURE_HOST_CFLAGS) -D$$(macro_$(1)) -Itest \
		-DFW_PATH=\"$(FW_BASE)_$(1).elf\" \
		test/test_sim.c $$(PURE_HOST_SRC) -o $$@ $$(SIM_LIBS)

test/test_trace_$(1): $$(SIM_DEPS) $(FW_BASE)_$(1).elf
	$$(HOSTCC) $$(SIM_CFLAGS) $$(SIM_DEFS) $$(PURE_HOST_CFLAGS) -D$$(macro_$(1)) -DTRACE -Itest \
		-DFW_PATH=\"$(FW_BASE)_$(1).elf\" \
		test/test_sim.c $$(PURE_HOST_SRC) -o $$@ $$(SIM_LIBS)

.PHONY: test-sim-$(1)
test-sim-$(1): test/test_sim_$(1)
	@echo "--- sim (ATtiny13a) variant: $(1) ---"
	./test/test_sim_$(1)
endef
$(foreach v,$(VARIANTS),$(eval $(call VARIANT_SIM_T13,$(v))))

# $(call VARIANT_SIM_X5,variant,chip-number)
define VARIANT_SIM_X5
test/test_sim_$(1)_t$(2): $$(SIM_DEPS) $(FW_BASE)_$(1)_t$(2).elf
	$$(HOSTCC) $$(SIM_CFLAGS) $$(SIM_DEFS) $$(PURE_HOST_CFLAGS) -D$$(macro_$(1)) -Itest \
		-DFW_PATH=\"$(FW_BASE)_$(1)_t$(2).elf\" \
		-DMCU_NAME=\"$$(mmcu_$(2))\" \
		-DF_CPU_HZ=$$(F_CPU_X5) \
		-DTARGET_TINYX5 \
		test/test_sim.c $$(PURE_HOST_SRC) -o $$@ $$(SIM_LIBS)

.PHONY: test-sim-$(1)-t$(2) test-fault-inject-$(1)-t$(2)
test-sim-$(1)-t$(2): test/test_sim_$(1)_t$(2)
	@echo "--- sim (ATtiny$(2)) variant: $(1) ---"
	./test/test_sim_$(1)_t$(2)
test-fault-inject-$(1)-t$(2): test/test_sim_$(1)_t$(2)
	@echo "--- fault-injection (ATtiny$(2)) variant: $(1) ---"
	./test/test_sim_$(1)_t$(2) fault-inject
endef
$(foreach v,$(VARIANTS),$(foreach n,$(TINYX5),$(eval $(call VARIANT_SIM_X5,$(v),$(n)))))

# Aggregate run targets.
# test-sim          : all variants on ATtiny13a
# test-sim-t<n>     : all variants on tinyx5 chip <n> (e.g. test-sim-t85)
# test-sim-secondary: all variants on every tinyx5 chip
# test-fault-inject : all variants x every tinyx5 chip
test-sim: $(foreach v,$(VARIANTS),test-sim-$(v))
$(foreach n,$(TINYX5),$(eval test-sim-t$(n): $(foreach v,$(VARIANTS),test-sim-$(v)-t$(n))))
test-sim-secondary: $(foreach n,$(TINYX5),test-sim-t$(n))
test-fault-inject: $(foreach v,$(VARIANTS),$(foreach n,$(TINYX5),test-fault-inject-$(v)-t$(n)))
.PHONY: test-sim test-sim-secondary test-fault-inject \
        $(foreach n,$(TINYX5),test-sim-t$(n))

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
#   - cppcheck misra : MISRA-C:2012 compliance gate (analyze-misra), clean
#                      except for the documented deviations in MISRA_COMPLIANCE.md
# -Wconversion is already enforced by the normal build (CFLAGS); these targets
# focus on deeper flow/lint analysis.
analyze: analyze-tidy analyze-cppcheck analyze-deep analyze-misra
	@echo "=== static analysis (clang-tidy + cppcheck + clang-analyzer + MISRA) clean ==="

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

# MISRA-C:2012 compliance analysis (cppcheck misra addon). Runs over every
# firmware TU, each under a representative variant -D: the core and the
# CD4053-simple driver under the default VARIANT's macro, the mute and relay
# drivers under their own. Findings are rule-labeled via test/misra_rules.txt;
# avr-libc/avr-gcc system-header findings are excluded (compliance boundary).
#
# GATING: fails the build on any finding NOT covered by a documented deviation
# in test/misra_suppressions.txt (each justified in MISRA_COMPLIANCE.md). The
# --suppressions-list waives those; --error-exitcode=2 makes cppcheck exit
# non-zero on anything left. Part of `analyze` -> `make test`.
.PHONY: analyze-misra
analyze-misra: $(FW_SOURCES) $(FW_HEADERS) $(MISRA_ADDON) $(MISRA_RULES) $(MISRA_SUPPRESS)
	@if ! command -v $(CPPCHECK) >/dev/null 2>&1; then \
		echo "cppcheck not installed; skipping MISRA analysis"; exit 0; \
	fi; \
	if ! command -v python3 >/dev/null 2>&1; then \
		echo "python3 not found (required by the cppcheck misra addon); skipping"; exit 0; \
	fi; \
	echo "MISRA-C:2012 analysis ($(CPPCHECK) + misra addon)"; \
	rc=0; out=`mktemp`; \
	for f in $(FW_SOURCES); do \
		case $$f in \
			*cd4053_with_mute*) m=CD4053_WITH_MUTE ;; \
			*tq2_l2_5v_relay*)  m=TQ2_L2_5V_RELAY ;; \
			*)                  m=$(macro_$(VARIANT)) ;; \
		esac; \
		PYTHONWARNINGS=ignore $(CPPCHECK) $(MISRA_CPPCHECK_FLAGS) \
			--suppressions-list=$(MISRA_SUPPRESS) --error-exitcode=2 \
			-D$$m $$f 2>>$$out || rc=1; \
	done; \
	if [ $$rc -ne 0 ]; then \
		echo "MISRA findings NOT covered by a documented deviation:"; \
		grep -E "misra-c2012" $$out || true; \
		echo ""; \
		echo "Fix it, or (if genuinely unavoidable) add a per-file entry to"; \
		echo "$(MISRA_SUPPRESS) with a matching record in MISRA_COMPLIANCE.md."; \
		echo "Run 'make analyze-misra-report' to see the full inventory."; \
		rm -f $$out *.dump *.ctu-info cppcheck-addon-ctu-file-list*; \
		exit 1; \
	fi; \
	rm -f $$out *.dump *.ctu-info cppcheck-addon-ctu-file-list*; \
	echo "MISRA-C:2012: clean (documented deviations waived per MISRA_COMPLIANCE.md)"

# Report-only companion to analyze-misra: shows the FULL inventory, INCLUDING
# the waived deviations (it omits --suppressions-list). Never fails the build.
# Use it when reviewing or maintaining MISRA_COMPLIANCE.md.
.PHONY: analyze-misra-report
analyze-misra-report: $(FW_SOURCES) $(FW_HEADERS) $(MISRA_ADDON) $(MISRA_RULES)
	@if ! command -v $(CPPCHECK) >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		echo "cppcheck and/or python3 not available; skipping MISRA report"; exit 0; \
	fi; \
	echo "MISRA-C:2012 full inventory (report-only, includes waived deviations)"; \
	out=`mktemp`; \
	for f in $(FW_SOURCES); do \
		case $$f in \
			*cd4053_with_mute*) m=CD4053_WITH_MUTE ;; \
			*tq2_l2_5v_relay*)  m=TQ2_L2_5V_RELAY ;; \
			*)                  m=$(macro_$(VARIANT)) ;; \
		esac; \
		echo "  --- $$f  (-D$$m) ---"; \
		PYTHONWARNINGS=ignore $(CPPCHECK) $(MISRA_CPPCHECK_FLAGS) -D$$m $$f 2>&1 \
			| grep -E "misra-c2012" | tee -a $$out || true; \
	done; \
	echo "--- summary: findings per rule ---"; \
	grep -oE "misra-c2012-[0-9.]+" $$out | sort | uniq -c | sort -rn || true; \
	echo "--- total: `grep -cE misra-c2012 $$out` (all waived per MISRA_COMPLIANCE.md unless noted) ---"; \
	rm -f $$out *.dump *.ctu-info cppcheck-addon-ctu-file-list*

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
	@echo "Variants: $(VARIANTS)  (select with VARIANT=<name>; default $(VARIANT))"
	@echo "MCUs: ATtiny13a (primary) + tinyx5 family t$(TINYX5)"
	@echo "Build:"
	@echo "  all (default)   build ALL ATtiny13a variant firmwares (.hex) + sizes"
	@echo "  all13           build all variant firmwares for ATtiny13a"
	@echo "  all85 / all45   build all variant firmwares for ATtiny85 / ATtiny45"
	@echo "  size            print flash/RAM usage for every ATtiny13a variant"
	@echo "  size85 / size45 print flash/RAM usage for every tinyx5 variant"
	@echo "Test (each runs across ALL variants):"
	@echo "  test            FAST full suite -- analyze, model, sim (all MCUs), coverage"
	@echo "  test-long       FULL exhaustive suite (minutes); alias: stress"
	@echo "  test-host       golden-model algorithm tests (host, variant-agnostic)"
	@echo "  test-model-check exhaustive state-space proof of invariants"
	@echo "  test-symbolic   exhaustive single-step property proof of step()"
	@echo "  test-symbolic-klee  same properties under KLEE (if installed)"
	@echo "  test-cbmc       CBMC SAT/SMT proof of the real bypass_pure.c (if installed)"
	@echo "  test-fuses      decode + verify the design fuse bytes (t13a + tinyx5)"
	@echo "  test-sim        real firmware in simavr, all variants (ATtiny13a)"
	@echo "  test-sim-t85 / test-sim-t45  all variants on that tinyx5 chip"
	@echo "  test-sim-secondary  all variants on every tinyx5 chip"
	@echo "  test-sim-<v>[-t<n>]  single variant, e.g. test-sim-relay / test-sim-relay-t45"
	@echo "  test-fault-inject  corrupt state, verify WDT recovery (all variants x tinyx5)"
	@echo "  test-mutation   inject firmware faults, verify the suite kills them"
	@echo "  trace           emit bypass_trace.vcd for VARIANT (GTKWave)"
	@echo "Analysis:"
	@echo "  analyze         static analysis of core + all drivers (3 analyzers)"
	@echo "  analyze-tidy / analyze-cppcheck / analyze-deep  individual analyzers"
	@echo "  analyze-misra   MISRA-C:2012 gate (cppcheck misra addon; see MISRA_COMPLIANCE.md)"
	@echo "  analyze-misra-report  full MISRA inventory incl. waived deviations (report-only)"
	@echo "  coverage        human-readable golden-model coverage report"
	@echo "  coverage-check  fail if coverage < COVERAGE_MIN ($(COVERAGE_MIN)%)"
	@echo "Hardware (act on VARIANT=$(VARIANT); <n> in {$(TINYX5)} for tinyx5):"
	@echo "  readfuses       print current fuse bytes (read-only)"
	@echo "  fuses / fuses<n>   write design fuse bytes (t13a / tinyx5)"
	@echo "  flash / flash<n>   flash the selected variant's firmware"
	@echo "  program / program<n>  fuses + flash (fresh chip)"
	@echo "Clean:"
	@echo "  clean           remove build + test artifacts"
	@echo "  clean-tests     remove only test binaries"
	@echo "  coverage-clean  remove coverage artifacts"
	@echo "Overrides: VARIANT=, PROGRAMMER=, COVERAGE_MIN=, HOSTCC=, HOST_DEFS=, SIM_DEFS="


# vim: tw=0 nowrap
