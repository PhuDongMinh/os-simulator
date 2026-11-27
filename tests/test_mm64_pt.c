/*
 * Unit tests for Module 1 (mm64.c):
 *   Stage 1 - five-level page-table tree and PTE API
 *   Stage 2 - init_mm
 * Run with tests/run_mm64_pt_tests.sh.
 */
#include "mm64-internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- allocation tracking / failure injection (linker --wrap) ---- */
void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void __real_free(void *ptr);
static atomic_long live_allocations;
/* Number of malloc/calloc calls that still succeed; -1 = never fail. */
static atomic_long fail_calloc_after = -1;

static int injected_failure(void)
{
   long left = atomic_load(&fail_calloc_after);
   if (left == 0) return 1;
   if (left > 0) atomic_fetch_sub(&fail_calloc_after, 1);
   return 0;
}

void *__wrap_malloc(size_t size)
{
   if (injected_failure()) return NULL;
   void *p = __real_malloc(size);
   if (p) atomic_fetch_add(&live_allocations, 1);
   return p;
}

void *__wrap_calloc(size_t n, size_t size)
{
   if (injected_failure()) return NULL;
   void *p = __real_calloc(n, size);
   if (p) atomic_fetch_add(&live_allocations, 1);
   return p;
}

void __wrap_free(void *ptr)
{
   if (ptr) atomic_fetch_sub(&live_allocations, 1);
   __real_free(ptr);
}

#define LIVE() atomic_load(&live_allocations)

/* ---- helpers ---- */
#define P PAGING_PTE_PRESENT_MASK
#define S PAGING_PTE_SWAPPED_MASK
#define D PAGING_PTE_DIRTY_MASK
#define PRESENT_PTE(fpn) ((addr_t)(P | (uint32_t)(fpn)))
#define SWAP_PTE(typ, off) ((addr_t)(S | (uint32_t)(typ) | ((uint32_t)(off) << 5)))

/* pgn of a virtual address */
#define PGN(va) ((addr_t)(va) >> 12)
#define USER_MAX_PGN (((addr_t)1 << 44) - 1)          /* VA 0x00ff_ffff_ffff_f000 */
#define KERNEL_MIN_PGN PGN(0xff00000000000000ULL)     /* first kernel-half page */
#define KERNEL_MAX_PGN PGN(0xfffffffffffff000ULL)

static void mm_new(struct mm_struct *mm)
{
   memset(mm, 0, sizeof(*mm));
   assert(mm64_pgd_create(mm) == 0);
}

struct node_counts { long p4d, pud, pmd, leaf, used; };

/* Count tree nodes and non-zero entries by walking the private layout. */
static struct node_counts count_nodes(const struct mm_struct *mm)
{
   struct node_counts c = {0};
   for (unsigned int a = 0; a < PAGING64_PT_ENTRIES; ++a) {
      struct pt_p4d *p4d = mm->pgd->child[a];
      if (!p4d) continue;
      ++c.p4d;
      for (unsigned int b = 0; b < PAGING64_PT_ENTRIES; ++b) {
         struct pt_pud *pud = p4d->child[b];
         if (!pud) continue;
         ++c.pud;
         for (unsigned int d = 0; d < PAGING64_PT_ENTRIES; ++d) {
            struct pt_pmd *pmd = pud->child[d];
            if (!pmd) continue;
            ++c.pmd;
            for (unsigned int e = 0; e < PAGING64_PT_ENTRIES; ++e) {
               struct pt_leaf *leaf = pmd->child[e];
               if (!leaf) continue;
               ++c.leaf;
               for (unsigned int f = 0; f < PAGING64_PT_ENTRIES; ++f)
                  if (leaf->entry[f]) ++c.used;
            }
         }
      }
   }
   return c;
}

static void expect_counts(const struct mm_struct *mm, long p4d, long pud,
                          long pmd, long leaf, long used)
{
   struct node_counts c = count_nodes(mm);
   assert(c.p4d == p4d && c.pud == pud && c.pmd == pmd);
   assert(c.leaf == leaf && c.used == used);
}

/* ---- tests ---- */

static void test_root_lifecycle(void)
{
   long base = LIVE();
   struct mm_struct mm;
   memset(&mm, 0, sizeof(mm));
   mm.p4d = mm.pud = mm.pmd = mm.pt = (addr_t *)&mm; /* junk placeholders */

   assert(mm64_pgd_create(NULL) == -1);
   assert(mm64_pgd_create(&mm) == 0);
   assert(mm.pgd != NULL && LIVE() == base + 1);
   assert(!mm.p4d && !mm.pud && !mm.pmd && !mm.pt);
   for (unsigned int i = 0; i < PAGING64_PT_ENTRIES; ++i) assert(mm.pgd->child[i] == NULL);
   assert(pthread_mutex_lock(&mm.pgd->lock) == 0);
   assert(pthread_mutex_unlock(&mm.pgd->lock) == 0);

   struct pt_pgd *root = mm.pgd;
   assert(mm64_pgd_create(&mm) == -1 && mm.pgd == root); /* no second root */

   mm64_pgd_destroy(&mm);
   assert(mm.pgd == NULL && LIVE() == base);
   mm64_pgd_destroy(&mm);  /* idempotent */
   mm64_pgd_destroy(NULL); /* NULL-safe */

   atomic_store(&fail_calloc_after, 0);
   assert(mm64_pgd_create(&mm) == -1 && mm.pgd == NULL);
   atomic_store(&fail_calloc_after, -1);
   assert(LIVE() == base);
}

static void test_init_pte(void)
{
   addr_t v = 0xdeadbeef;
   assert(init_pte(&v, 1, 0, 0, 0, 0, 0) == 0 && v == PRESENT_PTE(0)); /* fpn 0 valid */
   v = 0xffffffff;
   assert(init_pte(&v, 1, 8191, 0, 0, 0, 0) == 0 && v == PRESENT_PTE(8191)); /* stale bits cleared */
   assert(PAGING_PAGE_PRESENT(v) && PAGING_FPN(v) == 8191);
   /* 16-bit FPN: frames past the old 13-bit limit (32 MiB) are valid */
   assert(init_pte(&v, 1, 8192, 0, 0, 0, 0) == 0 && v == PRESENT_PTE(8192));
   assert(PAGING_FPN(v) == 8192);
   assert(init_pte(&v, 1, 65535, 1, 0, 0, 0) == 0 && v == (PRESENT_PTE(65535) | D));
   assert(PAGING_FPN(v) == 65535 && (v & PAGING_PTE_USRNUM_MASK) == 0); /* no bleed into flags */
   assert(PAGING64_MAX_RAM_FRAMES == 65536 && PAGING64_MAX_RAMSZ == 256ULL << 20);

   v = 123;
   assert(init_pte(&v, 1, 65536, 0, 0, 0, 0) == -1 && v == 123); /* FPN overflow, unchanged */
   assert(init_pte(&v, 1, 5, 1, 0, 0, 0) == 0 && v == (PRESENT_PTE(5) | D));

   assert(init_pte(&v, 1, 999, 0, 1, 3, 0x1fffff) == 0); /* swp wins over pre */
   assert(v == SWAP_PTE(3, 0x1fffff) && !PAGING_PAGE_PRESENT(v));
   assert(PAGING_PTE_SWPTYP(v) == 3 && PAGING_SWP(v) == 0x1fffff);
   assert(init_pte(&v, 0, 0, 1, 1, 0, 7) == 0 && v == (SWAP_PTE(0, 7) | D));

   v = 55;
   assert(init_pte(&v, 0, 0, 0, 1, 32, 0) == -1 && v == 55);
   assert(init_pte(&v, 0, 0, 0, 1, -1, 0) == -1 && v == 55);
   assert(init_pte(&v, 0, 0, 0, 1, 0, 0x200000) == -1 && v == 55);

   assert(init_pte(&v, 0, 7, 1, 0, 0, 0) == 0 && v == 0); /* empty, dirty ignored */
   assert(init_pte(NULL, 1, 1, 0, 0, 0, 0) == -1);
}

static void test_basic_ops(void)
{
   long before = LIVE();
   struct mm_struct mm;
   mm_new(&mm);
   long base = LIVE();
   struct pcb_t pcb;
   memset(&pcb, 0, sizeof(pcb));
   pcb.mm = &mm;

   /* lookups and clears never allocate */
   assert(pte_get_entry(&mm, 7, NULL) == 0 && LIVE() == base);
   assert(pte_clear_entry(&mm, 7, NULL) == 0 && LIVE() == base);

   assert(pte_set_fpn(&mm, 7, 42, NULL) == 0);
   assert(pte_get_entry(&mm, 7, NULL) == PRESENT_PTE(42));
   assert(LIVE() == base + 4); /* p4d, pud, pmd, leaf */
   expect_counts(&mm, 1, 1, 1, 1, 1);

   assert(pte_set_fpn(&mm, 7, 65536, NULL) == -1);       /* > 16-bit FPN */
   assert(pte_set_entry(&mm, 7, 1ULL << 32, NULL) == -1); /* > 32-bit PTE */
   assert(pte_get_entry(&mm, 7, NULL) == PRESENT_PTE(42));

   assert(pte_set_entry(&mm, 7, 0x12345678, NULL) == 0);
   assert(pte_get_entry(&mm, 7, NULL) == 0x12345678);

   assert(pte_set_swap(&pcb, 7, 2, 1000) == 0);
   assert(pte_get_entry(&mm, 7, NULL) == SWAP_PTE(2, 1000));
   assert(pte_set_swap(NULL, 7, 2, 1000) == -1);
   assert(pte_set_swap(&pcb, 7, 99, 1000) == -1);
   assert(pte_get_entry(&mm, 7, NULL) == SWAP_PTE(2, 1000));

   assert(pte_set_fpn(&mm, 7, 0, NULL) == 0); /* swap-in to frame 0 */
   assert(pte_get_entry(&mm, 7, NULL) == PRESENT_PTE(0));

   assert(pte_clear_entry(&mm, 7, NULL) == 0);
   assert(pte_get_entry(&mm, 7, NULL) == 0);
   assert(LIVE() == base + 4); /* tables are kept until destroy */

   /* invalid address spaces */
   struct mm_struct no_root;
   memset(&no_root, 0, sizeof(no_root));
   assert(pte_get_entry(NULL, 7, NULL) == 0);
   assert(pte_set_fpn(NULL, 7, 1, NULL) == -1);
   assert(pte_clear_entry(NULL, 7, NULL) == -1);
   assert(pte_get_entry(&no_root, 7, NULL) == 0);
   assert(pte_set_entry(&no_root, 7, 1, NULL) == -1);
   assert(pte_clear_entry(&no_root, 7, NULL) == -1);

   /* non-canonical page numbers (VA bits 56..63 not all equal) */
   addr_t bad[] = {(addr_t)1 << 44, PGN(0x7f00000000000000ULL), PGN(0xfe00000000000000ULL),
                   (addr_t)1 << 52, ~(addr_t)0};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      assert(pte_set_fpn(&mm, bad[i], 1, NULL) == -1);
      assert(pte_get_entry(&mm, bad[i], NULL) == 0);
      assert(pte_clear_entry(&mm, bad[i], NULL) == -1);
   }
   assert(LIVE() == base + 4);

   mm64_pgd_destroy(&mm);
   assert(LIVE() == before);
}

static void test_spec_example_indices(void)
{
   addr_t pgd, p4d, pud, pmd, pt;
   get_pd_from_address(0x39ffe1d9c9000ULL, &pgd, &p4d, &pud, &pmd, &pt);
   assert(pgd == 0x03 && p4d == 0x13f && pud == 0x1f8 && pmd == 0x0ec && pt == 0x1c9);
   get_pd_from_pagenum(PGN(0x39ffe1d9c9000ULL), &pgd, &p4d, &pud, &pmd, &pt);
   assert(pgd == 0x03 && p4d == 0x13f && pud == 0x1f8 && pmd == 0x0ec && pt == 0x1c9);

   get_pd_from_pagenum(KERNEL_MIN_PGN, &pgd, &p4d, &pud, &pmd, &pt);
   assert(pgd == 256 && p4d == 0 && pud == 0 && pmd == 0 && pt == 0);
   get_pd_from_pagenum(KERNEL_MAX_PGN, &pgd, &p4d, &pud, &pmd, &pt);
   assert(pgd == 511 && p4d == 511 && pud == 511 && pmd == 511 && pt == 511);
   get_pd_from_pagenum(USER_MAX_PGN, &pgd, &p4d, &pud, &pmd, &pt);
   assert(pgd == 255 && p4d == 511 && pud == 511 && pmd == 511 && pt == 511);
}

struct boundary {
   const char *name;
   addr_t last;   /* last page before the boundary */
   addr_t first;  /* first page after it */
   long p4d, pud, pmd, leaf; /* expected node counts with both mapped */
};

static void test_boundaries(void)
{
   const struct boundary cases[] = {
      /* PT index 511 -> 512: next leaf, same PMD */
      {"2 MiB (leaf 511|512)", PGN(0x1ff000), PGN(0x200000), 1, 1, 1, 2},
      /* PMD index 511 -> next PMD table, same PUD */
      {"1 GiB", PGN(0x3ffff000), PGN(0x40000000), 1, 1, 2, 2},
      {"512 GiB", PGN(0x7ffffff000ULL), PGN(0x8000000000ULL), 1, 2, 2, 2},
      {"256 TiB", PGN(0xfffffffff000ULL), PGN(0x1000000000000ULL), 2, 2, 2, 2},
      {"user top | kernel bottom", USER_MAX_PGN, KERNEL_MIN_PGN, 2, 2, 2, 2},
   };

   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      const struct boundary *c = &cases[i];
      long before = LIVE();
      struct mm_struct mm;
      mm_new(&mm);

      assert(pte_set_fpn(&mm, c->last, 100, NULL) == 0);
      assert(pte_set_fpn(&mm, c->first, 200, NULL) == 0);
      assert(pte_get_entry(&mm, c->last, NULL) == PRESENT_PTE(100));
      assert(pte_get_entry(&mm, c->first, NULL) == PRESENT_PTE(200));
      /* neighbours on both sides stay unmapped */
      assert(pte_get_entry(&mm, c->last - 1, NULL) == 0);
      if (c->first != KERNEL_MAX_PGN)
         assert(pte_get_entry(&mm, c->first + 1, NULL) == 0);
      expect_counts(&mm, c->p4d, c->pud, c->pmd, c->leaf, 2);
      assert(LIVE() == before + 1 + c->p4d + c->pud + c->pmd + c->leaf);

      /* the two pages are independent */
      assert(pte_clear_entry(&mm, c->last, NULL) == 0);
      assert(pte_get_entry(&mm, c->last, NULL) == 0);
      assert(pte_get_entry(&mm, c->first, NULL) == PRESENT_PTE(200));

      mm64_pgd_destroy(&mm);
      assert(LIVE() == before);
      printf("  boundary %-26s ok\n", c->name);
   }

   /* the very last kernel page */
   struct mm_struct mm;
   mm_new(&mm);
   assert(pte_set_fpn(&mm, KERNEL_MAX_PGN, 65535, NULL) == 0);
   assert(pte_get_entry(&mm, KERNEL_MAX_PGN, NULL) == PRESENT_PTE(65535));
   assert(pte_get_entry(&mm, KERNEL_MAX_PGN - 1, NULL) == 0);
   mm64_pgd_destroy(&mm);
}

static void test_isolation(void)
{
   long before = LIVE();
   struct mm_struct a, b;
   mm_new(&a);
   mm_new(&b);
   assert(a.pgd != b.pgd);
   struct pcb_t pa;
   memset(&pa, 0, sizeof(pa));
   pa.mm = &a;
   const addr_t pgn = PGN(0x400000); /* same virtual page in both */

   assert(pte_set_fpn(&a, pgn, 1, NULL) == 0);
   assert(pte_get_entry(&b, pgn, NULL) == 0); /* not visible in b */
   assert(pte_set_fpn(&b, pgn, 2, NULL) == 0);
   assert(pte_get_entry(&a, pgn, NULL) == PRESENT_PTE(1));
   assert(pte_get_entry(&b, pgn, NULL) == PRESENT_PTE(2));

   assert(pte_set_swap(&pa, pgn, 1, 77) == 0); /* acts on pa.mm only */
   assert(pte_get_entry(&a, pgn, NULL) == SWAP_PTE(1, 77));
   assert(pte_get_entry(&b, pgn, NULL) == PRESENT_PTE(2));

   assert(pte_clear_entry(&b, pgn, NULL) == 0);
   assert(pte_get_entry(&a, pgn, NULL) == SWAP_PTE(1, 77));

   mm64_pgd_destroy(&b);
   assert(pte_get_entry(&a, pgn, NULL) == SWAP_PTE(1, 77)); /* survives */
   assert(pte_get_entry(&b, pgn, NULL) == 0);
   mm64_pgd_destroy(&a);
   assert(LIVE() == before);
}

static void test_alloc_failure(void)
{
   long before = LIVE();
   struct mm_struct mm;
   mm_new(&mm);
   long base = LIVE();
   const addr_t pgn = PGN(0x39ffe1d9c9000ULL);

   /* Fail the p4d, pud, pmd, then leaf allocation of the same path.
    * Attempt k finds the k nodes kept by earlier attempts, may create one
    * more (except the first attempt) and fails on the next level. */
   for (long k = 0; k < 4; ++k) {
      atomic_store(&fail_calloc_after, k == 0 ? 0 : 1);
      assert(pte_set_fpn(&mm, pgn, 9, NULL) == -1);
      atomic_store(&fail_calloc_after, -1);
      assert(pte_get_entry(&mm, pgn, NULL) == 0);
      assert(LIVE() == base + k); /* partial path kept, nothing leaked */
   }
   assert(pte_set_fpn(&mm, pgn, 9, NULL) == 0);
   assert(pte_get_entry(&mm, pgn, NULL) == PRESENT_PTE(9));
   assert(LIVE() == base + 4);

   mm64_pgd_destroy(&mm);
   assert(LIVE() == before);
}

static void test_destroy_many(void)
{
   long before = LIVE();
   struct mm_struct mm;
   mm_new(&mm);
   enum { N = 3000 };
   static addr_t pgns[N];
   uint64_t x = 0x9e3779b97f4a7c15ULL;
   for (int i = 0; i < N; ++i) {
      x ^= x << 13; x ^= x >> 7; x ^= x << 17;
      addr_t p = x & USER_MAX_PGN;
      if (i % 3 == 0) p |= KERNEL_MIN_PGN; /* some kernel-half pages */
      if (i % 5 == 0 && i > 0) p = pgns[i - 1] + 1; /* runs of neighbours */
      pgns[i] = p;
      assert(pte_set_fpn(&mm, p, (addr_t)(i % 8192), NULL) == 0);
   }
   for (int i = 0; i < N; ++i) {
      /* a later duplicate may have overwritten an earlier pgn */
      int last = i;
      for (int j = i + 1; j < N; ++j) if (pgns[j] == pgns[i]) last = j;
      assert(pte_get_entry(&mm, pgns[i], NULL) == PRESENT_PTE(last % 8192));
   }
   mm64_pgd_destroy(&mm);
   assert(LIVE() == before);
}

enum { THREADS = 8, PER_THREAD = 1500 };

static addr_t concurrent_pgn(int t, int i)
{
   /* four shared subtrees, threads interleave inside the same leaves */
   return ((addr_t)(i % 4) << 27) | (addr_t)(i * THREADS + t);
}

struct worker { struct mm_struct *mm; pthread_barrier_t *barrier; int id; };

static void *concurrent_worker(void *arg)
{
   struct worker *w = arg;
   pthread_barrier_wait(w->barrier);
   for (int i = 0; i < PER_THREAD; ++i) {
      addr_t p = concurrent_pgn(w->id, i);
      assert(pte_set_fpn(w->mm, p, (addr_t)((i * THREADS + w->id) % 8192), NULL) == 0);
      (void)pte_get_entry(w->mm, concurrent_pgn((w->id + 1) % THREADS, i), NULL);
   }
   return NULL;
}

static void test_concurrent(void)
{
   long before = LIVE();
   struct mm_struct shared, mirror;
   mm_new(&shared);
   mm_new(&mirror);
   pthread_barrier_t barrier;
   assert(pthread_barrier_init(&barrier, NULL, THREADS) == 0);
   pthread_t th[THREADS];
   struct worker w[THREADS];
   for (int t = 0; t < THREADS; ++t) {
      w[t] = (struct worker){&shared, &barrier, t};
      assert(pthread_create(&th[t], NULL, concurrent_worker, &w[t]) == 0);
   }
   for (int t = 0; t < THREADS; ++t) assert(pthread_join(th[t], NULL) == 0);
   assert(pthread_barrier_destroy(&barrier) == 0);

   /* Same mappings built sequentially: racing creators must not have
    * produced duplicate (leaked) nodes or lost entries. */
   for (int t = 0; t < THREADS; ++t)
      for (int i = 0; i < PER_THREAD; ++i) {
         addr_t p = concurrent_pgn(t, i);
         addr_t fpn = (addr_t)((i * THREADS + t) % 8192);
         assert(pte_set_fpn(&mirror, p, fpn, NULL) == 0);
         assert(pte_get_entry(&shared, p, NULL) == PRESENT_PTE(fpn));
      }
   struct node_counts cs = count_nodes(&shared), cm = count_nodes(&mirror);
   assert(cs.p4d == cm.p4d && cs.pud == cm.pud && cs.pmd == cm.pmd);
   assert(cs.leaf == cm.leaf && cs.used == cm.used && cs.used == THREADS * PER_THREAD);

   mm64_pgd_destroy(&shared);
   mm64_pgd_destroy(&mirror);
   assert(LIVE() == before);
}

/* ===================== Stage 2: init_mm ===================== */

/* Test-only teardown of an mm built by init_mm (production cleanup is
 * free_pcb_memph, Stage 5). */
static void mm_teardown(struct mm_struct *mm)
{
   struct vm_area_struct *vma = mm->mmap;
   while (vma) {
      struct vm_area_struct *next_vma = vma->vm_next;
      struct vm_rg_struct *rg = vma->vm_freerg_list;
      while (rg) {
         struct vm_rg_struct *next_rg = rg->rg_next;
         free(rg);
         rg = next_rg;
      }
      free(vma);
      vma = next_vma;
   }
   mm->mmap = NULL;
   mm64_pgd_destroy(mm);
}

static void check_fresh_mm(struct mm_struct *mm, addr_t base)
{
   assert(mm->pgd != NULL);
   assert(!mm->p4d && !mm->pud && !mm->pmd && !mm->pt);
   for (unsigned int i = 0; i < PAGING64_PT_ENTRIES; ++i) assert(mm->pgd->child[i] == NULL);

   struct vm_area_struct *vma0 = mm->mmap;
   assert(vma0 != NULL && vma0->vm_next == NULL && vma0->vm_mm == mm);
   assert(vma0->vm_id == 0);
   assert(vma0->vm_start == base && vma0->vm_end == base && vma0->sbrk == base);
   struct vm_rg_struct *rg = vma0->vm_freerg_list;
   assert(rg != NULL && rg->rg_next == NULL && rg->vmaid == 0);
   assert(rg->rg_start == base && rg->rg_end == base);

   for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; ++i)
      assert(mm->symrgtbl[i].rg_start == 0 && mm->symrgtbl[i].rg_end == 0 &&
             mm->symrgtbl[i].rg_next == NULL);
   assert(mm->fifo_pgn == NULL && mm->kcpooltbl == NULL);
}

static void test_init_mm_user(void)
{
   long before = LIVE();
   struct pcb_t owner;
   memset(&owner, 0, sizeof(owner));
   struct mm_struct mm;
   memset(&mm, 0xa5, sizeof(mm)); /* stale content must be discarded */
   mm.pgd = NULL;

   assert(init_mm(&mm, &owner) == 0);
   check_fresh_mm(&mm, PAGING64_USER_VMA0_START);
   assert(mm.mmap->vm_limit == PAGING64_USER_END);
   assert(LIVE() == before + 3); /* root, vma0, first free region */

   /* the page table is usable immediately */
   assert(pte_set_fpn(&mm, PGN(PAGING64_USER_VMA0_START), 5, NULL) == 0);
   assert(pte_get_entry(&mm, PGN(PAGING64_USER_VMA0_START), NULL) == PRESENT_PTE(5));

   /* a second init on a live mm is refused and changes nothing */
   struct pt_pgd *root = mm.pgd;
   struct vm_area_struct *vma0 = mm.mmap;
   assert(init_mm(&mm, &owner) == -1 && mm.pgd == root && mm.mmap == vma0);
   assert(init_mm(NULL, &owner) == -1);

   mm_teardown(&mm);
   assert(LIVE() == before);
}

static void test_init_mm_kernel(void)
{
   long before = LIVE();
   struct mm_struct kmm;
   memset(&kmm, 0, sizeof(kmm));
   assert(init_mm(&kmm, NULL) == 0); /* os.c: init_mm(os.mm, NULL) */
   check_fresh_mm(&kmm, PAGING64_KERNEL_VMA0_START);
   assert(kmm.mmap->vm_limit == PAGING64_KERNEL_END);

   /* the single 57-bit layout (mm64.h) and its common.h aliases */
   assert(PAGING64_USER_START == 0x0000000000001000ULL);
   assert(PAGING64_USER_END == 0x00FFFFFFFFFFFFFFULL);
   assert(PAGING64_KERNEL_START == 0xFF00000000000000ULL);
   assert(PAGING64_KERNEL_END == 0xFFFFFFFFFFFFFFFFULL);
   assert(USER_SPACE_START == PAGING64_USER_START &&
          USER_SPACE_END == PAGING64_USER_END &&
          KERNEL_SPACE_START == PAGING64_KERNEL_START &&
          KERNEL_SPACE_END == PAGING64_KERNEL_END);

   addr_t kpgn = PGN(PAGING64_KERNEL_VMA0_START);
   addr_t pgd, p4d, pud, pmd, pt;
   get_pd_from_pagenum(kpgn, &pgd, &p4d, &pud, &pmd, &pt);
   assert(pgd == 256); /* kernel half */
   assert(pte_set_fpn(&kmm, kpgn, 65535, NULL) == 0);
   assert(pte_get_entry(&kmm, kpgn, NULL) == PRESENT_PTE(65535));

   mm_teardown(&kmm);
   assert(LIVE() == before);
}

static void test_init_mm_failure(void)
{
   long before = LIVE();
   struct pcb_t owner;
   memset(&owner, 0, sizeof(owner));
   /* allocation order inside init_mm: root, vma0, first region */
   for (long k = 0; k < 3; ++k) {
      struct mm_struct mm;
      memset(&mm, 0, sizeof(mm));
      atomic_store(&fail_calloc_after, k);
      assert(init_mm(&mm, &owner) == -1);
      atomic_store(&fail_calloc_after, -1);
      assert(mm.pgd == NULL && LIVE() == before); /* nothing leaked */
   }
}

static void test_init_mm_two_processes(void)
{
   long before = LIVE();
   struct pcb_t pa, pb;
   memset(&pa, 0, sizeof(pa));
   memset(&pb, 0, sizeof(pb));
   struct mm_struct a, b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   assert(init_mm(&a, &pa) == 0 && init_mm(&b, &pb) == 0);
   assert(a.pgd != b.pgd && a.mmap != b.mmap);
   /* same layout, same virtual page, different frames */
   addr_t pgn = PGN(PAGING64_USER_VMA0_START);
   assert(pte_set_fpn(&a, pgn, 10, NULL) == 0);
   assert(pte_set_fpn(&b, pgn, 20, NULL) == 0);
   assert(pte_get_entry(&a, pgn, NULL) == PRESENT_PTE(10));
   assert(pte_get_entry(&b, pgn, NULL) == PRESENT_PTE(20));
   mm_teardown(&a);
   mm_teardown(&b);
   assert(LIVE() == before);
}

int main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   test_root_lifecycle();     puts("root lifecycle            ok");
   test_init_pte();           puts("init_pte encoding         ok");
   test_basic_ops();          puts("get/set/clear/swap        ok");
   test_spec_example_indices(); puts("index split (spec)        ok");
   test_boundaries();         puts("table boundaries          ok");
   test_isolation();          puts("two mm, same pgn          ok");
   test_alloc_failure();      puts("allocation failure        ok");
   test_destroy_many();       puts("destroy frees all         ok");
   test_concurrent();         puts("concurrent shared tree    ok");
   test_init_mm_user();       puts("init_mm user              ok");
   test_init_mm_kernel();     puts("init_mm kernel            ok");
   test_init_mm_failure();    puts("init_mm alloc failure     ok");
   test_init_mm_two_processes(); puts("init_mm two processes     ok");
   puts("ALL MM64 PAGE-TABLE TESTS PASSED");
   return 0;
}
