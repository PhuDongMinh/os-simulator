#!/bin/sh
# free_pcb_memph (MM64) with AddressSanitizer: any leaked allocation fails.
# Usage: sh tests/run_mm_teardown_tests.sh
#        SANITIZERS=none sh tests/run_mm_teardown_tests.sh   (e.g. MinGW/Windows)
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ossim-teardown.XXXXXX")
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
    -c tests/test_mm_teardown.c -o "$build_dir/test.o"
"$cc" $flags -pthread "$build_dir"/*.o -o "$build_dir/test_mm_teardown"
"$build_dir/test_mm_teardown"
