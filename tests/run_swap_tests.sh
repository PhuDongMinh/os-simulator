#!/bin/sh
# Module 1 completion criteria end to end (swap-in/out, isolation,
# page boundaries) with AddressSanitizer: any leak fails.
# Usage: sh tests/run_swap_tests.sh
#        SANITIZERS=none sh tests/run_swap_tests.sh   (e.g. MinGW/Windows)
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ossim-swap.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc=${CC:-gcc}
sanitizers=${SANITIZERS:-address,undefined}
flags="-std=gnu11 -g -O1 -fno-omit-frame-pointer"
if [ "$sanitizers" != none ]; then
    flags="$flags -fsanitize=$sanitizers"
fi
inc="-iquote include -iquote src"

for f in libmem mm64 mm-memphy mm-vm mm syscall sys_mem sys_listsyscall libstd; do
    "$cc" $flags $inc -pthread -c "src/$f.c" -o "$build_dir/$f.o"
done
"$cc" $flags $inc -Wall -Wextra -Werror -pthread \
    -c tests/test_swap.c -o "$build_dir/test.o"
"$cc" $flags -pthread "$build_dir"/*.o -o "$build_dir/test_swap"
# The syscalls log every IO: keep the full log, show the verdicts, and keep
# the test's own exit status (assert/ASan failures must fail the script).
status=0
"$build_dir/test_swap" > "$build_dir/log" 2>&1 || status=$?
grep -E "PASS|FAIL|WARNING|ERROR|Assertion|Sanitizer" "$build_dir/log" || true
if [ "$status" -ne 0 ]; then
    tail -n 30 "$build_dir/log"
    exit "$status"
fi
