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

# 1. C <-> Stella #[repr("C")] tagged enum ABI interoperability
cat > "$work/c_abi.c" <<'EOF'
#include <stdint.h>

typedef struct {
	uint8_t tag;
	union {
		int32_t a;
		double b;
	} payload;
} CMsg;

CMsg c_transform_msg(CMsg m) {
	if (m.tag == 0) {
		return m;
	}
	if (m.tag == 1) {
		m.payload.a += 100;
		return m;
	}
	m.payload.b *= 2.0;
	return m;
}
EOF
"$cc" -c "$work/c_abi.c" -o "$work/c_abi.o"

# 2. Cross-module generic tagged enum
cat > "$work/option_mod.sl" <<'EOF'
mod option_mod;

pub enum Option<T>: u8 {
	None,
	Some(T)
}

pub enum Result<T, E>: u8 {
	Ok(T),
	Err(E)
}
EOF

# 3. Main runtime test covering all tagged enum & match features
cat > "$work/main.sl" <<'EOF'
mod main;
import "./option_mod.sl";

enum PlainCode: u8 {
	Zero = 0,
	One = 1,
	Two = 2
}

enum WideU64: u64 {
	Zero = 0,
	HighBit = 9223372036854775808,
	MaxU64 = 18446744073709551615
}

struct EmptyPayload {}

enum ZstEnum: u8 {
	A,
	B(EmptyPayload)
}

#[align(32)]
struct Aligned32 {
	pub x: u64
}

enum OverAlignedEnum: u8 {
	Small(u8),
	Big(Aligned32)
}

#[repr("C")]
pub enum CMsg: u8 {
	Empty = 0,
	Int(i32) = 1,
	Flt(f64) = 2
}

#[abi("C"), link_name("c_transform_msg")]
extern fn c_transform_msg(m: CMsg) -> CMsg;

enum Expr: u8 {
	Lit(i32),
	Pair(i32, i32),
	Move { x: i32, y: i32 },
	Zero
}

enum List: u8 {
	Nil,
	Cons(i32, *List)
}

fn match_int(x: i32) -> i32 {
	let mut acc: i32 = 0;
	for let mut i: i32 = 0; i < 5; i += 1 {
		match x + i {
			0 | 1 => acc += 10,
			2 => {
				acc += 100;
				break; // must break out of `for`, not `match`
			}
			_ => acc += 1,
		}
	}
	return acc;
}

fn match_plain(c: PlainCode) -> i32 {
	match c {
		.Zero => return 10,
		.One => return 20,
		.Two => return 30
	}
}

fn match_u64(w: WideU64) -> i32 {
	match w {
		.Zero => { return 1; }
		.HighBit => { return 2; }
		.MaxU64 => { return 3; }
	}
}

fn eval_expr(e: Expr) -> i32 {
	let res: i32 = {
		match e {
			.Lit(v) => yield v,
			.Pair(a, b) => yield a + b,
			.Move { x, y } => yield x * 10 + y,
			.Zero => yield 0,
		}
	};
	return res;
}

fn sum_list(node: *List) -> i32 {
	if node == null { return 0; }
	match node {
		.Nil => return 0,
		.Cons(head, tail) => return *head + sum_list(*tail),
	}
}

fn make_ok(v: i32) -> option_mod.Result<i32, PlainCode> {
	return .Ok(v);
}

#[abi("C"), link_name("main")]
pub fn main() -> i32 {
	// Integer match + break targeting enclosing loop
	if match_int(0) != 120 { return 1; }
	// Plain enum exhaustive match + contextual shorthand
	if match_plain(.One) != 20 { return 2; }
	// Full-range u64 discriminants
	if match_u64(.Zero) != 1 { return 3; }
	if match_u64(.HighBit) != 2 { return 4; }
	if match_u64(.MaxU64) != 3 { return 5; }

	// Tagged enum unit, tuple, struct variants + copy
	let e1: Expr = .Lit(7);
	let e2: Expr = .Pair(3, 4);
	let e3: Expr = .Move { x: 2, y: 5 };
	let e4: Expr = .Zero;
	let e3_copy: Expr = e3;
	if eval_expr(e1) != 7 { return 6; }
	if eval_expr(e2) != 7 { return 7; }
	if eval_expr(e3) != 25 { return 8; }
	if eval_expr(e3_copy) != 25 { return 9; }
	if eval_expr(e4) != 0 { return 10; }

	// Zero-copy pointer matching: *Enum (immutable) & *mut Enum (in-place mutation)
	let mut mut_e: Expr = .Move { x: 1, y: 2 };
	match &mut_e {
		.Move { x, y } => {
			*x += 4;
			*y += 8;
		}
		_ => {}
	}
	if eval_expr(mut_e) != 60 { return 11; }

	let ro_e: Expr = .Pair(11, 22);
	match &ro_e {
		.Pair(a, b) => {
			if *a + *b != 33 { return 12; }
		}
		_ => {}
	}

	// Cross-module generic tagged enum
	let r: option_mod.Result<i32, PlainCode> = make_ok(42);
	match r {
		.Ok(v) => { if v != 42 { return 13; } }
		.Err(_) => return 14,
	}
	let o: option_mod.Option<i32> = option_mod.Option<i32>.None;
	match o {
		.None => {}
		.Some(_) => return 15,
	}

	// Recursive pointer enum
	let n2: List = .Cons(5, null);
	let n1: List = .Cons(10, &n2);
	if sum_list(&n1) != 15 { return 16; }

	// ZST payload & over-aligned payload layout
	if @of.size(ZstEnum) != 1 { return 17; }
	if @of.align(OverAlignedEnum) < 32 { return 18; }
	if @of.size(CMsg) != 16 { return 19; }

	// C ABI round-trip
	let c1: CMsg = c_transform_msg(CMsg.Int(23));
	match c1 {
		.Int(v) => { if v != 123 { return 20; } }
		_ => return 21,
	}
	let c2: CMsg = c_transform_msg(.Flt(3.5));
	match c2 {
		.Flt(f) => { if f != 7.0 { return 22; } }
		_ => return 23,
	}

	return 0;
}
EOF

"$compiler" "$work/main.sl" --tar.relocation=pic --build.compile-only --build.output="$work/main.o" \
	|| fail "failed to compile runtime test"
"$cc" "$work/main.o" "$work/c_abi.o" -o "$work/main_bin" \
	|| fail "failed to link runtime test"
"$work/main_bin" || fail "runtime test exited with non-zero status"

# 4. Verify LLVM IR for integer and plain-enum match uses a single switch instruction
cat > "$work/ir_check.sl" <<'EOF'
mod ir_check;

enum Color: u8 { Red, Green, Blue }

pub fn sw_int(x: i32) -> i32 {
	match x {
		0 => return 10,
		1 | 2 => return 20,
		_ => return 30,
	}
}

pub fn sw_enum(c: Color) -> i32 {
	match c {
		.Red => return 1,
		.Green => return 2,
		.Blue => return 3,
	}
}
EOF
"$compiler" "$work/ir_check.sl" --emit.ir --build.output="$work/ir_check.ll" \
	|| fail "failed to emit IR for ir_check.sl"
grep -Eq "switch i32 %x[0-9]*, label %match.default" "$work/ir_check.ll" \
	|| fail "expected direct switch i32 in sw_int IR"
grep -Eq "switch i8 %c[0-9]*, label %match.unreach" "$work/ir_check.ll" \
	|| fail "expected direct switch i8 with unreachable default in sw_enum IR"

# 5. Negative / diagnostic tests
expect_fail() {
	name="$1"
	expected="$2"
	src="$work/$name.sl"
	log="$work/$name.log"
	cat > "$src"
	if "$compiler" "$src" --build.compile-only --build.output="$work/$name.o" >"$log" 2>&1; then
		fail "$name unexpectedly succeeded"
	fi
	if ! grep -Fq "$expected" "$log"; then
		cat "$log" >&2
		fail "$name missing expected diagnostic: $expected"
	fi
}

expect_fail "dup_discriminant" "duplicate enum discriminant value" <<'EOF'
mod t;
enum Bad: u8 { A = 1, B = 0, C }
EOF

expect_fail "overflow_discriminant" "does not fit backing type" <<'EOF'
mod t;
enum Bad: u8 { A = 256 }
EOF

expect_fail "non_exhaustive_enum" "non-exhaustive match on enum" <<'EOF'
mod t;
enum E: u8 { A, B }
fn test_fn(e: E) -> i32 {
	match e {
		.A => return 1,
	}
}
EOF

expect_fail "non_exhaustive_int" "non-exhaustive match on integer type" <<'EOF'
mod t;
fn test_fn(x: i32) -> i32 {
	match x {
		0 => return 1,
	}
}
EOF

expect_fail "unreachable_arm" "unreachable match arm" <<'EOF'
mod t;
fn test_fn(x: i32) -> i32 {
	match x {
		_ => return 1,
		0 => return 2,
	}
}
EOF

expect_fail "duplicate_pattern" "duplicate pattern in match" <<'EOF'
mod t;
enum E: u8 { A, B }
fn test_fn(e: E) -> i32 {
	match e {
		.A => return 1,
		.A => return 2,
		.B => return 3,
	}
}
EOF

expect_fail "or_pattern_binding" "bindings are not allowed in" <<'EOF'
mod t;
enum E: u8 { A(i32), B(i32) }
fn test_fn(e: E) -> i32 {
	match e {
		.A(x) | .B(x) => return x,
	}
}
EOF

expect_fail "partial_struct_pattern" "missing field" <<'EOF'
mod t;
enum E: u8 { Move { x: i32, y: i32 } }
fn test_fn(e: E) -> i32 {
	match e {
		.Move { x } => return x,
	}
}
EOF

expect_fail "missing_fat_arrow" 'expected `=>` after match pattern' <<'EOF'
mod t;
fn test_fn(x: i32) -> i32 {
	match x {
		0 { return 1; }
		_ => return 2,
	}
}
EOF

expect_fail "semicolon_in_match_arm" 'expected `,` but got `;`' <<'EOF'
mod t;
fn test_fn(x: i32) -> i32 {
	match x {
		0 => return 1;
		_ => return 2,
	}
}
EOF

expect_fail "recursive_by_value" "by-value recursive aggregate cycle" <<'EOF'
mod t;
enum Node: u8 { Leaf, Branch(Node) }
EOF

expect_fail "removed_switch" "error" <<'EOF'
mod t;
fn test_fn(x: i32) -> i32 {
	switch x {
		case 0 { return 1; }
		default { return 2; }
	}
}
EOF

expect_fail "tagged_enum_arith" "requires numeric operands" <<'EOF'
mod t;
enum E: u8 { A(i32), B(u8) }
fn test_fn(a: E, b: E) -> E {
	return a + b;
}
EOF

expect_fail "tagged_enum_eq" "cannot be applied" <<'EOF'
mod t;
enum E: u8 { A(i32), B(u8) }
fn test_fn(a: E, b: E) -> bool {
	return a == b;
}
EOF

expect_fail "tagged_enum_cast" "must be scalar" <<'EOF'
mod t;
enum E: u8 { A(i32), B(u8) }
fn test_fn(a: E) -> u8 {
	return a cast u8;
}
EOF

expect_fail "tagged_enum_bitcast" "bitcast requires scalar" <<'EOF'
mod t;
enum E: u8 { A(i32), B(u8) }
fn test_fn(a: E) -> u64 {
	return a bitcast u64;
}
EOF

expect_fail "unit_enum_arith" "requires numeric operands" <<'EOF'
mod t;
enum Color: u8 { Red, Green, Blue }
fn test_fn(a: Color, b: Color) -> Color {
	return a + b;
}
EOF

expect_fail "tagged_enum_ordering" "cannot be applied" <<'EOF'
mod t;
enum E: u8 { A(i32), B(u8) }
fn test_fn(a: E, b: E) -> bool {
	return a < b;
}
EOF
