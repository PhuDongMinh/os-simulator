#!/bin/sh
# Stage 1: five-level page-table tree and PTE API (src/mm64.c).
# Usage: sh tests/run_mm64_pt_tests.sh
#        SANITIZERS=none sh tests/run_mm64_pt_tests.sh   (e.g. MinGW/Windows)
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ossim-mm64-pt.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc=${CC:-gcc}
sanitizers=${SANITIZERS:-address,undefined}
flags="-std=gnu11 -g -O1 -fno-omit-frame-pointer"
if [ "$sanitizers" != none ]; then
    flags="$flags -fsanitize=$sanitizers"
fi
inc="-iquote include -iquote src"

# mm64.c still contains later-stage stubs that warn; the test itself is -Werror.
"$cc" $flags $inc -Wall -pthread -c src/mm64.c -o "$build_dir/mm64.o"
"$cc" $flags $inc -Wall -pthread -c src/mm-memphy.c -o "$build_dir/memphy.o"
"$cc" $flags $inc -Wall -Wextra -Werror -pthread \
    -c tests/test_mm64_pt.c -o "$build_dir/test.o"
"$cc" $flags -pthread "$build_dir/test.o" "$build_dir/mm64.o" "$build_dir/memphy.o" \
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free \
    -o "$build_dir/test_mm64_pt"
"$build_dir/test_mm64_pt"
