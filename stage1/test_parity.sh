#!/usr/bin/env bash
# stage1/test_parity.sh
# Output parity test: verifies Stage 1 codegen produces functionally equivalent
# programs to Stage 0 codegen.
#
# For each test program:
#   1. Build+run with Stage 0:  ./build/limceron-stage0 run <file>
#   2. Build+run with Stage 1:  lex -> parse -> codegen -> gcc -> run
#   3. Compare stdout output
#
# Usage:
#   bash stage1/test_parity.sh                   (from repo root)
#   make test-parity                              (via Makefile target)

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S0_BIN="$REPO_ROOT/build/limceron-stage0"
S1_DIR="$REPO_ROOT/stage1"
BUILD_DIR="$REPO_ROOT/build"
TMP_DIR="/tmp/parity_tests_$$"

LEXER_BIN="$BUILD_DIR/stage1-lexer"
PARSER_BIN="$BUILD_DIR/stage1-parser"
CODEGEN_BIN="$BUILD_DIR/stage1-codegen"

# ── Colors ────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
RESET='\033[0m'

# ── Counters ──────────────────────────────────────────────────
MATCH=0
DIFF=0
KNOWN_GAP=0
TOTAL=0

# ── Known gaps (tests expected to differ) ─────────────────────
declare -a KNOWN_GAPS=()

is_known_gap() {
    local name="$1"
    for gap in "${KNOWN_GAPS[@]}"; do
        if [ "$gap" = "$name" ]; then
            return 0
        fi
    done
    return 1
}

# ── Cleanup ───────────────────────────────────────────────────
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# ── Helpers ───────────────────────────────────────────────────

report_match() {
    TOTAL=$((TOTAL + 1))
    MATCH=$((MATCH + 1))
    printf "  ${GREEN}[MATCH]${RESET} %-28s %s\n" "$1" "— $2"
}

report_diff() {
    local name="$1"
    local reason="$2"
    TOTAL=$((TOTAL + 1))
    if is_known_gap "$name"; then
        KNOWN_GAP=$((KNOWN_GAP + 1))
        printf "  ${YELLOW}[DIFF]${RESET}  %-28s %s ${YELLOW}(known gap)${RESET}\n" "${name}.lceron" "— $reason"
    else
        DIFF=$((DIFF + 1))
        printf "  ${RED}[DIFF]${RESET}  %-28s %s\n" "${name}.lceron" "— $reason"
    fi
}

# ── Pre-flight ────────────────────────────────────────────────

if [ ! -x "$S0_BIN" ]; then
    echo "ERROR: Stage 0 compiler not found at $S0_BIN"
    echo "       Run 'make stage0' first."
    exit 1
fi

mkdir -p "$TMP_DIR"
mkdir -p "$BUILD_DIR"

# ── Build Stage 1 components if needed ────────────────────────

needs_build=false
for component in lexer parser codegen; do
    if [ ! -x "$BUILD_DIR/stage1-$component" ]; then
        needs_build=true
        break
    fi
done

if [ "$needs_build" = true ]; then
    printf "${BOLD}Compiling Stage 1 components...${RESET}\n"
    for component in lexer parser codegen; do
        src="$S1_DIR/$component.lceron"
        bin="$BUILD_DIR/stage1-$component"
        if [ ! -x "$bin" ]; then
            if "$S0_BIN" build "$src" -o "$bin" > /dev/null 2>&1; then
                printf "  compiled: stage1-%s\n" "$component"
            else
                printf "  ${RED}FAILED${RESET}: stage1-%s\n" "$component"
                echo "ERROR: Cannot compile stage1-$component. Aborting."
                exit 1
            fi
        fi
    done
    echo ""
fi

# Verify Stage 1 binaries exist
for component in lexer parser codegen; do
    if [ ! -x "$BUILD_DIR/stage1-$component" ]; then
        echo "ERROR: stage1-$component not found. Run 'make stage1-build' first."
        exit 1
    fi
done

# ── Write test programs ──────────────────────────────────────

write_test() {
    local name="$1"
    local content="$2"
    printf '%s' "$content" > "$TMP_DIR/$name.lceron"
}

# 1. hello — basic println
write_test "hello" 'fn main() {
    println("hello")
}
'

# 2. math — function with parameters and arithmetic
write_test "math" 'fn add(a: int, b: int) -> int {
    a + b
}

fn main() {
    println(to_string(add(3, 4)))
}
'

# 3. if_else — branching logic
write_test "if_else" 'fn classify(n: int) -> string {
    if n > 0 {
        return "positive"
    } else if n == 0 {
        return "zero"
    } else {
        return "negative"
    }
}

fn main() {
    println(classify(42))
    println(classify(0))
}
'

# 4. for_loop — iteration
write_test "for_loop" 'fn main() {
    for i in 0..5 {
        println(to_string(i))
    }
}
'

# 5. struct_init — struct definition and field access
write_test "struct_init" 'struct Point {
    x: int
    y: int
}

fn main() {
    let p = Point { x: 10, y: 20 }
    println(to_string(p.x))
}
'

# 6. string_ops — string concatenation and length
write_test "string_ops" 'fn main() {
    let a = "hello"
    let b = " world"
    let c = a + b
    println(c)
    println(to_string(str_len(a)))
}
'

# 7. enum — enum definition
write_test "enum" 'enum Color {
    Red
    Green
    Blue
}

fn main() {
    println("enum test")
}
'

# 8. multi_fn — multiple functions calling each other
write_test "multi_fn" 'fn double(n: int) -> int {
    n * 2
}

fn triple(n: int) -> int {
    n * 3
}

fn main() {
    let a = double(5)
    let b = triple(5)
    let c = double(triple(2))
    println(to_string(a))
    println(to_string(b))
    println(to_string(c))
}
'

# 9. let_bindings — typed let bindings
write_test "let_bindings" 'fn main() {
    let x: int = 42
    let name: string = "limceron"
    let flag: bool = true
    println(to_string(x))
    println(name)
}
'

# 10. bool_logic — boolean operators
write_test "bool_logic" 'fn check(a: bool, b: bool) -> bool {
    a && b
}

fn main() {
    let x = true
    let y = false
    let r = !x
    println(to_string(check(true, true)))
}
'

# ── Run parity tests ─────────────────────────────────────────

printf "${BOLD}Stage 1 Output Parity Tests${RESET}\n"
printf "===========================\n"
printf "  Comparing: Stage 0 run output vs Stage 1 (lex→parse→codegen→gcc→run)\n\n"

TEST_NAMES=(hello math if_else for_loop struct_init string_ops enum multi_fn let_bindings bool_logic)

for name in "${TEST_NAMES[@]}"; do
    src="$TMP_DIR/$name.lceron"

    # -- Stage 0: build then run --
    s0_bin="$TMP_DIR/${name}_s0_bin"
    s0_out="$TMP_DIR/${name}_s0.out"
    if ! "$S0_BIN" build "$src" -o "$s0_bin" > /dev/null 2>&1; then
        report_diff "$name" "Stage 0 build failed"
        continue
    fi
    if ! "$s0_bin" > "$s0_out" 2>/dev/null; then
        report_diff "$name" "Stage 0 binary crashed at runtime"
        continue
    fi

    # -- Stage 1: lex -> parse -> codegen -> gcc -> run --
    s1_ast="$TMP_DIR/${name}_s1_ast.txt"
    s1_c="$TMP_DIR/${name}_s1.c"
    s1_bin="$TMP_DIR/${name}_s1_bin"
    s1_out="$TMP_DIR/${name}_s1.out"

    # Step 1: Parse (lexer is called internally by parser via LEX_FILE)
    if ! LEX_FILE="$src" "$PARSER_BIN" > "$s1_ast" 2>/dev/null; then
        report_diff "$name" "Stage 1 parser failed"
        continue
    fi

    if ! grep -q '(program' "$s1_ast" 2>/dev/null; then
        report_diff "$name" "Stage 1 parser produced invalid AST"
        continue
    fi

    # Step 2: Codegen
    if ! CODEGEN_INPUT="$s1_ast" "$CODEGEN_BIN" "$s1_c" > /dev/null 2>&1; then
        report_diff "$name" "Stage 1 codegen failed"
        continue
    fi

    if [ ! -s "$s1_c" ]; then
        report_diff "$name" "Stage 1 codegen produced empty output"
        continue
    fi

    # Step 3: Compile with gcc
    if ! cc -std=c99 -O2 -w -o "$s1_bin" "$s1_c" -lm 2>/dev/null; then
        report_diff "$name" "Stage 1 C output failed to compile with gcc"
        continue
    fi

    # Step 4: Run
    if ! "$s1_bin" > "$s1_out" 2>/dev/null; then
        report_diff "$name" "Stage 1 binary crashed at runtime"
        continue
    fi

    # -- Compare stdout --
    if diff -q "$s0_out" "$s1_out" > /dev/null 2>&1; then
        # Determine descriptive reason
        lines=$(wc -l < "$s0_out" | tr -d ' ')
        case "$name" in
            hello)        reason="stdout identical ($lines lines)" ;;
            math)         reason="arithmetic output matches ($lines lines)" ;;
            if_else)      reason="branching output matches ($lines lines)" ;;
            for_loop)     reason="loop output matches ($lines lines)" ;;
            struct_init)  reason="struct field access matches ($lines lines)" ;;
            string_ops)   reason="string operations match ($lines lines)" ;;
            enum)         reason="enum output matches ($lines lines)" ;;
            multi_fn)     reason="multi-function output matches ($lines lines)" ;;
            let_bindings) reason="let binding output matches ($lines lines)" ;;
            bool_logic)   reason="boolean logic output matches ($lines lines)" ;;
            *)            reason="stdout identical ($lines lines)" ;;
        esac
        report_match "${name}.lceron" "$reason"
    else
        # Show the actual diff for debugging
        s0_content=$(cat "$s0_out" 2>/dev/null || echo "(empty)")
        s1_content=$(cat "$s1_out" 2>/dev/null || echo "(empty)")
        report_diff "$name" "output differs: S0='$(head -1 "$s0_out")...' S1='$(head -1 "$s1_out")...'"
    fi
done

# ── Summary ───────────────────────────────────────────────────

printf "\n===========================\n"
if [ "$DIFF" -eq 0 ]; then
    printf "${GREEN}${BOLD}Results: %d/%d matched" "$MATCH" "$TOTAL"
    if [ "$KNOWN_GAP" -gt 0 ]; then
        printf ", %d known gaps" "$KNOWN_GAP"
    fi
    printf "${RESET}\n"
else
    printf "${RED}${BOLD}Results: %d/%d matched, %d unexpected diffs" "$MATCH" "$TOTAL" "$DIFF"
    if [ "$KNOWN_GAP" -gt 0 ]; then
        printf ", %d known gaps" "$KNOWN_GAP"
    fi
    printf "${RESET}\n"
fi
printf "===========================\n"

# Exit 0 if all unexpected diffs are zero (known gaps are acceptable)
if [ "$DIFF" -gt 0 ]; then
    exit 1
fi

exit 0
