;; Golden WAT fixture for examples/wasm/poc/03_branch_classify.lceron
;;
;; Source agent: BranchClassify
;;   fn classify(score: int) -> string
;;   fn main() -> string
;;
;; Conventions assumed (same as 02):
;;   - `int`    -> i32
;;   - `string` -> multi-value (i32 ptr, i32 len)
;;   - String literals laid out in the data section
;;
;; Layout in linear memory (offsets 16..):
;;   "A" -> ptr 16, len 1
;;   "B" -> ptr 24, len 1
;;   "C" -> ptr 32, len 1
;;   "D" -> ptr 40, len 1
;;   "F" -> ptr 48, len 1
;;
;; Expected runtime result for main():
;;   classify(85) -> "B" -> ptr 24, len 1

(module $branch_classify
  (memory $memory 1)
  (export "memory" (memory $memory))

  (data (i32.const 16) "A")
  (data (i32.const 24) "B")
  (data (i32.const 32) "C")
  (data (i32.const 40) "D")
  (data (i32.const 48) "F")

  ;; fn classify(score: int) -> string
  ;;   if score >= 90 { return "A" }
  ;;   if score >= 80 { return "B" }
  ;;   if score >= 70 { return "C" }
  ;;   if score >= 60 { return "D" }
  ;;   return "F"
  (func $classify (param $score i32) (result i32 i32)
    local.get $score
    i32.const 90
    i32.ge_s
    (if
      (then
        i32.const 16
        i32.const 1
        return))
    local.get $score
    i32.const 80
    i32.ge_s
    (if
      (then
        i32.const 24
        i32.const 1
        return))
    local.get $score
    i32.const 70
    i32.ge_s
    (if
      (then
        i32.const 32
        i32.const 1
        return))
    local.get $score
    i32.const 60
    i32.ge_s
    (if
      (then
        i32.const 40
        i32.const 1
        return))
    i32.const 48
    i32.const 1)

  ;; fn main() -> string
  ;;   classify(85)
  (func $main (result i32 i32)
    i32.const 85
    call $classify)

  (export "main" (func $main))
  (export "classify" (func $classify)))
