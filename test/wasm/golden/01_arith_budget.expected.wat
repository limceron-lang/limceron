;; Golden WAT fixture for examples/wasm/poc/01_arith_budget.lceron
;;
;; Source agent: ArithBudget
;;   fn within_budget(used: int, cap: int) -> bool   ;; lowered to (i32, i32) -> i32
;;   fn double(x: int) -> int                        ;; lowered to (i32) -> i32
;;   fn main() -> int                                ;; lowered to () -> i32
;;
;; Conventions assumed:
;;   - `int`  -> i32   (32-bit signed, two's complement)
;;   - `bool` -> i32   (0 = false, non-zero = true)
;;   - debug names emitted with --debug-names
;;   - exports: `main`, `memory`
;;   - linear memory: 1 page reserved for stack / future string storage
;;
;; Expected runtime result: main() == 42

(module $arith_budget
  (memory $memory 1)
  (export "memory" (memory $memory))

  ;; fn within_budget(used: i32, cap: i32) -> i32
  (func $within_budget (param $used i32) (param $cap i32) (result i32)
    local.get $used
    local.get $cap
    i32.le_s)

  ;; fn double(x: i32) -> i32
  (func $double (param $x i32) (result i32)
    local.get $x
    i32.const 2
    i32.mul)

  ;; fn main() -> i32
  ;;   let a = double(21)
  ;;   if within_budget(a, 100) { a } else { 0 }
  (func $main (result i32)
    (local $a i32)
    i32.const 21
    call $double
    local.set $a
    local.get $a
    i32.const 100
    call $within_budget
    (if (result i32)
      (then local.get $a)
      (else i32.const 0)))

  (export "main" (func $main))
  (export "within_budget" (func $within_budget))
  (export "double" (func $double)))
