#!/bin/sh
set -eu
compiler=$1
cc=$2
work=$3
fixtures=$4
mkdir -p "$work"
for arch in x86_64 aarch64 riscv64; do
	cpu_arg=
	cc_cpu_arg=
	if [ "$arch" = riscv64 ]; then
		# Use hardware floating point for the freestanding runtime fixture.
		# The baseline rv64i target needs soft-float runtime helpers.
		cpu_arg=--tar.cpu=sifive-u74
		cc_cpu_arg=-mcpu=sifive-u74
	fi
	for opt in none high; do
		out="$work/$arch-$opt"
		"$compiler" "$fixtures/variadics.sl" --build.compile-only "--tar.triple=$arch-linux-gnu" $cpu_arg "--opt.level=$opt" "--build.output=$out.o"
		"$compiler" "$fixtures/variadics.sl" --emit.ir "--tar.triple=$arch-linux-gnu" $cpu_arg "--opt.level=$opt" "--build.output=$out.ll"
		if [ "$arch" = x86_64 ]; then
			"$cc" -no-pie "$fixtures/variadics.c" "$out.o" -o "$out"
			"$out"
		else
			# Link a freestanding foreign executable, even without an emulator.
			if [ "$arch" = aarch64 ]; then
				printf '.text\n.globl _start\n_start:\n bl main\n mov x8, #93\n svc #0\n' > "$out-start.s"
			else
				printf '.text\n.globl _start\n_start:\n call main\n li a7, 93\n ecall\n' > "$out-start.s"
			fi
			"$cc" "--target=$arch-linux-gnu" $cc_cpu_arg -ffreestanding -fno-stack-protector -nostdlib -static -fuse-ld=lld \
				"$fixtures/variadics.c" "$out-start.s" "$out.o" -o "$out"
			if command -v "qemu-$arch" >/dev/null 2>&1; then
				"qemu-$arch" "$out"
			else
				echo "$arch: compiled and linked; runtime skipped (qemu-$arch unavailable)"
			fi
		fi
	done
done
# Guard the target cursor storage layouts used by llvm.va_start/va_copy.
grep -Fq 'type { i32, i32, ptr, ptr }' "$work/x86_64-none.ll"
grep -Fq 'type { ptr, ptr, ptr, i32, i32 }' "$work/aarch64-none.ll"
grep -Fq 'type { ptr }' "$work/riscv64-none.ll"
# An immutable pointer cannot consume a borrowed cursor; borrowed destinations
# cannot be initialized by va_start or va_copy.
for operation in arg start copy; do
	case "$operation" in
		arg) signature='ap: *@va.list'; body='return @va.arg(ap, u32);'; diagnostic='requires a mutable pointer' ;;
		start) signature='ap: *mut @va.list, ...'; body='@va.start(ap); return 0;'; diagnostic='destination must be a local' ;;
		copy) signature='ap: *mut @va.list, src: *@va.list'; body='@va.copy(ap, src); return 0;'; diagnostic='destination must be a local' ;;
	esac
	if printf '#[abi("C")] pub fn invalid(%s) -> u32 { %s }\n' "$signature" "$body" | "$compiler" /dev/stdin --build.compile-only "--build.output=$work/invalid.o" > "$work/$operation.log" 2>&1; then
		echo "expected borrowed va-list $operation to fail" >&2
		exit 1
	fi
	grep -Fq "$diagnostic" "$work/$operation.log"
done
