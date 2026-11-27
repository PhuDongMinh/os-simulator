#!/usr/bin/env python3
"""Compile-only API/type checks plus C/C++ layout comparison for Stage B0."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC", "gcc")
CXX = os.environ.get("CXX", "g++")

ORDERS = {
    "os-mm-first": ["os-mm.h", "common.h"],
    "common-first": ["common.h", "os-mm.h"],
    "mm-first": ["mm.h", "os-mm.h"],
    "mm64-first": ["mm64.h", "common.h"],
    "syscall-first": ["syscall.h", "os-mm.h"],
    "libmem-first": ["libmem.h", "syscall.h"],
    "pthread-first": ["pthread.h", "os-mm.h", "common.h"],
    "private-first": ["mm64-internal.h", "mm64.h"],
    "nested-extern-c": ["os-mm.h", "common.h", "mm64-internal.h"],
}

CHECKS = r"""
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
#include <type_traits>
#define ASSERT(c) static_assert((c), #c)
#define TYPE(expr, expected_type) static_assert(std::is_same<typename std::remove_reference<decltype(expr)>::type, expected_type>::value, #expr)
#else
#define ASSERT(c) _Static_assert((c), #c)
#define TYPE(expr, type) _Static_assert(__builtin_types_compatible_p(__typeof__(expr), type), #expr)
#endif

#ifndef MM64
#error These checks target the configured MM64 architecture.
#endif
ASSERT(sizeof(addr_t) == 8);
ASSERT(sizeof(arg_t) == 8);
ASSERT(sizeof(mm_pte_t) == 4);
TYPE((addr_t)0, uint64_t);
TYPE((arg_t)0, uint64_t);
TYPE((mm_pte_t)0, uint32_t);

extern struct mm_struct b0_mm;
extern struct krnl_t b0_kernel;
extern struct pt_pgd b0_pgd;
extern struct pt_p4d b0_p4d;
extern struct pt_pud b0_pud;
extern struct pt_pmd b0_pmd;
extern struct pt_leaf b0_leaf;
TYPE(b0_mm.pgd, struct pt_pgd *);
TYPE(b0_kernel.krnl_pgd, struct pt_pgd *);
TYPE(b0_pgd.child[0], struct pt_p4d *);
TYPE(b0_p4d.child[0], struct pt_pud *);
TYPE(b0_pud.child[0], struct pt_pmd *);
TYPE(b0_pmd.child[0], struct pt_leaf *);
TYPE(b0_leaf.entry[0], mm_pte_t);
TYPE(b0_mm.p4d, addr_t *);
TYPE(b0_mm.pud, addr_t *);
TYPE(b0_mm.pmd, addr_t *);
TYPE(b0_mm.pt, addr_t *);
TYPE(b0_kernel.krnl_p4d, addr_t *);
TYPE(b0_kernel.krnl_pud, addr_t *);
TYPE(b0_kernel.krnl_pmd, addr_t *);
TYPE(b0_kernel.krnl_pt, addr_t *);

ASSERT(PAGING64_PT_INDEX_BITS == 9);
ASSERT(PAGING64_PT_ENTRIES == 512);
ASSERT(sizeof(b0_pgd.child) / sizeof(b0_pgd.child[0]) == 512);
ASSERT(sizeof(b0_p4d.child) / sizeof(b0_p4d.child[0]) == 512);
ASSERT(sizeof(b0_pud.child) / sizeof(b0_pud.child[0]) == 512);
ASSERT(sizeof(b0_pmd.child) / sizeof(b0_pmd.child[0]) == 512);
ASSERT(sizeof(b0_leaf.entry) / sizeof(b0_leaf.entry[0]) == 512);
ASSERT(sizeof(struct pt_leaf) == 2048);
ASSERT(PAGING64_PAGESZ == 4096);
ASSERT(PAGING64_ADDR_OFFST_MASK == 0xfffULL);
ASSERT(PAGING64_ADDR_OFFST(0xabcdefULL) == 0xdefULL);
/* MM64 widens the FPN to 16 bits (256 MiB RAM); see Document/STAGE_2_INIT_MM.md. */
ASSERT(PAGING_PTE_FPN_LOBIT == 0 && PAGING_PTE_FPN_HIBIT == 15);
ASSERT(PAGING_PTE_FPN_MASK == 0xffffU);
ASSERT(PAGING64_MAX_RAM_FRAMES == 65536ULL);
ASSERT(PAGING64_MAX_RAMSZ == 256ULL * 1024 * 1024);
ASSERT(PAGING_PTE_PRESENT_MASK == 0x80000000U);
ASSERT(PAGING_PTE_SWAPPED_MASK == 0x40000000U);
ASSERT(PAGING_PTE_RESERVE_MASK == 0x20000000U);
ASSERT(PAGING_PTE_DIRTY_MASK == 0x10000000U);
ASSERT(PAGING_PTE_USRNUM_MASK == 0x0fff0000U);
ASSERT((PAGING_PTE_FPN_MASK & PAGING_PTE_USRNUM_MASK) == 0);
ASSERT(PAGING_PTE_SWPTYP_MASK == 0x1fU);
ASSERT(PAGING_PTE_SWPOFF_MASK == 0x03ffffe0U);

/* Freeze existing public signatures, including their wider value containers. */
typedef addr_t (*get_sig)(struct mm_struct *, addr_t, struct pcb_t *);
typedef int (*set_sig)(struct mm_struct *, addr_t, uint64_t, struct pcb_t *);
typedef int (*fpn_sig)(struct mm_struct *, addr_t, addr_t, struct pcb_t *);
typedef int (*swap_sig)(struct pcb_t *, addr_t, int, addr_t);
typedef int (*clear_sig)(struct mm_struct *, addr_t, struct pcb_t *);
typedef int (*init_sig)(addr_t *, int, addr_t, int, int, int, addr_t);
typedef addr_t (*kmalloc_sig)(struct pcb_t *, int, int, addr_t, addr_t *);
typedef int (*kfree_sig)(struct pcb_t *, int, int);
typedef int (*free_sig)(struct pcb_t *);
typedef int (*libmalloc_sig)(struct pcb_t *, uint32_t, uint32_t);
typedef int (*libfree_sig)(struct pcb_t *, uint32_t);
typedef int (*read_sig)(struct pcb_t *, int, int, addr_t, BYTE *);
typedef int (*write_sig)(struct pcb_t *, int, int, addr_t, BYTE);
typedef int (*copy_sig)(struct pcb_t *, uint32_t, uint32_t, uint32_t, uint32_t);
typedef int (*syscall_sig)(struct krnl_t *, uint32_t, uint32_t, struct sc_regs *);
TYPE(&pte_get_entry, get_sig);
TYPE(&pte_set_entry, set_sig);
TYPE(&pte_set_fpn, fpn_sig);
TYPE(&pte_set_swap, swap_sig);
TYPE(&pte_clear_entry, clear_sig);
TYPE(&init_pte, init_sig);
TYPE(&__kmalloc, kmalloc_sig);
TYPE(&__kfree, kfree_sig);
TYPE(&free_pcb_memph, free_sig);
TYPE(&libkmem_malloc, libmalloc_sig);
TYPE(&libkmem_free, libfree_sig);
TYPE(&__read_kernel_mem, read_sig);
TYPE(&__write_kernel_mem, write_sig);
TYPE(&__read_user_mem, read_sig);
TYPE(&__write_user_mem, write_sig);
TYPE(&libkmem_copy_from_user, copy_sig);
TYPE(&libkmem_copy_to_user, copy_sig);
TYPE(&_syscall, syscall_sig);

#define LAYOUT { \
    sizeof(addr_t), sizeof(arg_t), sizeof(mm_pte_t), \
    sizeof(struct vm_rg_struct), offsetof(struct vm_rg_struct, rg_end), \
    sizeof(struct vm_area_struct), offsetof(struct vm_area_struct, vm_mm), \
    sizeof(struct framephy_struct), offsetof(struct framephy_struct, owner), \
    sizeof(struct memphy_struct), offsetof(struct memphy_struct, free_fp_list), \
    sizeof(struct mm_struct), offsetof(struct mm_struct, mmap), \
    offsetof(struct mm_struct, symrgtbl), offsetof(struct mm_struct, fifo_pgn), \
    offsetof(struct mm_struct, kcpooltbl), \
    sizeof(struct pcb_t), offsetof(struct pcb_t, mm), offsetof(struct pcb_t, krnl), \
    offsetof(struct pcb_t, kmap_slot), \
    sizeof(struct krnl_t), offsetof(struct krnl_t, mm), \
    offsetof(struct krnl_t, mram), offsetof(struct krnl_t, mswp), \
    offsetof(struct krnl_t, krnl_pgd), offsetof(struct krnl_t, krnl_pt), \
    sizeof(struct pt_pgd), offsetof(struct pt_pgd, child), \
    sizeof(struct pt_p4d), sizeof(struct pt_pud), sizeof(struct pt_pmd), \
    sizeof(struct pt_leaf) \
}
"""


def preamble(headers, cpp=False, nested=False):
    lines = ['extern "C" {'] if cpp and nested else []
    # Include related headers twice: include guards are part of this contract.
    for name in headers + ["mm64.h", "libmem.h", "syscall.h", "mm64-internal.h"]:
        delimiter = ("<", ">") if name == "pthread.h" else ('"', '"')
        lines.extend([f"#include {delimiter[0]}{name}{delimiter[1]}"] * 2)
    if cpp and nested:
        lines.append("}")
    return "\n".join(lines) + "\n" + CHECKS


def run(command, expect_success=True):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if (result.returncode == 0) != expect_success:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def main():
    with tempfile.TemporaryDirectory(prefix="ossim-b0-types-", dir="/tmp") as directory:
        build = Path(directory)
        expected = None
        count = 0
        for compiler, standard, suffix in ((CC, "gnu11", ".c"), (CXX, "gnu++14", ".cpp")):
            for search in ("-iquote",):
                for name, order in ORDERS.items():
                    source = build / (name + suffix)
                    source.write_text(preamble(order, suffix == ".cpp", name == "nested-extern-c") + r"""
int main(void) {
    size_t layout[] = LAYOUT;
    for (size_t i = 0; i < sizeof(layout) / sizeof(layout[0]); ++i)
        printf("%zu ", layout[i]);
    puts("");
    return 0;
}
""")
                    binary = build / "layout"
                    run([compiler, "-std=" + standard, search, "include", "-iquote", "src",
                         "-Wall", "-Wextra", "-Werror", "-pthread", str(source), "-o", str(binary)])
                    layout = run([str(binary)])
                    if expected is None:
                        expected = layout
                    assert layout == expected, (compiler, search, name, layout, expected)
                    count += 1
        print(f"PASS {count} C/C++ include-order/layout/API cases (production -iquote flags)")
        print("LAYOUT:", expected.strip())

        # An actual mixed-language executable checks the C/C++ structure ABI.
        c_source = build / "provider.c"
        c_source.write_text(preamble(["os-mm.h"]) + r"""
size_t b0_c_layout(size_t *out) {
    size_t values[] = LAYOUT;
    size_t n = sizeof(values) / sizeof(values[0]);
    for (size_t i = 0; i < n; ++i) out[i] = values[i];
    return n;
}
""")
        cpp_source = build / "consumer.cpp"
        cpp_source.write_text(preamble(["syscall.h"], cpp=True) + r"""
extern "C" size_t b0_c_layout(size_t *out);
int main() {
    size_t expected[] = LAYOUT;
    size_t from_c[sizeof(expected) / sizeof(expected[0])];
    size_t n = b0_c_layout(from_c);
    if (n != sizeof(expected) / sizeof(expected[0])) return 1;
    for (size_t i = 0; i < n; ++i) if (expected[i] != from_c[i]) return 2;
    return 0;
}
""")
        include = ["-iquote", "include", "-iquote", "src", "-pthread"]
        run([CC, "-std=gnu11", *include, "-c", str(c_source), "-o", str(build / "provider.o")])
        run([CXX, "-std=gnu++14", *include, str(cpp_source), str(build / "provider.o"),
             "-o", str(build / "mixed")])
        run([str(build / "mixed")])
        print("PASS mixed C/C++ struct layout ABI")

        # Referencing APIs must produce C symbols from both languages.
        references = "\n".join(
            f"{signature} b0_ref_{name} = &{name};" for signature, name in (
                ("get_sig", "pte_get_entry"), ("set_sig", "pte_set_entry"),
                ("fpn_sig", "pte_set_fpn"), ("swap_sig", "pte_set_swap"),
                ("clear_sig", "pte_clear_entry"), ("init_sig", "init_pte"),
                ("kmalloc_sig", "__kmalloc"), ("kfree_sig", "__kfree"),
                ("free_sig", "free_pcb_memph"), ("copy_sig", "libkmem_copy_from_user"),
                ("copy_sig", "libkmem_copy_to_user"), ("syscall_sig", "_syscall"),
                ("libmalloc_sig", "libkmem_malloc"), ("libfree_sig", "libkmem_free"),
                ("read_sig", "__read_kernel_mem"), ("write_sig", "__write_kernel_mem"),
                ("read_sig", "__read_user_mem"), ("write_sig", "__write_user_mem")))
        symbols = []
        for compiler, standard, suffix in ((CC, "gnu11", ".c"), (CXX, "gnu++14", ".cpp")):
            source = build / ("api" + suffix)
            source.write_text(preamble(["libmem.h", "syscall.h"]) + references)
            obj = build / ("api" + suffix + ".o")
            run([compiler, "-std=" + standard, *include, "-c", str(source), "-o", str(obj)])
            symbols.append(set(run(["nm", "-u", str(obj)]).split()))
        assert symbols[0] == symbols[1], symbols
        assert not any(symbol.startswith("_Z") for symbol in symbols[1]), symbols
        print("PASS public Module 3/PTE API signatures and C linkage")

        for label, body in {
            "flat root assignment": "void f(struct mm_struct *m, addr_t *p) { m->pgd = p; }",
            "pointer as PTE": "mm_pte_t f(struct pt_pgd *p) { return p; }",
            "wrong child level": "void f(struct pt_pgd *p, struct pt_pmd *q) { p->child[0] = q; }",
        }.items():
            for compiler, standard, suffix in ((CC, "gnu11", ".c"), (CXX, "gnu++14", ".cpp")):
                source = build / ("negative" + suffix)
                source.write_text(preamble(["mm64-internal.h"]) + body)
                run([compiler, "-std=" + standard, *include, "-Wall", "-Wextra", "-Werror",
                     "-fsyntax-only", str(source)], expect_success=False)
            print("PASS rejected:", label)
        print("Stage B0: all focused type checks passed.")


if __name__ == "__main__":
    main()
