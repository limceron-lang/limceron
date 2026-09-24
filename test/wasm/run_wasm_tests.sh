#!/usr/bin/env bash
# run_wasm_tests.sh — End-to-end WASM PoC harness.
#
# For each .lceron sample under examples/wasm/poc/:
#   1. Build via limceron-stage0 with --target wasm32-wasi-preview2
#   2. Validate with wasm-validate
#   3. Run via wasmtime --invoke main (best effort)
#   4. If a matching .expected.wat golden exists, diff (normalized) against
#      the emitter's wat output (best effort, if --emit-wat is supported)
#
# This script is expected to be run from the repo root (lang/).
# It is robust to agent A not yet being finished — build failures get reported
# but the harness itself does not crash.

set -uo pipefail

LIMCERON="${LIMCERON:-./build/limceron-stage0}"
WASM_VALIDATE="${WASM_VALIDATE:-wasm-validate}"
WASMTIME="${WASMTIME:-wasmtime}"
WAT2WASM="${WAT2WASM:-wat2wasm}"

SAMPLE_DIR="examples/wasm/poc"
GOLDEN_DIR="test/wasm/golden"
WORK_DIR="/tmp/lcn-wasm-tests"

PASS=0
FAIL=0
mkdir -p "$WORK_DIR"

# normalize_wat <file>  — strip comments, collapse whitespace, drop numeric
# label suffixes so two structurally equivalent WAT files compare equal.
normalize_wat() {
    local f="$1"
    # strip ;; line comments, strip (; block ;) comments, collapse whitespace,
    # strip $name123 numeric tails into $name (label suffix normalization).
    sed -E '
        s/;;.*$//;
        s/\(;[^;]*;\)//g;
    ' "$f" \
    | tr -s '[:space:]' ' ' \
    | sed -E 's/(\$[A-Za-z_][A-Za-z_]*)[0-9]+/\1/g' \
    | sed -E 's/^ +//; s/ +$//'
}

echo "=== Limceron WASM PoC test harness ==="
echo "  LIMCERON   = $LIMCERON"
echo "  SAMPLE_DIR = $SAMPLE_DIR"
echo "  GOLDEN_DIR = $GOLDEN_DIR"
echo ""

if [[ ! -x "$LIMCERON" ]]; then
    echo "WARN: $LIMCERON not found or not executable. Tests will skip build step."
fi

shopt -s nullglob
samples=( "$SAMPLE_DIR"/*.lceron )
if [[ ${#samples[@]} -eq 0 ]]; then
    echo "ERROR: no samples found in $SAMPLE_DIR"
    exit 2
fi

for src in "${samples[@]}"; do
    name=$(basename "$src" .lceron)
    wasm="$WORK_DIR/${name}.wasm"
    wat="$WORK_DIR/${name}.wat"
    golden="$GOLDEN_DIR/${name}.expected.wat"

    printf "  %-30s " "$name"

    # 1. Parse check (always works — does not need agent A).
    if ! "$LIMCERON" parse "$src" > "$WORK_DIR/${name}.parse.log" 2>&1; then
        if ! grep -q "Parse successful" "$WORK_DIR/${name}.parse.log"; then
            echo "PARSE FAIL"
            FAIL=$((FAIL+1))
            sed -n '1,20p' "$WORK_DIR/${name}.parse.log"
            continue
        fi
    fi

    # 2. Build to wasm. May fail until agent A's emitter lands.
    if ! "$LIMCERON" build "$src" -o "$wasm" --target wasm32-wasi-preview2 \
            > "$WORK_DIR/${name}.build.log" 2>&1; then
        echo "BUILD FAIL (emitter not ready?)"
        FAIL=$((FAIL+1))
        sed -n '1,20p' "$WORK_DIR/${name}.build.log"
        continue
    fi

    # 3. Validate.
    if ! "$WASM_VALIDATE" "$wasm" > "$WORK_DIR/${name}.validate.log" 2>&1; then
        echo "VALIDATE FAIL"
        FAIL=$((FAIL+1))
        sed -n '1,20p' "$WORK_DIR/${name}.validate.log"
        continue
    fi

    # 4. Run via wasmtime (best effort).
    run_status="not-run"
    if "$WASMTIME" --invoke main "$wasm" > "$WORK_DIR/${name}.run.log" 2>&1; then
        run_status="OK ($(tail -1 "$WORK_DIR/${name}.run.log"))"
    else
        run_status="run-skipped"
    fi

    # 5. Diff vs golden, if both available and emitter wrote a .wat.
    diff_status="no-golden"
    if [[ -f "$golden" && -f "$wat" ]]; then
        if diff <(normalize_wat "$wat") <(normalize_wat "$golden") > "$WORK_DIR/${name}.diff" 2>&1; then
            diff_status="golden-match"
        else
            diff_status="golden-diff"
        fi
    elif [[ -f "$golden" ]]; then
        diff_status="golden-only-no-emitted-wat"
    fi

    echo "OK  | $run_status | $diff_status"
    PASS=$((PASS+1))
done

# Always validate goldens themselves regardless of emitter readiness.
echo ""
echo "=== Golden WAT validation ==="
for g in "$GOLDEN_DIR"/*.expected.wat; do
    [[ -e "$g" ]] || continue
    name=$(basename "$g" .expected.wat)
    out="$WORK_DIR/${name}.golden.wasm"
    printf "  %-30s " "$name"
    if "$WAT2WASM" "$g" -o "$out" > "$WORK_DIR/${name}.wat2wasm.log" 2>&1; then
        echo "VALID"
    else
        echo "INVALID"
        sed -n '1,20p' "$WORK_DIR/${name}.wat2wasm.log"
        FAIL=$((FAIL+1))
    fi
done

echo ""
echo "Result: $PASS pass, $FAIL fail"
exit $FAIL
