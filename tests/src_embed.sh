#!/bin/sh
set -eu

compiler=$1
cc=$2
work_dir=$3

mkdir -p "$work_dir/data" "$work_dir/sub"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# Create fixture files (text, binary with NUL/high bytes, 4-byte, empty, sub-module file)
printf 'Hello, Stella!' >"$work_dir/data/hello.txt"
printf '\000\377\001\177\000\042' >"$work_dir/data/binary.bin"
printf 'ABCD' >"$work_dir/data/four.bin"
: >"$work_dir/data/empty.bin"
printf 'SUB!' >"$work_dir/sub/local.txt"

cat >"$work_dir/sub/helper.sl" <<'EOF'
mod sub.helper;

pub const SUB_DATA: []u8 = @src.embed("local.txt");

pub macro sub_embed() -> []u8 {
	yield @src.embed("local.txt");
}

pub macro caller_embed(path: []u8) -> []u8 {
	yield @src.embed(path);
}
EOF

cat >"$work_dir/embed_test.sl" <<'EOF'
import "./sub/helper.sl";

pub const EMBED_SLICE: []u8 = @src.embed("data/hello.txt");
pub const EMBED_INFER_ARRAY: [_]u8 = @src.embed("data/binary.bin");
pub const EMBED_EXPLICIT_ARRAY: [4]u8 = @src.embed("data/four.bin");
pub const EMBED_EMPTY: []u8 = @src.embed("data/empty.bin");

fn generic_third<T>(idx: usize) -> T {
	let bytes: []u8 = @src.embed("data/four.bin");
	return bytes[idx] cast T;
}

#[abi("C"), link_name("run_embed_tests")]
pub fn run_embed_tests() -> i32 {
	if EMBED_SLICE.len != 14 {
		return 1;
	}
	if EMBED_SLICE[0] != 'H' || EMBED_SLICE[13] != '!' {
		return 2;
	}

	if EMBED_INFER_ARRAY.len != 6 {
		return 3;
	}
	if EMBED_INFER_ARRAY[0] != 0 || EMBED_INFER_ARRAY[1] != 0xFF ||
	   EMBED_INFER_ARRAY[2] != 1 || EMBED_INFER_ARRAY[3] != 0x7F ||
	   EMBED_INFER_ARRAY[4] != 0 || EMBED_INFER_ARRAY[5] != 0x22 {
		return 4;
	}

	if EMBED_EXPLICIT_ARRAY.len != 4 || EMBED_EXPLICIT_ARRAY[0] != 'A' || EMBED_EXPLICIT_ARRAY[3] != 'D' {
		return 5;
	}

	if EMBED_EMPTY.len != 0 {
		return 6;
	}

	let local_slice: []u8 = @src.embed("data/hello.txt");
	if local_slice.len != 14 || local_slice[7] != 'S' {
		return 7;
	}

	let local_arr: [_]u8 = @src.embed("data/four.bin");
	if local_arr.len != 4 || local_arr[1] != 'B' {
		return 8;
	}

	let mut mut_arr: [_]u8 = @src.embed("data/four.bin");
	mut_arr[0] = 'Z';
	if mut_arr[0] != 'Z' || mut_arr[1] != 'B' {
		return 9;
	}

	let static static_slice: []u8 = @src.embed("data/hello.txt");
	if static_slice.len != 14 || static_slice[0] != 'H' {
		return 10;
	}

	if @src.embed("data/four.bin").len != 4 || @src.embed("data/four.bin")[2] != 'C' {
		return 11;
	}

	let sub_slice: []u8 = @src.embed("data/hello.txt")[0..5];
	if sub_slice.len != 5 || sub_slice[0] != 'H' || sub_slice[4] != 'o' {
		return 12;
	}

	// Imported module relative path + macro relative to defining module vs caller
	if sub.helper.SUB_DATA.len != 4 || sub.helper.SUB_DATA[0] != 'S' || sub.helper.SUB_DATA[3] != '!' {
		return 13;
	}
	let from_sub_macro: []u8 = sub.helper.sub_embed();
	if from_sub_macro.len != 4 || from_sub_macro[1] != 'U' {
		return 14;
	}
	let from_caller_macro: []u8 = sub.helper.caller_embed("data/four.bin");
	if from_caller_macro.len != 4 || from_caller_macro[3] != 'D' {
		return 15;
	}

	if generic_third<u8>(2) != 'C' {
		return 16;
	}

	return 0;
}
EOF

cat >"$work_dir/driver.c" <<'EOF'
#include <stdio.h>

extern int run_embed_tests(void);

int main(void) {
	int rc = run_embed_tests();
	if (rc != 0) {
		fprintf(stderr, "run_embed_tests failed at step %d\n", rc);
		return 1;
	}
	return 0;
}
EOF

"$compiler" "$work_dir/sub/helper.sl" --build.compile-only --tar.relocation=pic --build.output="$work_dir/sub_helper.o"
"$compiler" "$work_dir/embed_test.sl" --build.compile-only --tar.relocation=pic --emit.depfile="$work_dir/embed_test.d" --build.output="$work_dir/embed_test.o"
"$cc" "$work_dir/embed_test.o" "$work_dir/sub_helper.o" "$work_dir/driver.c" -o "$work_dir/embed_test"
"$work_dir/embed_test"

# Verify depfile contains embedded files
grep -q "hello.txt" "$work_dir/embed_test.d" || fail "depfile missing hello.txt"
grep -q "binary.bin" "$work_dir/embed_test.d" || fail "depfile missing binary.bin"
grep -q "local.txt" "$work_dir/embed_test.d" || fail "depfile missing sub/local.txt"

# Negative diagnostic tests
expect_fail() {
	name=$1
	code=$2
	expected_msg=$3
	src="$work_dir/neg_${name}.sl"
	printf '%s\n' "$code" >"$src"
	if "$compiler" "$src" --build.compile-only --build.output="$work_dir/neg_${name}.o" >"$work_dir/neg_${name}.out" 2>&1; then
		fail "$name unexpectedly succeeded"
	fi
	if ! grep -q "$expected_msg" "$work_dir/neg_${name}.out"; then
		cat "$work_dir/neg_${name}.out" >&2
		fail "$name did not mention '$expected_msg'"
	fi
}

expect_fail missing_file 'pub fn test_fn() -> []u8 { return @src.embed("data/does_not_exist.bin"); }' "file not found"
expect_fail dir_path 'pub fn test_fn() -> []u8 { return @src.embed("data"); }' "path is a directory"
expect_fail empty_path 'pub fn test_fn() -> []u8 { return @src.embed(""); }' "must not be empty"
expect_fail non_string 'pub fn test_fn() -> []u8 { return @src.embed(true); }' "must be a string literal"
expect_fail cstring_arg 'pub fn test_fn() -> []u8 { return @src.embed(c"data/four.bin"); }' "must be a string literal"
expect_fail no_arg 'pub fn test_fn() -> []u8 { return @src.embed(); }' "expects one file path argument"
expect_fail two_args 'pub fn test_fn() -> []u8 { return @src.embed("a", "b"); }' "expected \`)\`"
expect_fail len_mismatch 'pub fn test_fn() -> i32 { let x: [3]u8 = @src.embed("data/four.bin"); return x[0] cast i32; }' "expected \`\[3\]u8\`, got \`\[\]u8\`"
