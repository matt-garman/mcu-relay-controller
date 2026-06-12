#!/usr/bin/env bash
#
# Mutation testing for the attiny13_bypass firmware.
#
# WHY THIS EXISTS
# ---------------
# A passing test suite proves the tests PASS on correct code; it does not prove
# the tests would FAIL on broken code. Mutation testing closes that gap: it
# injects a small, deliberate fault ("mutant") into the PRODUCTION sources
# (attiny13_bypass.c / bypass_config.h), rebuilds, and runs a fast test target.
# A correct, adequate suite must DETECT the fault -- the test target must FAIL
# (the mutant is "killed"). A mutant that survives (tests still pass) marks a
# real hole in the suite.
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
"attiny13_bypass.c	s@++debounce_counter_@--debounce_counter_@	test-sim	ISR integrator: increment-on-press becomes decrement (counter never rises -> never toggles)"
"attiny13_bypass.c	s@debounce_counter_ >= PRESSED_THRESH@debounce_counter_ > PRESSED_THRESH@	test-sim	press threshold off-by-one (>= becomes >); lock-step must catch the 1-tick divergence"
"attiny13_bypass.c	s@PORTB |=  (1 << LED_PIN)@PORTB \&= (uint8_t)~(1 << LED_PIN)@	test-sim	set_engaged LED output inverted (lights become dark)"
"attiny13_bypass.c	s@cd4053_pin_set_high@cd4053_pin_set_low@	test-sim	engaged routes CD4053 the wrong way (PB2 stuck low)"
"bypass_config.h	s@#define PRESSED_THRESH (8)@#define PRESSED_THRESH (4)@	test-sim	press threshold shortened 8->4 (timing/noise-count regression)"
"bypass_config.h	s@#define RELEASE_THRESH (25)@#define RELEASE_THRESH (15)@	test-sim	release lock-out shortened 25->15 (noise-count regression)"
)

# Files copied into each sandbox (sources + harness + Makefile).
copy_tree() {
    local dst="$1"
    mkdir -p "$dst/test"
    cp "$PROJ_DIR/attiny13_bypass.c" "$PROJ_DIR/bypass_config.h" "$PROJ_DIR/Makefile" "$dst/"
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
