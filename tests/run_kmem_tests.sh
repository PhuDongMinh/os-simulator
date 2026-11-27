#!/usr/bin/env sh
# Kernel memory tests: __kmalloc / __kfree on the MM64 page table + MEMPHY.
set -eu

cd "$(dirname "$0")/.."
cc=${CC:-cc}
build_dir=${BUILD_DIR:-/tmp/ossim_kmem_tests}
mkdir -p "$build_dir"
inc="-iquote include -iquote src"
flags="-std=gnu11 -g -O0 -Wall"

for f in libmem mm64 mm-memphy mm-vm mm syscall sys_mem sys_listsyscall libstd; do
    "$cc" $flags $inc -pthread -c "src/$f.c" -o "$build_dir/$f.o"
done
"$cc" $flags $inc -Werror -pthread -c tests/test_kmem.c -o "$build_dir/test_kmem.o"
"$cc" -pthread "$build_dir"/*.o -o "$build_dir/test_kmem"
"$build_dir/test_kmem"
