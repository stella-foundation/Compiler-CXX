#!/usr/bin/env sh
set -eu

compiler="$1"
cc="$2"
work="$3"

mkdir -p "$work"

fail() {
	echo "FAIL: $1" >&2
	exit 1
}

# 1. Runtime C <-> Stella ABI & Repr interoperability test
cat > "$work/c_side.c" <<'EOF'
#include <stdint.h>

typedef struct {
	int32_t a;
	float b;
} SmallMixed;

typedef struct {
	int32_t a;
	int32_t b;
	int32_t c;
} TripleI32;

typedef struct {
	int64_t x;
	int64_t y;
	int64_t z;
} BigStruct;

int32_t c_shared_counter = 10;

SmallMixed c_roundtrip_small(SmallMixed s) {
	s.a += 5;
	s.b += 1.5f;
	return s;
}

TripleI32 c_roundtrip_triple(TripleI32 t) {
	t.a += 1;
	t.b += 2;
	t.c += 3;
	return t;
}

BigStruct c_roundtrip_big(BigStruct b) {
	b.x += 100;
	b.y += 200;
	b.z += 300;
	return b;
}

int32_t c_invoke_cb(int32_t (*cb)(int32_t), int32_t v) {
	return cb(v) + 1;
}
EOF

cat > "$work/stella_side.sl" <<'EOF'
mod test_abi;

// Default #[repr("Stella")] reorders { u8, u64, u8 } from 24B -> 16B.
#[layout(16, 8)]
struct Reordered {
	a: u8,
	b: u64,
	c: u8,
}

// #[repr("C")] preserves declaration order: 24B.
#[repr("C"), layout(24, 8)]
struct CLayout {
	a: u8,
	b: u64,
	c: u8,
}

struct OpaqueHandle {
	secret: u64,
}

#[repr("C")]
struct SmallMixed {
	a: i32,
	b: f32,
}

#[repr("C")]
struct TripleI32 {
	a: i32,
	b: i32,
	c: i32,
}

#[repr("C")]
struct BigStruct {
	x: i64,
	y: i64,
	z: i64,
}

#[link_name]
extern let mut c_shared_counter: i32;

#[abi("C"), link_name]
extern fn c_roundtrip_small(s: SmallMixed) -> SmallMixed;

#[abi("C"), link_name]
extern fn c_roundtrip_triple(t: TripleI32) -> TripleI32;

#[abi("C"), link_name]
extern fn c_roundtrip_big(b: BigStruct) -> BigStruct;

#[abi("C"), link_name]
extern fn c_invoke_cb(cb: #[abi("C")] fn(i32) -> i32, v: i32) -> i32;

// #[abi("C")] keeps Stella mangling unless #[link_name] is present.
#[abi("C")]
pub fn mangled_c_abi(x: i32) -> i32 {
	return x + 1;
}

#[abi("C")]
fn add_ten_c(x: i32) -> i32 {
	return x + 10;
}

fn stella_fast_helper(r: Reordered) -> u64 {
	return (r.a cast u64) + r.b + (r.c cast u64);
}

#[abi("C"), link_name]
pub fn test_opaque_ptr(h: *OpaqueHandle) -> u64 {
	return h.secret;
}

#[abi("C"), link_name("run_abi_tests")]
pub fn run_abi_tests() -> i32 {
	// 1. Verify field reordering & offsets
	if @of.offset(Reordered, .b) != 0 { return 1; }
	if @of.offset(CLayout, .b) != 8 { return 2; }

	let r: Reordered = { .a = 3, .b = 100, .c = 7 };
	if stella_fast_helper(r) != 110 { return 3; }

	// 2. Verify extern global
	c_shared_counter += 32;
	if c_shared_counter != 42 { return 4; }

	// 3. Verify C-ABI struct passing/returning (8B, 12B, 24B sret/byval)
	let sm: SmallMixed = c_roundtrip_small({ .a = 10, .b = 2.0 });
	if sm.a != 15 || sm.b != 3.5 { return 5; }

	let tr: TripleI32 = c_roundtrip_triple({ .a = 1, .b = 2, .c = 3 });
	if tr.a != 2 || tr.b != 4 || tr.c != 6 { return 6; }

	let bg: BigStruct = c_roundtrip_big({ .x = 1, .y = 2, .z = 3 });
	if bg.x != 101 || bg.y != 202 || bg.z != 303 { return 7; }

	// 4. Verify #[abi("C")] function pointer callback
	if c_invoke_cb(add_ten_c, 5) != 16 { return 8; }

	// 5. Verify #[abi("C")] Stella-mangled fn callable from Stella
	if mangled_c_abi(41) != 42 { return 9; }

	let oh: OpaqueHandle = { .secret = 777 };
	if test_opaque_ptr(&oh) != 777 { return 10; }

	// 6. Verify variadic macro array-literal splice mapping ([box(...)])
	if sum_mapped(10, 20, 30) != 60 { return 11; }
	if sum_mapped() != 0 { return 12; }

	// 7. Verify generic overload specificity: Concrete > Constrained Generic > Unconstrained Generic
	let sv_i32: i32 = 1;
	let sv_i64: i64 = 1;
	let sv_bool: bool = true;
	if pick_spec(sv_i32) != 1 { return 13; }
	if pick_spec(sv_i64) != 2 { return 14; }
	if pick_spec(sv_bool) != 3 { return 15; }

	return 0;
}

fn pick_spec(x: i32) -> i32 { return 1; }
fn pick_spec<T: i>(x: T) -> i32 { return 2; }
fn pick_spec<T>(x: T) -> i32 { return 3; }

fn id_i32(x: i32) -> i32 {
	return x;
}

fn sum_slice(xs: []i32) -> i32 {
	let mut acc: i32 = 0;
	for let mut i: usize = 0; i < xs.len; i += 1 {
		acc += xs[i];
	}
	return acc;
}

macro sum_mapped(...) -> i32 {
	yield sum_slice(([id_i32(...)])[..]);
}
EOF

cat > "$work/main.c" <<'EOF'
#include <stdint.h>
int32_t run_abi_tests(void);
int main(void) {
	return run_abi_tests();
}
EOF

"$compiler" "$work/stella_side.sl" --emit.ir --build.output="$work/stella_side.ll"
"$compiler" "$work/stella_side.sl" --build.compile-only --build.output="$work/stella_side.o"
"$cc" -c "$work/c_side.c" -o "$work/c_side.o"
"$cc" "$work/main.c" "$work/c_side.o" "$work/stella_side.o" -o "$work/abi_test"
"$work/abi_test"

# 2. Verify IR calling conventions & mangling
grep -Fq 'define internal fastcc i64 @"st$test_abi.stella_fast_helper' "$work/stella_side.ll" || fail "expected fastcc on native Stella fn"
grep -Fq 'define i32 @"st$test_abi.mangled_c_abi' "$work/stella_side.ll" || fail "expected #[abi(\"C\")] without #[link_name] to remain Stella-mangled"
grep -Fq 'define i32 @run_abi_tests()' "$work/stella_side.ll" || fail "expected #[link_name(\"run_abi_tests\")] symbol"

# 3. Negative diagnostic checks
expect_err() {
	name="$1"
	expected="$2"
	if "$compiler" /dev/stdin --build.compile-only --build.output="$work/$name.o" > "$work/$name.log" 2>&1; then
		fail "$name unexpectedly succeeded"
	fi
	grep -Fq "$expected" "$work/$name.log" || fail "$name missing expected diagnostic '$expected'"
}

expect_err ffi_byval_stella_struct 'is not FFI-safe' <<'EOF'
struct S { x: i32 }
#[abi("C")] pub fn take_s(s: S) -> void {}
EOF

expect_err repr_c_contains_stella 'cannot contain #[repr("Stella")]' <<'EOF'
struct S { x: i32 }
#[repr("C")] struct C { s: S }
EOF

expect_err fn_ptr_abi_mismatch 'mismatch' <<'EOF'
fn stella_cb(x: i32) -> i32 { return x; }
pub fn bad() -> #[abi("C")] fn(i32) -> i32 { return stella_cb; }
EOF

expect_err must_tail_cross_abi 'requires every reachable return to be a direct call with an LLVM-compatible signature' <<'EOF'
#[abi("C")] fn c_target(x: i32) -> i32 { return x; }
#[must_tail] pub fn caller(x: i32) -> i32 { return c_target(x); }
EOF

expect_err stella_variadic 'variadic functions (...) are only supported with #[abi("C")]' <<'EOF'
pub fn bad(x: i32, ...) -> void {}
EOF

expect_err bad_abi_name 'unsupported ABI' <<'EOF'
#[abi("Bogus")] pub fn bad_abi() -> void {}
EOF

expect_err bad_repr_name 'unsupported representation' <<'EOF'
#[repr("Bogus")] struct S { x: i32 }
EOF

expect_err untyped_lit_generic 'no matching public function' <<'EOF'
fn only_gen<T: i>(x: T) -> i32 { return 1; }
pub fn bad() -> i32 { return only_gen(42); }
EOF

expect_err dup_generic_overload 'duplicate overload for generic function' <<'EOF'
fn dup<T: i>(x: T) -> i32 { return 1; }
fn dup<U: i>(x: U) -> i32 { return 2; }
EOF

echo "All ABI and data representation tests passed!"
