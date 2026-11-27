#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d /tmp/ossim-stage-a.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc=${CC:-gcc}
sanitizers=${SANITIZERS:-address,undefined}
flags="-std=gnu11 -g -O1 -fno-omit-frame-pointer -ffunction-sections -fdata-sections"
if [ "$sanitizers" != none ]; then
    flags="$flags -fsanitize=$sanitizers"
fi

# -iquote avoids the existing project sched.h/system <sched.h> collision.
"$cc" $flags -iquote include -Wall -Wextra -Werror -pthread \
    -c src/mm-memphy.c -o "$build_dir/memphy.o"
"$cc" $flags -iquote include -Wall -Wextra -Werror -pthread \
    -c tests/test_memphy.c -o "$build_dir/test.o"
# Compile the real Module 3 wrapper, discarding unrelated functions at link.
# Its pre-existing missing pte_clear_entry declaration is outside Stage A.
"$cc" $flags -iquote include -Wno-implicit-function-declaration -pthread \
    -c src/libmem.c -o "$build_dir/libmem.o"
# libmem defines __read/__write, names also referenced by sanitizer/libc.
# Localize unrelated definitions so they cannot become dynamic link roots.
objcopy --keep-global-symbol=MEMPHY_get_freefp_contig "$build_dir/libmem.o"
# libmem's DMA translation calls pte_get_entry (mm64.c) through a function
# pointer, which --gc-sections cannot drop: link the page table too.
"$cc" $flags -iquote include -iquote src -Wall -pthread \
    -c src/mm64.c -o "$build_dir/mm64.o"
"$cc" $flags -pthread "$build_dir/test.o" "$build_dir/memphy.o" \
    "$build_dir/libmem.o" "$build_dir/mm64.o" \
    -Wl,--gc-sections -Wl,--wrap=malloc -Wl,--wrap=free \
    -o "$build_dir/test_memphy"
"$build_dir/test_memphy"
