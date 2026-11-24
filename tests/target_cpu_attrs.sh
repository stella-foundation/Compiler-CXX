#!/bin/sh
set -eu
compiler=$1
llc=$2
work=$3
mkdir -p "$work"

cat >"$work/vec.sl" <<'EOF'
pub fn add_arrays(dst: *mut f64, a: *f64, b: *f64, n: i32) -> void {
    for let mut i: i32 = 0; i < n; i += 1 {
        dst[i] = a[i] + b[i];
    }
}
EOF

# 1. Explicit CPU emits target-cpu in IR and preserves AVX2 through LTO bitcode.
"$compiler" "$work/vec.sl" --emit.ir --opt.level=high --tar.triple=x86_64-linux-gnu --tar.cpu=skylake --build.output="$work/skylake.ll"
grep -Fq '"target-cpu"="skylake"' "$work/skylake.ll"

"$compiler" "$work/vec.sl" --build.compile-only --opt.level=high --opt.lto=full --tar.triple=x86_64-linux-gnu --tar.cpu=skylake --build.output="$work/skylake.bc"
"$llc" -filetype=asm "$work/skylake.bc" -o "$work/skylake.s"
grep -Eq 'vaddpd.*%ymm' "$work/skylake.s"

# 2. Native CPU emits target-cpu or target-features in IR.
"$compiler" "$work/vec.sl" --emit.ir --opt.level=high --tar.cpu=native --build.output="$work/native.ll"
grep -Eq '"target-(cpu|features)"=' "$work/native.ll"
