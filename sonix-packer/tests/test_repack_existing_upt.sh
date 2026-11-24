#!/usr/bin/env bash

# SPDX-License-Identifier: GPL-3.0-only
# Negative integration tests for the HiBy R1 UPT repacker.

set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 ORIGINAL_UPT PLAYER WORKDIR"
    exit 2
fi

INPUT="$(realpath "$1")"
PLAYER="$(realpath "$2")"
WORK="$(realpath "$3")"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKER="$HERE/../repack_existing_upt.py"

TESTDIR="$(mktemp -d "$WORK/repacker-tests.XXXXXX")"

trap 'rm -rf -- "$TESTDIR"' EXIT

PASSED=0
FAILED=0

expect_failure() {
    local name="$1"
    shift

    if python3 "$PACKER" "$@" \
        --model R1 \
        --workdir "$WORK" \
        --dry-run \
        > "$TESTDIR/$name.log" 2>&1
    then
        echo "[FAIL] $name: unexpectedly accepted"
        FAILED=$((FAILED + 1))
    else
        if grep -q '^FAIL:' "$TESTDIR/$name.log"; then
            echo "[PASS] $name: correctly rejected"
            grep '^FAIL:' "$TESTDIR/$name.log"
            PASSED=$((PASSED + 1))
        else
            echo "[FAIL] $name: unexpected error"
            cat "$TESTDIR/$name.log"
            FAILED=$((FAILED + 1))
        fi
    fi
}

# Test 1: corrupted firmware copy.

cp "$INPUT" "$TESTDIR/corrupted.upt"

python3 - "$TESTDIR/corrupted.upt" <<'PY'
import sys

with open(sys.argv[1], "r+b") as f:
    f.seek(100000)
    original = f.read(1)
    assert original
    f.seek(100000)
    f.write(bytes([original[0] ^ 0xFF]))
PY

expect_failure corrupted_firmware \
    --input "$TESTDIR/corrupted.upt" \
    --player "$PLAYER" \
    --output "$TESTDIR/result1.upt"

# Test 2: wrong executable architecture.

cp /bin/true "$TESTDIR/wrong-player"

expect_failure wrong_architecture \
    --input "$INPUT" \
    --player "$TESTDIR/wrong-player" \
    --output "$TESTDIR/result2.upt"

# Test 3: input and output are the same file.
# Use a disposable copy, never the original firmware.

cp "$INPUT" "$TESTDIR/same-file.upt"

BEFORE="$(sha256sum "$TESTDIR/same-file.upt")"

expect_failure same_input_output \
    --input "$TESTDIR/same-file.upt" \
    --player "$PLAYER" \
    --output "$TESTDIR/same-file.upt"

AFTER="$(sha256sum "$TESTDIR/same-file.upt")"

if [ "$BEFORE" = "$AFTER" ]; then
    echo "[PASS] Input file remained unchanged"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] Input file was modified"
    FAILED=$((FAILED + 1))
fi

# Test 4: existing output.

printf 'DO NOT OVERWRITE\n' > "$TESTDIR/existing.upt"

expect_failure existing_output \
    --input "$INPUT" \
    --player "$PLAYER" \
    --output "$TESTDIR/existing.upt"

if [ "$(cat "$TESTDIR/existing.upt")" = "DO NOT OVERWRITE" ]; then
    echo "[PASS] Existing output remained unchanged"
    PASSED=$((PASSED + 1))
else
    echo "[FAIL] Existing output was modified"
    FAILED=$((FAILED + 1))
fi

echo
echo "=== TEST SUMMARY ==="
echo "Passed: $PASSED"
echo "Failed: $FAILED"

[ "$FAILED" -eq 0 ]

echo "ALL REGRESSION TESTS PASSED"
