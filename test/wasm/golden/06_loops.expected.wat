;; Golden WAT fixture for examples/wasm/poc/06_loops.lceron
;;
;; L2 deliverable: pin the SHAPE that the emitter must produce for a
;; bounded ReAct loop (`for _ in 0..N` over a host call). The fixture
;; below is a hand-rolled simplification of the real emitter output —
;; it intentionally drops the budget fences, the bump-allocator
;; preamble, and the SSA-register naming churn, but preserves the load-
;; bearing structural invariants that L2 is responsible for:
;;
;;   1. The compiled module contains an outer `(block $exit ...)` and
;;      an inner `(loop $dispatch ...)`, with the basic-block contents
;;      sandwiched between them as `(block $bb_N ...)`.
;;
;;   2. The dispatch table at the top of the body is a `br_table`
;;      indexed by `$bb` that branches to one of {bb_0, bb_1, ..., $exit}.
;;
;;   3. The loop CFG has FOUR basic blocks beyond the entry:
;;        bb1 (for.cond), bb2 (for.body), bb3 (for.inc), bb4 (for.exit).
;;      Anything fewer is missing the back-edge; anything extra is a
;;      lowering bug (continue must NOT jump to cond, it must jump to
;;      the inc block so the loop variable still advances).
;;
;;   4. The cond block ends with `i64.lt_s` + an `if/else` that sets
;;      `$bb` to either the body index or the exit index, followed by
;;      `br $dispatch` (the back-edge).
;;
;;   5. The inc block stores `i + 1` back into the loop-variable slot
;;      and jumps to the cond block (NOT to the body — the
;;      compiler-managed counter advance lives here).
;;
;;   6. The exit block loads the result, returns, and the dispatch
;;      table falls off into `$exit` to close the loop cleanly.
;;
;; Expected runtime (with Visual-DAG MockClient seeded to "ESCALATE"):
;;   main() returns 24.
;;
;; This fixture is validated by `test/wasm/run_wasm_tests.sh` via
;; `wat2wasm` -- it must parse cleanly even though it is not a
;; byte-for-byte match for the emitter output.

(module $loops_06
  (import "vdag:llm" "classify"
    (func $hi_llm_classify (param i32 i32 i32 i32 i32) (result i32)))

  (memory (export "memory") 1)
  (global $bump_ptr (mut i32) (i32.const 9280))
  (data (i32.const 1024) "\12\00\00\00" "react-bounded-step")

  ;; main: returns i64
  ;;   let max_iter = 3
  ;;   let mut total = 0
  ;;   for _ in 0..max_iter { total = total + llm.classify(...)? }
  ;;   total
  (func $main (result i64)
    (local $bb i32)
    (local $max_iter i64)
    (local $total i64)
    (local $i i64)
    (local $len i64)

    i32.const 0
    local.set $bb

    (block $exit
      (loop $dispatch
        (block $bb_4
          (block $bb_3
            (block $bb_2
              (block $bb_1
                (block $bb_0
                  local.get $bb
                  br_table $bb_0 $bb_1 $bb_2 $bb_3 $bb_4 $exit
                )
                ;; ===== bb0 (entry) =====
                ;; max_iter = 3; total = 0; i = 0
                i64.const 3
                local.set $max_iter
                i64.const 0
                local.set $total
                i64.const 0
                local.set $i
                i32.const 1
                local.set $bb
                br $dispatch
              )
              ;; ===== bb1 (for.cond) — i < max_iter ? body : exit =====
              local.get $i
              local.get $max_iter
              i64.lt_s
              (if
                (then i32.const 2 local.set $bb)
                (else i32.const 4 local.set $bb))
              br $dispatch
            )
            ;; ===== bb2 (for.body) — total = total + llm.classify(...) =====
            i32.const 1028           ;; "react-bounded-step" data ptr
            i32.const 18             ;; length
            i32.const 8192           ;; out buf
            i32.const 1024           ;; out cap
            i32.const 9216           ;; status slot
            call $hi_llm_classify
            i64.extend_i32_s
            local.set $len
            ;; L5 `?`: if len < 0 return len
            local.get $len
            i64.const 0
            i64.lt_s
            (if
              (then local.get $len return))
            local.get $total
            local.get $len
            i64.add
            local.set $total
            i32.const 3
            local.set $bb
            br $dispatch
          )
          ;; ===== bb3 (for.inc) — i = i + 1; jmp cond =====
          local.get $i
          i64.const 1
          i64.add
          local.set $i
          i32.const 1
          local.set $bb
          br $dispatch
        )
        ;; ===== bb4 (for.exit) — return total =====
        local.get $total
        return
      ) ;; close loop $dispatch
    )   ;; close block $exit
    i64.const 0
  )

  (export "main" (func $main)))
