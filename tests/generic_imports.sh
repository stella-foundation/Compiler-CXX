#!/bin/sh
set -eu
compiler=$1
cc=$2
work=$3
fixtures=$4
mkdir -p "$work"
printf 'extern int generic_import_test(void); int main(void) { return generic_import_test() != 42; }\n' > "$work/runtime.c"
for opt in none high; do
	for source in generic_import generic_import_user; do
		"$compiler" "$fixtures/$source.sl" --build.compile-only "--opt.level=$opt" "--build.output=$work/$source-$opt.o"
	done
	"$cc" -no-pie "$work/runtime.c" "$work/generic_import-$opt.o" "$work/generic_import_user-$opt.o" -o "$work/test-$opt"
	"$work/test-$opt"
done

if printf 'import "%s/generic_import.sl"; pub fn invalid() -> void { let value: generic.imported.Envelope<i32>; }\n' "$fixtures" | "$compiler" /dev/stdin --build.compile-only "--build.output=$work/invalid.o" > "$work/invalid.log" 2>&1; then
	echo "expected bounded generic type to reject a non-implementing argument" >&2
	exit 1
fi
grep -Fq 'E_GENERIC_CONSTRAINT' "$work/invalid.log"
