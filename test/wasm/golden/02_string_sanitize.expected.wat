;; Golden WAT fixture for examples/wasm/poc/02_string_sanitize.lceron
;;
;; Source agent: StringSanitize
;;   fn sanitize(input: string) -> string
;;   fn main() -> string
;;
;; Conventions assumed:
;;   - `string` -> (i32 ptr, i32 len) passed and returned as multi-value
;;   - `len(s: string) -> i64` is a builtin; in WASM lowered to a function
;;     that just returns the len component as i64
;;   - String literals live in the data section starting at offset 16
;;     (offsets 0..16 reserved for runtime use / null sentinel)
;;   - Exports: `main`, `memory`
;;
;; Layout of strings in data:
;;   "hello world"  -> ptr 16, len 11
;;   "REJECTED"     -> ptr 32, len 8
;;
;; Expected runtime result: main() returns ptr=16, len=11
;; (i.e. "hello world", since len(11) <= 100)

(module $string_sanitize
  (memory $memory 1)
  (export "memory" (memory $memory))

  ;; "hello world" at offset 16 (length 11)
  (data (i32.const 16) "hello world")
  ;; "REJECTED" at offset 32 (length 8)
  (data (i32.const 32) "REJECTED")

  ;; builtin: len(s) -> i64
  ;; Lowered: takes the (ptr, len) pair and returns the len as i64.
  (func $len (param $ptr i32) (param $len i32) (result i64)
    local.get $len
    i64.extend_i32_u)

  ;; fn sanitize(input: string) -> string
  ;;   if len(input) > 100 { "REJECTED" } else { input }
  (func $sanitize (param $in_ptr i32) (param $in_len i32) (result i32 i32)
    local.get $in_ptr
    local.get $in_len
    call $len
    i64.const 100
    i64.gt_s
    (if (result i32 i32)
      (then
        i32.const 32
        i32.const 8)
      (else
        local.get $in_ptr
        local.get $in_len)))

  ;; fn main() -> string
  ;;   sanitize("hello world")
  (func $main (result i32 i32)
    i32.const 16
    i32.const 11
    call $sanitize)

  (export "main" (func $main))
  (export "sanitize" (func $sanitize))
  (export "len" (func $len)))
