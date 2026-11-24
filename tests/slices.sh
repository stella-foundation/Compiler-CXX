#!/bin/sh
set -eu

compiler=$1
cc=$2
work_dir=$3

mkdir -p "$work_dir"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# 1. Runtime test: slices, [_]T, "" as []u8, c"" as *u8, slicing, mutability, coercions, generics
cat >"$work_dir/slices_test.sl" <<'EOF'
pub const GREET: []u8 = "Hello, world!";
pub const CGREET: *u8 = c"Hello, world!";
pub const EMPTY: []u8 = "";
pub const EMPTY_ARR_LIT: []u8 = [];

fn sum_slice(s: []u8) -> u32 {
	let mut total: u32 = 0;
	for let mut i: usize = 0; i < s.len; i += 1 {
		total += s[i] cast u32;
	}
	return total;
}

fn fill_slice(s: []mut u8, val: u8) -> void {
	for let mut i: usize = 0; i < s.len; i += 1 {
		s[i] = val;
	}
}

fn first_elem<T>(s: []T) -> T {
	return s[0];
}

#[abi("C"), link_name("run_slice_tests")]
pub fn run_slice_tests() -> i32 {
	let a: [_]u8 = [1, 2, 3, 4];
	let b: [4]u8 = [1, 2, 3, 4];
	if a.len != 4 || b.len != 4 {
		return 1;
	}

	let c: []u8 = a[0..4];
	if c.len != 4 || sum_slice(c) != 10 {
		return 2;
	}
	if sum_slice(a[1..3]) != 5 {
		return 3;
	}
	if sum_slice(a[..2]) != 3 {
		return 4;
	}
	if sum_slice(a[2..]) != 7 {
		return 5;
	}
	if sum_slice(a[..]) != 10 {
		return 6;
	}
	if sum_slice(c[1..3]) != 5 {
		return 7;
	}

	// Coercions: *[N]T -> []T and *mut [N]T -> []mut T -> []T
	if sum_slice(&b) != 10 {
		return 8;
	}
	if first_elem(&b) != 1 {
		return 9;
	}

	// Mutable slice and mutation through []mut u8
	let mut buf: [4]u8 = [10, 20, 30, 40];
	fill_slice(buf[1..3], 7);
	if buf[0] != 10 || buf[1] != 7 || buf[2] != 7 || buf[3] != 40 {
		return 10;
	}
	let ms: []mut u8 = &buf;
	if sum_slice(ms) != 64 {
		return 11;
	}

	// Pointer slicing
	let p: *u8 = &a[0];
	let ps: []u8 = p[1..4];
	if ps.len != 3 || sum_slice(ps) != 9 {
		return 12;
	}

	// String literals: "" is []u8, c"" is *u8
	let text: []u8 = "Hello, world!";
	let c_text: *u8 = c"Hello, world!";
	if text.len != 13 || GREET.len != 13 {
		return 13;
	}
	if text[0] != 'H' || text[12] != '!' {
		return 14;
	}
	if c_text[0] != 'H' || c_text[12] != '!' || c_text[13] != 0 || CGREET[13] != 0 {
		return 15;
	}

	// Empty slices and empty c-string
	if EMPTY.len != 0 || EMPTY_ARR_LIT.len != 0 || EMPTY_ARR_LIT.ptr != null {
		return 16;
	}
	let c_empty: *u8 = c"";
	if c_empty[0] != 0 {
		return 17;
	}

	// Interior NUL bytes in both "" and c""
	let nul_slice: []u8 = "a\0b";
	let nul_cstr: *u8 = c"a\0b";
	if nul_slice.len != 3 || nul_slice[0] != 'a' || nul_slice[1] != 0 || nul_slice[2] != 'b' {
		return 18;
	}
	if nul_cstr[0] != 'a' || nul_cstr[1] != 0 || nul_cstr[2] != 'b' || nul_cstr[3] != 0 {
		return 19;
	}

	// Array initialization from "" (no NUL) and c"" (with NUL)
	let arr_no_nul: [_]u8 = "hi";
	let arr_with_nul: [_]u8 = c"hi";
	if arr_no_nul.len != 2 || arr_no_nul[0] != 'h' || arr_no_nul[1] != 'i' {
		return 20;
	}
	if arr_with_nul.len != 3 || arr_with_nul[0] != 'h' || arr_with_nul[1] != 'i' || arr_with_nul[2] != 0 {
		return 21;
	}

	return 0;
}
EOF

cat >"$work_dir/driver.c" <<'EOF'
#include <stdio.h>
extern int run_slice_tests(void);
int main(void) {
	int rc = run_slice_tests();
	if (rc != 0) {
		fprintf(stderr, "slice test failed with code %d\n", rc);
		return 1;
	}
	return 0;
}
EOF

"$compiler" "$work_dir/slices_test.sl" --build.compile-only --tar.relocation=pic "--build.output=$work_dir/slices_test.o"
"$cc" "$work_dir/driver.c" "$work_dir/slices_test.o" -o "$work_dir/slices_runner"
"$work_dir/slices_runner"

# 2. Diagnostic tests
expect_fail() {
	name=$1
	expected=$2
	log="$work_dir/$name.log"
	if "$compiler" /dev/stdin --build.compile-only "--build.output=$work_dir/$name.o" >"$log" 2>&1; then
		fail "expected $name to fail compilation"
	fi
	grep -Fq "$expected" "$log" || {
		cat "$log" >&2
		fail "missing expected diagnostic '$expected' in $name"
	}
}

expect_fail "immutable_slice_write" "lvalue" <<'EOF'
pub fn test_fn(s: []u8) -> void {
	s[0] = 1;
}
EOF

expect_fail "ptr_slice_no_end" "slicing a pointer requires an explicit end bound" <<'EOF'
pub fn test_fn(p: *u8) -> []u8 {
	return p[0..];
}
EOF

expect_fail "arr_slice_oob" "slice end bound is out of bounds for array" <<'EOF'
pub fn test_fn() -> []u8 {
	let a: [4]u8 = [1, 2, 3, 4];
	return a[0..5];
}
EOF

expect_fail "arr_slice_inverted" "slice start bound exceeds end bound" <<'EOF'
pub fn test_fn() -> []u8 {
	let a: [4]u8 = [1, 2, 3, 4];
	return a[3..1];
}
EOF

expect_fail "string_not_cstr" "type mismatch" <<'EOF'
pub fn test_fn() -> *u8 {
	return "hello";
}
EOF

expect_fail "slice_eq_disallowed" "cannot be applied" <<'EOF'
pub fn test_fn(a: []u8, b: []u8) -> bool {
	return a == b;
}
EOF
