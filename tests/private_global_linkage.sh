#!/bin/sh
set -eu

compiler=$1
c_compiler=$2
nm_tool=$3
work_dir=$4
fixture_dir=$5
source_dir="$work_dir/source"
moved_dir="$work_dir/moved/source"

mkdir -p "$source_dir" "$moved_dir"
cp "$fixture_dir"/*.sl "$source_dir/"
cp "$fixture_dir"/*.sl "$moved_dir/"

compile_object() {
	input=$1
	output=$2
	shift 2
	"$compiler" "$input" --build.compile-only --tar.relocation=pic \
		"--build.output=$output" "$@"
}

compile_object "$source_dir/private_a.sl" "$source_dir/private_a.o"
compile_object "$source_dir/private_b.sl" "$source_dir/private_b.o"
compile_object "$moved_dir/private_a.sl" "$moved_dir/private_a.o"
compile_object "$moved_dir/private_b.sl" "$moved_dir/private_b.o"
compile_object "$source_dir/private_var_a.sl" "$source_dir/private_var_a.o"
compile_object "$source_dir/private_var_b.sl" "$source_dir/private_var_b.o"
compile_object "$moved_dir/private_var_a.sl" "$moved_dir/private_var_a.o"
compile_object "$moved_dir/private_var_b.sl" "$moved_dir/private_var_b.o"

private_symbols() {
	"$nm_tool" -a --defined-only "$1" | awk '$NF ~ /#priv:/ { print $NF }' | sort
}

for name in private_a private_b private_var_a private_var_b; do
	if "$nm_tool" -g "$source_dir/$name.o" | grep -Fq '#priv:'; then
		echo "private global in $name.o has external linkage" >&2
		exit 1
	fi
	if [ "$(private_symbols "$source_dir/$name.o")" != "$(private_symbols "$moved_dir/$name.o")" ]; then
		echo "private global symbol names changed after relocating the source tree" >&2
		exit 1
	fi
done

# Repeated and parallel compilations must produce the same owner-local symbol
# names; import discovery order and unrelated imports must not rename a module.
mkdir -p "$work_dir/repeat" "$work_dir/parallel_a" "$work_dir/parallel_b"
compile_object "$source_dir/private_a.sl" "$work_dir/repeat/private_a.o"
if [ "$(private_symbols "$source_dir/private_a.o")" != \
	"$(private_symbols "$work_dir/repeat/private_a.o")" ]; then
	echo "repeated build changed private symbol identity" >&2
	exit 1
fi
compile_object "$source_dir/private_a.sl" "$work_dir/parallel_a/private_a.o" &
pid_a=$!
compile_object "$source_dir/private_b.sl" "$work_dir/parallel_b/private_b.o" &
pid_b=$!
wait "$pid_a"
wait "$pid_b"
if [ "$(private_symbols "$source_dir/private_a.o")" != \
	"$(private_symbols "$work_dir/parallel_a/private_a.o")" ] ||
	[ "$(private_symbols "$source_dir/private_b.o")" != \
	"$(private_symbols "$work_dir/parallel_b/private_b.o")" ]; then
	echo "parallel build changed private symbol identity" >&2
	exit 1
fi

cat > "$work_dir/main.c" <<'EOF'
#include <stdint.h>

extern int32_t private_a_value(void);
extern int32_t private_b_value(void);
extern int32_t optimized_caller(void);
extern int32_t optimized_mutable_caller(void);
extern int32_t optimized_multi_caller(void);
extern int32_t optimized_multi_reversed(void);
extern int32_t optimized_multi_extra(void);
extern int32_t diamond_caller(void);
extern int32_t optimized_var_sum(void);
extern int32_t *optimized_var_a_address(void);
extern int32_t *optimized_var_b_address(void);
extern int32_t *private_var_a_address(void);
extern int32_t *private_var_b_address(void);

int main(void) {
	return private_a_value() != 11 || private_b_value() != 29 ||
		optimized_caller() != 42 || optimized_mutable_caller() != 11 ||
		optimized_mutable_caller() != 12 || optimized_multi_caller() != 40 ||
		optimized_multi_reversed() != 40 || optimized_multi_extra() != 40 ||
		diamond_caller() != 34 || optimized_var_sum() != 36 ||
		optimized_var_a_address() == optimized_var_b_address() ||
		optimized_var_a_address() != private_var_a_address() ||
		optimized_var_b_address() != private_var_b_address() ||
		*optimized_var_a_address() != 13 || *optimized_var_b_address() != 23;
}
EOF

compile_object "$source_dir/optimized_owner.sl" "$source_dir/optimized_owner.o" \
	--opt.level=high
compile_object "$source_dir/optimized_caller.sl" "$source_dir/optimized_caller.o" \
	--opt.level=high
compile_object "$source_dir/optimized_mutable_owner.sl" \
	"$source_dir/optimized_mutable_owner.o" --opt.level=high
compile_object "$source_dir/optimized_mutable_caller.sl" \
	"$source_dir/optimized_mutable_caller.o" --opt.level=high
compile_object "$source_dir/optimized_multi_caller.sl" \
	"$source_dir/optimized_multi_caller.o" --opt.level=high
compile_object "$source_dir/optimized_multi_reversed.sl" \
	"$source_dir/optimized_multi_reversed.o" --opt.level=high
compile_object "$source_dir/optimized_multi_extra.sl" \
	"$source_dir/optimized_multi_extra.o" --opt.level=high
compile_object "$source_dir/diamond_caller.sl" "$source_dir/diamond_caller.o" \
	--opt.level=high
compile_object "$source_dir/optimized_var_caller.sl" \
	"$source_dir/optimized_var_caller.o" --opt.level=high

# Also exercise unoptimized imports, where references resolve at link time to
# each owner's canonical local storage and exported accessor functions.
compile_object "$source_dir/optimized_multi_reversed.sl" \
	"$source_dir/optimized_multi_reversed_o0.o"
compile_object "$source_dir/optimized_var_caller.sl" \
	"$source_dir/optimized_var_caller_o0.o"

"$nm_tool" -u "$source_dir/optimized_mutable_caller.o" |
	grep -Fq 'optimized_mutable_owner' || {
		echo "mutable private state was detached from its owning module" >&2
		exit 1
}

"$c_compiler" "$work_dir/main.c" \
	"$source_dir/private_a.o" "$source_dir/private_b.o" \
	"$source_dir/optimized_owner.o" "$source_dir/optimized_caller.o" \
	"$source_dir/optimized_mutable_owner.o" "$source_dir/optimized_mutable_caller.o" \
	"$source_dir/optimized_multi_caller.o" \
	"$source_dir/optimized_multi_reversed.o" \
	"$source_dir/optimized_multi_extra.o" "$source_dir/diamond_caller.o" \
	"$source_dir/optimized_var_caller.o" \
	"$source_dir/private_var_a.o" "$source_dir/private_var_b.o" \
	-o "$work_dir/private_global_linkage"
"$work_dir/private_global_linkage"

cat > "$work_dir/main_o0.c" <<'EOF'
#include <stdint.h>
extern int32_t optimized_multi_reversed(void);
extern int32_t optimized_var_sum(void);
extern int32_t *optimized_var_a_address(void);
extern int32_t *optimized_var_b_address(void);
extern int32_t *private_var_a_address(void);
extern int32_t *private_var_b_address(void);
int main(void) {
	return optimized_multi_reversed() != 40 || optimized_var_sum() != 36 ||
		optimized_var_a_address() == optimized_var_b_address() ||
		optimized_var_a_address() != private_var_a_address() ||
		optimized_var_b_address() != private_var_b_address() ||
		*optimized_var_a_address() != 13 || *optimized_var_b_address() != 23;
}
EOF
"$c_compiler" "$work_dir/main_o0.c" "$source_dir/private_a.o" \
	"$source_dir/private_b.o" "$source_dir/private_var_a.o" \
	"$source_dir/private_var_b.o" "$source_dir/optimized_multi_reversed_o0.o" \
	"$source_dir/optimized_var_caller_o0.o" -o "$work_dir/private_global_linkage_o0"
"$work_dir/private_global_linkage_o0"

# Confirm the optimized import was folded through the private constant instead
# of relying on an external private-global definition.
"$compiler" "$source_dir/optimized_caller.sl" --emit.ir --opt.level=high \
	"--build.output=$work_dir/optimized_caller.ll"
grep -Eq 'ret i32 42$' "$work_dir/optimized_caller.ll" || {
	echo "optimized import did not fold the private constant" >&2
	exit 1
}
if grep -E 'call .*@optimized_owner|@.*FRACUNIT#priv:' "$work_dir/optimized_caller.ll"; then
	echo "optimized caller retained a cross-module private-global reference" >&2
	exit 1
fi

"$compiler" "$source_dir/optimized_multi_caller.sl" --emit.ir --opt.level=high \
	"--build.output=$work_dir/optimized_multi_caller.ll"
grep -Eq 'ret i32 40$' "$work_dir/optimized_multi_caller.ll" || {
	echo "same-named private globals from two imported modules collided in LLVM IR" >&2
	exit 1
}

"$compiler" "$source_dir/optimized_mutable_caller.sl" --emit.ir --opt.level=high \
	"--build.output=$work_dir/optimized_mutable_caller.ll"
grep -Eq 'call .*@optimized_mutable_owner' "$work_dir/optimized_mutable_caller.ll" || {
	echo "optimized import no longer calls the canonical mutable-state owner" >&2
	exit 1
}
if grep -Fq 'PRIVATE_STATE#priv:' "$work_dir/optimized_mutable_caller.ll"; then
	echo "optimized import retained a cross-module private mutable-global reference" >&2
	exit 1
fi

# A trait method implementation without explicit 'pub' (and non-public trait/target)
# must have internal linkage (t) rather than external linkage (T).
cat > "$work_dir/private_trait_vis.sl" <<'EOF'
trait Noise {
	fn noise(self: *Self) -> void;
}

struct Dog {
	name: *u8,
	age: u8,
}

impl Noise for Dog {
	fn noise(_self: *Self) -> void {}
}

pub fn main() -> void {
	let stella: Dog = { .name = null, .age = 4 };
	stella.noise();
}
EOF

compile_object "$work_dir/private_trait_vis.sl" "$work_dir/private_trait_vis_o0.o" --opt.level=none
if "$nm_tool" -g "$work_dir/private_trait_vis_o0.o" | grep -Fq 'noise'; then
	echo "private trait method in private_trait_vis_o0.o has external linkage" >&2
	exit 1
fi
if ! "$nm_tool" "$work_dir/private_trait_vis_o0.o" | grep -Eq '[0-9a-fA-F]+ t .*[Nn]oise.*[Dd]og.*noise'; then
	echo "private trait method in private_trait_vis_o0.o missing internal symbol" >&2
	exit 1
fi

compile_object "$work_dir/private_trait_vis.sl" "$work_dir/private_trait_vis_opt.o" --opt.level=high
if "$nm_tool" -g "$work_dir/private_trait_vis_opt.o" | grep -Fq 'noise'; then
	echo "inlined private trait method in private_trait_vis_opt.o retained external linkage" >&2
	exit 1
fi
