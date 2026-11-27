/* Kernel memory (__kmalloc / __kfree) on top of the MM64 page table and the
 * free/used-list MEMPHY. Run with tests/run_kmem_tests.sh. */
#include "common.h"
#include "mm.h"
#include "mm64.h"
#include "libmem.h"
#include "dma.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libmem.c references the DMA service of os.c: not used here */
int dma_submit(uint32_t pid, uint64_t s, uint64_t d, uint64_t l, dma_done_fn f, void *c)
{
  (void)pid; (void)s; (void)d; (void)l; (void)f; (void)c;
  return -1;
}

#define RAM_FRAMES 8

static struct memphy_struct ram;
static struct krnl_t krnl;

static int count_list(struct framephy_struct *fp)
{
  int n = 0;
  for (; fp != NULL; fp = fp->fp_next)
    n++;
  return n;
}

static int free_frames(void) { return count_list(ram.free_fp_list); }
static int used_frames(void) { return count_list(ram.used_fp_list); }

static void setup(void)
{
  memset(&krnl, 0, sizeof(krnl));
  assert(init_memphy(&ram, RAM_FRAMES * OS_PAGESZ, 1) == 0);
  krnl.mram = &ram;
  krnl.mm = calloc(1, sizeof(struct mm_struct));
  assert(krnl.mm != NULL && init_mm(krnl.mm, NULL) == 0);
  krnl.krnl_pgd = krnl.mm->pgd;
}

static void new_proc(struct pcb_t *p, uint32_t pid)
{
  memset(p, 0, sizeof(*p));
  p->pid = pid;
  p->krnl = &krnl;
  for (int k = 0; k < PAGING_MAX_SYMTBL_SZ; k++)
    p->kmap_slot[k] = -1;
}

/* Empty kernel free list at boot: kmalloc must grow the vma (was: always -1) */
static void test_first_alloc_grows_vma(void)
{
  struct pcb_t p;
  addr_t a = 0;
  BYTE v = 0;

  new_proc(&p, 1);
  assert(__kmalloc(&p, 0, 0, 100, &a) == 0);
  assert(a >= KERNEL_SPACE_START && a % OS_PAGESZ == 0);
  assert(free_frames() == RAM_FRAMES - 1 && used_frames() == 1);

  assert(__write_kernel_mem(&p, 0, 0, 99, 0x5A) == 0);
  assert(__read_kernel_mem(&p, 0, 0, 99, &v) == 0 && v == 0x5A);
  assert(__write_kernel_mem(&p, 0, 0, 100, 1) == -1);   /* out of region */

  assert(__kmalloc(&p, 0, 0, 10, &a) == -1);            /* rgid busy */

  assert(__kfree(&p, 0, 0) == 0);
  assert(free_frames() == RAM_FRAMES && used_frames() == 0);
  assert(__read_kernel_mem(&p, 0, 0, 0, &v) == -1);     /* freed */
  assert(__kfree(&p, 0, 0) == -1);                      /* double free */
  printf("PASS test_first_alloc_grows_vma\n");
}

/* Multi-page regions get contiguous frames; freed space is reused */
static void test_contiguous_and_reuse(void)
{
  struct pcb_t p;
  addr_t a = 0, b = 0, c = 0;

  new_proc(&p, 2);
  assert(__kmalloc(&p, 0, 1, 3 * OS_PAGESZ, &a) == 0);
  assert(__kmalloc(&p, 0, 2, OS_PAGESZ + 1, &b) == 0);
  assert(b >= a + 3 * OS_PAGESZ);                      /* no overlap */
  assert(used_frames() == 5);

  addr_t f0 = PAGING_FPN(pte_get_entry(krnl.mm, OS_VADDR_PGN(a), NULL));
  for (int i = 1; i < 3; i++)
    assert(PAGING_FPN(pte_get_entry(krnl.mm, OS_VADDR_PGN(a + i * OS_PAGESZ), NULL))
           == f0 + i);

  assert(__kfree(&p, 0, 1) == 0);
  assert(pte_get_entry(krnl.mm, OS_VADDR_PGN(a), NULL) == 0);  /* PTE cleared */
  assert(__kmalloc(&p, 0, 3, 2 * OS_PAGESZ, &c) == 0);
  assert(c == a);                                     /* first fit reuse */

  assert(__kfree(&p, 0, 2) == 0 && __kfree(&p, 0, 3) == 0);
  assert(free_frames() == RAM_FRAMES && used_frames() == 0);
  printf("PASS test_contiguous_and_reuse\n");
}

/* Not enough RAM: nothing changes (no frame, no slot, no vma growth) */
static void test_out_of_memory_changes_nothing(void)
{
  struct pcb_t p;
  addr_t a = 0, b = 0;

  new_proc(&p, 3);
  struct vm_area_struct *vma = krnl.mm->mmap;
  addr_t sbrk_before = vma->sbrk;

  assert(__kmalloc(&p, 0, 0, (RAM_FRAMES + 1) * OS_PAGESZ, &a) == -1);
  assert(vma->sbrk == sbrk_before && p.kmap_slot[0] == -1);
  assert(free_frames() == RAM_FRAMES && used_frames() == 0);

  assert(__kmalloc(&p, 0, 0, RAM_FRAMES * OS_PAGESZ, &b) == 0);  /* exact fit */
  assert(free_frames() == 0);
  assert(__kfree(&p, 0, 0) == 0);
  assert(free_frames() == RAM_FRAMES);
  printf("PASS test_out_of_memory_changes_nothing\n");
}

/* Two processes share the kernel symbol table without clashing */
static void test_two_processes(void)
{
  struct pcb_t p, q;
  addr_t a = 0, b = 0;
  BYTE v = 0;

  new_proc(&p, 4);
  new_proc(&q, 5);
  assert(__kmalloc(&p, 0, 0, 16, &a) == 0);
  assert(__kmalloc(&q, 0, 0, 16, &b) == 0);           /* same rgid, other pcb */
  assert(a != b && p.kmap_slot[0] != q.kmap_slot[0]);
  assert(__write_kernel_mem(&p, 0, 0, 0, 1) == 0);
  assert(__write_kernel_mem(&q, 0, 0, 0, 2) == 0);
  assert(__read_kernel_mem(&p, 0, 0, 0, &v) == 0 && v == 1);
  assert(__read_kernel_mem(&q, 0, 0, 0, &v) == 0 && v == 2);
  assert(__kfree(&p, 0, 0) == 0 && __kfree(&q, 0, 0) == 0);
  assert(free_frames() == RAM_FRAMES);
  printf("PASS test_two_processes\n");
}

int main(void)
{
  setup();
  test_first_alloc_grows_vma();
  test_contiguous_and_reuse();
  test_out_of_memory_changes_nothing();
  test_two_processes();
  printf("ALL KMEM TESTS PASSED\n");
  return 0;
}
