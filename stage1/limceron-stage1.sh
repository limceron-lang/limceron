#!/usr/bin/env bash
# stage1/limceron-stage1.sh
# Limceron Stage 1 Compiler Driver
#
# Orchestrates the Stage 1 pipeline: lex -> parse -> typecheck -> codegen -> cc
# Equivalent to what main.c does for Stage 0, but using the self-hosted components.
#
# Usage:
#   limceron-stage1.sh build <source.lceron> [-o <output>]
#   limceron-stage1.sh emit  <source.lceron> [-o <output.c>]
#   limceron-stage1.sh run   <source.lceron>

set -euo pipefail

STAGE1_DIR="$(cd "$(dirname "$0")" && pwd)"

# Resolve project root: walk up from script location until we find the Makefile
_dir="$STAGE1_DIR"
ROOT_DIR=""
for _i in 1 2 3 4 5; do
    if [ -f "$_dir/Makefile" ] && [ -d "$_dir/src" ]; then
        ROOT_DIR="$_dir"
        break
    fi
    _dir="$(cd "$_dir/.." && pwd)"
done
[ -n "$ROOT_DIR" ] || { echo "error: cannot find project root from $STAGE1_DIR" >&2; exit 1; }

RT_DIR="$ROOT_DIR/runtime"
BUILD_DIR="$ROOT_DIR/build"

LEXER_BIN="$BUILD_DIR/stage1-lexer"
PARSER_BIN="$BUILD_DIR/stage1-parser"
TYPECHECK_BIN="$BUILD_DIR/stage1-typecheck"
CODEGEN_BIN="$BUILD_DIR/stage1-codegen"

# ── Helpers ──────────────────────────────────────────────────

die() { echo "error: $1" >&2; exit 1; }

check_binaries() {
    for bin in "$LEXER_BIN" "$PARSER_BIN" "$TYPECHECK_BIN" "$CODEGEN_BIN"; do
        [ -x "$bin" ] || die "$(basename "$bin") not found. Run 'make stage1-build' first."
    done
}

usage() {
    echo "Limceron Stage 1 Compiler"
    echo ""
    echo "Usage:"
    echo "  $(basename "$0") build <source.lceron> [-o <output>]"
    echo "  $(basename "$0") emit  <source.lceron> [-o <output.c>]"
    echo "  $(basename "$0") run   <source.lceron>"
    exit 1
}

# ── Runtime Compilation ──────────────────────────────────────

RT_FILES="budget json http llm mcp channel threads select event httpd
          dashboard_api memory kb string_utils mcp_server stdlib_rt sqlite3
          access_control entropy drift onnx_model capability_fence delegation
          postgres_driver"

compile_runtime() {
    local rt_objs=""
    mkdir -p /tmp/lcn_rt_stage1

    for name in $RT_FILES; do
        local src="$RT_DIR/$name.c"
        local obj="/tmp/lcn_rt_stage1/$name.o"

        [ -f "$src" ] || continue

        # Skip recompilation if object is newer than source
        if [ -f "$obj" ] && [ "$obj" -nt "$src" ]; then
            rt_objs="$rt_objs $obj"
            continue
        fi

        local extra_flags=""
        case "$name" in
            sqlite3)
                extra_flags="-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -w"
                ;;
            mysql_driver)
                extra_flags="-I/opt/homebrew/opt/mysql-client/include -I/usr/include/mysql"
                ;;
            postgres_driver)
                local pg_inc
                pg_inc="$(pg_config --includedir 2>/dev/null || true)"
                [ -n "$pg_inc" ] && extra_flags="-I$pg_inc"
                ;;
        esac

        if cc -std=c99 -O2 -Wall -I"$RT_DIR" $extra_flags -c "$src" -o "$obj" 2>/dev/null; then
            rt_objs="$rt_objs $obj"
        else
            echo "  warning: failed to compile runtime $name.c (skipping)" >&2
        fi
    done

    echo "$rt_objs"
}

# ── Pipeline ─────────────────────────────────────────────────

# Run the Stage 1 pipeline: lex -> parse -> typecheck -> codegen
# Produces a .c file at $TMP_C
run_pipeline() {
    local source="$1"
    local tmp_ast="/tmp/lcn_s1_ast_$$.txt"
    local tmp_c="$2"

    [ -f "$source" ] || die "source file not found: $source"

    echo "Limceron Stage 1 — Compiling: $source" >&2

    # Step 1: Typecheck (lex + parse + check internally)
    local tc_out
    tc_out=$(LEX_FILE="$source" "$TYPECHECK_BIN" 2>&1) || true
    if echo "$tc_out" | grep -q 'ERRORS:'; then
        echo "  Type check: FAILED" >&2
        echo "$tc_out" >&2
        rm -f "$tmp_ast"
        return 1
    fi
    echo "  Type check: OK" >&2

    # Step 2: Parse (produces S-expression AST)
    LEX_FILE="$source" "$PARSER_BIN" > "$tmp_ast" 2>/dev/null || true

    if ! grep -q '(program' "$tmp_ast" 2>/dev/null; then
        echo "  Parse: FAILED (no AST produced)" >&2
        rm -f "$tmp_ast"
        return 1
    fi
    echo "  Parsed: $(grep -c '(fn ' "$tmp_ast" || echo 0) functions" >&2

    # Step 3: Codegen (AST -> C99)
    CODEGEN_INPUT="$tmp_ast" "$CODEGEN_BIN" "$tmp_c" > /dev/null 2>&1 || true

    if [ ! -f "$tmp_c" ]; then
        echo "  Codegen: FAILED (no C file produced)" >&2
        rm -f "$tmp_ast"
        return 1
    fi
    echo "  Codegen: $tmp_c" >&2

    rm -f "$tmp_ast"
    return 0
}

# ── Commands ─────────────────────────────────────────────────

cmd_emit() {
    local source="$1"
    local output="${2:-}"
    local tmp_c="/tmp/lcn_s1_emit_$$.c"

    run_pipeline "$source" "$tmp_c" || exit 1

    if [ -n "$output" ]; then
        mv "$tmp_c" "$output"
        echo "  Output: $output" >&2
    else
        cat "$tmp_c"
        rm -f "$tmp_c"
    fi
}

cmd_build() {
    local source="$1"
    local output="${2:-}"
    local tmp_c="/tmp/lcn_s1_build_$$.c"

    # Default output name: strip path and extension
    if [ -z "$output" ]; then
        output="$(basename "$source" .lceron)"
    fi

    run_pipeline "$source" "$tmp_c" || exit 1

    # Check if the generated C needs the runtime
    local needs_rt=false
    if grep -q '#include "lcn_runtime.h"' "$tmp_c" 2>/dev/null; then
        needs_rt=true
    fi

    local rt_objs=""
    if [ "$needs_rt" = true ]; then
        echo "  Runtime: compiling..." >&2
        rt_objs=$(compile_runtime)
        echo "  Runtime: ready" >&2
    fi

    # Platform-specific link flags
    local ldflags="-lm"
    case "$(uname -s)" in
        Linux) ldflags="-lm -lpthread" ;;
    esac

    # Compile and link
    local cc_cmd="cc -std=c99 -O2 -Wall -Wno-unused-function -Wno-unused-variable"
    if [ "$needs_rt" = true ]; then
        cc_cmd="$cc_cmd -I\"$RT_DIR\" \"$tmp_c\" $rt_objs -o \"$output\" $ldflags"
    else
        cc_cmd="$cc_cmd \"$tmp_c\" -o \"$output\" $ldflags"
    fi

    if ! eval "$cc_cmd" 2>&1; then
        echo "error: C compilation failed" >&2
        echo "  command: $cc_cmd" >&2
        rm -f "$tmp_c"
        exit 1
    fi

    rm -f "$tmp_c"
    echo "  Output: $output" >&2
    echo "  Build complete." >&2
}

cmd_run() {
    local source="$1"
    local tmp_bin="/tmp/lcn_s1_run_$$"

    cmd_build "$source" "$tmp_bin"

    echo "" >&2
    echo "--- Running $source ---" >&2
    echo "" >&2
    "$tmp_bin"
    local rc=$?
    rm -f "$tmp_bin"
    return $rc
}

# ── Main ─────────────────────────────────────────────────────

[ $# -ge 1 ] || usage

COMMAND="$1"
shift

check_binaries

case "$COMMAND" in
    build)
        [ $# -ge 1 ] || die "build requires a source file"
        SOURCE="$1"; shift
        OUTPUT=""
        if [ $# -ge 2 ] && [ "$1" = "-o" ]; then
            OUTPUT="$2"; shift 2
        fi
        cmd_build "$SOURCE" "$OUTPUT"
        ;;
    emit)
        [ $# -ge 1 ] || die "emit requires a source file"
        SOURCE="$1"; shift
        OUTPUT=""
        if [ $# -ge 2 ] && [ "$1" = "-o" ]; then
            OUTPUT="$2"; shift 2
        fi
        cmd_emit "$SOURCE" "$OUTPUT"
        ;;
    run)
        [ $# -ge 1 ] || die "run requires a source file"
        cmd_run "$1"
        ;;
    *)
        die "unknown command: $COMMAND (use build, emit, or run)"
        ;;
esac
