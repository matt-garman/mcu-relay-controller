#!/usr/bin/env bash
#
# Mutation testing for the bypass firmware (core + output-driver variants).
#
# WHY THIS EXISTS
# ---------------
# A passing test suite proves the tests PASS on correct code; it does not prove
# the tests would FAIL on broken code. Mutation testing closes that gap: it
# injects a small, deliberate fault ("mutant") into the PRODUCTION sources
# (bypass_core.c, the output drivers, or bypass_config.h), rebuilds, and runs a
# fast test target. A correct, adequate suite must DETECT the fault -- the test
# target must FAIL (the mutant is "killed"). A mutant that survives (tests still
# pass) marks a real hole in the suite.
#
# Core/config mutants map to the single fast variant target `test-sim-cd4053`
# (the core debounce/WDT logic is shared by every variant, so one variant
# suffices to kill them). Output-driver mutants map to their own variant target
# (`test-sim-relay` / `test-sim-mute` / `test-sim-cd4053`).
#
# This operates entirely on a throwaway COPY of the tree; it never modifies the
# real sources. It is wired into `make test-mutation` and is intentionally NOT
# part of the default `make test` (it rebuilds the firmware once per mutant).
#
# Each mutation lists the fast `make` target expected to kill it, so the
# mutation->test mapping is explicit and the run stays quick.
#
# A note on self-referential oracles: the host golden-model tests pull
# RELEASE_THRESH/PRESSED_THRESH from bypass_config.h (the single source of
# truth), so they intentionally CANNOT catch a threshold change (expectation and
# code move together). The threshold mutants below are therefore mapped to
# `test-sim`, where the simavr noise test asserts a HARD-CODED toggle count and
# the lock-step co-sim compares the real binary against an independent model.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"

# Each entry: file<TAB>sed-expression<TAB>make-target<TAB>description
# The sed expression uses '@' as delimiter to avoid clashing with C operators.
MUTATIONS=(
# --- core debounce algorithm (bypass_core.c) -----------------------------------
"bypass_core.c	s@++debounce_counter_@--debounce_counter_@	test-sim-cd4053	ISR integrator: increment-on-press becomes decrement (counter never rises -> never toggles)"
"bypass_core.c	s@debounce_counter_ >= PRESSED_THRESH@debounce_counter_ > PRESSED_THRESH@	test-sim-cd4053	press threshold off-by-one (>= becomes >); lock-step must catch the 1-tick divergence"
"bypass_core.c	s@PORTB |=  (1 << LED_PIN)@PORTB \&= (uint8_t)~(1 << LED_PIN)@	test-sim-cd4053	set_engaged LED output inverted (lights become dark)"
"bypass_config.h	s@#define PRESSED_THRESH (8)@#define PRESSED_THRESH (4)@	test-sim-cd4053	press threshold shortened 8->4 (timing/noise-count regression)"
"bypass_config.h	s@#define RELEASE_THRESH (25)@#define RELEASE_THRESH (15)@	test-sim-cd4053	release lock-out shortened 25->15 (noise-count regression)"
# --- ISR bounds guards (bypass_core.c) -----------------------------------------
"bypass_core.c	s@if (debounce_counter_ < RELEASE_THRESH) { ++debounce_counter_; }@++debounce_counter_;@	test-sim-cd4053	ISR increment: remove saturation guard (counter wraps from 255->0 after 256 sustained ticks)"
"bypass_core.c	s@if (debounce_counter_ > 0) { --debounce_counter_; }@--debounce_counter_;@	test-sim-cd4053	ISR decrement: remove underflow guard (counter wraps 0->255 on release; lock-step catches divergence)"
# --- lockout mechanism (bypass_core.c) -----------------------------------------
"bypass_core.c	s@debounce_counter_ = RELEASE_THRESH;@debounce_counter_ = 0;@g	test-sim-cd4053	toggle lockout: counter reset to 0 instead of RELEASE_THRESH (immediate re-arm, no hold lockout)"
"bypass_core.c	s@program_state_ = RELEASE_DEBOUNCE_WAIT;@program_state_ = PRESS_DEBOUNCE_WAIT;@g	test-sim-cd4053	toggle lockout: stays in PRESS_DEBOUNCE_WAIT after toggle (counter=25 >= 8 -> immediate re-toggle cascade)"
# --- watchdog handshake (bypass_core.c) ----------------------------------------
"bypass_core.c	s@wdt_reset(); // \"pet the dog\"@(void)0; /* MUTANT: no WDT reset */@	test-sim-cd4053	WDT pet removed from main loop: watchdog fires within ~250ms; test_watchdog_not_tripped_normally catches it"
"bypass_core.c	s@timer_isr_called_ = TIMER_ISR_CALLED;@timer_isr_called_ = TIMER_ISR_NOT_CALLED;@	test-sim-cd4053	WDT handshake: ISR clears its own flag -> main never sees CALLED -> WDT fires within timeout"
# --- main-loop sanity guard / toggle dispatch (bypass_core.c) -------------------
"bypass_core.c	s@if ( (program_state_ > RELEASE_DEBOUNCE_WAIT)@if ( 0 \&\& (program_state_ > RELEASE_DEBOUNCE_WAIT)@	test-sim-cd4053	sanity guard disabled: DDRB/state corruption goes undetected; corruption test catches it"
"bypass_core.c	s@else { set_bypass_state(); }@else { set_engaged_state(); }@	test-sim-cd4053	toggle: always sets ENGAGED (never returns to BYPASS); round-trip and lock-step tests catch it"
# --- CD4053 simple output driver -----------------------------------------------
"bypass_output_cd4053_simple.c	s@pin_set_high(CD4053_PIN)@pin_set_low(CD4053_PIN)@	test-sim-cd4053	engaged routes CD4053 the wrong way (PB2 stuck low); control-output test catches it"
# --- TQ2 relay output driver ---------------------------------------------------
"bypass_output_tq2_l2_5v_relay.c	s@_delay_ms(TQ2_L2_5V_PULSE_MS)@_delay_ms(1)@g	test-sim-relay	relay coil pulse shortened to 1ms (< 4ms datasheet min); pulse-width test catches it"
"bypass_output_tq2_l2_5v_relay.c	s@pin_set_high(RELAY_SET_PIN)@pin_set_high(RELAY_RESET_PIN)@	test-sim-relay	engage pulses the wrong (RESET) coil; relay test catches SET-not-pulsed / RESET-moved"
# --- CD4053 with-mute output driver --------------------------------------------
"bypass_output_cd4053_with_mute.c	s@_delay_ms(CD4053_MUTE_DELAY_MS)@_delay_ms(1)@g	test-sim-mute	mute settle window shortened to 1ms; mute-window timing test catches it"
)

# Files copied into each sandbox (all firmware sources + headers + harness +
# Makefile). Copying the whole source set keeps this robust as variants are
# added or renamed.
copy_tree() {
    local dst="$1"
    mkdir -p "$dst/test"
    cp "$PROJ_DIR"/*.c "$PROJ_DIR"/*.h "$PROJ_DIR/Makefile" "$dst/"
    cp "$PROJ_DIR"/test/*.c "$PROJ_DIR"/test/*.h "$dst/test/"
}

killed=0
survived=0
errored=0
SURVIVORS=()

# Sanity: the unmutated tree must PASS the targets we rely on, otherwise a
# "killed" result is meaningless (it would just mean the baseline is broken).
echo "=== mutation testing: baseline sanity check ==="
BASE_DIR="$(mktemp -d)"
copy_tree "$BASE_DIR"
if make -C "$BASE_DIR" test-sim >/dev/null 2>&1; then
    echo "baseline test-sim: PASS"
else
    echo "ERROR: baseline test-sim FAILS on unmutated tree; aborting." >&2
    rm -rf "$BASE_DIR"
    exit 2
fi
rm -rf "$BASE_DIR"
echo

echo "=== mutation testing: ${#MUTATIONS[@]} mutants ==="
idx=0
for entry in "${MUTATIONS[@]}"; do
    idx=$((idx + 1))
    IFS=$'\t' read -r file sed_expr target desc <<< "$entry"

    work="$(mktemp -d)"
    copy_tree "$work"

    # Apply the mutation; confirm it actually changed the file.
    if ! sed -i "$sed_expr" "$work/$file"; then
        echo "[$idx] ERROR  applying sed to $file: $desc"
        errored=$((errored + 1)); rm -rf "$work"; continue
    fi
    if cmp -s "$work/$file" "$PROJ_DIR/$file"; then
        echo "[$idx] ERROR  mutation did not change $file (pattern stale?): $desc"
        errored=$((errored + 1)); rm -rf "$work"; continue
    fi

    # Run the mapped target. Killed == nonzero exit (build or test failure
    # both count as "the suite did not silently accept the fault").
    if make -C "$work" "$target" >/dev/null 2>&1; then
        echo "[$idx] SURVIVED ($target): $desc"
        survived=$((survived + 1))
        SURVIVORS+=("$file: $desc")
    else
        echo "[$idx] killed   ($target): $desc"
        killed=$((killed + 1))
    fi
    rm -rf "$work"
done

echo
echo "=== mutation summary: $killed killed, $survived survived, $errored errored ==="
if [ "$survived" -ne 0 ]; then
    echo "SURVIVING MUTANTS (test suite gap -- a real fault went undetected):"
    for s in "${SURVIVORS[@]}"; do echo "  - $s"; done
fi
if [ "$survived" -ne 0 ] || [ "$errored" -ne 0 ]; then
    exit 1
fi
echo "all mutants killed: the suite detects every injected fault."
exit 0

