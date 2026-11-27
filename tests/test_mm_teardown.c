/* free_pcb_memph (MM64): a finished process gives back every frame and
 * swap slot, and every host allocation of its mm.  Built with
 * AddressSanitizer by tests/run_mm_teardown_tests.sh: any leak fails. */
#include "common.h"
#include "mm.h"
#include "mm64.h"
#include "libmem.h"
#include "dma.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libmem.c helper without a header prototype */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt);

/* libmem.c references the DMA service of os.c: not used here */
int dma_submit(uint32_t pid, uint64_t s, uint64_t d, uint64_t l, dma_done_fn f, void *c)
{
  (void)pid; (void)s; (void)d; (void)l; (void)f; (void)c;
  return -1;
}

#define PG ((addr_t)PAGING64_PAGESZ)
#define UBASE PAGING64_USER_VMA0_START

static struct memphy_struct ram, swp0, swp1;
static struct memphy_struct *swp_tbl[PAGING_MAX_MMSWP];
static struct krnl_t krnl;
static struct mm_struct kmm;

static int count_fp(struct framephy_struct *fp)
{
  int n = 0;
  for (; fp != NULL; fp = fp->fp_next) n++;
  return n;
}

static void free_fp_nodes(struct framephy_struct *fp)
{
  while (fp) { struct framephy_struct *n = fp->fp_next; free(fp); fp = n; }
}

static void free_dev(struct memphy_struct *mp)
{
  free_fp_nodes(mp->free_fp_list);
  free_fp_nodes(mp->used_fp_list);
  free(mp->storage);
}

static void setup(int ram_frames)
{
  memset(&krnl, 0, sizeof(krnl));
  memset(&kmm, 0, sizeof(kmm));
  memset(swp_tbl, 0, sizeof(swp_tbl));
  assert(init_memphy(&ram, (addr_t)ram_frames * PG, 1) == 0);
  assert(init_memphy(&swp0, 4 * PG, 1) == 0);
  assert(init_memphy(&swp1, 2 * PG, 1) == 0);
  swp_tbl[0] = &swp0;
  swp_tbl[1] = &swp1;
  krnl.mram = &ram;
  krnl.mswp = swp_tbl;
  krnl.active_mswp = &swp0;
  krnl.active_mswp_id = 0;
  assert(init_mm(&kmm, NULL) == 0);
  krnl.mm = &kmm;
}

static void teardown(void)
{
  /* kernel mm (lives for the whole run in os.c) */
  while (kmm.mmap) {
    struct vm_area_struct *v = kmm.mmap;
    while (v->vm_freerg_list) {
      struct vm_rg_struct *n = v->vm_freerg_list->rg_next;
      free(v->vm_freerg_list);
      v->vm_freerg_list = n;
    }
    kmm.mmap = v->vm_next;
    free(v);
  }
  mm64_pgd_destroy(&kmm);
  free_dev(&ram);
  free_dev(&swp0);
  free_dev(&swp1);
}

static void new_proc(struct pcb_t *p, struct mm_struct *mm, uint32_t pid)
{
  memset(p, 0, sizeof(*p));
  memset(mm, 0, sizeof(*mm));
  p->pid = pid;
  p->krnl = &krnl;
  p->mm = mm;
  for (int k = 0; k < PAGING_MAX_SYMTBL_SZ; k++)
    p->kmap_slot[k] = -1;
  assert(init_mm(mm, p) == 0);
}

/* RAM pages, pages in two swap devices, a free region: all returned */
static void test_release_everything(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  setup(2);
  new_proc(&p, &mm, 1);

  /* 4 pages with 2 RAM frames: the 2 oldest end up in swap0 */
  assert(vm_map_ram(&p, UBASE, UBASE + 2 * PG, UBASE, 2, NULL) == 0);
  assert(vm_map_ram(&p, UBASE, UBASE + 4 * PG, UBASE + 2 * PG, 2, NULL) == 0);
  assert(count_fp(ram.free_fp_list) == 0 && count_fp(swp0.used_fp_list) == 2);

  /* one more page in swap1, as if the active swap had changed */
  addr_t off;
  assert(MEMPHY_get_freefp(&swp1, &off) == 0);
  assert(pte_set_swap(&p, (UBASE + 9 * PG) >> PAGING64_ADDR_PT_SHIFT, 1, off) == 0);

  /* a region in the free list and a symbol entry */
  struct vm_rg_struct *rg = init_vm_rg(UBASE + 4 * PG, UBASE + 5 * PG);
  assert(rg != NULL && enlist_vm_freerg_list(&mm, rg) == 0);
  mm.symrgtbl[3].rg_start = UBASE;
  mm.symrgtbl[3].rg_end = UBASE + 10;

  assert(free_pcb_memph(&p) == 0);
  assert(count_fp(ram.used_fp_list) == 0 && count_fp(ram.free_fp_list) == 2);
  assert(count_fp(swp0.used_fp_list) == 0 && count_fp(swp0.free_fp_list) == 4);
  assert(count_fp(swp1.used_fp_list) == 0 && count_fp(swp1.free_fp_list) == 2);
  assert(mm.pgd == NULL && mm.mmap == NULL && mm.fifo_pgn == NULL);
  assert(krnl.fifo_pgn == NULL);              /* global FIFO emptied */
  assert(mm.symrgtbl[3].rg_start == 0 && mm.symrgtbl[3].rg_end == 0);

  teardown();
  printf("PASS test_release_everything\n");
}

/* Kernel regions of the process are not touched (they belong to __kfree) */
static void test_kernel_memory_untouched(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  addr_t ka = 0;
  setup(4);
  new_proc(&p, &mm, 2);

  assert(vm_map_ram(&p, UBASE, UBASE + PG, UBASE, 1, NULL) == 0);
  assert(__kmalloc(&p, 0, 0, 100, &ka) == 0);
  assert(count_fp(ram.used_fp_list) == 2);

  assert(free_pcb_memph(&p) == 0);
  assert(count_fp(ram.used_fp_list) == 1);            /* kernel frame stays */
  assert(PAGING_PAGE_PRESENT(pte_get_entry(&kmm, ka >> PAGING64_ADDR_PT_SHIFT, NULL)));
  assert(__kfree(&p, 0, 0) == 0);
  assert(count_fp(ram.used_fp_list) == 0);

  /* never on the kernel mm itself, nor on bad arguments */
  struct pcb_t k = p;
  k.mm = &kmm;
  assert(free_pcb_memph(&k) == -1);
  assert(free_pcb_memph(NULL) == -1);

  teardown();
  printf("PASS test_kernel_memory_untouched\n");
}

/* Fresh mm, and a debug pattern (owns no frame): still fully freed */
static void test_empty_and_pattern(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  setup(2);

  new_proc(&p, &mm, 3);
  assert(free_pcb_memph(&p) == 0);
  assert(mm.pgd == NULL && mm.mmap == NULL);

  new_proc(&p, &mm, 4);
  assert(vmap_pgd_memset(&p, UBASE, 2) == 0);
  assert(free_pcb_memph(&p) == -1);                   /* reported ... */
  assert(mm.pgd == NULL && mm.mmap == NULL);          /* ... but freed */
  assert(count_fp(ram.free_fp_list) == 2);

  teardown();
  printf("PASS test_empty_and_pattern\n");
}

int main(void)
{
  test_release_everything();
  test_kernel_memory_untouched();
  test_empty_and_pattern();
  printf("ALL TEARDOWN TESTS PASSED\n");
  return 0;
}
