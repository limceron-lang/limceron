# Limceron Language — Bootstrap Build System
#
# This Makefile is ONLY used for bootstrapping.
# Once Stage 2 is built, all further builds use `limceron build`.

CC       ?= cc
CFLAGS   := -std=c99 -O2 -Wall -Wextra -Werror -pedantic
LDFLAGS  :=

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    LDFLAGS  += -lm -lpthread
endif
ifeq ($(UNAME_S),Darwin)
    PLATFORM := darwin
    LDFLAGS  += -lm
endif

ifeq ($(UNAME_M),x86_64)
    ARCH := x86_64
endif
ifeq ($(UNAME_M),aarch64)
    ARCH := arm64
endif
ifeq ($(UNAME_M),arm64)
    ARCH := arm64
endif

# Paths
S0_SRC     := src
S0_INC     := include
S0_TEST    := test
S0_CFLAGS  := $(CFLAGS) -I$(S0_INC) -I$(S0_SRC)

# Runtime paths
RT_DIR     := runtime
RT_SRCS    := $(RT_DIR)/budget.c $(RT_DIR)/json.c $(RT_DIR)/http.c \
              $(RT_DIR)/llm.c $(RT_DIR)/mcp.c $(RT_DIR)/channel.c \
              $(RT_DIR)/event.c $(RT_DIR)/httpd.c $(RT_DIR)/dashboard_api.c \
              $(RT_DIR)/memory.c $(RT_DIR)/kb.c $(RT_DIR)/stdlib_rt.c \
              $(RT_DIR)/access_control.c $(RT_DIR)/onnx_model.c \
              $(RT_DIR)/postgres_driver.c \
              $(RT_DIR)/capability_fence.c \
              $(RT_DIR)/string_utils.c $(RT_DIR)/mcp_server.c \
              $(RT_DIR)/threads.c $(RT_DIR)/select.c $(RT_DIR)/green_threads.c \
              $(RT_DIR)/mesh.c \
              $(RT_DIR)/entropy.c $(RT_DIR)/drift.c \
              $(RT_DIR)/delegation.c \
              $(RT_DIR)/supervisor.c \
              $(RT_DIR)/router.c $(RT_DIR)/a2a.c \
              $(RT_DIR)/sqlite3.c
RT_OBJS    := $(patsubst $(RT_DIR)/%.c,build/runtime/%.o,$(RT_SRCS))
RT_CFLAGS  := -std=c99 -O2 -Wall -I$(RT_DIR)

# ONNX Runtime: auto-detect via pkg-config, enable real inference if available
ONNX_CFLAGS  := $(shell pkg-config --cflags libonnxruntime 2>/dev/null)
ONNX_LDFLAGS := $(shell pkg-config --libs libonnxruntime 2>/dev/null)
ifneq ($(ONNX_LDFLAGS),)
  RT_CFLAGS += -DLCN_HAS_ONNXRUNTIME $(ONNX_CFLAGS)
  LDFLAGS   += $(ONNX_LDFLAGS)
endif

# PostgreSQL (libpq): auto-detect via pg_config or pkg-config
PG_CFLAGS  := $(shell pg_config --includedir 2>/dev/null | sed 's/^/-I/' 2>/dev/null)
PG_LDFLAGS := $(shell pg_config --libdir 2>/dev/null | sed 's/^/-L/' 2>/dev/null)
ifeq ($(PG_LDFLAGS),)
  PG_CFLAGS  := $(shell pkg-config --cflags libpq 2>/dev/null)
  PG_LDFLAGS := $(shell pkg-config --libs libpq 2>/dev/null)
endif
ifneq ($(PG_LDFLAGS),)
  RT_CFLAGS += -DLCN_HAS_POSTGRES $(PG_CFLAGS)
  LDFLAGS   += $(PG_LDFLAGS) -lpq
endif

# Source files (order matters for dependencies)
S0_SRCS    := $(S0_SRC)/arena.c $(S0_SRC)/lexer.c $(S0_SRC)/parser.c \
              $(S0_SRC)/markdown.c $(S0_SRC)/typecheck.c $(S0_SRC)/codegen.c \
              $(S0_SRC)/ir_gen.c $(S0_SRC)/ir_emit_arm64.c $(S0_SRC)/ir_emit_x86.c \
              $(S0_SRC)/lsp.c $(S0_SRC)/package.c \
              $(S0_SRC)/target.c $(S0_SRC)/security.c $(S0_SRC)/main.c
S0_OBJS    := $(patsubst $(S0_SRC)/%.c,build/stage0/%.o,$(S0_SRCS))

# Library objects (everything except main.o — for linking with tests)
S0_LIB_OBJS := $(filter-out build/stage0/main.o,$(S0_OBJS))

# Test sources
S0_TEST_SRC    := $(S0_TEST)/test_runner.c
S0_TEST_IR_SRC := $(S0_TEST)/test_ir.c

# Output
S0_BIN     := build/limceron-stage0
TEST_BIN   := build/test-stage0
TEST_IR_BIN := build/test-ir

# Stage 1 & 2
S1_DIR     := stage1
S1_BIN     := build/limceron-stage1
S2_DIR     := stage2
S2_BIN     := build/limceron

BUILD_DIR  := build

# ============================================================
# Targets
# ============================================================

.PHONY: all bootstrap stage0 stage1 stage1-build stage2 stage2-build verify clean install test test-stage0 \
        test-ir test-stage1 test-parity test-multifile test-bootstrap lex parse emit build-lceron run runtime dashboard

all: stage0

# Full bootstrap: Stage 0 (C) -> Stage 1 (self-hosted) -> Stage 2 (self-compiled) -> verify
bootstrap: stage0 stage1-build stage2-build test-bootstrap
	@echo ""
	@echo "=== Bootstrap complete ==="
	@echo "Stage 2 components: build/stage2-*"
	@echo "Platform: $(PLATFORM)-$(ARCH)"
	@echo ""

# -- Stage 0: C bootstrap compiler --

stage0: $(S0_BIN)
	@echo "=== Stage 0 complete ==="

$(S0_BIN): $(S0_OBJS) | $(BUILD_DIR)
	$(CC) -o $@ $^ $(LDFLAGS)
	@xattr -dr com.apple.quarantine $@ 2>/dev/null || true
	@xattr -dr com.apple.provenance $@ 2>/dev/null || true

build/stage0/%.o: $(S0_SRC)/%.c $(S0_INC)/lcn.h $(S0_SRC)/ir.h | build/stage0
	$(CC) $(S0_CFLAGS) -c $< -o $@

# -- Runtime library --

runtime: $(RT_OBJS)
	@echo "=== Runtime compiled ==="

build/runtime/%.o: $(RT_DIR)/%.c $(RT_DIR)/lcn_runtime.h | build/runtime
	$(CC) $(RT_CFLAGS) -c $< -o $@

# SQLite amalgamation needs special flags: suppress warnings, single-threaded
build/runtime/sqlite3.o: $(RT_DIR)/sqlite3.c | build/runtime
	$(CC) -std=c99 -O2 -I$(RT_DIR) -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -w -c $< -o $@

# -- Tests --

test: test-stage0 test-ir test-multifile

test-stage0: $(TEST_BIN)
	@./$(TEST_BIN)

$(TEST_BIN): $(S0_TEST_SRC) $(S0_LIB_OBJS) $(S0_INC)/lcn.h $(S0_TEST)/test.h | $(BUILD_DIR)
	$(CC) $(S0_CFLAGS) -I$(S0_TEST) -o $@ $(S0_TEST_SRC) $(S0_LIB_OBJS) $(LDFLAGS)
	@xattr -dr com.apple.quarantine $@ 2>/dev/null || true
	@xattr -dr com.apple.provenance $@ 2>/dev/null || true

test-ir: $(TEST_IR_BIN)
	@./$(TEST_IR_BIN)

$(TEST_IR_BIN): $(S0_TEST_IR_SRC) $(S0_LIB_OBJS) $(S0_INC)/lcn.h $(S0_SRC)/ir.h $(S0_TEST)/test.h | $(BUILD_DIR)
	$(CC) $(S0_CFLAGS) -I$(S0_TEST) -o $@ $(S0_TEST_IR_SRC) $(S0_LIB_OBJS) $(LDFLAGS)
	@xattr -dr com.apple.quarantine $@ 2>/dev/null || true
	@xattr -dr com.apple.provenance $@ 2>/dev/null || true

# -- Multi-file integration test --

test-multifile: $(S0_BIN)
	@echo "── Multi-file import test ──"
	@./$(S0_BIN) build examples/language/multifile/main.lceron -o /tmp/lcn_multifile_test 2>&1
	@rm -f /tmp/lcn_multifile_test
	@echo "  PASS: multi-file example compiles"

# -- Stage 1 self-hosted compiler tests --

test-stage1: $(S0_BIN)
	@bash stage1/test_stage1.sh

# -- Stage 1 output parity tests (Stage 0 vs Stage 1 codegen) --

test-parity: stage0
	@bash stage1/test_parity.sh

# -- Debug tools --

lex: $(S0_BIN)
	@if [ -z "$(FILE)" ]; then echo "Usage: make lex FILE=path/to/file.lceron"; exit 1; fi
	./$(S0_BIN) lex $(FILE)

parse: $(S0_BIN)
	@if [ -z "$(FILE)" ]; then echo "Usage: make parse FILE=path/to/file.lceron"; exit 1; fi
	./$(S0_BIN) parse $(FILE)

emit: $(S0_BIN)
	@if [ -z "$(FILE)" ]; then echo "Usage: make emit FILE=path/to/file.lceron"; exit 1; fi
	./$(S0_BIN) emit $(FILE) $(if $(OUT),-o $(OUT),)

# -- Build & Run Limceron programs --

build-lceron: $(S0_BIN)
	@if [ -z "$(FILE)" ]; then echo "Usage: make build-lceron FILE=path/to/file.lceron [OUT=binary]"; exit 1; fi
	./$(S0_BIN) build $(FILE) $(if $(OUT),-o $(OUT),-o build/$(notdir $(basename $(FILE))))

run: $(S0_BIN)
	@if [ -z "$(FILE)" ]; then echo "Usage: make run FILE=path/to/file.lceron"; exit 1; fi
	./$(S0_BIN) run $(FILE)

# -- Dashboard server --

dashboard: runtime
	@echo "=== Dashboard runtime compiled ==="
	@echo "Start with: LCN_DASHBOARD=1 ./build/limceron-stage0 run <file>"
	@echo "  or open dashboard/index.html in a browser for mock mode"

# -- Stage 1: compile individual self-hosted components --

stage1-build: stage0
	@echo "=== Building Stage 1 components with Stage 0 ==="
	@$(S0_BIN) build $(S1_DIR)/lexer.lceron -o $(BUILD_DIR)/stage1-lexer 2>&1 | tail -1
	@$(S0_BIN) build $(S1_DIR)/parser.lceron -o $(BUILD_DIR)/stage1-parser 2>&1 | tail -1
	@$(S0_BIN) build $(S1_DIR)/typecheck.lceron -o $(BUILD_DIR)/stage1-typecheck 2>&1 | tail -1
	@$(S0_BIN) build $(S1_DIR)/codegen.lceron -o $(BUILD_DIR)/stage1-codegen 2>&1 | tail -1
	@cp $(S1_DIR)/limceron-stage1.sh $(BUILD_DIR)/limceron-stage1.sh
	@chmod +x $(BUILD_DIR)/limceron-stage1.sh
	@echo "=== Stage 1 build complete ==="

# -- Stage 2: Stage 1 compiles itself --
# Uses Stage 1 components (built by Stage 0) to compile Stage 1 source.
# Pipeline: lex -> parse -> codegen -> gcc -> Stage 2 binaries

stage2-build: stage1-build runtime
	@echo "=== Building Stage 2 (Stage 1 compiling itself) ==="
	@mkdir -p /tmp/lcn_s2_build
	@s2_ok=0; s2_fail=0; \
	for component in lexer parser typecheck codegen; do \
		src=$(S1_DIR)/$$component.lceron; \
		ast=/tmp/lcn_s2_build/s2_$${component}_ast.txt; \
		c_out=/tmp/lcn_s2_build/s2_$${component}.c; \
		bin=$(BUILD_DIR)/stage2-$$component; \
		LEX_FILE="$$src" $(BUILD_DIR)/stage1-parser > "$$ast" 2>/dev/null || true; \
		if ! grep -q '(program' "$$ast" 2>/dev/null; then \
			echo "  SKIP: stage2-$$component (parse failed)"; \
			s2_fail=$$((s2_fail + 1)); \
			continue; \
		fi; \
		CODEGEN_INPUT="$$ast" $(BUILD_DIR)/stage1-codegen "$$c_out" > /dev/null 2>&1 || true; \
		if [ ! -s "$$c_out" ]; then \
			echo "  SKIP: stage2-$$component (codegen failed)"; \
			s2_fail=$$((s2_fail + 1)); \
			continue; \
		fi; \
		if $(CC) -std=c99 -O2 -w -I$(RT_DIR) -o "$$bin" "$$c_out" $(RT_OBJS) $(LDFLAGS) 2>/dev/null; then \
			echo "  OK:   stage2-$$component"; \
			s2_ok=$$((s2_ok + 1)); \
		else \
			echo "  SKIP: stage2-$$component (C compilation failed)"; \
			s2_fail=$$((s2_fail + 1)); \
		fi; \
	done; \
	echo "=== Stage 2: $$s2_ok built, $$s2_fail skipped ==="
	@rm -rf /tmp/lcn_s2_build

# -- Bootstrap verification test --
test-bootstrap: stage1-build runtime
	@bash stage1/test_bootstrap.sh

# -- Legacy full-binary bootstrap (future: when all components self-compile) --

stage1: stage0
	@echo "=== Building Stage 1 with Stage 0 ==="
	$(S0_BIN) build $(S1_DIR)/src -o $(S1_BIN)

stage2: stage1
	@echo "=== Building Stage 2 with Stage 1 ==="
	$(S1_BIN) build $(S2_DIR)/src -o $(S2_BIN)

verify: stage2
	@echo "=== Verifying bootstrap ==="
	$(S2_BIN) build $(S2_DIR)/src -o $(S2_BIN)-verify
	@if diff $(S2_BIN) $(S2_BIN)-verify > /dev/null 2>&1; then \
		echo "=== Bootstrap verification PASSED ==="; \
		rm $(S2_BIN)-verify; \
	else \
		echo "=== Bootstrap verification FAILED ==="; \
		exit 1; \
	fi

# -- Utilities --

install: bootstrap
	install -m 755 $(S2_BIN) /usr/local/bin/limceron

clean:
	rm -rf build
	rm -rf .lceron-cache
	rm -f /tmp/lcn_*.c /tmp/lcn_rt_*.o /tmp/lcn_run_*
	rm -rf /tmp/lcn_s2_build /tmp/lcn_bootstrap_test_* /tmp/lcn_rt_stage1

# -- Directory creation --

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

build/stage0:
	mkdir -p build/stage0

build/runtime:
	mkdir -p build/runtime
