/* Module 1 mapping: vm_map_ram / alloc_pages_range / vmap_page_range /
 * vmap_pgd_memset / __swap_cp_page on the pt_pgd tree.
 * Run with tests/run_mm64_map_tests.sh (AddressSanitizer catches leaks). */
#include "mm64.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PG ((addr_t)PAGING64_PAGESZ)
#define UBASE PAGING64_USER_VMA0_START
#define PGN(a) ((addr_t)(a) >> PAGING64_ADDR_PT_SHIFT)

/* Strong definition overrides mm64.c's weak reference: tests pin frames */
static int pinned_fpn = -1;
int dma_frame_pinned(uint64_t fpn) { return (int)fpn == pinned_fpn; }

static struct memphy_struct ram, swp;
static struct krnl_t krnl;
static struct pcb_t proc;
static struct mm_struct mm;

static int count_fp(struct framephy_struct *fp)
{
  int n = 0;
  for (; fp != NULL; fp = fp->fp_next) n++;
  return n;
}

static int fifo_len(void)
{
  int n = 0;
  for (struct pgn_t *p = mm.fifo_pgn; p != NULL; p = p->pg_next) n++;
  return n;
}

/* FIFO page numbers, newest first */
static void fifo_get(addr_t *out, int n)
{
  struct pgn_t *p = mm.fifo_pgn;
  for (int i = 0; i < n; i++, p = p->pg_next) {
    assert(p != NULL);
    out[i] = p->pgn;
  }
  assert(p == NULL);
}

static addr_t pte_of(addr_t vaddr) { return pte_get_entry(&mm, PGN(vaddr), NULL); }
static int present(addr_t pte) { return PAGING_PAGE_PRESENT(pte) && !(pte & PAGING_PTE_SWAPPED_MASK); }
static int swapped(addr_t pte) { return !PAGING_PAGE_PRESENT(pte) && (pte & PAGING_PTE_SWAPPED_MASK); }

static void setup(int ram_frames, int swp_frames)
{
  memset(&krnl, 0, sizeof(krnl));
  memset(&proc, 0, sizeof(proc));
  memset(&mm, 0, sizeof(mm));
  assert(init_memphy(&ram, (addr_t)ram_frames * PG, 1) == 0);
  assert(init_memphy(&swp, (addr_t)swp_frames * PG, 1) == 0);
  krnl.mram = &ram;
  krnl.active_mswp = (swp_frames > 0) ? &swp : NULL;
  krnl.active_mswp_id = 0;
  proc.pid = 1;
  proc.krnl = &krnl;
  proc.mm = &mm;
  assert(init_mm(&mm, &proc) == 0);
  pinned_fpn = -1;
}

static void teardown(void)
{
  struct pgn_t *p = mm.fifo_pgn;
  while (p) { struct pgn_t *n = p->pg_next; free(p); p = n; }
  struct vm_area_struct *v = mm.mmap;
  while (v) {
    struct vm_rg_struct *r = v->vm_freerg_list;
    while (r) { struct vm_rg_struct *n = r->rg_next; free(r); r = n; }
    struct vm_area_struct *n = v->vm_next;
    free(v);
    v = n;
  }
  mm64_pgd_destroy(&mm);
  free(ram.storage);
  free(swp.storage);
  /* memphy frame nodes */
  struct framephy_struct *lists[] = {ram.free_fp_list, ram.used_fp_list,
                                     swp.free_fp_list, swp.used_fp_list};
  for (int i = 0; i < 4; i++) {
    struct framephy_struct *f = lists[i];
    while (f) { struct framephy_struct *n = f->fp_next; free(f); f = n; }
  }
}

/* Normal mapping: PRESENT, distinct frames, FIFO, ret_rg, RAM accounting */
static void test_map_basic(void)
{
  struct vm_rg_struct rg;
  setup(8, 4);

  assert(vm_map_ram(&proc, UBASE, UBASE + 3 * PG, UBASE, 3, &rg) == 0);
  assert(rg.rg_start == UBASE && rg.rg_end == UBASE + 3 * PG);
  for (int i = 0; i < 3; i++)
    assert(present(pte_of(UBASE + i * PG)));
  assert(PAGING_FPN(pte_of(UBASE)) != PAGING_FPN(pte_of(UBASE + PG)));
  assert(count_fp(ram.used_fp_list) == 3 && count_fp(ram.free_fp_list) == 5);

  addr_t f[3];
  fifo_get(f, 3);
  assert(f[0] == PGN(UBASE + 2 * PG) && f[2] == PGN(UBASE)); /* newest first */

  /* 0 pages: valid no-op */
  assert(vm_map_ram(&proc, UBASE, UBASE + 3 * PG, UBASE + 3 * PG, 0, NULL) == 0);
  teardown();
  printf("PASS test_map_basic\n");
}

/* Refusals change nothing */
static void test_map_refused(void)
{
  setup(8, 4);
  assert(vm_map_ram(&proc, UBASE, UBASE + 2 * PG, UBASE, 2, NULL) == 0);

  /* overlaps a mapped page */
  assert((int64_t)vm_map_ram(&proc, UBASE, UBASE + 3 * PG, UBASE + PG, 2, NULL) < 0);
  /* misaligned, outside [astart, aend), below the vma, NULL caller */
  assert((int64_t)vm_map_ram(&proc, UBASE, UBASE + 9 * PG, UBASE + 5 * PG + 1, 1, NULL) < 0);
  assert((int64_t)vm_map_ram(&proc, UBASE, UBASE + 4 * PG, UBASE + 3 * PG, 2, NULL) < 0);
  assert((int64_t)vm_map_ram(&proc, 0, 4 * PG, 0, 1, NULL) < 0);
  assert((int64_t)vm_map_ram(NULL, UBASE, UBASE + PG, UBASE, 1, NULL) < 0);

  assert(count_fp(ram.used_fp_list) == 2 && fifo_len() == 2);
  assert(pte_of(UBASE + 2 * PG) == 0 && pte_of(UBASE + 3 * PG) == 0);
  teardown();
  printf("PASS test_map_refused\n");
}

/* RAM full: oldest pages go to swap, with their content */
static void test_evict_to_swap(void)
{
  setup(4, 4);
  assert(vm_map_ram(&proc, UBASE, UBASE + 4 * PG, UBASE, 4, NULL) == 0);
  addr_t f0 = PAGING_FPN(pte_of(UBASE));
  addr_t f1 = PAGING_FPN(pte_of(UBASE + PG));
  assert(MEMPHY_write(&ram, f0 * PG + 7, 0xA1) == 0);
  assert(MEMPHY_write(&ram, f1 * PG + 4095, 0xB2) == 0);

  assert(vm_map_ram(&proc, UBASE, UBASE + 6 * PG, UBASE + 4 * PG, 2, NULL) == 0);

  /* the two oldest pages (UBASE, UBASE+PG) were evicted */
  addr_t p0 = pte_of(UBASE), p1 = pte_of(UBASE + PG);
  assert(swapped(p0) && swapped(p1));
  assert(GETVAL(p0, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT) == 0);
  BYTE v = 0;
  assert(MEMPHY_read(&swp, PAGING_PTE_SWP(p0) * PG + 7, &v) == 0 && v == (BYTE)0xA1);
  assert(MEMPHY_read(&swp, PAGING_PTE_SWP(p1) * PG + 4095, &v) == 0 && v == (BYTE)0xB2);

  /* their frames now back the new pages */
  addr_t n0 = PAGING_FPN(pte_of(UBASE + 4 * PG)), n1 = PAGING_FPN(pte_of(UBASE + 5 * PG));
  assert(present(pte_of(UBASE + 4 * PG)) && present(pte_of(UBASE + 5 * PG)));
  assert((n0 == f0 && n1 == f1) || (n0 == f1 && n1 == f0));

  assert(fifo_len() == 4);                 /* victims left the FIFO */
  addr_t f[4];
  fifo_get(f, 4);
  assert(f[3] == PGN(UBASE + 2 * PG));     /* new oldest */
  assert(count_fp(ram.free_fp_list) == 0 && count_fp(swp.used_fp_list) == 2);
  teardown();
  printf("PASS test_evict_to_swap\n");
}

/* Not enough RAM + swap: everything is rolled back */
static void test_out_of_memory_rollback(void)
{
  setup(2, 1);
  assert(vm_map_ram(&proc, UBASE, UBASE + 2 * PG, UBASE, 2, NULL) == 0);
  addr_t before0 = pte_of(UBASE), before1 = pte_of(UBASE + PG);
  addr_t fifo_before[2];
  fifo_get(fifo_before, 2);

  /* needs 3 frames: 0 free, only 1 swap slot -> fails after 1 eviction */
  assert((int64_t)vm_map_ram(&proc, UBASE, UBASE + 5 * PG, UBASE + 2 * PG, 3, NULL) < 0);

  assert(pte_of(UBASE) == before0 && pte_of(UBASE + PG) == before1);
  for (int i = 2; i < 5; i++)
    assert(pte_of(UBASE + i * PG) == 0);
  addr_t fifo_after[2];
  fifo_get(fifo_after, 2);
  assert(fifo_after[0] == fifo_before[0] && fifo_after[1] == fifo_before[1]);
  assert(count_fp(swp.free_fp_list) == 1 && count_fp(swp.used_fp_list) == 0);
  assert(count_fp(ram.used_fp_list) == 2);

  /* no swap device at all: same, and free frames taken first are returned */
  teardown();
  setup(3, 0);
  assert(vm_map_ram(&proc, UBASE, UBASE + 2 * PG, UBASE, 2, NULL) == 0);
  assert((int64_t)vm_map_ram(&proc, UBASE, UBASE + 4 * PG, UBASE + 2 * PG, 2, NULL) < 0);
  assert(count_fp(ram.free_fp_list) == 1 && count_fp(ram.used_fp_list) == 2);
  assert(pte_of(UBASE + 2 * PG) == 0);
  teardown();
  printf("PASS test_out_of_memory_rollback\n");
}

/* A frame pinned by the DMA is never chosen as victim */
static void test_skip_pinned(void)
{
  setup(2, 2);
  assert(vm_map_ram(&proc, UBASE, UBASE + 2 * PG, UBASE, 2, NULL) == 0);
  pinned_fpn = (int)PAGING_FPN(pte_of(UBASE));       /* the oldest page */

  assert(vm_map_ram(&proc, UBASE, UBASE + 3 * PG, UBASE + 2 * PG, 1, NULL) == 0);
  assert(present(pte_of(UBASE)));                     /* kept */
  assert(swapped(pte_of(UBASE + PG)));                /* next oldest went */

  /* every resident page pinned/new-only: no victim -> fails cleanly */
  pinned_fpn = (int)PAGING_FPN(pte_of(UBASE));
  teardown();
  setup(1, 1);
  assert(vm_map_ram(&proc, UBASE, UBASE + PG, UBASE, 1, NULL) == 0);
  pinned_fpn = (int)PAGING_FPN(pte_of(UBASE));
  assert((int64_t)vm_map_ram(&proc, UBASE, UBASE + 2 * PG, UBASE + PG, 1, NULL) < 0);
  assert(present(pte_of(UBASE)) && fifo_len() == 1);
  teardown();
  printf("PASS test_skip_pinned\n");
}

/* vmap_page_range maps min(pgnum, frames) pages with given frames */
static void test_vmap_page_range(void)
{
  struct framephy_struct f2 = {.fpn = 6, .fp_next = NULL};
  struct framephy_struct f1 = {.fpn = 5, .fp_next = &f2};
  struct vm_rg_struct rg;
  setup(8, 0);

  assert(vmap_page_range(&proc, UBASE, 3, &f1, &rg) == 0);  /* only 2 frames */
  assert(rg.rg_start == UBASE && rg.rg_end == UBASE + 2 * PG);
  assert(PAGING_FPN(pte_of(UBASE)) == 5 && PAGING_FPN(pte_of(UBASE + PG)) == 6);
  assert(pte_of(UBASE + 2 * PG) == 0 && fifo_len() == 2);
  assert((int64_t)vmap_page_range(&proc, UBASE, 1, &f1, NULL) < 0); /* mapped */
  teardown();
  printf("PASS test_vmap_page_range\n");
}

/* vmap_pgd_memset writes the pattern on all pages or none */
static void test_pgd_memset(void)
{
  setup(4, 0);
  assert(vmap_pgd_memset(&proc, UBASE, 3) == 0);
  for (int i = 0; i < 3; i++)
    assert(pte_of(UBASE + i * PG) == 0xdeadbeef);
  assert(vmap_pgd_memset(&proc, UBASE + 1, 1) == -1);     /* misaligned */
  assert(vmap_pgd_memset(&proc, 0, 1) == -1);             /* page zero */
  assert(vmap_pgd_memset(NULL, UBASE, 1) == -1);
  teardown();
  printf("PASS test_pgd_memset\n");
}

/* __swap_cp_page copies a whole frame and rejects bad frames */
static void test_swap_cp_page(void)
{
  setup(2, 2);
  for (addr_t i = 0; i < PG; i++)
    assert(MEMPHY_write(&ram, PG + i, (BYTE)(i * 7)) == 0);
  assert(__swap_cp_page(&ram, 1, &swp, 0) == 0);
  for (addr_t i = 0; i < PG; i++) {
    BYTE v;
    assert(MEMPHY_read(&swp, i, &v) == 0 && v == (BYTE)(i * 7));
  }
  assert(__swap_cp_page(&ram, 2, &swp, 0) == -1);  /* frame out of RAM */
  assert(__swap_cp_page(NULL, 0, &swp, 0) == -1);
  teardown();
  printf("PASS test_swap_cp_page\n");
}

int main(void)
{
  test_map_basic();
  test_map_refused();
  test_evict_to_swap();
  test_out_of_memory_rollback();
  test_skip_pinned();
  test_vmap_page_range();
  test_pgd_memset();
  test_swap_cp_page();
  printf("ALL MM64 MAPPING TESTS PASSED\n");
  return 0;
}
