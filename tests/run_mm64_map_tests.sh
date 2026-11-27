#!/bin/sh
# Module 1 mapping (vm_map_ram, alloc_pages_range, vmap_page_range, ...).
# Usage: sh tests/run_mm64_map_tests.sh
#        SANITIZERS=none sh tests/run_mm64_map_tests.sh   (e.g. MinGW/Windows)
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ossim-mm64-map.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc=${CC:-gcc}
sanitizers=${SANITIZERS:-address,undefined}
flags="-std=gnu11 -g -O1 -fno-omit-frame-pointer"
if [ "$sanitizers" != none ]; then
    flags="$flags -fsanitize=$sanitizers"
fi
inc="-iquote include -iquote src"

"$cc" $flags $inc -Wall -pthread -c src/mm64.c -o "$build_dir/mm64.o"
"$cc" $flags $inc -Wall -pthread -c src/mm-memphy.c -o "$build_dir/memphy.o"
"$cc" $flags $inc -Wall -Wextra -Werror -pthread \
    -c tests/test_mm64_map.c -o "$build_dir/test.o"
"$cc" $flags -pthread "$build_dir/test.o" "$build_dir/mm64.o" "$build_dir/memphy.o" \
    -o "$build_dir/test_mm64_map"
"$build_dir/test_mm64_map"
