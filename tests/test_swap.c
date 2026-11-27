/* Module 1 completion criteria, end to end through the real user path:
 *   __alloc -> __write/__read -> pg_setval/pg_getval -> pg_getpage
 *   -> sys_memmap (IO_READ/IO_WRITE/SWP) -> MEMPHY
 * - small RAM forces swapping, yet every byte written is read back;
 * - two processes using the same virtual address keep separate data;
 * - accesses across page boundaries hit the right frames;
 * - pg_getpage refuses never-mapped pages and the kernel mm, and a failed
 *   swap-in changes nothing;
 * - a page is swapped back in from the device its PTE names.
 * Built with AddressSanitizer by tests/run_swap_tests.sh. */
#include "common.h"
#include "mm.h"
#include "mm64.h"
#include "libmem.h"
#include "dma.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libmem.c helpers without a header prototype */
int pg_getpage(struct mm_struct *mm, addr_t pgn, int *fpn, struct pcb_t *caller);

/* libmem.c references the DMA service of os.c: not used here */
int dma_submit(uint32_t pid, uint64_t s, uint64_t d, uint64_t l, dma_done_fn f, void *c)
{
  (void)pid; (void)s; (void)d; (void)l; (void)f; (void)c;
  return -1;
}

#define PG ((addr_t)PAGING64_PAGESZ)
#define UBASE PAGING64_USER_VMA0_START
#define PGN(a) ((addr_t)(a) >> PAGING64_ADDR_PT_SHIFT)

static struct memphy_struct ram, swp0, swp1;
static struct memphy_struct *swp_tbl[PAGING_MAX_MMSWP];
static struct krnl_t krnl;
static struct mm_struct kmm;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static int count_fp(struct framephy_struct *fp)
{
  int n = 0;
  for (; fp != NULL; fp = fp->fp_next) n++;
  return n;
}

static void free_dev(struct memphy_struct *mp)
{
  struct framephy_struct *lists[] = {mp->free_fp_list, mp->used_fp_list};
  for (int i = 0; i < 2; i++) {
    struct framephy_struct *f = lists[i];
    while (f) { struct framephy_struct *n = f->fp_next; free(f); f = n; }
  }
  free(mp->storage);
}

static void setup(int ram_frames, int swp0_frames, int swp1_frames)
{
  memset(&krnl, 0, sizeof(krnl));
  memset(&kmm, 0, sizeof(kmm));
  memset(swp_tbl, 0, sizeof(swp_tbl));
  assert(init_memphy(&ram, (addr_t)ram_frames * PG, 1) == 0);
  assert(init_memphy(&swp0, (addr_t)swp0_frames * PG, 1) == 0);
  assert(init_memphy(&swp1, (addr_t)swp1_frames * PG, 1) == 0);
  swp_tbl[0] = &swp0;
  swp_tbl[1] = &swp1;
  krnl.mram = &ram;
  krnl.mswp = swp_tbl;
  krnl.active_mswp = &swp0;
  krnl.active_mswp_id = 0;
  krnl.registry_lock = &reg_lock;
  assert(init_mm(&kmm, NULL) == 0);
  krnl.mm = &kmm;
}

static void teardown(void)
{
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

/* A process registered like the loader does (syscalls look the PCB up) */
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
  for (int j = 0; j < MAX_PROCESS_REGISTRY; j++)
    if (krnl.proc_registry[j] == NULL) { krnl.proc_registry[j] = p; break; }
}

static void end_proc(struct pcb_t *p)
{
  for (int j = 0; j < MAX_PROCESS_REGISTRY; j++)
    if (krnl.proc_registry[j] == p) krnl.proc_registry[j] = NULL;
  assert(free_pcb_memph(p) == 0);
}

static int swapped_pages(struct mm_struct *mm, addr_t from, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    if (pte_get_entry(mm, PGN(from + (addr_t)i * PG), NULL) & PAGING_PTE_SWAPPED_MASK)
      s++;
  return s;
}

static BYTE pattern(int r, int k) { return (BYTE)(r * 37 + k * 11 + 1); }

/* RAM of 2 frames, 4 one-page regions: data survives every swap */
static void test_small_ram_keeps_data(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  const addr_t offs[] = {0, 100, PG - 1};
  addr_t a;
  setup(2, 8, 0);
  new_proc(&p, &mm, 1);

  for (int r = 0; r < 4; r++)
    assert(__alloc(&p, 0, r, PG, &a) == 0);
  assert(swapped_pages(&mm, UBASE, 4) == 2);         /* 2 in RAM, 2 swapped */

  for (int r = 0; r < 4; r++)                        /* each write may swap */
    for (int k = 0; k < 3; k++)
      assert(__write(&p, 0, r, offs[k], pattern(r, k)) == 0);

  for (int pass = 0; pass < 2; pass++)               /* read twice, any order */
    for (int r = 3; r >= 0; r--)
      for (int k = 0; k < 3; k++) {
        BYTE v = 0;
        assert(__read(&p, 0, r, offs[k], &v) == 0);
        assert(v == pattern(r, k));
      }

  /* invariants: 4 pages = 2 resident + 2 swapped, no frame/slot leaked */
  assert(swapped_pages(&mm, UBASE, 4) == 2);
  assert(count_fp(ram.free_fp_list) == 0 && count_fp(swp0.used_fp_list) == 2);
  end_proc(&p);
  assert(count_fp(ram.used_fp_list) == 0 && count_fp(swp0.used_fp_list) == 0);
  teardown();
  printf("PASS test_small_ram_keeps_data\n");
}

/* Same virtual address in two processes, separate data */
static void test_two_processes_same_va(void)
{
  struct pcb_t p1, p2;
  struct mm_struct m1, m2;
  addr_t a1 = 0, a2 = 0;
  BYTE v = 0;
  setup(8, 4, 0);
  new_proc(&p1, &m1, 1);
  new_proc(&p2, &m2, 2);

  assert(__alloc(&p1, 0, 0, 100, &a1) == 0);
  assert(__alloc(&p2, 0, 0, 100, &a2) == 0);
  assert(a1 == a2);                                  /* same virtual address */
  assert(PAGING_FPN(pte_get_entry(&m1, PGN(a1), NULL)) !=
         PAGING_FPN(pte_get_entry(&m2, PGN(a2), NULL)));

  assert(__write(&p1, 0, 0, 10, 11) == 0);
  assert(__write(&p2, 0, 0, 10, 22) == 0);
  assert(__read(&p1, 0, 0, 10, &v) == 0 && v == 11);
  assert(__read(&p2, 0, 0, 10, &v) == 0 && v == 22);

  end_proc(&p1);
  assert(__read(&p2, 0, 0, 10, &v) == 0 && v == 22); /* p2 unaffected */
  end_proc(&p2);
  assert(count_fp(ram.used_fp_list) == 0);
  teardown();
  printf("PASS test_two_processes_same_va\n");
}

/* Bytes on both sides of page boundaries land in the right frames */
static void test_page_boundaries(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  addr_t a;
  const addr_t offs[] = {PG - 1, PG, 2 * PG - 1, 2 * PG, 3 * PG + 9};
  setup(8, 0, 0);
  new_proc(&p, &mm, 1);

  assert(__alloc(&p, 0, 0, 3 * PG + 10, &a) == 0);
  for (int k = 0; k < 5; k++)
    assert(__write(&p, 0, 0, offs[k], (BYTE)(0x50 + k)) == 0);
  for (int k = 0; k < 5; k++) {
    BYTE v = 0;
    assert(__read(&p, 0, 0, offs[k], &v) == 0 && v == (BYTE)(0x50 + k));
    /* and physically: frame of that page, offset inside the page */
    addr_t va = a + offs[k];
    addr_t fpn = PAGING_FPN(pte_get_entry(&mm, PGN(va), NULL));
    assert(MEMPHY_read(&ram, fpn * PG + (va & (PG - 1)), &v) == 0 &&
           v == (BYTE)(0x50 + k));
  }
  assert(__write(&p, 0, 0, 3 * PG + 10, 1) == -1);   /* past the region */
  end_proc(&p);
  teardown();
  printf("PASS test_page_boundaries\n");
}

/* pg_getpage contract and all-or-nothing swap-in */
static void test_getpage_contract(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  addr_t a;
  int fpn = -1;
  setup(1, 1, 0);
  new_proc(&p, &mm, 1);

  assert(pg_getpage(&mm, PGN(UBASE + 50 * PG), &fpn, &p) == -1);  /* unmapped */
  assert(pg_getpage(&kmm, PGN(PAGING64_KERNEL_VMA0_START), &fpn, &p) == -1);
  assert(pg_getpage(&mm, PGN(UBASE), NULL, &p) == -1);

  /* A then B: B's allocation evicts A into the only swap slot */
  assert(__alloc(&p, 0, 0, PG, &a) == 0);
  assert(__write(&p, 0, 0, 5, 77) == 0);
  assert(__alloc(&p, 0, 1, PG, &a) == 0);
  addr_t pte_a = pte_get_entry(&mm, PGN(UBASE), NULL);
  addr_t pte_b = pte_get_entry(&mm, PGN(UBASE + PG), NULL);
  assert(pte_a & PAGING_PTE_SWAPPED_MASK);
  assert(PAGING_PAGE_PRESENT(pte_b));

  /* swap-in of A needs a slot for B: none -> fails, nothing changes */
  BYTE v = 0;
  assert(__read(&p, 0, 0, 5, &v) == -1);
  assert(pte_get_entry(&mm, PGN(UBASE), NULL) == pte_a);
  assert(pte_get_entry(&mm, PGN(UBASE + PG), NULL) == pte_b);
  assert(mm.fifo_pgn != NULL && mm.fifo_pgn->pgn == PGN(UBASE + PG) &&
         mm.fifo_pgn->pg_next == NULL);
  assert(count_fp(swp0.free_fp_list) == 0 && count_fp(ram.free_fp_list) == 0);

  end_proc(&p);
  teardown();
  printf("PASS test_getpage_contract\n");
}

/* A page swapped to swap1 comes back from swap1 after the active device
 * changed, and its slot there is released */
static void test_swap_in_from_its_device(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  addr_t a;
  BYTE v = 0;
  setup(1, 2, 2);
  krnl.active_mswp = &swp1;
  krnl.active_mswp_id = 1;
  new_proc(&p, &mm, 1);

  assert(__alloc(&p, 0, 0, PG, &a) == 0);
  assert(__write(&p, 0, 0, 0, 42) == 0);
  assert(__alloc(&p, 0, 1, PG, &a) == 0);           /* A -> swap1 */
  assert(count_fp(swp1.used_fp_list) == 1);

  krnl.active_mswp = &swp0;                          /* switch active swap */
  krnl.active_mswp_id = 0;
  assert(__read(&p, 0, 0, 0, &v) == 0 && v == 42);   /* A back, B -> swap0 */
  assert(count_fp(swp1.used_fp_list) == 0);          /* A's slot released */
  assert(count_fp(swp0.used_fp_list) == 1);
  assert(GETVAL(pte_get_entry(&mm, PGN(UBASE + PG), NULL),
                PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT) == 0);

  end_proc(&p);
  assert(count_fp(swp0.used_fp_list) == 0 && count_fp(ram.used_fp_list) == 0);
  teardown();
  printf("PASS test_swap_in_from_its_device\n");
}

/* find_victim_page detaches the oldest evictable page */
static void test_find_victim_page(void)
{
  struct pcb_t p;
  struct mm_struct mm;
  addr_t a, vic = 0;
  setup(4, 0, 0);
  new_proc(&p, &mm, 1);

  assert(find_victim_page(&mm, &vic) == -1);         /* empty FIFO */
  assert(__alloc(&p, 0, 0, 2 * PG, &a) == 0);        /* pages UBASE, +PG */
  assert(find_victim_page(&mm, &vic) == 0 && vic == PGN(UBASE));
  assert(mm.fifo_pgn != NULL && mm.fifo_pgn->pgn == PGN(UBASE + PG) &&
         mm.fifo_pgn->pg_next == NULL);
  assert(find_victim_page(&mm, NULL) == -1);

  end_proc(&p);
  teardown();
  printf("PASS test_find_victim_page\n");
}

int main(void)
{
  test_small_ram_keeps_data();
  test_two_processes_same_va();
  test_page_boundaries();
  test_getpage_contract();
  test_swap_in_from_its_device();
  test_find_victim_page();
  printf("ALL SWAP TESTS PASSED\n");
  return 0;
}
