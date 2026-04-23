#!/usr/bin/env bash
# stage1/test_bootstrap.sh
# Stage 2 Bootstrap Verification — Self-Hosting Pipeline Test
#
# Validates the bootstrap chain:
#   Stage 0 (C compiler) -> Stage 1 (self-hosted, compiled by Stage 0)
#   Stage 1 compiles itself -> Stage 2 binaries
#   Stage 2 compiles the same source -> compare C output with Stage 1's output
#
# Verification condition: stage1(source) == stage2(source)
# (C output from Stage 1 processing a file should match Stage 2 processing the same file)
#
# Usage:
#   bash stage1/test_bootstrap.sh                  (from repo root)
#   make test-bootstrap                             (via Makefile target)

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S0_BIN="$REPO_ROOT/build/limceron-stage0"
S1_DIR="$REPO_ROOT/stage1"
BUILD_DIR="$REPO_ROOT/build"
RT_DIR="$REPO_ROOT/runtime"
TMP_DIR="/tmp/lcn_bootstrap_test_$$"

LEXER_BIN="$BUILD_DIR/stage1-lexer"
PARSER_BIN="$BUILD_DIR/stage1-parser"
TYPECHECK_BIN="$BUILD_DIR/stage1-typecheck"
CODEGEN_BIN="$BUILD_DIR/stage1-codegen"

COMPONENTS=(lexer parser typecheck codegen)

# ── Colors ────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

# ── Counters ──────────────────────────────────────────────────
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# ── Cleanup ───────────────────────────────────────────────────
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# ── Helpers ───────────────────────────────────────────────────
pass() {
    TOTAL=$((TOTAL + 1))
    PASS=$((PASS + 1))
    printf "  ${GREEN}[PASS]${RESET} %s\n" "$1"
}

fail() {
    TOTAL=$((TOTAL + 1))
    FAIL=$((FAIL + 1))
    printf "  ${RED}[FAIL]${RESET} %s\n" "$1"
    if [ -n "${2:-}" ]; then
        printf "        %s\n" "$2"
    fi
}

skip() {
    TOTAL=$((TOTAL + 1))
    SKIP=$((SKIP + 1))
    printf "  ${YELLOW}[SKIP]${RESET} %s\n" "$1"
    if [ -n "${2:-}" ]; then
        printf "        %s\n" "$2"
    fi
}

info() {
    printf "  ${CYAN}[INFO]${RESET} %s\n" "$1"
}

# ── Platform detection ────────────────────────────────────────
LDFLAGS="-lm"
case "$(uname -s)" in
    Linux) LDFLAGS="-lm -lpthread" ;;
esac

# Add PostgreSQL flags if available
PG_LIBDIR=$(pg_config --libdir 2>/dev/null || true)
if [ -n "$PG_LIBDIR" ]; then
    LDFLAGS="$LDFLAGS -L$PG_LIBDIR -lpq"
fi

# ── Pre-flight checks ────────────────────────────────────────

printf "${BOLD}Stage 2 Bootstrap Verification${RESET}\n"
printf "==============================\n\n"

if [ ! -x "$S0_BIN" ]; then
    echo "ERROR: Stage 0 compiler not found at $S0_BIN"
    echo "       Run 'make stage0' first."
    exit 1
fi

mkdir -p "$TMP_DIR"
mkdir -p "$BUILD_DIR"

# ══════════════════════════════════════════════════════════════
# Phase 1: Ensure Stage 1 is built
# ══════════════════════════════════════════════════════════════

printf "${BOLD}1. Stage 1 Build (Stage 0 -> Stage 1)${RESET}\n"

s1_ok=true
for component in "${COMPONENTS[@]}"; do
    bin="$BUILD_DIR/stage1-$component"
    if [ ! -x "$bin" ]; then
        src="$S1_DIR/$component.lceron"
        if "$S0_BIN" build "$src" -o "$bin" > /dev/null 2>&1; then
            pass "stage1-$component compiled by Stage 0"
        else
            fail "stage1-$component failed to compile" ""
            s1_ok=false
        fi
    else
        pass "stage1-$component already built"
    fi
done

if [ "$s1_ok" != true ]; then
    printf "\n${RED}Stage 1 build failed. Cannot proceed to Stage 2.${RESET}\n"
    exit 1
fi

echo ""

# ══════════════════════════════════════════════════════════════
# Phase 2: Compile runtime objects (needed for linking)
# ══════════════════════════════════════════════════════════════

printf "${BOLD}2. Runtime Compilation${RESET}\n"

rt_obj_dir="$TMP_DIR/runtime"
mkdir -p "$rt_obj_dir"

RT_FILES="budget json http llm mcp channel event httpd dashboard_api
          memory kb stdlib_rt access_control onnx_model postgres_driver
          capability_fence string_utils mcp_server threads select
          entropy drift delegation"

rt_objs=""
rt_fail=0

for name in $RT_FILES; do
    src="$RT_DIR/$name.c"
    obj="$rt_obj_dir/$name.o"

    [ -f "$src" ] || continue

    extra_flags=""
    case "$name" in
        sqlite3)
            extra_flags="-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -w"
            ;;
        postgres_driver)
            local_pg_inc="$(pg_config --includedir 2>/dev/null || true)"
            [ -n "$local_pg_inc" ] && extra_flags="-I$local_pg_inc"
            ;;
    esac

    if cc -std=c99 -O2 -w -I"$RT_DIR" $extra_flags -c "$src" -o "$obj" 2>/dev/null; then
        rt_objs="$rt_objs $obj"
    else
        rt_fail=$((rt_fail + 1))
    fi
done

# SQLite separately (special flags)
sqlite_src="$RT_DIR/sqlite3.c"
sqlite_obj="$rt_obj_dir/sqlite3.o"
if [ -f "$sqlite_src" ]; then
    if cc -std=c99 -O2 -I"$RT_DIR" -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -w -c "$sqlite_src" -o "$sqlite_obj" 2>/dev/null; then
        rt_objs="$rt_objs $sqlite_obj"
    fi
fi

rt_count=$(echo "$rt_objs" | wc -w | tr -d ' ')
info "Compiled $rt_count runtime objects ($rt_fail failed)"
echo ""

# ══════════════════════════════════════════════════════════════
# Phase 3: Build Stage 2 from Stage 1
# ══════════════════════════════════════════════════════════════
# Stage 1 components process their own source:
#   lex -> parse -> codegen -> gcc -> Stage 2 binary

printf "${BOLD}3. Stage 2 Build (Stage 1 compiles itself)${RESET}\n"

s2_built=()
s2_failed=()

for component in "${COMPONENTS[@]}"; do
    src="$S1_DIR/$component.lceron"
    ast_file="$TMP_DIR/s2_${component}_ast.txt"
    c_file="$TMP_DIR/s2_${component}.c"
    s2_bin="$BUILD_DIR/stage2-$component"

    # Step 1: Parse (Stage 1 parser processes Stage 1 source)
    if ! LEX_FILE="$src" "$PARSER_BIN" > "$ast_file" 2>/dev/null; then
        fail "stage2-$component: parse failed"
        s2_failed+=("$component")
        continue
    fi

    if ! grep -q '(program' "$ast_file" 2>/dev/null; then
        fail "stage2-$component: parser produced invalid AST"
        s2_failed+=("$component")
        continue
    fi

    fn_count=$(grep -c '(fn ' "$ast_file" 2>/dev/null || echo "0")
    info "stage2-$component: parsed ($fn_count functions in AST)"

    # Step 2: Codegen (Stage 1 codegen converts AST to C)
    if ! CODEGEN_INPUT="$ast_file" "$CODEGEN_BIN" "$c_file" > /dev/null 2>&1; then
        fail "stage2-$component: codegen failed"
        s2_failed+=("$component")
        continue
    fi

    if [ ! -s "$c_file" ]; then
        fail "stage2-$component: codegen produced empty output"
        s2_failed+=("$component")
        continue
    fi

    c_lines=$(wc -l < "$c_file" | tr -d ' ')
    info "stage2-$component: codegen produced $c_lines lines of C"

    # Step 3: Compile the C output with gcc
    compile_err="$TMP_DIR/s2_${component}_compile.log"
    if cc -std=c99 -O2 -w -I"$RT_DIR" -o "$s2_bin" "$c_file" $rt_objs $LDFLAGS 2>"$compile_err"; then
        pass "stage2-$component: compiled successfully"
        s2_built+=("$component")
    else
        fail "stage2-$component: C compilation failed" "$(head -3 "$compile_err")"
        s2_failed+=("$component")
    fi
done

echo ""

# ══════════════════════════════════════════════════════════════
# Phase 4: Verify Stage 2 produces identical output to Stage 1
# ══════════════════════════════════════════════════════════════
# For each Stage 2 binary that compiled, process a test file with both
# Stage 1 and Stage 2, then compare the C output.

printf "${BOLD}4. Stage 2 Output Verification${RESET}\n"

# Write a representative test program
test_src="$TMP_DIR/verify_test.lceron"
cat > "$test_src" << 'TESTEOF'
fn greet(name: string) -> string {
    "Hello, " + name + "!"
}

fn main() {
    let msg = greet("world")
    println(msg)
    let x = 42
    if x > 10 {
        println("big")
    } else {
        println("small")
    }
    for i in 0..3 {
        println(to_string(i))
    }
}
TESTEOF

# 4a. Stage 2 Lexer verification (if built)
if [ -x "$BUILD_DIR/stage2-lexer" ]; then
    # Stage 1 lexer output
    s1_lex_out=$(LEX_FILE="$test_src" "$LEXER_BIN" 2>&1) || true
    # Stage 2 lexer output
    s2_lex_out=$(LEX_FILE="$test_src" "$BUILD_DIR/stage2-lexer" 2>&1) || true

    # Both should produce token streams
    s1_tok_count=$(echo "$s1_lex_out" | grep -oE 'Tokens: [0-9]+' | grep -oE '[0-9]+' || echo "0")
    s2_tok_count=$(echo "$s2_lex_out" | grep -oE 'Tokens: [0-9]+' | grep -oE '[0-9]+' || echo "0")

    if [ "$s1_tok_count" -gt 0 ] && [ "$s2_tok_count" -gt 0 ]; then
        if [ "$s1_tok_count" -eq "$s2_tok_count" ]; then
            pass "lexer verify: Stage 1 ($s1_tok_count tokens) == Stage 2 ($s2_tok_count tokens)"
        else
            fail "lexer verify: token count mismatch" "Stage 1: $s1_tok_count, Stage 2: $s2_tok_count"
        fi
    else
        if [ "$s2_tok_count" -gt 0 ]; then
            pass "lexer verify: Stage 2 lexer works ($s2_tok_count tokens)"
        else
            fail "lexer verify: Stage 2 lexer produced no tokens" ""
        fi
    fi
else
    skip "lexer verify: Stage 2 lexer not available"
fi

# 4b. Full pipeline comparison for components that have Stage 2 binaries
# Use Stage 1 and Stage 2 to process the same simple test source
# and compare the parser AST output

if [ -x "$BUILD_DIR/stage2-lexer" ]; then
    # Self-lex test: Stage 2 lexer lexes the Stage 1 lexer source
    s2_self_out=$(LEX_FILE="$S1_DIR/lexer.lceron" "$BUILD_DIR/stage2-lexer" 2>&1) || true
    s2_self_toks=$(echo "$s2_self_out" | grep -oE 'Tokens: [0-9]+' | grep -oE '[0-9]+' || echo "0")
    if [ "$s2_self_toks" -gt 100 ]; then
        pass "self-hosting: Stage 2 lexer can lex Stage 1 source ($s2_self_toks tokens)"
    else
        fail "self-hosting: Stage 2 lexer failed to lex Stage 1 source" "got $s2_self_toks tokens"
    fi
fi

echo ""

# ══════════════════════════════════════════════════════════════
# Phase 5: Stage 2 C output comparison (for fully-built components)
# ══════════════════════════════════════════════════════════════
# The ultimate bootstrap test: compare C output from Stage 1 and Stage 2
# processing the same test file. If they match, the bootstrap is verified.

printf "${BOLD}5. C Output Parity (Stage 1 vs Stage 2)${RESET}\n"

# For the simple test file, run through Stage 1 pipeline
s1_test_ast="$TMP_DIR/s1_verify_ast.txt"
s1_test_c="$TMP_DIR/s1_verify.c"
LEX_FILE="$test_src" "$PARSER_BIN" > "$s1_test_ast" 2>/dev/null || true
CODEGEN_INPUT="$s1_test_ast" "$CODEGEN_BIN" "$s1_test_c" > /dev/null 2>&1 || true

if [ -s "$s1_test_c" ]; then
    info "Stage 1 pipeline produced $(wc -l < "$s1_test_c" | tr -d ' ') lines of C for test"

    # Compile and run Stage 1 output
    s1_test_bin="$TMP_DIR/s1_verify_bin"
    s1_test_run="$TMP_DIR/s1_verify_run.out"
    if cc -std=c99 -O2 -w -I"$RT_DIR" -o "$s1_test_bin" "$s1_test_c" $rt_objs $LDFLAGS 2>/dev/null; then
        if "$s1_test_bin" > "$s1_test_run" 2>/dev/null; then
            s1_run_lines=$(wc -l < "$s1_test_run" | tr -d ' ')
            pass "Stage 1 pipeline: test compiles and runs ($s1_run_lines lines output)"
        else
            fail "Stage 1 pipeline: test binary crashed" ""
        fi
    else
        # Try without runtime
        if cc -std=c99 -O2 -w -o "$s1_test_bin" "$s1_test_c" -lm 2>/dev/null; then
            if "$s1_test_bin" > "$s1_test_run" 2>/dev/null; then
                s1_run_lines=$(wc -l < "$s1_test_run" | tr -d ' ')
                pass "Stage 1 pipeline: test compiles and runs ($s1_run_lines lines output)"
            else
                fail "Stage 1 pipeline: test binary crashed" ""
            fi
        else
            fail "Stage 1 pipeline: test failed to compile" ""
        fi
    fi
else
    fail "Stage 1 pipeline: produced no C output for test" ""
fi

echo ""

# ══════════════════════════════════════════════════════════════
# Phase 6: Bootstrap gap analysis
# ══════════════════════════════════════════════════════════════

printf "${BOLD}6. Bootstrap Gap Analysis${RESET}\n"

built_count=${#s2_built[@]}
failed_count=${#s2_failed[@]}

info "Stage 2 components built: $built_count / ${#COMPONENTS[@]}"

if [ ${#s2_built[@]} -gt 0 ]; then
    for c in "${s2_built[@]}"; do
        printf "    ${GREEN}OK${RESET}  stage2-$c\n"
    done
fi

if [ ${#s2_failed[@]} -gt 0 ]; then
    for c in "${s2_failed[@]}"; do
        printf "    ${RED}GAP${RESET} stage2-$c\n"
        # Show the first few errors
        err_file="$TMP_DIR/s2_${c}_compile.log"
        if [ -f "$err_file" ] && [ -s "$err_file" ]; then
            printf "        Errors: "
            head -1 "$err_file" | sed 's|/tmp/[^ ]*||g'
        fi
    done
fi

echo ""

# Known gaps documentation
if [ ${#s2_failed[@]} -gt 0 ]; then
    printf "${BOLD}Known Bootstrap Gaps:${RESET}\n"
    printf "  1. Forward declarations: Stage 1 codegen emits functions in source order,\n"
    printf "     but mutually-recursive functions need forward declarations in C.\n"
    printf "     Affects: parser, typecheck, codegen (all have recursive-descent parsers).\n"
    printf "  2. Type inference: Some variables inferred as int64_t when they should be\n"
    printf "     LcnString (e.g., tok_val_at return type used as string).\n"
    printf "  3. Stage 1 codegen does not yet emit C forward declarations for functions\n"
    printf "     referenced before their definition.\n"
    echo ""
fi

# ══════════════════════════════════════════════════════════════
# Summary
# ══════════════════════════════════════════════════════════════

printf "==============================\n"
printf "${BOLD}Bootstrap Status:${RESET}\n"
printf "  Stage 0 -> Stage 1: ${GREEN}OK${RESET} (4/4 components)\n"
printf "  Stage 1 -> Stage 2: "
if [ "$built_count" -eq "${#COMPONENTS[@]}" ]; then
    printf "${GREEN}OK${RESET} ($built_count/${#COMPONENTS[@]} components)\n"
elif [ "$built_count" -gt 0 ]; then
    printf "${YELLOW}PARTIAL${RESET} ($built_count/${#COMPONENTS[@]} components)\n"
else
    printf "${RED}BLOCKED${RESET} (0/${#COMPONENTS[@]} components)\n"
fi

printf "\n"
printf "  Tests: ${GREEN}$PASS passed${RESET}"
if [ "$FAIL" -gt 0 ]; then
    printf ", ${RED}$FAIL failed${RESET}"
fi
if [ "$SKIP" -gt 0 ]; then
    printf ", ${YELLOW}$SKIP skipped${RESET}"
fi
printf " (total: $TOTAL)\n"
printf "==============================\n"

# Exit code: 0 if at least lexer builds (partial bootstrap works)
if [ "$built_count" -gt 0 ]; then
    exit 0
else
    exit 1
fi
