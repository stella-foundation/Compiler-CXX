#!/bin/sh
set -eu

compiler=$1
work_dir=$2

mkdir -p "$work_dir"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# Test 1: Typos in identifiers, spell checking, and zero cascading errors
log="$work_dir/ident_typo.log"
"$compiler" /dev/stdin --build.compile-only >"$log" 2>&1 <<'EOF' || true
pub fn test() -> i32 {
    let count: i32 = 42;
    return cont + 1;
}
EOF

grep -Fq 'error[E_UNDEFINED_NAME]: undefined identifier `cont`' "$log" || fail "missing E_UNDEFINED_NAME"
grep -Fq '= help: did you mean `count`?' "$log" || fail "missing did you mean count help"
grep -Fq '= help: replace with `count`' "$log" || fail "missing replace with count fixit"
grep -Fq "return cont + 1;" "$log" || fail "missing stdin source snippet"
if grep -Fq "return type mismatch" "$log"; then
	fail "unexpected cascading return type mismatch"
fi
if grep -Fq "requires numeric operands" "$log"; then
	fail "unexpected cascading operator error"
fi

# Test 2: JSON diagnostics format with populated fixits
json_log="$work_dir/json_typo.log"
"$compiler" /dev/stdin --build.compile-only --diag.format=json >"$json_log" 2>&1 <<'EOF' || true
pub fn test() -> i32 {
    let count: i32 = 42;
    return cont + 1;
}
EOF

grep -Fq '"code":"E_UNDEFINED_NAME"' "$json_log" || fail "missing JSON E_UNDEFINED_NAME"
grep -Fq '"replacement":"count"' "$json_log" || fail "missing JSON fixit replacement"
grep -Fq '"help":"did you mean `count`?"' "$json_log" || fail "missing JSON help"

# Test 3: Unknown type error reporting
type_log="$work_dir/unknown_type.log"
"$compiler" /dev/stdin --build.compile-only >"$type_log" 2>&1 <<'EOF' || true
pub fn test(x: ui8) -> void {}
EOF

grep -Fq 'error[E_UNDEFINED_TYPE]: unknown type `ui8`' "$type_log" || fail "missing E_UNDEFINED_TYPE"
if grep -Fq "= help:" "$type_log"; then
	fail "unexpected help on unknown type"
fi

# Test 4: Member typo spell check
member_log="$work_dir/member_typo.log"
"$compiler" /dev/stdin --build.compile-only >"$member_log" 2>&1 <<'EOF' || true
struct Point {
    x: i32,
    y: i32,
}

pub fn test(p: *Point) -> i32 {
    return p.x1;
}
EOF

grep -Fq 'error[E_UNKNOWN_MEMBER]: no field `x1` on aggregate' "$member_log" || fail "missing E_UNKNOWN_MEMBER"
grep -Fq '= help: did you mean `x`?' "$member_log" || fail "missing did you mean x help"
grep -Fq '= help: replace with `x`' "$member_log" || fail "missing replace with x fixit"

# Test 5: Duplicate function redefinition with related note snippet
dup_log="$work_dir/dup.log"
"$compiler" /dev/stdin --build.compile-only >"$dup_log" 2>&1 <<'EOF' || true
pub fn foo() -> i32 {
    return 1;
}

pub fn foo() -> i32 {
    return 2;
}
EOF

grep -Fq 'error[E_DUPLICATE_DECLARATION]: redefinition of function `foo` with identical parameter types' "$dup_log" || fail "missing duplicate declaration error"
grep -Fq "= note: previous definition of 'foo' was here" "$dup_log" || fail "missing previous definition note"

# Test 6: Statement keyword typo with fixit and clean recovery (lei -> let)
let_log="$work_dir/let_typo.log"
"$compiler" /dev/stdin --build.compile-only >"$let_log" 2>&1 <<'EOF' || true
pub fn main() -> u8 {
    lei x: u8 = 1;
    let y: u8 = 1;
    return x + y;
}
EOF

grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: unknown keyword `lei`' "$let_log" || fail "missing unknown keyword error"
grep -Fq '= help: did you mean `let`?' "$let_log" || fail "missing did you mean let help"
grep -Fq '= help: replace with `let`' "$let_log" || fail "missing replace with let fixit"
if grep -Fq "undefined identifier" "$let_log"; then
	fail "unexpected cascading undefined identifier error"
fi
if grep -Fq "expected ';'" "$let_log"; then
	fail "unexpected expected semicolon error"
fi

# Test 7: Return keyword typo (retun -> return)
ret_log="$work_dir/ret_typo.log"
"$compiler" /dev/stdin --build.compile-only >"$ret_log" 2>&1 <<'EOF' || true
pub fn main() -> i32 {
    retun 42;
}
EOF

grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: unknown keyword `retun`' "$ret_log" || fail "missing unknown keyword retun error"
grep -Fq '= help: did you mean `return`?' "$ret_log" || fail "missing did you mean return help"
grep -Fq '= help: replace with `return`' "$ret_log" || fail "missing replace with return fixit"

# Test 8: Semicolon insertion fixit
semi_log="$work_dir/semi.log"
"$compiler" /dev/stdin --build.compile-only >"$semi_log" 2>&1 <<'EOF' || true
pub fn main() -> i32 {
    let x: i32 = 1
    return x;
}
EOF

grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected `;` but got `return`' "$semi_log" || fail "missing expected semicolon error"
grep -Fq '= help: insert `;`' "$semi_log" || fail "missing insert semicolon help"

# Test 9: Rejection of negative array length constant
neg_arr_log="$work_dir/neg_arr.log"
"$compiler" /dev/stdin --build.compile-only >"$neg_arr_log" 2>&1 <<'EOF' || true
const X: i32 = -1;
let arr: [X]u8;
pub fn main() -> void {}
EOF

grep -Fq 'error[E_GENERAL]: array length is not a valid non-negative compile-time constant' "$neg_arr_log" || fail "missing non-negative array length error"

# Test 10: Circular dependency detection in constant definitions
cycle_log="$work_dir/cycle_const.log"
"$compiler" /dev/stdin --build.compile-only >"$cycle_log" 2>&1 <<'EOF' || true
const A: u32 = B;
const B: u32 = A;
let arr: [A]u8;
pub fn main() -> void {}
EOF

grep -Fq 'circular dependency in constant definition' "$cycle_log" || fail "missing circular dependency error"

# Test 11: Named consts, arithmetic, enum member, and forward reference in array dimensions
const_arr_log="$work_dir/const_arr.o"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/const_arr.o" >"$work_dir/const_arr.log" 2>&1 <<'EOF' || fail "const array compilation failed"
enum BufferSize: u32 {
    Small = 16,
    Large = 64
}

let arr_forward: [LIMIT]u8;
const LIMIT: usize = 32;

const WIDTH: usize = 10;
const HEIGHT: usize = 4;
let arr_expr: [WIDTH * HEIGHT + 2]u8;

let arr_enum: [BufferSize.Large]u8;

pub fn test_local_const() -> u8 {
    const LOCAL_LEN: usize = 8;
    let mut local_arr: [LOCAL_LEN]u8;
    return local_arr[0];
}
pub fn main() -> void {}
EOF

# Test 12: Generic function and generic struct with named const arguments
generic_const_log="$work_dir/generic_const.log"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/generic_const.o" >"$generic_const_log" 2>&1 <<'EOF' || fail "generic const compilation failed"
const N_ELEM: usize = 12;

struct Buffer<N: usize> {
    data: [N]u8,
}

fn create_buffer<N: usize>() -> Buffer<N> {
    let mut b: Buffer<N>;
    return b;
}

pub fn main() -> void {
    let buf: Buffer<N_ELEM> = create_buffer<N_ELEM>();
}
EOF

# Test 13: Inexact float literal requires explicit ~ agreement
lossy_err_log="$work_dir/lossy_float_err.log"
"$compiler" /dev/stdin --build.compile-only >"$lossy_err_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    let a: f32 = 0.1;
}
EOF
grep -Fq "cannot initialize \`a\`: value not representable in \`f32\`" "$lossy_err_log" || fail "missing unrepresented float error"

# Test 14: Lossy float explicit agreement with ~ succeeds
lossy_ok_log="$work_dir/lossy_float_ok.log"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/lossy_float_ok.o" >"$lossy_ok_log" 2>&1 <<'EOF' || fail "lossy float agreement failed"
pub fn main() -> void {
    let a: f32 = 0.1~;
    let b: f64 = 0.1~;
    let c: f32 = -0.1~;
    let d: f32 = 0.1 ~;
    let e: f32 = 0.5;
}
EOF

# Test 15: 0.1 cast f32 fails without ~ and suggests ~
cast_nofit_log="$work_dir/cast_nofit.log"
"$compiler" /dev/stdin --build.compile-only >"$cast_nofit_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    let a: f32 = 0.1 cast f32;
}
EOF
grep -Fq "floating-point literal value is not exactly representable in \`f32\`; append \`~\` to agree to lossy rounding" "$cast_nofit_log" || fail "missing cast lossy suggestion"

# Test 16: Duplicate ~ suffix on float literal is rejected
dup_tilde_log="$work_dir/dup_tilde.log"
"$compiler" /dev/stdin --build.compile-only >"$dup_tilde_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    let a: f32 = 0.1~~;
}
EOF
grep -Fq "duplicate \`~\` suffix on floating-point literal" "$dup_tilde_log" || fail "missing duplicate tilde error"

# Test 17: Call with inexact float literal suggests appending ~
call_float_log="$work_dir/call_float.log"
"$compiler" /dev/stdin --build.compile-only >"$call_float_log" 2>&1 <<'EOF' || true
fn target(x: f32) -> void {}
pub fn main() -> void {
    target(0.1);
}
EOF
grep -Fq "argument 1 of call to \`target\` has a value not exactly representable in \`f32\`; append \`~\` to agree to lossy rounding" "$call_float_log" || fail "missing call float lossy suggestion"

# Test 18: @diag.err builtin emits formatted error with preserved case and custom or standard code
diag_err_log="$work_dir/diag_err.log"
"$compiler" /dev/stdin --build.compile-only >"$diag_err_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    @diag.err("E_TARGET_UNSUPPORTED", "Kernel requires x86_64 with SSE2 enabled");
}
EOF
grep -Fq 'error[E_TARGET_UNSUPPORTED]: Kernel requires x86_64 with SSE2 enabled' "$diag_err_log" || fail "missing E_TARGET_UNSUPPORTED error"
grep -Fq '^~~~~~~~~' "$diag_err_log" || fail "missing @diag.err underline span"

# Test 19: @diag.warn builtin emits warning and succeeds with exit code 0
diag_warn_log="$work_dir/diag_warn.log"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/diag_warn.o" >"$diag_warn_log" 2>&1 <<'EOF' || fail "@diag.warn should exit 0"
pub fn main() -> void {
    @diag.warn("W_NUMERIC_OVERFLOW", "Potential overflow detected");
}
EOF
grep -Fq 'warning[W_NUMERIC_OVERFLOW]: Potential overflow detected' "$diag_warn_log" || fail "missing W_NUMERIC_OVERFLOW warning"
grep -Fq '^~~~~~~~~~' "$diag_warn_log" || fail "missing @diag.warn underline span"

# Test 20: @diag.warn with --diag.warn-error fails with exit code 1
diag_warn_err_log="$work_dir/diag_warn_err.log"
"$compiler" /dev/stdin --build.compile-only --diag.warn-error >"$diag_warn_err_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    @diag.warn("W_NUMERIC_OVERFLOW", "Potential overflow detected");
}
EOF
grep -Fq 'error[W_NUMERIC_OVERFLOW]: Potential overflow detected' "$diag_warn_err_log" || fail "missing warning-as-error conversion"

# Test 21: @diag.err in JSON format
diag_json_log="$work_dir/diag_json.log"
"$compiler" /dev/stdin --build.compile-only --diag.format=json >"$diag_json_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    @diag.err("E_TARGET_UNSUPPORTED", "Kernel requires x86_64 with SSE2 enabled");
}
EOF
grep -Fq '"code":"E_TARGET_UNSUPPORTED"' "$diag_json_log" || fail "missing JSON E_TARGET_UNSUPPORTED"
grep -Fq '"message":"Kernel requires x86_64 with SSE2 enabled"' "$diag_json_log" || fail "missing JSON message"
grep -Fq '"phase":"backend"' "$diag_json_log" || fail "missing JSON backend phase"

# Test 22: @diag.err with custom code
diag_custom_log="$work_dir/diag_custom.log"
"$compiler" /dev/stdin --build.compile-only >"$diag_custom_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    @diag.err("E_KERNEL_PANIC", "Fatal kernel assertion");
}
EOF
grep -Fq 'error[E_KERNEL_PANIC]: Fatal kernel assertion' "$diag_custom_log" || fail "missing custom code error"

# Test 23: Call overload rejection note with pointer mutability mismatch
overload_log="$work_dir/overload_const.log"
"$compiler" /dev/stdin --build.compile-only >"$overload_log" 2>&1 <<'EOF' || true
struct Vec {
    x: f32,
}
impl Vec {
    pub fn len(v: *mut Vec) -> f32 {
        return v.x;
    }
}
pub fn main() -> void {
    let obj: Vec = { .x = 1.0 };
    Vec.len(&obj);
}
EOF
grep -Fq 'error[E_INVALID_CALL]: no matching public function for call to `Vec.len` with 1 argument(s)' "$overload_log" || fail "missing E_INVALID_CALL for overload mismatch"
grep -Fq "= note: candidate function not viable: cannot convert argument 1 from '*Vec' to '*mut Vec' (cannot convert immutable pointer to mutable pointer)" "$overload_log" || fail "missing candidate rejection note for mutability mismatch"
if grep -Fq 'st$' "$overload_log"; then
    fail "unexpected mangled symbol in overload diagnostic types"
fi

# Test 24: Call overload rejection note with arity mismatch
arity_log="$work_dir/overload_arity.log"
"$compiler" /dev/stdin --build.compile-only >"$arity_log" 2>&1 <<'EOF' || true
fn add(a: i32, b: i32) -> i32 {
    return a + b;
}
pub fn main() -> void {
    add(1);
}
EOF
grep -Fq 'error[E_INVALID_CALL]: no matching public function for call to `add` with 1 argument(s)' "$arity_log" || fail "missing E_INVALID_CALL for arity mismatch"
grep -Fq "= note: candidate function not viable: expects 2 arguments, but 1 were supplied" "$arity_log" || fail "missing candidate rejection note for arity mismatch"

# Test 25: Immutable binding rejects assignment
immut_assign_log="$work_dir/immut_assign.log"
"$compiler" /dev/stdin --build.compile-only >"$immut_assign_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    let x: i32 = 1;
    x = 2;
}
EOF
grep -Fq "error[E_IMMUTABLE_ASSIGNMENT]: cannot assign to immutable binding \`x\`" "$immut_assign_log" || fail "missing E_IMMUTABLE_ASSIGNMENT"

# Test 26: User example: *mut Vec2 with auto-deref assignment and passing *mut to *
user_example_log="$work_dir/user_example.log"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/user_example.o" >"$user_example_log" 2>&1 <<'EOF' || fail "user example compilation failed"
struct Vec2 {
    x: f32,
    y: f32,
}
impl Vec2 {
    pub fn length(v: *Vec2) -> f32 {
        return v.x + v.y;
    }
}
pub fn normalize(v: *mut Vec2) -> void {
    let len: f32 = Vec2.length(v);
    v.x = v.x / len;
    v.y = v.y / len;
}
pub fn main() -> void {
    let mut v: Vec2 = { .x = 3.0, .y = 4.0 };
    normalize(&v);
}
EOF

# Test 27: Mutation through immutable pointer is rejected
immut_ptr_log="$work_dir/immut_ptr.log"
"$compiler" /dev/stdin --build.compile-only >"$immut_ptr_log" 2>&1 <<'EOF' || true
struct Vec2 {
    x: f32,
    y: f32,
}
pub fn mutate_immut(v: *Vec2) -> void {
    v.x = 1.0;
}
EOF
grep -Fq "left-hand side of assignment is not a valid lvalue" "$immut_ptr_log" || fail "missing lvalue error on mutating through immutable pointer"

# Test 28: *const is rejected as a pointer qualifier
const_ptr_log="$work_dir/const_ptr.log"
"$compiler" /dev/stdin --build.compile-only >"$const_ptr_log" 2>&1 <<'EOF' || true
pub fn test(v: *const i32) -> void {}
EOF
grep -Fq "expected type, got \`const\`" "$const_ptr_log" || fail "missing expected type error for *const"

# Test 29: var is rejected
var_rejected_log="$work_dir/var_rejected.log"
"$compiler" /dev/stdin --build.compile-only >"$var_rejected_log" 2>&1 <<'EOF' || true
var x: i32 = 1;
EOF
grep -Fq "unexpected top-level token \`var\`" "$var_rejected_log" || fail "missing rejection of var at top-level"

# Test 30: Colon required for variable declarations
missing_var_colon_log="$work_dir/missing_var_colon.log"
"$compiler" /dev/stdin --build.compile-only >"$missing_var_colon_log" 2>&1 <<'EOF' || true
pub fn main() -> void {
    let buffer RingBuffer = init();
}
EOF
grep -Fq 'expected `:`' "$missing_var_colon_log" || fail "missing expected colon on var decl"
grep -Fq '= help: insert `:`' "$missing_var_colon_log" || fail "missing insert colon help on var decl"

# Test 31: Colon required for function argument types
missing_arg_colon_log="$work_dir/missing_arg_colon.log"
"$compiler" /dev/stdin --build.compile-only >"$missing_arg_colon_log" 2>&1 <<'EOF' || true
fn add(x u8, y u8) -> u8 {
    return x + y;
}
EOF
grep -Fq 'expected `:`' "$missing_arg_colon_log" || fail "missing expected colon on fn arg"
grep -Fq '= help: insert `:`' "$missing_arg_colon_log" || fail "missing insert colon help on fn arg"

# Test 32: Combined multiple storage types (static, thread_local)
combined_storage_log="$work_dir/combined_storage.log"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/combined_storage.o" >"$combined_storage_log" 2>&1 <<'EOF' || fail "combined storage types compilation failed"
struct DeviceRingBuffer {
    head: i32,
    tail: i32,
}

let mut static thread_local buffer_manager: DeviceRingBuffer = { .head = 0, .tail = 0 };
let static thread_local read_only_tls: i32 = 100;
let thread_local static reversed_tls: i32 = 200;

pub fn test_local() -> i32 {
    let mut static thread_local local_manager: DeviceRingBuffer = { .head = 1, .tail = 2 };
    let static thread_local local_tls: i32 = 300;
    return buffer_manager.head + local_manager.head + read_only_tls + reversed_tls + local_tls;
}
EOF

# Test 33: Colon required for generic parameters with constraints or types
missing_generic_colon_log="$work_dir/missing_generic_colon.log"
"$compiler" /dev/stdin --build.compile-only >"$missing_generic_colon_log" 2>&1 <<'EOF' || true
fn add<T u i>(x: T, y: T) -> T {
    return x + y;
}
EOF
grep -Fq 'expected `:`' "$missing_generic_colon_log" || fail "missing expected colon on generic param"
grep -Fq '= help: insert `:`' "$missing_generic_colon_log" || fail "missing insert colon help on generic param"

# Test 34: Generic function with colon syntax
valid_generic_colon_log="$work_dir/valid_generic_colon.log"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/valid_generic_colon.o" >"$valid_generic_colon_log" 2>&1 <<'EOF' || fail "generic colon syntax compilation failed"
fn add<T: u | i>(x: T, y: T) -> T {
    return x + y;
}

pub fn test() -> u32 {
    return add(1 cast u32, 2 cast u32);
}
EOF

# Test 35: UFCS Priority 1 shadowing of free functions by non-callable struct fields
ufcs_shadow_log="$work_dir/ufcs_shadow.log"
"$compiler" /dev/stdin --build.compile-only >"$ufcs_shadow_log" 2>&1 <<'EOF' || true
struct Point {
    x: i32,
    y: i32,
}
pub fn x(_: Point, val: i32) -> i32 {
    return val;
}
pub fn test(p: Point) -> i32 {
    return p.x(1);
}
EOF
grep -Fq 'call of non-function value' "$ufcs_shadow_log" || fail "missing non-function value error for shadowed field"
if grep -Fq "integer literal requires" "$ufcs_shadow_log"; then
	fail "unexpected cascading integer literal error"
fi

# Test 36: UFCS auto-ref rejection on rvalue temporary in method chain
ufcs_rvalue_log="$work_dir/ufcs_rvalue.log"
"$compiler" /dev/stdin --build.compile-only >"$ufcs_rvalue_log" 2>&1 <<'EOF' || true
struct Point {
    x: i32,
    y: i32,
}
pub fn make_point() -> Point {
    return { .x = 1, .y = 2 };
}
pub fn modify(_: *mut Point) -> void {}
pub fn test() -> void {
    make_point().modify();
}
EOF
grep -Fq 'error[E_INVALID_CALL]: no matching function for UFCS call to `modify`' "$ufcs_rvalue_log" || fail "missing UFCS no matching function error"
grep -Fq 'receiver is an rvalue temporary and cannot be auto-referenced' "$ufcs_rvalue_log" || fail "missing rvalue auto-ref rejection note"

# Test 37: UFCS auto-ref rejection for immutable receiver to mutable pointer
ufcs_mut_log="$work_dir/ufcs_mut.log"
"$compiler" /dev/stdin --build.compile-only >"$ufcs_mut_log" 2>&1 <<'EOF' || true
struct Point {
    x: i32,
    y: i32,
}
pub fn modify(_: *mut Point) -> void {}
pub fn test() -> void {
    let p: Point = { .x = 1, .y = 2 };
    p.modify();
}
EOF
grep -Fq 'error[E_INVALID_CALL]: no matching function for UFCS call to `modify`' "$ufcs_mut_log" || fail "missing UFCS no matching function error"
grep -Fq 'receiver is immutable and cannot be passed to mutable pointer parameter' "$ufcs_mut_log" || fail "missing immutable receiver rejection note"

# Test 38: UFCS ambiguous call error
ufcs_amb_log="$work_dir/ufcs_amb.log"
"$compiler" /dev/stdin --build.compile-only >"$ufcs_amb_log" 2>&1 <<'EOF' || true
struct Point {
    x: i32,
}
pub fn amb(_: Point, _: i64, _: i32) -> void {}
pub fn amb(_: Point, _: i32, _: i64) -> void {}
pub fn test() -> void {
    let p: Point = { .x = 1 };
    p.amb(0, 0);
}
EOF
grep -Fq 'error[E_INVALID_CALL]: ambiguous call to overloaded function `amb` via UFCS' "$ufcs_amb_log" || fail "missing ambiguous UFCS call error"

# Test 39: UFCS non-existent method diagnostic
ufcs_missing_log="$work_dir/ufcs_missing.log"
"$compiler" /dev/stdin --build.compile-only >"$ufcs_missing_log" 2>&1 <<'EOF' || true
struct Point {
    x: i32,
    y: i32,
}
pub fn test(p: Point) -> void {
    p.non_existent(1, 2);
}
EOF
grep -Fq 'error[E_INVALID_CALL]: no field or method `non_existent` found for type `Point`' "$ufcs_missing_log" || fail "missing no field or method error"
if grep -Fq "integer literal requires" "$ufcs_missing_log"; then
	fail "unexpected cascading integer literal error"
fi

# Test 40: Missing colon on enum underlying type produces clean non-cascading syntax error
missing_enum_colon_log="$work_dir/missing_enum_colon.log"
"$compiler" /dev/stdin --build.compile-only >"$missing_enum_colon_log" 2>&1 <<'EOF' || true
pub enum Color u32 {
    Red = 1,
    Green = 2,
    Blue = 3,
}

pub fn get_color() -> Color {
    return Color.Red;
}
EOF
grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected `:` before enum underlying type' "$missing_enum_colon_log" || fail "missing expected colon error on enum"
grep -Fq '= help: insert `: `' "$missing_enum_colon_log" || fail "missing insert colon fixit on enum"
if grep -Fq "expected type" "$missing_enum_colon_log"; then
    fail "unexpected cascading expected type error on enum"
fi

# Test 41: Missing colon on struct field produces clean non-cascading syntax error
missing_struct_colon_log="$work_dir/missing_struct_colon.log"
"$compiler" /dev/stdin --build.compile-only >"$missing_struct_colon_log" 2>&1 <<'EOF' || true
pub struct LegacyPoint {
    x i32,
    y i32,
}
EOF
grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected `:` after struct field name' "$missing_struct_colon_log" || fail "missing expected colon error on struct field"
grep -Fq '= help: insert `: `' "$missing_struct_colon_log" || fail "missing insert colon fixit on struct field"
if grep -Fq "expected type" "$missing_struct_colon_log"; then
    fail "unexpected cascading expected type error on struct field"
fi

# Test 42: Missing colon on union field produces clean non-cascading syntax error
missing_union_colon_log="$work_dir/missing_union_colon.log"
"$compiler" /dev/stdin --build.compile-only >"$missing_union_colon_log" 2>&1 <<'EOF' || true
pub union LegacyUnion {
    a i32,
    b f32,
}
EOF
grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected `:` after union field name' "$missing_union_colon_log" || fail "missing expected colon error on union field"
grep -Fq '= help: insert `: `' "$missing_union_colon_log" || fail "missing insert colon fixit on union field"
if grep -Fq "expected type" "$missing_union_colon_log"; then
    fail "unexpected cascading expected type error on union field"
fi

# Test 43: Positive parser validation: pub enum Name: Type, pub struct Name { field: Type }, nested, arrays
valid_colon_syntax_log="$work_dir/valid_colon_syntax.log"
"$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/valid_colon_syntax.o" >"$valid_colon_syntax_log" 2>&1 <<'EOF' || fail "valid colon syntax compilation failed"
pub enum Status: u8 {
    Inactive = 0,
    Active = 1,
}

pub struct Vertex {
    pos: [3]f32,
    color: u32,
}

pub struct Mesh {
    vertices: [16]Vertex,
    count: usize,
    status: Status,
}

pub union ValueUnion {
    int_val: i64,
    flt_val: f64,
}

pub fn make_mesh() -> Mesh {
    let mut m: Mesh;
    m.count = 0;
    m.status = Status.Active;
    return m;
}
EOF

# Test 44: E_INVALID_SELF: using 'Self' outside of trait or impl
invalid_self_log="$work_dir/invalid_self.log"
"$compiler" /dev/stdin --build.compile-only >"$invalid_self_log" 2>&1 <<'EOF' || true
pub fn foo(x: Self) -> void {}
EOF
grep -Fq 'error[E_INVALID_SELF]: `Self` is only allowed within trait definitions and impl blocks' "$invalid_self_log" || fail "missing E_INVALID_SELF error"

# Test 45: E_TRAIT_DEFAULT_IMPL: declaring a method with a body in a trait declaration
trait_default_impl_log="$work_dir/trait_default_impl.log"
"$compiler" /dev/stdin --build.compile-only >"$trait_default_impl_log" 2>&1 <<'EOF' || true
trait Foo {
    fn bar() -> void {}
}
EOF
grep -Fq 'error[E_TRAIT_DEFAULT_IMPL]: trait methods cannot have default implementations' "$trait_default_impl_log" || fail "missing E_TRAIT_DEFAULT_IMPL error"

# Test 46: E_TRAIT_MISSING_METHOD: impl block missing a required trait method
trait_missing_method_log="$work_dir/trait_missing_method.log"
"$compiler" /dev/stdin --build.compile-only >"$trait_missing_method_log" 2>&1 <<'EOF' || true
trait Foo {
    fn bar() -> void;
}
struct Point { x: i32 }
impl Foo for Point {}
EOF
grep -Fq 'error[E_TRAIT_MISSING_METHOD]: missing implementation for trait method `bar` of trait `Foo`' "$trait_missing_method_log" || fail "missing E_TRAIT_MISSING_METHOD error"

# Test 47: E_TRAIT_UNKNOWN_METHOD: impl block with a method not in the trait
trait_unknown_method_log="$work_dir/trait_unknown_method.log"
"$compiler" /dev/stdin --build.compile-only >"$trait_unknown_method_log" 2>&1 <<'EOF' || true
trait Foo {
    fn bar() -> void;
}
struct Point { x: i32 }
impl Foo for Point {
    fn bar() -> void {}
    fn baz() -> void {}
}
EOF
grep -Fq 'error[E_TRAIT_UNKNOWN_METHOD]: method `baz` is not declared in trait `Foo`' "$trait_unknown_method_log" || fail "missing E_TRAIT_UNKNOWN_METHOD error"

# Test 48: E_TRAIT_SIGNATURE_MISMATCH: parameter type mismatch in impl
trait_sig_mismatch_log="$work_dir/trait_sig_mismatch.log"
"$compiler" /dev/stdin --build.compile-only >"$trait_sig_mismatch_log" 2>&1 <<'EOF' || true
trait Foo {
    fn bar(x: i32) -> void;
}
struct Point { x: i32 }
impl Foo for Point {
    fn bar(x: f32) -> void {}
}
EOF
grep -Fq 'error[E_TRAIT_SIGNATURE_MISMATCH]: parameter 1 of method `bar` has mismatched type: expected `i32`, got `f32`' "$trait_sig_mismatch_log" || fail "missing E_TRAIT_SIGNATURE_MISMATCH error"

# Test 49: E_DUPLICATE_IMPL: duplicate impl blocks for same trait and type
duplicate_impl_log="$work_dir/duplicate_impl.log"
"$compiler" /dev/stdin --build.compile-only >"$duplicate_impl_log" 2>&1 <<'EOF' || true
trait Foo {
    fn bar() -> void;
}
struct Point { x: i32 }
impl Foo for Point {
    fn bar() -> void {}
}
impl Foo for Point {
    fn bar() -> void {}
}
EOF
grep -Fq 'error[E_DUPLICATE_IMPL]: duplicate implementation of trait `Foo` for type `Point`' "$duplicate_impl_log" || fail "missing E_DUPLICATE_IMPL error"

# Test 50: E_GENERIC_CONSTRAINT: calling trait-bounded function with non-conforming type
generic_constraint_log="$work_dir/generic_constraint.log"
"$compiler" /dev/stdin --build.compile-only >"$generic_constraint_log" 2>&1 <<'EOF' || true
trait Printable {
    fn print(p: *Self) -> void;
}
struct Point { x: i32 }
pub fn show<T: Printable>(x: *T) -> void {
    x.print();
}
pub fn test() -> void {
    let p: Point = { .x = 1 };
    show(&p);
}
EOF
grep -Fq 'error[E_GENERIC_CONSTRAINT]: type `Point` does not implement trait `Printable`' "$generic_constraint_log" || fail "missing E_GENERIC_CONSTRAINT error"

# Test 51: E_ORPHAN_RULE: implementing foreign trait for foreign type
orphan_rule_dir="$work_dir/orphan_test"
mkdir -p "$orphan_rule_dir"
cat > "$orphan_rule_dir/foreign.sl" <<'EOF'
pub trait ForeignTrait {
    fn execute(p: *Self) -> void;
}
pub struct ForeignType {
    pub x: i32,
}
EOF
orphan_rule_log="$orphan_rule_dir/orphan.log"
"$compiler" /dev/stdin --build.compile-only >"$orphan_rule_log" 2>&1 <<EOF || true
import "$orphan_rule_dir/foreign.sl";

impl ForeignTrait for ForeignType {
    fn execute(p: *Self) -> void {}
}
EOF
grep -Fq 'error[E_ORPHAN_RULE]: cannot implement foreign trait `ForeignTrait` for foreign type `ForeignType`' "$orphan_rule_log" || fail "missing E_ORPHAN_RULE error"

# Test 52: Rejection of removed #[unused] attribute
unused_attr_log="$work_dir/unused_attr.log"
"$compiler" /dev/stdin --build.compile-only >"$unused_attr_log" 2>&1 <<'EOF' || true
pub fn test(#[unused] x: i32) -> void {}
EOF
grep -Fq 'error[E_GENERAL]: unknown attribute `#[unused]`' "$unused_attr_log" || fail "missing unknown attribute error for #[unused]"

# Test 53: Parameter unused suppression with _name and _, allowing multiple _
param_suppress_log="$work_dir/param_suppress.log"
"$compiler" /dev/stdin --build.compile-only >"$param_suppress_log" 2>&1 <<'EOF' || true
pub fn test(_self: i32, _: f64, _: *u8) -> void {}
EOF
if grep -Fq "unused parameter" "$param_suppress_log"; then
	fail "unexpected unused parameter warning for suppressed parameters"
fi
if grep -Fq "duplicate parameter" "$param_suppress_log"; then
	fail "unexpected duplicate parameter error for multiple _ parameters"
fi

# Test 54: Unused parameter without leading underscore warns
param_warn_log="$work_dir/param_warn.log"
"$compiler" /dev/stdin --build.compile-only >"$param_warn_log" 2>&1 <<'EOF' || true
pub fn test(self: i32) -> void {}
EOF
grep -Fq 'warning[W_GENERAL]: unused parameter `self`' "$param_warn_log" || fail "missing unused parameter warning for self"

# Test 55: Local binding unused suppression with _name and _, allowing multiple _
local_suppress_log="$work_dir/local_suppress.log"
"$compiler" /dev/stdin --build.compile-only >"$local_suppress_log" 2>&1 <<'EOF' || true
pub fn test() -> void {
    let _a: i32 = 1;
    let _: i32 = 2;
    let _: i32 = 3;
}
EOF
if grep -Fq "unused" "$local_suppress_log"; then
	fail "unexpected unused warning for suppressed local variables"
fi
if grep -Fq "duplicate" "$local_suppress_log"; then
	fail "unexpected duplicate local binding error for multiple _"
fi

# Test 56: Reference to _ in expression fails with E_UNDEFINED_NAME
use_underscore_log="$work_dir/use_underscore.log"
"$compiler" /dev/stdin --build.compile-only >"$use_underscore_log" 2>&1 <<'EOF' || true
pub fn test(_: i32) -> void {
    let _unused_res: i32 = _;
}
EOF
grep -Fq 'error[E_UNDEFINED_NAME]: undefined identifier `_`' "$use_underscore_log" || fail "missing E_UNDEFINED_NAME for referencing _"
if grep -Fq "did you mean" "$use_underscore_log"; then
	fail "unexpected did you mean suggestion for _"
fi

# Test 57: Private function and global unused suppression with leading underscore
private_suppress_log="$work_dir/private_suppress.log"
"$compiler" /dev/stdin --build.compile-only >"$private_suppress_log" 2>&1 <<'EOF' || true
fn _helper() -> void {}
let _private_global: i32 = 10;
const _PRIVATE_CONST: i32 = 20;
pub fn main() -> void {}
EOF
if grep -Fq "unused private" "$private_suppress_log"; then
	fail "unexpected unused private warning for _-prefixed symbols"
fi

# Test 58: pub import is a public re-export and is exempt from unused import warnings
pub_import_dir="$work_dir/pub_import_test"
mkdir -p "$pub_import_dir"
cat > "$pub_import_dir/target.sl" <<'EOF'
pub fn reexported_func() -> void {}
EOF
pub_import_log="$pub_import_dir/pub_import.log"
"$compiler" /dev/stdin --build.compile-only >"$pub_import_log" 2>&1 <<EOF || true
pub import "$pub_import_dir/target.sl";
pub fn main() -> void {}
EOF
if grep -Fq "unused import" "$pub_import_log"; then
	fail "unexpected unused import warning for pub import"
fi

private_import_log="$pub_import_dir/private_import.log"
"$compiler" /dev/stdin --build.compile-only >"$private_import_log" 2>&1 <<EOF || true
import "$pub_import_dir/target.sl";
pub fn main() -> void {}
EOF
grep -Fq "unused import" "$private_import_log" || fail "missing unused import warning for private import"

# Test 59: struct is strictly data-only (functions inside struct are a hard parse error)
struct_fn_log="$work_dir/struct_fn_rejected.log"
"$compiler" /dev/stdin --build.compile-only >"$struct_fn_log" 2>&1 <<'EOF' || true
struct Foo {
    x: i32,
    pub fn bar() -> void {}
}
EOF
grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected struct field name, got `fn`' "$struct_fn_log" || fail "missing hard parse error for fn inside struct"

# Test 60: struct rejects inline constants or variables
struct_const_log="$work_dir/struct_const_rejected.log"
"$compiler" /dev/stdin --build.compile-only >"$struct_const_log" 2>&1 <<'EOF' || true
struct Foo {
    const LIMIT: i32 = 10;
    x: i32,
}
EOF
grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected struct field name, got `const`' "$struct_const_log" || fail "missing hard parse error for const inside struct"

# Test 61: file-level struct path.to.module; is rejected
struct_mod_hdr_log="$work_dir/struct_mod_hdr_rejected.log"
"$compiler" /dev/stdin --build.compile-only >"$struct_mod_hdr_log" 2>&1 <<'EOF' || true
struct path.to.module;
pub fn main() -> void {}
EOF
grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected `{` but got `.`' "$struct_mod_hdr_log" || fail "missing parse error for file-level struct path;"

# Test 62: Duplicate file-level mod declaration is rejected
dup_mod_hdr_log="$work_dir/dup_mod_hdr.log"
"$compiler" /dev/stdin --build.compile-only >"$dup_mod_hdr_log" 2>&1 <<'EOF' || true
mod alpha.beta;
mod gamma.delta;
pub fn main() -> void {}
EOF
grep -Fq "error[E_PARSE_UNEXPECTED_TOKEN]: at most one file-level \`mod\` declaration is allowed per file" "$dup_mod_hdr_log" || fail "missing duplicate file-level mod error"

# Test 63: File-level mod declaration after top-level item is rejected
late_mod_hdr_log="$work_dir/late_mod_hdr.log"
"$compiler" /dev/stdin --build.compile-only >"$late_mod_hdr_log" 2>&1 <<'EOF' || true
pub fn helper() -> void {}
mod alpha.beta;
EOF
grep -Fq "error[E_PARSE_UNEXPECTED_TOKEN]: at most one file-level \`mod\` declaration is allowed per file, and it must appear before all other declarations" "$late_mod_hdr_log" || fail "missing misplaced file-level mod error"

# Test 64: Prohibiting mod from being used as a concrete type
mod_as_type_log="$work_dir/mod_as_type.log"
"$compiler" /dev/stdin --build.compile-only >"$mod_as_type_log" 2>&1 <<'EOF' || true
mod math_utils {
    pub fn add(a: i32, b: i32) -> i32 { return a + b; }
}
pub fn bad(x: math_utils) -> void {}
EOF
grep -Fq 'error[E_UNDEFINED_TYPE]: `math_utils` is a module, not a type' "$mod_as_type_log" || fail "missing mod-as-type error"

# Test 65: Prohibiting mod from being used as a value
mod_as_val_log="$work_dir/mod_as_val.log"
"$compiler" /dev/stdin --build.compile-only >"$mod_as_val_log" 2>&1 <<'EOF' || true
mod math_utils {
    pub fn add(a: i32, b: i32) -> i32 { return a + b; }
}
pub fn bad() -> void {
    let _x: i32 = math_utils;
}
EOF
grep -Fq 'error[E_INVALID_EXPR]: module `math_utils` cannot be used as a value' "$mod_as_val_log" || fail "missing mod-as-value error"

# Test 66: Prohibiting inherent impl on a mod
impl_on_mod_log="$work_dir/impl_on_mod.log"
"$compiler" /dev/stdin --build.compile-only >"$impl_on_mod_log" 2>&1 <<'EOF' || true
mod math_utils {}
impl math_utils {
    pub fn foo() -> void {}
}
EOF
grep -Fq 'error[E_UNDEFINED_TYPE]: `math_utils` is a module, not a type' "$impl_on_mod_log" || fail "missing impl-on-mod error"

# Test 67: Orphan rule for inherent impl on foreign type
inherent_orphan_log="$orphan_rule_dir/inherent_orphan.log"
"$compiler" /dev/stdin --build.compile-only >"$inherent_orphan_log" 2>&1 <<EOF || true
import "$orphan_rule_dir/foreign.sl";

impl ForeignType {
    pub fn extra(self: *Self) -> i32 { return self.x; }
}
EOF
grep -Fq 'error[E_ORPHAN_RULE]: cannot define inherent implementation for foreign type `ForeignType`' "$inherent_orphan_log" || fail "missing inherent E_ORPHAN_RULE error"

# Test 68: Generic trait impl bound enforcement
generic_trait_bound_log="$work_dir/generic_trait_bound.log"
"$compiler" /dev/stdin --build.compile-only >"$generic_trait_bound_log" 2>&1 <<'EOF' || true
trait Printable {
    fn print(self: *Self) -> i32;
}
struct Unprintable {
    x: i32,
}
struct Box<T> {
    value: T,
}
impl<T: Printable> Printable for Box<T> {
    fn print(self: *Self) -> i32 {
        return self.value.print();
    }
}
pub fn show<T: Printable>(item: *T) -> i32 {
    return item.print();
}
pub fn test() -> i32 {
    let b: Box<Unprintable> = { .value = { .x = 1 } };
    return show(&b);
}
EOF
grep -Fq 'error[E_GENERIC_CONSTRAINT]: type `Box<Unprintable>` does not implement trait `Printable`' "$generic_trait_bound_log" || fail "missing generic trait bound E_GENERIC_CONSTRAINT error"

# Test 69: Field visibility tests (private by default, explicit pub)
vis_dir="$work_dir/visibility_tests"
mkdir -p "$vis_dir"

cat > "$vis_dir/owner.sl" <<'EOF'
pub struct P {
    pub x: i32,
    y: i32,
    cb: fn() -> i32,
}

pub struct Open {
    pub a: i32,
}

pub union U {
    pub i: i32,
    num: f32,
}

pub enum E: u8 {
    A = 1,
    B = 2,
}

pub struct Box<T> {
    v: T,
}

impl<T> Box<T> {
    pub fn get(self: *Self) -> *T {
        return &self.v;
    }
}

pub fn make_p() -> P {
    return { .x = 10, .y = 20, .cb = dummy_cb };
}

pub fn make_box(val: i32) -> Box<i32> {
    return { .v = val };
}

fn dummy_cb() -> i32 {
    return 42;
}

pub macro read_y(p: *P) -> i32 {
    yield p.y;
}

pub fn same_file_check() -> i32 {
    let mut p: P = { .x = 1, .y = 2, .cb = dummy_cb };
    p.y = 3;
    let _py: *i32 = &p.y;
    let _off: usize = @of.offset(P, .y);
    let u: U = { .num = 3.14~ };
    let _num: f32 = u.num;
    return p.y;
}
EOF

# 1. p.y read is rejected
vis_log1="$vis_dir/test1.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log1" 2>&1 <<EOF || true
import "$vis_dir/owner.sl";
pub fn test(p: *P) -> i32 {
    return p.y;
}
EOF
grep -Fq 'error[E_PRIVATE_MEMBER]: field `y` of `P` is private' "$vis_log1" || fail "missing E_PRIVATE_MEMBER on member read"

# 2. p.y = 1 and &p.y are rejected with no cascading errors
vis_log2="$vis_dir/test2.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log2" 2>&1 <<EOF || true
import "$vis_dir/owner.sl";
pub fn test(p: *mut P) -> void {
    p.y = 1;
    let _py: *i32 = &p.y;
}
EOF
grep -Fq 'error[E_PRIVATE_MEMBER]: field `y` of `P` is private' "$vis_log2" || fail "missing E_PRIVATE_MEMBER on member write / addrof"

# 3. p.x compiles
"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t3.o" <<EOF || fail "public field read failed to compile"
import "$vis_dir/owner.sl";
pub fn test(p: *P) -> i32 {
    return p.x;
}
EOF

# 4. Literal struct with private field is rejected
vis_log4="$vis_dir/test4.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log4" 2>&1 <<EOF || true
import "$vis_dir/owner.sl";
pub fn test() -> P {
    return { .x = 1, .y = 2, .cb = null };
}
EOF
grep -Fq 'error[E_PRIVATE_MEMBER]: cannot construct `P` with a literal outside its declaring module: field `y` is private' "$vis_log4" || fail "missing E_PRIVATE_MEMBER on struct literal"

# 5. Literal struct with all pub fields compiles
"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t5.o" <<EOF || fail "all-pub struct literal failed to compile"
import "$vis_dir/owner.sl";
pub fn test() -> Open {
    return { .a = 42 };
}
EOF

# 6. Union private variant literal & access rejected
vis_log6="$vis_dir/test6.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log6" 2>&1 <<EOF || true
import "$vis_dir/owner.sl";
pub fn test(u: *U) -> f32 {
    let _u: U = { .num = 1.0~ };
    return u.num;
}
EOF
grep -Fq 'error[E_PRIVATE_MEMBER]: union variant `num` of `U` is private' "$vis_log6" || fail "missing E_PRIVATE_MEMBER on union variant lit"
grep -Fq 'error[E_PRIVATE_MEMBER]: field `num` of `U` is private' "$vis_log6" || fail "missing E_PRIVATE_MEMBER on union variant access"

# 7. Union pub variant literal & access compiles
"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t7.o" <<EOF || fail "union pub variant literal and access failed"
import "$vis_dir/owner.sl";
pub fn test(u: *U) -> i32 {
    let _u: U = { .i = 10 };
    return u.i;
}
EOF

# 8. Enum variants unchanged
"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t8.o" <<EOF || fail "enum variants failed"
import "$vis_dir/owner.sl";
pub fn test() -> E {
    return E.A;
}
EOF

# 9. @of.offset on private field rejected, on pub field compiles
vis_log9="$vis_dir/test9.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log9" 2>&1 <<EOF || true
import "$vis_dir/owner.sl";
pub fn test() -> usize {
    return @of.offset(P, .y);
}
EOF
grep -Fq 'error[E_PRIVATE_MEMBER]: field `y` of `P` is private' "$vis_log9" || fail "missing E_PRIVATE_MEMBER on @of.offset"

"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t9.o" <<EOF || fail "@of.offset on pub field failed"
import "$vis_dir/owner.sl";
pub fn test() -> usize {
    return @of.offset(P, .x);
}
EOF

# 10. Generic Box<T> private field rejected, pub method compiles
vis_log10="$vis_dir/test10.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log10" 2>&1 <<EOF || true
import "$vis_dir/owner.sl";
pub fn test(b: *Box<i32>) -> i32 {
    return b.v;
}
EOF
grep -Fq 'error[E_PRIVATE_MEMBER]: field `v` of `Box<i32>` is private' "$vis_log10" || fail "missing E_PRIVATE_MEMBER on generic private field"

"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t10.o" <<EOF || fail "generic pub method call failed"
import "$vis_dir/owner.sl";
pub fn test(b: *Box<i32>) -> *i32 {
    return b.get();
}
EOF

# 11. p.cb() private fn-pointer field with no matching method -> E_PRIVATE_MEMBER
vis_log11="$vis_dir/test11.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log11" 2>&1 <<EOF || true
import "$vis_dir/owner.sl";
pub fn test(p: *P) -> i32 {
    return p.cb();
}
EOF
grep -Fq 'error[E_PRIVATE_MEMBER]: field `cb` of `P` is private' "$vis_log11" || fail "missing E_PRIVATE_MEMBER on hidden private field call"

# 12. p.cb() when a UFCS free function cb(p: *P) is visible -> resolves to function
"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t12.o" <<EOF || fail "UFCS resolution when private field hidden failed"
import "$vis_dir/owner.sl";
pub fn cb(p: *P) -> i32 {
    return 123;
}
pub fn test(p: *P) -> i32 {
    return p.cb();
}
EOF

# 13. Owner's pub macro reading private field compiles externally (def-site hygiene)
"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/t13.o" <<EOF || fail "macro def-site hygiene failed"
import "$vis_dir/owner.sl";
pub fn test(p: *P) -> i32 {
    return read_y(p);
}
EOF

# 14. Same-file check compiles
"$compiler" "$vis_dir/owner.sl" --build.compile-only --build.output="$vis_dir/owner.o" || fail "same-file private field access failed"

# 15. struct S { pub pub x: i32 } is a parse error
vis_log15="$vis_dir/test15.log"
"$compiler" /dev/stdin --build.compile-only >"$vis_log15" 2>&1 <<'EOF' || true
struct S {
    pub pub x: i32,
}
EOF
grep -Fq 'error[E_PARSE_EXPECTED_TOKEN]: expected struct field name, got `pub`' "$vis_log15" || fail "missing parse error for pub pub"

# 16. Opaque struct compilation and constraint validation
opaque_log1="$vis_dir/opaque_diag1.log"
"$compiler" /dev/stdin --build.compile-only >"$opaque_log1" 2>&1 <<'EOF' || true
struct OpaqueType;

pub fn invalid_storage(x: OpaqueType) -> void {}
EOF
grep -Fq "must have a non-void, non-zero-sized storage type" "$opaque_log1" || fail "missing storage type error for by-value opaque struct parameter"

opaque_log2="$vis_dir/opaque_diag2.log"
"$compiler" /dev/stdin --build.compile-only >"$opaque_log2" 2>&1 <<'EOF' || true
struct OpaqueType;

pub fn invalid_sizeof() -> usize {
    return @of.size(OpaqueType);
}
pub fn invalid_alignof() -> usize {
    return @of.align(OpaqueType);
}
pub fn invalid_offsetof() -> usize {
    return @of.offset(OpaqueType, .field);
}
EOF
grep -Fq "cannot compute @of.size for an incomplete or invalid type" "$opaque_log2" || fail "missing @of.size error for opaque struct"
grep -Fq "cannot compute @of.align for an incomplete or invalid type" "$opaque_log2" || fail "missing @of.align error for opaque struct"
grep -Fq "cannot compute @of.offset for an incomplete or invalid type" "$opaque_log2" || fail "missing @of.offset error for opaque struct"

"$compiler" /dev/stdin --build.compile-only --build.output="$vis_dir/opaque_ok.o" <<'EOF' || fail "opaque struct pointer compilation failed"
struct OpaqueHandle;
pub struct PublicOpaque;

pub fn identity(handle: *OpaqueHandle) -> *OpaqueHandle {
    return handle;
}

pub fn check_null(handle: *OpaqueHandle) -> bool {
    return handle == null;
}
EOF

# Test 67: Immutable-by-default function parameters
mut_param_dir="$work_dir/mut_param"
mkdir -p "$mut_param_dir"

# A: gcd without mut -> error[E_IMMUTABLE_ASSIGNMENT] with note and fix-it
mut_param_a="$mut_param_dir/a.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_a" 2>&1 <<'EOF' || true
pub fn gcd(a: u64, b: u64) -> u64 {
    for b != 0 {
        let t: u64 = b;
        b = a % b;
        a = t;
    }
    return a;
}
EOF
grep -Fq "error[E_IMMUTABLE_ASSIGNMENT]: cannot assign to immutable parameter \`b\`" "$mut_param_a" || fail "missing E_IMMUTABLE_ASSIGNMENT on parameter b"
grep -Fq "= note: parameter declared here" "$mut_param_a" || fail "missing parameter declared here note"
grep -Fq "= help: replace with \`mut b\`" "$mut_param_a" || fail "missing mut b fix-it"
grep -Fq "error[E_IMMUTABLE_ASSIGNMENT]: cannot assign to immutable parameter \`a\`" "$mut_param_a" || fail "missing E_IMMUTABLE_ASSIGNMENT on parameter a"
grep -Fq "= help: replace with \`mut a\`" "$mut_param_a" || fail "missing mut a fix-it"

# B: gcd with mut a, mut b -> compiles
"$compiler" /dev/stdin --build.compile-only "--build.output=$mut_param_dir/b.o" <<'EOF' || fail "mutable param gcd failed to compile"
pub fn gcd(mut a: u64, mut b: u64) -> u64 {
    for b != 0 {
        let t: u64 = b;
        b = a % b;
        a = t;
    }
    return a;
}
EOF

# C: compound assignment rejected on immutable parameter
mut_param_c="$mut_param_dir/c.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_c" 2>&1 <<'EOF' || true
pub fn test_compound(x: i32) -> void {
    x += 1;
}
EOF
grep -Fq "cannot assign to immutable parameter \`x\`" "$mut_param_c" || fail "missing compound assign failure on immutable parameter"

# D: pointee is still writable with immutable pointer parameter
"$compiler" /dev/stdin --build.compile-only "--build.output=$mut_param_dir/d.o" <<'EOF' || fail "pointee mutation on immutable param failed"
struct S {
    x: i32,
}
pub fn test_pointee(p: *mut S) -> void {
    p.x = 1;
    *p = { .x = 2 };
}
EOF

# E: pointer binding itself is immutable
mut_param_e="$mut_param_dir/e.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_e" 2>&1 <<'EOF' || true
struct S {
    x: i32,
}
pub fn test_ptr_reassign(p: *mut S, q: *mut S) -> void {
    p = q;
}
EOF
grep -Fq "cannot assign to immutable parameter \`p\`" "$mut_param_e" || fail "missing assignment failure when reassigning pointer parameter"

# F: by-value struct field write rejected vs mut compiles
mut_param_f="$mut_param_dir/f.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_f" 2>&1 <<'EOF' || true
struct S {
    x: i32,
}
pub fn test_struct_field(s: S) -> void {
    s.x = 1;
}
EOF
grep -Fq "left-hand side of assignment is not a valid lvalue" "$mut_param_f" || fail "missing lvalue error for by-value struct field on immutable param"

"$compiler" /dev/stdin --build.compile-only "--build.output=$mut_param_dir/f_ok.o" <<'EOF' || fail "by-value struct field on mut param failed"
struct S {
    x: i32,
}
pub fn test_struct_field_mut(mut s: S) -> void {
    s.x = 1;
}
EOF

# G: &x yields *T on immutable param, rejected when *mut T expected; compiles with mut x
mut_param_g="$mut_param_dir/g.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_g" 2>&1 <<'EOF' || true
pub fn take_mut(p: *mut i32) -> void {}
pub fn test_borrow(x: i32) -> void {
    take_mut(&x);
}
EOF
grep -Fq "cannot convert argument 1 from '*i32' to '*mut i32'" "$mut_param_g" || fail "missing type mismatch passing &immutable_param to *mut"

"$compiler" /dev/stdin --build.compile-only "--build.output=$mut_param_dir/g_ok.o" <<'EOF' || fail "passing &mut_param to *mut failed"
pub fn take_mut2(p: *mut i32) -> void {}
pub fn test_borrow_mut(mut x: i32) -> void {
    take_mut2(&x);
}
EOF

# H: generic function with mut parameter instantiated
"$compiler" /dev/stdin --build.compile-only "--build.output=$mut_param_dir/h.o" <<'EOF' || fail "generic function with mut parameter failed"
pub fn generic_func<T>(mut x: T) -> T {
    x = x;
    return x;
}
pub fn test_gen() -> i32 {
    return generic_func<i32>(42);
}
EOF

# I: impl method with mut parameter
"$compiler" /dev/stdin --build.compile-only "--build.output=$mut_param_dir/i.o" <<'EOF' || fail "impl method with mut parameter failed"
struct Counter {
    val: u32,
}
impl Counter {
    pub fn bump(mut n: u32) -> u32 {
        n += 1;
        return n;
    }
}
EOF

# J: mut not allowed on macro params, extern fn params, trait prototypes, and comptime type params
mut_param_j1="$mut_param_dir/j1.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_j1" 2>&1 <<'EOF' || true
macro m(mut x: expr) {
    x;
}
EOF
grep -Fq "\`mut\` is not allowed on macro parameters" "$mut_param_j1" || fail "missing macro mut param error"

mut_param_j2="$mut_param_dir/j2.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_j2" 2>&1 <<'EOF' || true
extern fn ext(mut x: i32) -> void;
EOF
grep -Fq "\`mut\` is not allowed on \`extern fn\` parameters" "$mut_param_j2" || fail "missing extern fn mut param error"

mut_param_j3="$mut_param_dir/j3.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_j3" 2>&1 <<'EOF' || true
trait Foo {
    fn bar(mut x: i32) -> void;
}
EOF
grep -Fq "\`mut\` is not allowed in trait method prototypes" "$mut_param_j3" || fail "missing trait method mut param error"

mut_param_j4="$mut_param_dir/j4.log"
"$compiler" /dev/stdin --build.compile-only >"$mut_param_j4" 2>&1 <<'EOF' || true
pub fn test_comptime(mut T: type) -> void {}
EOF
grep -Fq "\`mut\` is not allowed on comptime \`type\` parameters" "$mut_param_j4" || fail "missing comptime type mut param error"

# Variadic builtins mutate their operand (only the destination for @va.copy).
va_mut_dir="$work_dir/va_mutability"
mkdir -p "$va_mut_dir"
for builtin in start arg end copy; do
    case "$builtin" in
        arg) operation='let value: u32 = @va.arg(ap, u32);' ;;
        copy) operation='@va.copy(ap, src);' ;;
        *) operation="@va.$builtin(ap);" ;;
    esac
    for binding in local parameter; do
        va_log="$va_mut_dir/${builtin}_${binding}.log"
        if [ "$binding" = local ]; then
            signature='src: @va.list, ...'
            declaration='let ap: @va.list;'
        else
            signature='ap: @va.list, src: @va.list, ...'
            declaration=''
        fi
        if "$compiler" /dev/stdin --build.compile-only >"$va_log" 2>&1 <<EOF
#[abi("C")] pub fn test($signature) -> void {
    $declaration
    $operation
}
EOF
        then
            fail "@va.$builtin accepted immutable $binding"
        fi
        if [ "$binding" = local ]; then
            kind=binding
        else
            kind=parameter
        fi
        grep -Fq "error[E_IMMUTABLE_ASSIGNMENT]: @va.$builtin cannot mutate immutable $kind \`ap\`" "$va_log" || fail "missing @va.$builtin immutable $binding error"
        grep -Fq "= note: $kind declared here" "$va_log" || fail "missing @va.$builtin declaration note"
        grep -Fq '= help: replace with `mut ap`' "$va_log" || fail "missing @va.$builtin mut fixit"
        # Locals also retain the existing declaration errors for immutable,
        # uninitialized storage. Parameters should produce only the operand error.
        if [ "$binding" = parameter ]; then
            [ "$(grep -c 'error\[' "$va_log")" -eq 1 ] || fail "cascading @va.$builtin parameter errors"
        fi
    done
done

# The fixit targets the declaration, not the builtin operand.
va_json="$va_mut_dir/fixit.json"
if "$compiler" /dev/stdin --build.compile-only --diag.format=json >"$va_json" 2>&1 <<'EOF'
pub fn consume(ap: @va.list) -> u32 {
    return @va.arg(ap, u32);
}
EOF
then
    fail "@va.arg accepted immutable parameter in JSON test"
fi
grep -Fq '"code":"E_IMMUTABLE_ASSIGNMENT"' "$va_json" || fail "missing va JSON error code"
grep -Fq '"replacement":"mut ap"' "$va_json" || fail "missing va JSON mut fixit"
grep -Fq '"fixits":[{"span":{"file":"/dev/stdin","start":{"line":1,"column":16},"end":{"line":1,"column":18}}' "$va_json" || fail "va fixit does not target parameter declaration"

# Mutable locals support the complete lifecycle, including a mutable local copy source.
"$compiler" /dev/stdin --build.compile-only "--build.output=$va_mut_dir/locals.o" >"$va_mut_dir/locals.log" 2>&1 <<'EOF' || fail "mutable va locals failed"
#[abi("C")] pub fn sum(count: u32, ...) -> u32 {
    let mut ap: @va.list;
    @va.start(ap);
    let value: u32 = @va.arg(ap, u32);
    let mut copied: @va.list;
    @va.copy(copied, ap);
    let next: u32 = @va.arg(copied, u32);
    @va.end(copied);
    @va.end(ap);
    return value + next;
}
EOF

# Both immutable and mutable parameters can be copy sources. Only mutable
# parameters can be consumed or ended directly.
"$compiler" /dev/stdin --build.compile-only "--build.output=$va_mut_dir/params.o" >"$va_mut_dir/params.log" 2>&1 <<'EOF' || fail "va parameter mutability failed"
pub fn copy_immutable(src: @va.list) -> u32 {
    let mut dst: @va.list;
    @va.copy(dst, src);
    let value: u32 = @va.arg(dst, u32);
    @va.end(dst);
    return value;
}
pub fn copy_mutable(mut src: @va.list) -> u32 {
    let mut dst: @va.list;
    @va.copy(dst, src);
    let value: u32 = @va.arg(src, u32);
    @va.end(dst);
    @va.end(src);
    return value;
}
pub fn consume_one(mut ap: @va.list) -> u32 {
    return @va.arg(ap, u32);
}
EOF

# Mutability does not relax the existing local destination requirement.
for builtin in start copy; do
    case "$builtin" in
        start) operation='@va.start(ap);' ;;
        copy) operation='@va.copy(ap, src);' ;;
    esac
    va_log="$va_mut_dir/${builtin}_mutable_parameter.log"
    if "$compiler" /dev/stdin --build.compile-only >"$va_log" 2>&1 <<EOF
#[abi("C")] pub fn test(mut ap: @va.list, src: @va.list, ...) -> void {
    $operation
}
EOF
    then
        fail "@va.$builtin accepted parameter destination"
    fi
    grep -Fq "@va.$builtin destination must be a local @va.list variable" "$va_log" || fail "missing @va.$builtin local destination error"
    if grep -Fq 'E_IMMUTABLE_ASSIGNMENT' "$va_log"; then
        fail "@va.$builtin rejected mutable parameter as immutable"
    fi
done

sec_dir="$work_dir/compile_time_security"
mkdir -p "$sec_dir"
if "$compiler" /dev/stdin --build.compile-only --build.output="$sec_dir/oob.o" >"$sec_dir/oob.log" 2>&1 <<'EOF'
pub fn bad_index(s: []i32) -> i32 {
    let arr: [3]i32 = [1, 2, 3];
    let a: i32 = arr[-1];
    let b: i32 = arr[3];
    let c: i32 = s[-1];
    return a + b + c;
}
EOF
then
    fail "negative or out-of-bounds array/slice index compiled"
fi
grep -Fq "array index cannot be negative" "$sec_dir/oob.log" || fail "missing negative array index error"
grep -Fq "array index 3 is out of bounds for array of length 3" "$sec_dir/oob.log" || fail "missing out-of-bounds array index error"
grep -Fq "slice index cannot be negative" "$sec_dir/oob.log" || fail "missing negative slice index error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$sec_dir/ub.o" >"$sec_dir/ub.log" 2>&1 <<'EOF'
pub fn bad_ops(mut x: i32) -> i32 {
    let a: i32 = x / 0;
    let b: i32 = x % 0;
    x /= 0;
    let c: i32 = x << 32;
    let d: i32 = x >> -1;
    return a + b + c + d;
}
EOF
then
    fail "division by zero or out-of-bounds shift compiled"
fi
grep -Fq "division by zero" "$sec_dir/ub.log" || fail "missing division by zero error"
grep -Fq "modulo by zero" "$sec_dir/ub.log" || fail "missing modulo by zero error"
grep -Fq "shift count out of bounds for 32-bit operand" "$sec_dir/ub.log" || fail "missing shift bounds error"

# Generic trait bounds using qualified imported traits mark the import as used
trait_bound_import_dir="$work_dir/trait_bound_import"
mkdir -p "$trait_bound_import_dir"
cat > "$trait_bound_import_dir/iface.sl" <<'EOF'
mod iface;
pub trait Marker {
    fn mark(self: *Self) -> i32;
}
EOF
trait_bound_import_log="$trait_bound_import_dir/use_bound.log"
"$compiler" /dev/stdin --build.compile-only --build.output="$trait_bound_import_dir/use_bound.o" >"$trait_bound_import_log" 2>&1 <<EOF || fail "generic trait bound failed to compile"
mod consumer;
import "$trait_bound_import_dir/iface.sl";
pub fn run<T: iface.Marker>(v: *T) -> i32 {
    return v.mark();
}
EOF
if grep -Fq "unused import" "$trait_bound_import_log"; then
    fail "unexpected unused import warning when qualified trait is used in generic bound"
fi

# Strong typing checks: bool<->ptr/float/enum casts, volatile-discarding casts, *void/*Opaque deref
strong_dir="$work_dir/strong_typing"
mkdir -p "$strong_dir"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/cast_bool.o" >"$strong_dir/cast_bool.log" 2>&1 <<'EOF'
enum Color: u8 { Red, Green }
pub fn bad_casts(b: bool, p: *u8, x: f64, c: Color) -> void {
    let p2: *u8 = b cast *u8;
    let b2: bool = p cast bool;
    let f2: f64 = b cast f64;
    let b3: bool = x cast bool;
    let c2: Color = b cast Color;
    let _p2: *u8 = p2;
    let _b2: bool = b2;
    let _f2: f64 = f2;
    let _b3: bool = b3;
    let _c2: Color = c2;
    let _c: Color = c;
}
EOF
then
    fail "invalid bool casts compiled"
fi
grep -Fq 'cannot cast between `bool` and `*u8`' "$strong_dir/cast_bool.log" || fail "missing bool->ptr cast error"
grep -Fq 'cannot cast between `*u8` and `bool`' "$strong_dir/cast_bool.log" || fail "missing ptr->bool cast error"
grep -Fq 'cannot cast between `bool` and `f64`' "$strong_dir/cast_bool.log" || fail "missing bool->f64 cast error"
grep -Fq 'cannot cast between `f64` and `bool`' "$strong_dir/cast_bool.log" || fail "missing f64->bool cast error"
grep -Fq 'cannot cast between `bool` and' "$strong_dir/cast_bool.log" || fail "missing bool->enum cast error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/cast_vol.o" >"$strong_dir/cast_vol.log" 2>&1 <<'EOF'
pub fn drop_vol(p: *volatile u32) -> *u32 {
    return p cast *u32;
}
EOF
then
    fail "volatile-discarding cast compiled"
fi
grep -Fq 'cast cannot discard `volatile` qualifier' "$strong_dir/cast_vol.log" || fail "missing volatile cast-away error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/deref_inc.o" >"$strong_dir/deref_inc.log" 2>&1 <<'EOF'
struct Opaque;
pub fn bad_deref(vp: *void, op: *Opaque) -> void {
    *vp;
    *op;
}
EOF
then
    fail "dereference of *void or *Opaque compiled"
fi
grep -Fq 'cannot dereference pointer to incomplete type `void`' "$strong_dir/deref_inc.log" || fail "missing *void deref error"
grep -Fq 'cannot dereference pointer to incomplete type `Opaque`' "$strong_dir/deref_inc.log" || fail "missing *Opaque deref error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/defer_empty.o" >"$strong_dir/defer_empty.log" 2>&1 <<'EOF'
pub fn bad_defer() -> void {
    defer ;
}
EOF
then
    fail "empty defer compiled"
fi
grep -Fq 'defer requires a statement' "$strong_dir/defer_empty.log" || fail "missing empty defer error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/defer_nested.o" >"$strong_dir/defer_nested.log" 2>&1 <<'EOF'
fn ping() -> void {}
pub fn bad_nested_defer() -> void {
    defer defer ping();
}
EOF
then
    fail "nested single-stmt defer compiled"
fi
grep -Fq 'nested defer is not allowed' "$strong_dir/defer_nested.log" || fail "missing nested defer error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/if_missing_else.o" >"$strong_dir/if_missing_else.log" 2>&1 <<'EOF'
pub fn bad_if_expr(y: u8) -> u8 {
    let x: u8 = if y < 2 {
        yield y;
    };
    return x;
}
EOF
then
    fail "if block expression without else compiled"
fi
grep -Fq 'block expression does not yield a value on every path' "$strong_dir/if_missing_else.log" || fail "missing if without else error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/for_bounded.o" >"$strong_dir/for_bounded.log" 2>&1 <<'EOF'
pub fn bad_for_expr() -> i32 {
    let x: i32 = for let mut i: i32 = 0; i < 10; i += 1 {
        yield i;
    };
    return x;
}
EOF
then
    fail "bounded for block expression compiled"
fi
grep -Fq 'block expression does not yield a value on every path' "$strong_dir/for_bounded.log" || fail "missing bounded for error"

if "$compiler" /dev/stdin --build.compile-only --build.output="$strong_dir/for_break.o" >"$strong_dir/for_break.log" 2>&1 <<'EOF'
pub fn bad_for_break(flag: bool) -> i32 {
    let x: i32 = for {
        if flag {
            break;
        }
        yield 1;
    };
    return x;
}
EOF
then
    fail "for block expression with break compiled"
fi
grep -Fq 'block expression does not yield a value on every path' "$strong_dir/for_break.log" || fail "missing for with break error"

# Mandatory `->` in functions, function types, traits, and impls
arrow_dir="$work_dir/mandatory_arrow"
mkdir -p "$arrow_dir"

for case_name in fn_ret fn_void fn_omitted extern_fn fn_type trait_fn impl_fn; do
    case "$case_name" in
        fn_ret) code='pub fn f() i32 { return 1; }' ;;
        fn_void) code='pub fn f() void {}' ;;
        fn_omitted) code='pub fn f() {}' ;;
        extern_fn) code='extern fn f() void;' ;;
        fn_type) code='pub fn f() -> void { let p: fn(i32) i32 = null; }' ;;
        trait_fn) code='trait T { fn m(self: *Self) void; }' ;;
        impl_fn) code='struct S { x: i32 } impl S { fn m(self: *Self) void {} }' ;;
    esac
    if printf '%s\n' "$code" | "$compiler" /dev/stdin --build.compile-only --build.output="$arrow_dir/$case_name.o" >"$arrow_dir/$case_name.log" 2>&1; then
        fail "missing '->' in $case_name compiled unexpectedly"
    fi
    grep -Fq 'expected `->`' "$arrow_dir/$case_name.log" || fail "missing expected \`->\` diagnostic for $case_name"
done

echo "All diagnostic tests passed successfully!"
