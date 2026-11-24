#!/bin/sh
set -eu

compiler=$1
c_compiler=$2
llc=$3
work_dir=$4
fixture_dir=$5
features="$fixture_dir/features.sl"

mkdir -p "$work_dir"

"$compiler" "$fixture_dir/generic_constraints.sl" --build.compile-only \
	"--build.output=$work_dir/generic_constraints.o"

compile_ir() {
	target=$1
	opt=$2
	out=$3
	"$compiler" "$features" --emit.ir "--opt.level=$opt" "--tar.triple=$target" \
		"--build.output=$out"
}

ir_instructions() {
	ir_file=$1
	function=$2
	awk -v fn="$function" '
		$0 ~ "^define .*@" fn "\\(" { active = 1; next }
		active && /^}/ { exit }
		active {
			line = $0
			if (line ~ /^[[:space:]]*[A-Za-z0-9_.-]+:[[:space:]]*$/) next
			if (line ~ /^[[:space:]]*$/) next
			gsub(/%[A-Za-z0-9$._-]+/, "%v", line)
			sub(/^[[:space:]]+/, "", line)
			print line
		}' "$ir_file"
}

asm_instructions() {
	asm_file=$1
	function=$2
	awk -v fn="$function" '
		$0 ~ "^" fn ":" { active = 1; next }
		active && /^[A-Za-z_.$][A-Za-z0-9_.$]*:[[:space:]]*$/ { exit }
		active {
			line = $0
			sub(/#.*/, "", line)
			if (line !~ /^[[:space:]]*[A-Za-z][A-Za-z0-9_.]*([[:space:]]|$)/) next
			gsub(/%[A-Za-z][A-Za-z0-9]*/, "%reg", line)
			gsub(/\$[0-9]+/, "$imm", line)
			gsub(/#[0-9]+/, "#imm", line)
			gsub(/[[:space:]]+/, " ", line)
			sub(/^ /, "", line)
			sub(/ $/, "", line)
			print line
		}' "$asm_file"
}

assert_same_ir() {
	ir_file=$1
	left=$2
	right=$3
	if [ "$(ir_instructions "$ir_file" "$left")" != \
		"$(ir_instructions "$ir_file" "$right")" ]; then
		echo "unoptimized/optimized IR differs for $left and $right in $ir_file" >&2
		ir_instructions "$ir_file" "$left" >&2
		ir_instructions "$ir_file" "$right" >&2
		exit 1
	fi
}

assert_wrap_vs_nuw_ir() {
	ir_file=$1
	wrap_fn=$2
	plain_fn=$3
	wrap_ir=$(ir_instructions "$ir_file" "$wrap_fn")
	plain_ir=$(ir_instructions "$ir_file" "$plain_fn")
	if [ "$wrap_ir" = "$plain_ir" ]; then
		echo "expected nuw difference between $wrap_fn and $plain_fn in $ir_file" >&2
		printf '%s\n' "$plain_ir" >&2
		exit 1
	fi
	wrap_stripped=$(printf '%s\n' "$wrap_ir" | sed 's/ nuw / /g')
	plain_stripped=$(printf '%s\n' "$plain_ir" | sed 's/ nuw / /g')
	if [ "$wrap_stripped" != "$plain_stripped" ]; then
		echo "IR differs beyond nuw for $wrap_fn and $plain_fn in $ir_file" >&2
		printf '%s\n' "$wrap_ir" >&2
		printf '%s\n' "$plain_ir" >&2
		exit 1
	fi
}

assert_same_asm() {
	asm_file=$1
	left=$2
	right=$3
	if [ "$(asm_instructions "$asm_file" "$left")" != \
		"$(asm_instructions "$asm_file" "$right")" ]; then
		echo "generated instructions differ for $left and $right in $asm_file" >&2
		asm_instructions "$asm_file" "$left" >&2
		asm_instructions "$asm_file" "$right" >&2
		exit 1
	fi
}

expect_return() {
	ir_file=$1
	function=$2
	return_value=$3
	if ! ir_instructions "$ir_file" "$function" | grep -Fqx "ret $return_value"; then
		echo "$function did not fold to '$return_value' in $ir_file" >&2
		ir_instructions "$ir_file" "$function" >&2
		exit 1
	fi
}

expect_failure() {
	input=$1
	name=$2
	shift 2
	log_file="$work_dir/$name.log"
	if "$compiler" "$input" --build.compile-only "--build.output=$work_dir/$name.o" \
		>"$log_file" 2>&1; then
		echo "expected $input to fail compilation" >&2
		exit 1
	fi
	for diagnostic in "$@"; do
		if ! grep -Fq "$diagnostic" "$log_file"; then
			echo "missing diagnostic '$diagnostic' for $input" >&2
			cat "$log_file" >&2
			exit 1
		fi
	done
}

check_ir_file() {
	ir_file=$1
	for intrinsic in uadd.sat.i8 usub.sat.i8 sadd.sat.i8 ssub.sat.i8 \
		uadd.sat.i129 usub.sat.i129 sadd.sat.i129 ssub.sat.i129; do
		if ! grep -Fq "@llvm.$intrinsic" "$ir_file"; then
			echo "missing LLVM saturation intrinsic llvm.$intrinsic in $ir_file" >&2
			exit 1
		fi
	done
	if ! awk '/^[[:space:]]*(tail )?call / && $0 !~ /@llvm\./ { print; failed = 1 }
		END { exit failed }' "$ir_file"; then
		echo "non-intrinsic call found in generated IR $ir_file" >&2
		exit 1
	fi
	for function in sat_add_constant wrap_add_constant sat_add_signed_constant \
		sat_sub_signed_constant sat_add_u129_constant wrap_add_u129_constant; do
		case $function in
			sat_add_constant) expected='i8 -1' ;;
			wrap_add_constant) expected='i8 14' ;;
			sat_add_signed_constant) expected='i8 127' ;;
			sat_sub_signed_constant) expected='i8 -128' ;;
			sat_add_u129_constant) expected='i129 -1' ;;
			wrap_add_u129_constant) expected='i129 0' ;;
		esac
		expect_return "$ir_file" "$function" "$expected"
	done
}

x86_target=x86_64-unknown-linux-gnu
arm_target=aarch64-unknown-linux-gnu
x86_o0="$work_dir/features.x86.o0.ll"
x86_o2="$work_dir/features.x86.o2.ll"
arm_o0="$work_dir/features.arm.o0.ll"
arm_o2="$work_dir/features.arm.o2.ll"
arm_supported=no
if "$llc" --version | grep -Eiq '^[[:space:]]*aarch64[[:space:]]'; then
	arm_supported=yes
fi
compile_ir "$x86_target" none "$x86_o0"
compile_ir "$x86_target" high "$x86_o2"
if [ "$arm_supported" = yes ]; then
	compile_ir "$arm_target" none "$arm_o0"
	compile_ir "$arm_target" high "$arm_o2"
fi

check_ir_file "$x86_o0"
check_ir_file "$x86_o2"
if [ "$arm_supported" = yes ]; then
	check_ir_file "$arm_o0"
	check_ir_file "$arm_o2"
fi

for ir_file in "$x86_o0" "$x86_o2"; do
	assert_same_ir "$ir_file" range_extract range_extract_manual
	assert_wrap_vs_nuw_ir "$ir_file" wrap_add plain_add
	assert_wrap_vs_nuw_ir "$ir_file" wrap_precedence plain_precedence
	assert_wrap_vs_nuw_ir "$ir_file" wrap_sub plain_sub
done

ufcs_ir="$work_dir/ufcs.ll"
"$compiler" "$fixture_dir/ufcs.sl" --emit.ir --opt.level=none "--tar.triple=$x86_target" \
	"--build.output=$ufcs_ir"
assert_same_ir "$ufcs_ir" test_ufcs_direct test_direct_call
assert_same_ir "$ufcs_ir" test_ufcs_chain test_direct_chain
if [ "$arm_supported" = yes ]; then
	for ir_file in "$arm_o0" "$arm_o2"; do
		assert_same_ir "$ir_file" range_extract range_extract_manual
		assert_wrap_vs_nuw_ir "$ir_file" wrap_add plain_add
		assert_wrap_vs_nuw_ir "$ir_file" wrap_precedence plain_precedence
		assert_wrap_vs_nuw_ir "$ir_file" wrap_sub plain_sub
	done
fi

for opt in 0 2; do
	for arch in x86 arm; do
		if [ "$arch" = arm ] && [ "$arm_supported" != yes ]; then
			continue
		fi
		if [ "$arch" = x86 ]; then
			ir_file=$x86_o0
			[ "$opt" = 0 ] || ir_file=$x86_o2
		else
			ir_file=$arm_o0
			[ "$opt" = 0 ] || ir_file=$arm_o2
		fi
		asm_file="$work_dir/features.$arch.o$opt.s"
		"$llc" "-O=$opt" -filetype=asm "$ir_file" -o "$asm_file"
		assert_same_asm "$asm_file" range_extract range_extract_manual
		assert_same_asm "$asm_file" wrap_add plain_add
		assert_same_asm "$asm_file" wrap_precedence plain_precedence
		assert_same_asm "$asm_file" wrap_sub plain_sub
		if [ "$arch" = x86 ]; then
			call_pattern='^[[:space:]]*callq?[[:space:]]'
		else
			call_pattern='^[[:space:]]*(bl|blr)[[:space:]]'
		fi
		if grep -Eq "$call_pattern" "$asm_file"; then
			echo "generated helper call found in $asm_file" >&2
			grep -En "$call_pattern" "$asm_file" >&2
			exit 1
		fi
	done
done

# A layout annotation is a compile-time assertion. Removing those annotations
# must leave the generated instructions for the same generic field access intact.
sed '/^#\[layout(/d' "$features" > "$work_dir/features.without_layout.sl"
"$compiler" "$work_dir/features.without_layout.sl" --emit.ir --opt.level=none \
	"--tar.triple=$x86_target" --build.output="$work_dir/features.without_layout.ll"
"$llc" -O=0 -filetype=asm "$work_dir/features.without_layout.ll" \
	-o "$work_dir/features.without_layout.s"
"$llc" -O=0 -filetype=asm "$x86_o0" -o "$work_dir/features.layout.s"
if [ "$(asm_instructions "$work_dir/features.layout.s" cell_load)" != \
	"$(asm_instructions "$work_dir/features.without_layout.s" cell_load)" ]; then
	echo "layout assertions changed generated instructions for cell_load" >&2
	exit 1
fi

expect_failure "$fixture_dir/invalid_ranges.sl" invalid_ranges \
	"high bound must be greater than or equal" \
	"outside the integer width" \
	"bound must be a compile-time integer constant" \
	"bound must be nonnegative" \
	"requires an integer operand"
expect_failure "$fixture_dir/invalid_arithmetic.sl" invalid_arithmetic \
	'operator `+%` requires integer operands' \
	"requires operands of the same type"
expect_failure "$fixture_dir/invalid_layout.sl" invalid_layout \
	"E_LAYOUT_MISMATCH" "expected size 12" "target ABI size is 16"
expect_failure "$fixture_dir/invalid_layout_alignment.sl" invalid_layout_alignment \
	"E_LAYOUT_MISMATCH" "expected alignment 4" "target ABI alignment is 8"
expect_failure "$fixture_dir/invalid_layout_arity.sl" invalid_layout_arity \
	"expects exactly two integer arguments: size and alignment"
expect_failure "$fixture_dir/invalid_generic_layout.sl" invalid_generic_layout \
	"E_LAYOUT_MISMATCH" "expected size 8" "target ABI size is 4" \
	"expected alignment 8" "target ABI alignment is 4"
expect_failure "$fixture_dir/invalid_old_integer_types.sl" invalid_old_integer_types \
	'unknown type `ui8`' 'unknown type `si8`' \
	'unknown type `uisize`' 'unknown type `sisize`'
expect_failure "$fixture_dir/invalid_old_generic_constraints.sl" \
	invalid_old_generic_constraints \
	'no matching public function for call to `old_unsigned_constraint`' \
	'no matching public function for call to `old_signed_constraint`'

if "$llc" --version | grep -Eiq '^[[:space:]]*x86[[:space:]]+-'; then
	if "$compiler" "$fixture_dir/invalid_layout_32bit.sl" --build.compile-only \
		--tar.triple=i386-unknown-linux-gnu \
		"--build.output=$work_dir/invalid_layout_32bit.o" \
		>"$work_dir/invalid_layout_32bit.log" 2>&1; then
		echo "expected 32-bit target layout assertion to fail" >&2
		exit 1
	fi
	for diagnostic in "E_LAYOUT_MISMATCH" "expected size 16" "target ABI size is 8" \
		"expected alignment 8" "target ABI alignment is 4"; do
		if ! grep -Fq "$diagnostic" "$work_dir/invalid_layout_32bit.log"; then
			echo "missing target-specific layout diagnostic '$diagnostic'" >&2
			cat "$work_dir/invalid_layout_32bit.log" >&2
			exit 1
		fi
	done
fi

"$compiler" "$features" --build.compile-only --opt.level=none \
	"--tar.triple=$x86_target" "--build.output=$work_dir/features.x86.o"
cat > "$work_dir/runtime.c" <<'EOF'
#include <stdint.h>
#include <limits.h>

extern uint8_t range_extract_runtime(uint32_t);
extern uint8_t range_single_runtime(unsigned __int128);
extern uint8_t range_low_bit_runtime(int16_t);
extern uint8_t array_index(const uint8_t *, uintptr_t);
extern uint8_t wrap_add(uint8_t, uint8_t);
extern uint8_t plain_add(uint8_t, uint8_t);
extern uint8_t wrap_sub(uint8_t, uint8_t);
extern uint8_t wrap_precedence(uint8_t, uint8_t, uint8_t);
extern uint8_t compound_add(uint8_t);
extern int8_t wrap_signed(int8_t, int8_t);
extern int8_t wrap_sub_signed(int8_t, int8_t);
extern uint8_t sat_add_u8(uint8_t, uint8_t);
extern uint8_t sat_sub_u8(uint8_t, uint8_t);
extern int8_t sat_add_s8(int8_t, int8_t);
extern int8_t sat_sub_s8(int8_t, int8_t);
extern uint8_t sat_add_constant(void);
extern uint8_t wrap_add_constant(void);
extern int8_t sat_add_signed_constant(void);
extern int8_t sat_sub_signed_constant(void);
extern uint8_t wrap_mul(uint8_t, uint8_t);
extern uint8_t sat_mul_u8(uint8_t, uint8_t);
extern int8_t sat_mul_s8(int8_t, int8_t);
extern uint8_t compound_arith_variants(uint8_t);
extern intptr_t isize_identity(intptr_t);

int main(void) {
	const uint8_t bytes[] = {3, 7, 11, 19};
	return isize_identity(INTPTR_MIN) != INTPTR_MIN ||
		range_extract_runtime(0xabc) != 21 ||
		range_extract_runtime(UINT32_MAX) != 31 ||
		range_single_runtime(((unsigned __int128)1) << 127) != 1 ||
		range_low_bit_runtime(-3) != 1 || array_index(bytes, 2) != 11 ||
		wrap_add(250, 20) != 14 || plain_add(200, 20) != 220 ||
		wrap_precedence(2, 3, 4) != 14 || wrap_sub(10, 20) != 246 ||
		compound_add(254) != 255 || wrap_signed(INT8_MAX, 1) != INT8_MIN ||
		wrap_sub_signed(INT8_MIN, 1) != INT8_MAX ||
		sat_add_u8(250, 20) != UINT8_MAX || sat_sub_u8(10, 20) != 0 ||
		sat_add_s8(100, 100) != INT8_MAX || sat_add_s8(-100, -100) != INT8_MIN ||
		sat_sub_s8(-120, 20) != INT8_MIN || sat_sub_s8(120, -20) != INT8_MAX ||
		sat_add_constant() != UINT8_MAX || wrap_add_constant() != 14 ||
		sat_add_signed_constant() != INT8_MAX ||
		sat_sub_signed_constant() != INT8_MIN ||
		wrap_mul(20, 20) != 144 || sat_mul_u8(20, 20) != UINT8_MAX ||
		sat_mul_s8(20, 20) != INT8_MAX || sat_mul_s8(-20, 20) != INT8_MIN ||
		sat_mul_s8(-20, -20) != INT8_MAX ||
		compound_arith_variants(250) != UINT8_MAX;
}
EOF
"$c_compiler" "$work_dir/runtime.c" "$work_dir/features.x86.o" \
	-o "$work_dir/language_features_runtime"
"$work_dir/language_features_runtime"

"$compiler" "$fixture_dir/ufcs.sl" --build.compile-only --opt.level=none \
	"--tar.triple=$x86_target" "--build.output=$work_dir/ufcs.x86.o"
cat > "$work_dir/ufcs_runtime.c" <<'EOF'
#include <stdint.h>

struct Point {
	int32_t x;
	int32_t y;
};

extern int32_t test_ufcs_chain(int32_t);
extern int32_t test_direct_chain(int32_t);
extern int32_t test_builder_chain(void);
extern int32_t test_auto_ref(void);
extern int32_t test_auto_ref_mut(void);
extern int32_t test_auto_deref(const struct Point *);
extern float test_struct_scoped_ufcs(void);
extern int32_t test_mod_and_generic_impl(int32_t);
extern int32_t test_generic_ufcs(int32_t);
extern int32_t test_primitive_ufcs(int32_t);
extern int32_t test_priority1_fn_ptr(void);
extern int32_t test_ufcs_point_call(void);

int main(void) {
	struct Point p = {3, 4};
	return test_ufcs_chain(10) != 26 ||
		test_direct_chain(10) != 26 ||
		test_builder_chain() != 200 ||
		test_auto_ref() != 30 ||
		test_auto_ref_mut() != 14 ||
		test_auto_deref(&p) != 25 ||
		test_struct_scoped_ufcs() != 4.0f ||
		test_mod_and_generic_impl(21) != 42 ||
		test_generic_ufcs(42) != 42 ||
		test_primitive_ufcs(-42) != 42 ||
		test_priority1_fn_ptr() != 42 ||
		test_ufcs_point_call() != 25;
}
EOF
"$c_compiler" "$work_dir/ufcs_runtime.c" "$work_dir/ufcs.x86.o" \
	-o "$work_dir/ufcs_runtime"
"$work_dir/ufcs_runtime"

"$compiler" "$fixture_dir/traits.sl" --build.compile-only --opt.level=none \
	"--tar.triple=$x86_target" "--build.output=$work_dir/traits.x86.o"
cat > "$work_dir/traits_runtime.c" <<'EOF'
#include <stdint.h>

extern int32_t test_trait_basic(void);
extern int32_t test_trait_generic(void);
extern int32_t test_trait_multiple_bounds(void);
extern int32_t test_trait_primitive(void);
extern int32_t test_trait_generic_impl(void);

int main(void) {
	return test_trait_basic() != 30 ||
		test_trait_generic() != 20 ||
		test_trait_multiple_bounds() != 30 ||
		test_trait_primitive() != 70 ||
		test_trait_generic_impl() != 260;
}
EOF
"$c_compiler" "$work_dir/traits_runtime.c" "$work_dir/traits.x86.o" \
	-o "$work_dir/traits_runtime"
"$work_dir/traits_runtime"

"$compiler" "$fixture_dir/stdlib_traits.sl" --build.compile-only --opt.level=none \
	"--tar.triple=$x86_target" "--build.output=$work_dir/stdlib_traits.x86.o"
cat > "$work_dir/stdlib_traits_runtime.c" <<'EOF'
#include <stdint.h>

extern int32_t test_stream_copy(void);
extern int32_t test_ord_sort(void);
extern int32_t test_disposable(void);

int main(void) {
	return test_stream_copy() != 42 ||
		test_ord_sort() != 100 ||
		test_disposable() != 200;
}
EOF
"$c_compiler" "$work_dir/stdlib_traits_runtime.c" "$work_dir/stdlib_traits.x86.o" \
	-o "$work_dir/stdlib_traits_runtime"
"$work_dir/stdlib_traits_runtime"

cat > "$work_dir/defer.sl" <<'EOF'
struct Log {
	idx: i32,
	buf: [8]i32,
}

fn push_log(log: *mut Log, v: i32) -> void {
	log.buf[log.idx] = v;
	log.idx = log.idx + 1;
}

#[abi("C"), link_name("test_defer_lifo_basic")]
pub fn test_defer_lifo_basic() -> i32 {
	let mut log: Log = { .idx = 0, .buf = [0, 0, 0, 0, 0, 0, 0, 0] };
	{
		defer push_log(&log, 1);
		defer {
			if true {
				push_log(&log, 2);
			}
		}
		defer push_log(&log, 3);
		push_log(&log, 10);
	}
	if log.idx != 4 { return 1; }
	if log.buf[0] != 10 { return 2; }
	if log.buf[1] != 3 { return 3; }
	if log.buf[2] != 2 { return 4; }
	if log.buf[3] != 1 { return 5; }
	return 0;
}

fn helper_early_return(log: *mut Log, early: bool) -> void {
	defer push_log(log, 1);
	if early {
		return;
	}
	defer push_log(log, 2);
	push_log(log, 9);
}

#[abi("C"), link_name("test_defer_early_return")]
pub fn test_defer_early_return() -> i32 {
	let mut log1: Log = { .idx = 0, .buf = [0, 0, 0, 0, 0, 0, 0, 0] };
	helper_early_return(&log1, true);
	if log1.idx != 1 || log1.buf[0] != 1 { return 1; }
	let mut log2: Log = { .idx = 0, .buf = [0, 0, 0, 0, 0, 0, 0, 0] };
	helper_early_return(&log2, false);
	if log2.idx != 3 || log2.buf[0] != 9 || log2.buf[1] != 2 || log2.buf[2] != 1 { return 2; }
	return 0;
}

#[abi("C"), link_name("test_defer_loop_and_stmt")]
pub fn test_defer_loop_and_stmt() -> i32 {
	let mut log: Log = { .idx = 0, .buf = [0, 0, 0, 0, 0, 0, 0, 0] };
	for let mut i: i32 = 0; i < 2; i = i + 1 {
		defer push_log(&log, 100 + i);
		defer if i == 1 {
			push_log(&log, 200);
		}
		if i == 0 {
			continue;
		}
	}
	if log.idx != 3 { return 1; }
	if log.buf[0] != 100 || log.buf[1] != 200 || log.buf[2] != 101 { return 2; }
	return 0;
}
EOF
"$compiler" "$work_dir/defer.sl" --build.compile-only --opt.level=none \
	"--tar.triple=$x86_target" "--build.output=$work_dir/defer.x86.o"
cat > "$work_dir/defer_runtime.c" <<'EOF'
#include <stdint.h>

extern int32_t test_defer_lifo_basic(void);
extern int32_t test_defer_early_return(void);
extern int32_t test_defer_loop_and_stmt(void);

int main(void) {
	return test_defer_lifo_basic() != 0 ||
		test_defer_early_return() != 0 ||
		test_defer_loop_and_stmt() != 0;
}
EOF
"$c_compiler" "$work_dir/defer_runtime.c" "$work_dir/defer.x86.o" \
	-o "$work_dir/defer_runtime"
"$work_dir/defer_runtime"

cat > "$work_dir/block_expr.sl" <<'EOF'
struct Pair {
	a: i32,
	b: i32,
}

enum Choice: u8 {
	Zero,
	One(i32),
	Two(i32, i32),
}

#[abi("C"), link_name("test_if_block_expr")]
pub fn test_if_block_expr(y: u8) -> u8 {
	let x: u8 = if y < 2 {
		yield y;
	} else {
		yield 2;
	};
	return x;
}

#[abi("C"), link_name("test_else_if_block_expr")]
pub fn test_else_if_block_expr(v: i32) -> i32 {
	return if v < 0 {
		yield -1;
	} else if v == 0 {
		yield 0;
	} else {
		yield 1;
	};
}

#[abi("C"), link_name("test_for_block_expr")]
pub fn test_for_block_expr(target: i32) -> i32 {
	let found: i32 = for let mut i: i32 = 0; true; i += 1 {
		if i == target {
			yield i * 10;
		}
		if i > 20 {
			yield -1;
		}
	};
	let infinite: i32 = for {
		if found > 0 {
			yield found + 1;
		}
		yield 0;
	};
	return infinite;
}

#[abi("C"), link_name("test_match_block_expr")]
pub fn test_match_block_expr(kind: i32, a: i32, b: i32) -> i32 {
	let c: Choice = if kind == 0 {
		yield .Zero;
	} else if kind == 1 {
		yield .One(a);
	} else {
		yield .Two(a, b);
	};
	return match c {
		.Zero => yield 100,
		.One(x) => yield x * 2,
		.Two(x, y) => yield x + y,
	};
}

#[abi("C"), link_name("test_nested_and_break_block_expr")]
pub fn test_nested_and_break_block_expr(limit: i32) -> i32 {
	let mut acc: i32 = 0;
	for let mut i: i32 = 0; i < 10; i += 1 {
		let step: i32 = if i >= limit {
			break;
		} else {
			yield i;
		};
		acc += step;
	}
	let cond_res: i32 = if if acc > 0 {
		let p: Pair = { .a = acc, .b = 1 };
		yield p.a > 2;
	} else {
		yield false;
	} {
		yield acc + 100;
	} else {
		yield acc;
	};
	return cond_res;
}

#[abi("C"), link_name("test_defer_in_control_block_expr")]
pub fn test_defer_in_control_block_expr(flag: i32) -> i32 {
	let mut log: i32 = 0;
	let res: i32 = if flag != 0 {
		defer log += 10;
		yield 5;
	} else {
		defer log += 20;
		yield 7;
	};
	return res + log;
}
EOF
"$compiler" "$work_dir/block_expr.sl" --build.compile-only --opt.level=none \
	"--tar.triple=$x86_target" "--build.output=$work_dir/block_expr.x86.o"
cat > "$work_dir/block_expr_runtime.c" <<'EOF'
#include <stdint.h>

extern uint8_t test_if_block_expr(uint8_t y);
extern int32_t test_else_if_block_expr(int32_t v);
extern int32_t test_for_block_expr(int32_t target);
extern int32_t test_match_block_expr(int32_t kind, int32_t a, int32_t b);
extern int32_t test_nested_and_break_block_expr(int32_t limit);
extern int32_t test_defer_in_control_block_expr(int32_t flag);

int main(void) {
	if (test_if_block_expr(1) != 1 || test_if_block_expr(5) != 2) return 1;
	if (test_else_if_block_expr(-5) != -1 || test_else_if_block_expr(0) != 0 || test_else_if_block_expr(9) != 1) return 2;
	if (test_for_block_expr(3) != 31 || test_for_block_expr(50) != 0) return 3;
	if (test_match_block_expr(0, 0, 0) != 100 || test_match_block_expr(1, 7, 0) != 14 || test_match_block_expr(2, 4, 9) != 13) return 4;
	if (test_nested_and_break_block_expr(4) != 106 || test_nested_and_break_block_expr(2) != 1) return 5;
	if (test_defer_in_control_block_expr(1) != 15 || test_defer_in_control_block_expr(0) != 27) return 6;
	return 0;
}
EOF
"$c_compiler" "$work_dir/block_expr_runtime.c" "$work_dir/block_expr.x86.o" \
	-o "$work_dir/block_expr_runtime"
"$work_dir/block_expr_runtime"
