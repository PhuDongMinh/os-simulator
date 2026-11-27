/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#if defined(MM64)

#include "mm64-internal.h"
#include <pthread.h>

/*
 * ===================================================================
 *  Five-level page-table tree (Stage 1)
 *
 *  mm->pgd -> pt_pgd -> pt_p4d -> pt_pud -> pt_pmd -> pt_leaf -> mm_pte_t
 *
 *  - Nodes live on the host heap and are created lazily (demand
 *    allocation): a NULL child means "nothing mapped below here".
 *  - Each tree is protected by the mutex in its root.  Every public
 *    pte_* call takes that lock for the duration of one walk, so it is
 *    safe to call while holding mmvm_lock/kmm_lock (lock order:
 *    mmvm_lock/kmm_lock -> pgd->lock -> memphy_lock).  pte_* never calls
 *    MEMPHY.
 *  - pgn = vaddr >> 12 of a canonical 57-bit address: VA bits 56..63
 *    are all 0 (user half, PGD index 0..255) or all 1 (kernel half,
 *    PGD index 256..511).  In pgn terms, pgn >> 44 is 0 or 0xFF.
 *    Anything else is rejected.
 * ===================================================================
 */

/* pgn bit holding VA bit 56 (the canonical sign bit) and the value of
 * pgn >> PT_PGN_SIGN_BIT for a kernel-half address (VA bits 56..63 = 1). */
#define PT_PGN_SIGN_BIT (PAGING64_CPU_BUS_WIDTH - 1 - PAGING64_ADDR_PT_SHIFT) /* 44 */
#define PT_PGN_HIGH_KERNEL   ((((addr_t)1) << (MM64_BITS_PER_LONG - PAGING64_CPU_BUS_WIDTH + 1)) - 1) /* 0xFF */

/* Largest values each PTE field can hold (derived from mm.h masks). */
#define PTE_FPN_MAX    ((addr_t)(PAGING_PTE_FPN_MASK >> PAGING_PTE_FPN_LOBIT))
#define PTE_SWPTYP_MAX ((addr_t)(PAGING_PTE_SWPTYP_MASK >> PAGING_PTE_SWPTYP_LOBIT))
#define PTE_SWPOFF_MAX ((addr_t)(PAGING_PTE_SWPOFF_MASK >> PAGING_PTE_SWPOFF_LOBIT))

static int pgn_is_canonical(addr_t pgn)
{
  addr_t high = pgn >> PT_PGN_SIGN_BIT;
  return high == 0 || high == PT_PGN_HIGH_KERNEL;
}

/* Root of @mm if @pgn may be looked up in it, NULL otherwise. */
static struct pt_pgd *pt_root_for(struct mm_struct *mm, addr_t pgn)
{
  if (mm == NULL || mm->pgd == NULL || !pgn_is_canonical(pgn))
    return NULL;
  return mm->pgd;
}

/* Descend one level: make *slot point to an existing or (create) a new,
 * zeroed child.  Leaves the walker with NULL when the child is missing
 * and must not be created, or when allocation fails. */
#define PT_DESCEND(slot, create)                          \
  do {                                                    \
    if (*(slot) == NULL) {                                \
      if (!(create))                                      \
        return NULL;                                      \
      *(slot) = calloc(1, sizeof(**(slot)));              \
      if (*(slot) == NULL)                                \
        return NULL;                                      \
    }                                                     \
  } while (0)

/*
 * pt_walk_locked - find the leaf entry of @pgn
 * @root  : tree root, its lock must be held by the caller
 * @pgn   : canonical page number
 * @create: 0 = lookup only (never allocates),
 *          1 = allocate missing intermediate nodes and the leaf
 *
 * Return a pointer to the 32-bit PTE inside the leaf, or NULL when the
 * path does not exist (lookup) or a node allocation failed (create).
 * After a failed create, nodes allocated earlier on the path are kept:
 * they are empty, harmless and released by mm64_pgd_destroy().
 */
static mm_pte_t *pt_walk_locked(struct pt_pgd *root, addr_t pgn, int create)
{
  addr_t pgd, p4d, pud, pmd, pt;
  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);

  struct pt_p4d **p4d_slot = &root->child[pgd];
  PT_DESCEND(p4d_slot, create);
  struct pt_pud **pud_slot = &(*p4d_slot)->child[p4d];
  PT_DESCEND(pud_slot, create);
  struct pt_pmd **pmd_slot = &(*pud_slot)->child[pud];
  PT_DESCEND(pmd_slot, create);
  struct pt_leaf **leaf_slot = &(*pmd_slot)->child[pmd];
  PT_DESCEND(leaf_slot, create);

  return &(*leaf_slot)->entry[pt];
}

#undef PT_DESCEND

/*
 * mm64_pgd_create - create the (empty) root of @mm's page table
 * Return 0 on success, -1 if @mm is NULL, already has a root, or on
 * allocation/mutex failure (then @mm is left unchanged).
 */
int mm64_pgd_create(struct mm_struct *mm)
{
  if (mm == NULL || mm->pgd != NULL)
    return -1;

  struct pt_pgd *root = calloc(1, sizeof(*root));
  if (root == NULL)
    return -1;
  if (pthread_mutex_init(&root->lock, NULL) != 0) {
    free(root);
    return -1;
  }

  mm->pgd = root;
  /* Legacy placeholders are never roots or caches in MM64. */
  mm->p4d = NULL;
  mm->pud = NULL;
  mm->pmd = NULL;
  mm->pt = NULL;
  return 0;
}

/*
 * mm64_pgd_destroy - free every node of @mm's page table and its root
 *
 * Only the tree is released: frames and swap slots referenced by the
 * PTEs must have been returned by the caller beforehand (free_pcb_memph).
 * No other thread may use the tree during or after this call.
 * Safe on NULL @mm or on an mm without a root.
 */
void mm64_pgd_destroy(struct mm_struct *mm)
{
  if (mm == NULL || mm->pgd == NULL)
    return;

  struct pt_pgd *root = mm->pgd;
  for (unsigned int i5 = 0; i5 < PAGING64_PT_ENTRIES; ++i5) {
    struct pt_p4d *p4d = root->child[i5];
    if (p4d == NULL)
      continue;
    for (unsigned int i4 = 0; i4 < PAGING64_PT_ENTRIES; ++i4) {
      struct pt_pud *pud = p4d->child[i4];
      if (pud == NULL)
        continue;
      for (unsigned int i3 = 0; i3 < PAGING64_PT_ENTRIES; ++i3) {
        struct pt_pmd *pmd = pud->child[i3];
        if (pmd == NULL)
          continue;
        for (unsigned int i2 = 0; i2 < PAGING64_PT_ENTRIES; ++i2)
          free(pmd->child[i2]);
        free(pmd);
      }
      free(pud);
    }
    free(p4d);
  }

  pthread_mutex_destroy(&root->lock);
  free(root);
  mm->pgd = NULL;
}

/*
 * init_pte - build a PTE value in *@pte
 * @pre   : page present in RAM (ignored when @swp != 0)
 * @fpn   : frame number, 0 is a valid frame (16 bits in MM64)
 * @drt   : dirty bit
 * @swp   : page is in swap -> PRESENT=0, SWAPPED=1
 * @swptyp: swap device id (5 bits)
 * @swpoff: frame number inside the swap device (21 bits)
 *
 * The whole entry is rebuilt (stale bits are cleared).  @pre == @swp == 0
 * yields the empty entry 0.  A field that does not fit returns -1 and
 * leaves *@pte unchanged, so values are never silently truncated.
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn,    // FPN
             int drt,    // dirty
             int swp,    // swap
             int swptyp, // swap type
             addr_t swpoff) // swap offset
{
  mm_pte_t val = 0;

  if (pte == NULL)
    return -1;

  if (swp != 0) {
    if (swptyp < 0 || (addr_t)swptyp > PTE_SWPTYP_MAX || swpoff > PTE_SWPOFF_MAX)
      return -1;
    val = PAGING_PTE_SWAPPED_MASK |
          ((mm_pte_t)swptyp << PAGING_PTE_SWPTYP_LOBIT) |
          ((mm_pte_t)swpoff << PAGING_PTE_SWPOFF_LOBIT);
  } else if (pre != 0) {
    if (fpn > PTE_FPN_MAX)
      return -1;
    val = PAGING_PTE_PRESENT_MASK | ((mm_pte_t)fpn << PAGING_PTE_FPN_LOBIT);
  }

  if (drt != 0 && val != 0)
    val |= PAGING_PTE_DIRTY_MASK;

  *pte = val;
  return 0;
}

/*
 * get_pd_from_address - split a virtual address into 5 table indices
 * Each index is 9 bits (0..511): PGD bits 56-48, P4D 47-39, PUD 38-30,
 * PMD 29-21, PT 20-12.  Bits 63-57 are ignored.
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	*pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
	*p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
	*pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
	*pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
	*pt = (addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT;

	return 0;
}

/*
 * get_pd_from_pagenum - split a page number into 5 table indices
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}

/* Get PTE page table entry
 * @mm     : address space to look in (user mm or kernel mm)
 * @pgn    : page number
 * @caller : unused, kept for API compatibility
 *
 * Return the 32-bit entry value (widened to addr_t).  A missing path,
 * an invalid pgn or an mm without a root all read as 0 (not mapped).
 * Never allocates.
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  (void)caller;
  struct pt_pgd *root = pt_root_for(mm, pgn);
  if (root == NULL)
    return 0;

  pthread_mutex_lock(&root->lock);
  mm_pte_t *pte = pt_walk_locked(root, pgn, 0);
  addr_t val = (pte != NULL) ? *pte : 0;
  pthread_mutex_unlock(&root->lock);

  return val;
}

/* Set PTE page table entry
 * @mm     : address space
 * @pgn    : page number
 * @pte_val: raw entry, must fit in 32 bits
 *
 * Creates missing tables.  Return 0 on success, -1 on invalid arguments
 * or allocation failure (the entry is then unchanged).
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  (void)caller;
  if (pte_val > UINT32_MAX)
    return -1;
  struct pt_pgd *root = pt_root_for(mm, pgn);
  if (root == NULL)
    return -1;

  pthread_mutex_lock(&root->lock);
  mm_pte_t *pte = pt_walk_locked(root, pgn, 1);
  if (pte != NULL)
    *pte = (mm_pte_t)pte_val;
  pthread_mutex_unlock(&root->lock);

  return (pte != NULL) ? 0 : -1;
}

/*
 * pte_set_fpn - map @pgn of @mm to RAM frame @fpn
 * Result: PRESENT=1, SWAPPED=0, DIRTY=0, FPN=@fpn.  @fpn must fit the
 * 16-bit FPN field (RAM <= 256 MiB), otherwise -1 and nothing changes.
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  addr_t val;
  if (init_pte(&val, 1, fpn, 0, 0, 0, 0) != 0)
    return -1;
  return pte_set_entry(mm, pgn, val, caller);
}

/*
 * pte_set_swap - mark @pgn of caller->mm as swapped out
 * Result: PRESENT=0, SWAPPED=1, swap type=@swptyp (swap device id),
 * swap offset=@swpoff (frame inside that device).
 * Note: the legacy signature has no mm argument; the caller's own user
 * address space is used.
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  addr_t val;
  if (caller == NULL)
    return -1;
  if (init_pte(&val, 0, 0, 0, 1, swptyp, swpoff) != 0)
    return -1;
  return pte_set_entry(caller->mm, pgn, val, caller);
}

/*
 * ===================================================================
 *  Mapping of user pages to RAM (Module 1)
 *
 *  Ported from the huyendtn branch (HuyenDtn) onto the pt_pgd tree.
 *
 *  - All functions below work on caller->mm (the user address space).
 *    Kernel memory has its own path (__kmalloc: contiguous frames, never
 *    in the FIFO).
 *  - The caller holds mmvm_lock: it protects mm->fifo_pgn, the vma list
 *    and the check-then-map sequence below.  pte_* take the tree lock and
 *    MEMPHY_* take memphy_lock (order mmvm_lock -> pgd->lock -> memphy).
 *  - FIFO: mm->fifo_pgn, newest page at the head, oldest at the tail.
 *    [OPEN] per-process FIFO, as on huyendtn; API_CONTRACT §7 still has
 *    to decide between per-process and global.
 * ===================================================================
 */

/* DMA pinning lives in libmem.c (M4).  Weak, so that mm64.c still links
 * on its own (unit tests): then no frame is ever pinned. */
extern int dma_frame_pinned(uint64_t fpn) __attribute__((weak));

static int frame_pinned(addr_t fpn)
{
  return dma_frame_pinned != NULL && dma_frame_pinned(fpn);
}

/* Create the path to the PTE of @pgn (no entry is written).
 * *is_free tells whether the entry is still 0 (never mapped).
 * Return 0, or -1 for an invalid mm/pgn or an allocation failure. */
static int pt_reserve(struct mm_struct *mm, addr_t pgn, int *is_free)
{
  struct pt_pgd *root = pt_root_for(mm, pgn);
  if (root == NULL)
    return -1;

  pthread_mutex_lock(&root->lock);
  mm_pte_t *pte = pt_walk_locked(root, pgn, 1);
  if (pte != NULL)
    *is_free = (*pte == 0);
  pthread_mutex_unlock(&root->lock);

  return (pte != NULL) ? 0 : -1;
}

/* Is [addr, addr + pgnum pages) page aligned and inside one vm area? */
static int range_in_vma(struct mm_struct *mm, addr_t addr, int pgnum)
{
  if (mm == NULL || pgnum < 0 || addr % PAGING64_PAGESZ != 0)
    return 0;

  for (struct vm_area_struct *vma = mm->mmap; vma != NULL; vma = vma->vm_next) {
    if (addr < vma->vm_start || addr > vma->vm_limit)
      continue;
    if (pgnum == 0 ||
        (addr_t)pgnum * PAGING64_PAGESZ - 1 <= vma->vm_limit - addr)
      return 1;
  }
  return 0;
}

static void free_pgn_list(struct pgn_t *pg)
{
  while (pg != NULL) {
    struct pgn_t *next = pg->pg_next;
    free(pg);
    pg = next;
  }
}

/* Before any frame is taken: create the table paths, check that every
 * page is still unmapped and allocate its FIFO node (in *pending).
 * On failure nothing is kept but empty table nodes (harmless). */
static int prepare_pages(struct mm_struct *mm, addr_t base_pgn, int pgnum,
                         struct pgn_t **pending)
{
  *pending = NULL;
  for (int i = 0; i < pgnum; i++) {
    int is_free = 0;
    if (pt_reserve(mm, base_pgn + i, &is_free) != 0 || !is_free ||
        enlist_pgn_node(pending, base_pgn + i) != 0) {
      free_pgn_list(*pending);
      *pending = NULL;
      return -1;
    }
  }
  return 0;
}

/* Write the PTEs of @pgnum prepared pages, then put them in the FIFO.
 * Paths exist and fpn come from the RAM device, so pte_set_fpn cannot
 * fail in practice; if it does, every PTE written here is cleared again
 * and -1 is returned (the caller still owns the frames). */
static int map_prepared(struct mm_struct *mm, addr_t addr, int pgnum,
                        struct framephy_struct *frames, struct pgn_t *pending,
                        struct vm_rg_struct *ret_rg)
{
  addr_t base_pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  struct framephy_struct *fr = frames;
  int i;

  for (i = 0; i < pgnum; i++, fr = fr->fp_next)
    if (pte_set_fpn(mm, base_pgn + i, fr->fpn, NULL) != 0)
      break;
  if (i < pgnum) {
    while (i-- > 0)
      pte_clear_entry(mm, base_pgn + i, NULL);
    free_pgn_list(pending);
    return -1;
  }

  /* pending: newest page first; splice it in front of the FIFO */
  if (pending != NULL) {
    struct pgn_t *last = pending;
    while (last->pg_next != NULL)
      last = last->pg_next;
    last->pg_next = mm->fifo_pgn;
    mm->fifo_pgn = pending;
  }

  if (ret_rg != NULL) {
    ret_rg->vmaid = 0;
    ret_rg->rg_start = addr;
    ret_rg->rg_end = addr + (addr_t)pgnum * PAGING64_PAGESZ;
    ret_rg->rg_next = NULL;
  }
  return 0;
}

/*
 * vmap_pgd_memset - fill the PTEs of a range with a debug pattern
 * (sys_memmap SYSMEM_MAP_OP).  The range must lie in a vm area of
 * caller->mm.  All paths are created first, so the pattern is written
 * either on every page or on none.  Return 0 or -1.
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  const uint64_t pattern = 0xdeadbeef;

  if (caller == NULL || !range_in_vma(caller->mm, addr, pgnum))
    return -1;

  addr_t base_pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  for (int i = 0; i < pgnum; i++) {
    int is_free;
    if (pt_reserve(caller->mm, base_pgn + i, &is_free) != 0)
      return -1;
  }
  for (int i = 0; i < pgnum; i++)
    pte_set_entry(caller->mm, base_pgn + i, pattern, caller);

  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 * @addr  : page-aligned start, inside a vm area of caller->mm
 * @pgnum : number of pages wanted
 * @frames: RAM frames to use, in order; only min(pgnum, list length)
 *          pages are mapped ("no guarantee all given pages are mapped")
 * @ret_rg: if not NULL, receives the region really mapped
 *
 * Every target page must be unmapped (PTE == 0).  Mapped pages enter the
 * FIFO.  The frame list stays owned by the caller.
 * Return 0, or (addr_t)-1 with nothing mapped.
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{
  if (caller == NULL || !range_in_vma(caller->mm, addr, pgnum))
    return (addr_t)-1;

  int mapped = 0;
  for (struct framephy_struct *fr = frames; fr != NULL && mapped < pgnum;
       fr = fr->fp_next)
    mapped++;

  struct pgn_t *pending = NULL;
  if (prepare_pages(caller->mm, addr >> PAGING64_ADDR_PT_SHIFT, mapped,
                    &pending) != 0)
    return (addr_t)-1;

  if (map_prepared(caller->mm, addr, mapped, frames, pending, ret_rg) != 0)
    return (addr_t)-1;
  return 0;
}

/* A page chosen for eviction, kept aside until every frame is secured */
struct pending_evict {
  struct pgn_t *victim;          /* its FIFO node, detached */
  addr_t swpfpn;                 /* reserved slot in the active swap */
  struct pending_evict *next;
};

/* Oldest FIFO page of @mm that may be evicted: resident and not pinned by
 * the DMA.  Return the link that points to its node (NULL if none). */
static struct pgn_t **fifo_pick_victim(struct mm_struct *mm, addr_t *vicfpn)
{
  struct pgn_t **pick = NULL;

  for (struct pgn_t **link = &mm->fifo_pgn; *link != NULL;
       link = &(*link)->pg_next) {
    addr_t pte = pte_get_entry(mm, (*link)->pgn, NULL);
    if (PAGING_PAGE_PRESENT(pte) && !(pte & PAGING_PTE_SWAPPED_MASK) &&
        !frame_pinned(PAGING_FPN(pte))) {
      pick = link;
      *vicfpn = PAGING_FPN(pte);
    }
  }
  return pick;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller (caller->mm, caller->krnl->mram / active_mswp)
 * @req_pgnum : request page num
 * @frm_lst   : receives a host list of req_pgnum frames (caller frees
 *              the list nodes; the frames themselves belong to caller->mm)
 *
 * Free RAM frames are used first.  When RAM is full, the oldest pages of
 * caller->mm's FIFO are copied to the active swap device and their frames
 * reused.  Victims are only committed (PTE -> swapped) once all frames are
 * secured, so a failure changes nothing.
 * Return 0; (addr_t)-3000 when not enough frames can be obtained;
 * (addr_t)-1 for invalid arguments.
 */
addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  if (frm_lst == NULL)
    return (addr_t)-1;
  *frm_lst = NULL;
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mram == NULL || req_pgnum < 0)
    return (addr_t)-1;

  struct mm_struct *mm = caller->mm;
  struct memphy_struct *ram = caller->krnl->mram;
  struct memphy_struct *swp = caller->krnl->active_mswp;
  struct pending_evict *evicts = NULL;
  struct framephy_struct **tail = frm_lst;
  int from_free = 0;   /* the first from_free frames came from free RAM */
  int got;

  for (got = 0; got < req_pgnum; got++) {
    struct framephy_struct *fr = malloc(sizeof(*fr));
    addr_t fpn = 0;
    if (fr == NULL)
      break;

    if (evicts == NULL && MEMPHY_get_freefp(ram, &fpn) == 0) {
      from_free++;
    } else {
      /* RAM is full: evict the oldest evictable page of this process */
      struct pending_evict *ev = (swp != NULL) ? malloc(sizeof(*ev)) : NULL;
      struct pgn_t **link = (ev != NULL) ? fifo_pick_victim(mm, &fpn) : NULL;
      if (link == NULL || MEMPHY_get_freefp(swp, &ev->swpfpn) != 0) {
        free(ev);
        free(fr);
        break;
      }
      if (__swap_cp_page(ram, fpn, swp, ev->swpfpn) != 0) {
        MEMPHY_put_freefp(swp, ev->swpfpn);
        free(ev);
        free(fr);
        break;
      }
      ev->victim = *link;            /* detach from the FIFO */
      *link = ev->victim->pg_next;
      ev->victim->pg_next = NULL;
      ev->next = evicts;
      evicts = ev;
    }

    fr->fpn = fpn;
    fr->owner = mm;
    fr->fp_next = NULL;
    *tail = fr;
    tail = &fr->fp_next;
  }

  if (got < req_pgnum) {
    /* Victims still own their frames (RAM was only read): put them back
     * at the old end of the FIFO, oldest last, and release swap slots. */
    struct pgn_t **end = &mm->fifo_pgn;
    while (*end != NULL)
      end = &(*end)->pg_next;
    while (evicts != NULL) {
      struct pending_evict *ev = evicts;
      evicts = ev->next;
      MEMPHY_put_freefp(swp, ev->swpfpn);
      *end = ev->victim;
      end = &ev->victim->pg_next;
      free(ev);
    }
    /* Frames from free RAM go back; the evicted ones were never ours */
    struct framephy_struct *fr = *frm_lst;
    for (int i = 0; fr != NULL; i++) {
      struct framephy_struct *next = fr->fp_next;
      if (i < from_free)
        MEMPHY_put_freefp(ram, fr->fpn);
      free(fr);
      fr = next;
    }
    *frm_lst = NULL;
    return (addr_t)-3000;
  }

  /* Commit the evictions: victims now live in swap */
  while (evicts != NULL) {
    struct pending_evict *ev = evicts;
    evicts = ev->next;
    pte_set_swap(caller, ev->victim->pgn, (int)caller->krnl->active_mswp_id,
                 ev->swpfpn);
    free(ev->victim);
    free(ev);
  }
  return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end (exclusive)
 * @mapstart  : start mapping point, page aligned, in [astart, aend)
 * @incpgnum  : number of mapped page; mapstart + incpgnum pages <= aend
 * @ret_rg    : returned region (may be NULL)
 *
 * All or nothing: on success every page is PRESENT and in the FIFO; on
 * failure no PTE, frame, swap slot or FIFO entry has changed.
 * Return 0 or (addr_t)-1.
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL;

  if (caller == NULL || caller->mm == NULL ||
      !range_in_vma(caller->mm, mapstart, incpgnum) ||
      mapstart < astart || mapstart > aend ||
      (addr_t)incpgnum > (aend - mapstart) / PAGING64_PAGESZ)
    return (addr_t)-1;

  /* 1. Paths, "unmapped" check and FIFO nodes: cheap and reversible */
  struct pgn_t *pending = NULL;
  if (prepare_pages(caller->mm, mapstart >> PAGING64_ADDR_PT_SHIFT, incpgnum,
                    &pending) != 0)
    return (addr_t)-1;

  /* 2. Frames (may evict to swap; changes nothing on failure).
   * The original template warns about endless swapping when RAM is far too
   * small: here a request that cannot be met simply fails. */
  if (alloc_pages_range(caller, incpgnum, &frm_lst) != 0) {
    free_pgn_list(pending);
    return (addr_t)-1;
  }

  /* 3. PTEs + FIFO */
  int rc = map_prepared(caller->mm, mapstart, incpgnum, frm_lst, pending, ret_rg);

  /* The list nodes are host bookkeeping only; on failure also return the
   * frames (an evicted victim already lives in swap, its frame is ours). */
  while (frm_lst != NULL) {
    struct framephy_struct *next = frm_lst->fp_next;
    if (rc != 0)
      MEMPHY_put_freefp(caller->krnl->mram, frm_lst->fpn);
    free(frm_lst);
    frm_lst = next;
  }
  return (rc == 0) ? 0 : (addr_t)-1;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 * Return 0, or -1 if a device is missing or a frame is out of range
 * (the destination may then be partially written).
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  if (mpsrc == NULL || mpdst == NULL)
    return -1;

  for (addr_t off = 0; off < PAGING64_PAGESZ; off++) {
    BYTE data;
    if (MEMPHY_read(mpsrc, srcfpn * PAGING64_PAGESZ + off, &data) != 0 ||
        MEMPHY_write(mpdst, dstfpn * PAGING64_PAGESZ + off, data) != 0)
      return -1;
  }
  return 0;
}

/*
 * init_mm - initialize an empty address space (Stage 2)
 * @mm:     address space to initialize; its previous content is discarded
 *          (it must not own a page table yet)
 * @caller: owning process, or NULL for the shared kernel address space
 *          (os.c: init_mm(os.mm, NULL)).  Only used to pick the default
 *          base of vma0; init_mm never dereferences it.
 *
 * Result on success (return 0):
 *   mm->pgd            empty five-level root (mm64_pgd_create)
 *   mm->mmap           vma0: vm_id 0, vm_start = vm_end = sbrk = base,
 *                      vm_limit = PAGING64_USER_END (user) or
 *                      PAGING64_KERNEL_END (kernel),
 *                      vm_mm = mm, vm_next = NULL, free list holding one
 *                      empty region [base, base)
 *   mm->symrgtbl       all regions zero (unallocated)
 *   mm->fifo_pgn       NULL, mm->kcpooltbl NULL
 *   mm->p4d/pud/pmd/pt NULL (legacy placeholders)
 *   base = PAGING64_USER_VMA0_START   when caller != NULL
 *          PAGING64_KERNEL_VMA0_START when caller == NULL
 * vma0 is grown later by Module 2 (inc_vma_limit -> vm_map_ram).
 *
 * On failure (return -1) nothing is leaked and @mm holds no root.
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  if (mm == NULL || mm->pgd != NULL)
    return -1;

  memset(mm, 0, sizeof(*mm));
  if (mm64_pgd_create(mm) != 0)
    return -1;

  addr_t base = (caller != NULL) ? PAGING64_USER_VMA0_START
                                 : PAGING64_KERNEL_VMA0_START;

  struct vm_area_struct *vma0 = calloc(1, sizeof(*vma0));
  struct vm_rg_struct *first_rg = init_vm_rg(base, base);
  if (vma0 == NULL || first_rg == NULL) {
    free(vma0);
    free(first_rg);
    mm64_pgd_destroy(mm);
    return -1;
  }

  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = base;
  vma0->vm_end = base;
  vma0->sbrk = base;
  /* Last address the area may grow to: the end of its half */
  vma0->vm_limit = (caller != NULL) ? PAGING64_USER_END : PAGING64_KERNEL_END;
  vma0->vm_freerg_list = NULL;
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);
  vma0->vm_next = NULL;
  vma0->vm_mm = mm;         /* point vma owner backward */

  mm->mmap = vma0;
  /* symrgtbl, fifo_pgn and kcpooltbl are zero/NULL from the memset */
  return 0;
}

/*
 * init_vm_rg - allocate a region node [rg_start, rg_end) of vma 0
 * Return NULL when the host allocation fails.
 */
struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));
  if (rgnode == NULL)
    return NULL;

  rgnode->vmaid = 0;
  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int print_list_fp(struct framephy_struct *ifp)
{
  struct framephy_struct *fp = ifp;

  printf("print_list_fp: ");
  if (fp == NULL) { printf("NULL list\n"); return -1;}
  printf("\n");
  while (fp != NULL)
  {
    printf("fp[" FORMAT_ADDR "]\n", fp->fpn);
    fp = fp->fp_next;
  }
  printf("\n");
  return 0;
}

int print_list_rg(struct vm_rg_struct *irg)
{
  struct vm_rg_struct *rg = irg;

  printf("print_list_rg: ");
  if (rg == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (rg != NULL)
  {
    printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "]\n", rg->rg_start, rg->rg_end);
    rg = rg->rg_next;
  }
  printf("\n");
  return 0;
}

int print_list_vma(struct vm_area_struct *ivma)
{
  struct vm_area_struct *vma = ivma;

  printf("print_list_vma: ");
  if (vma == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (vma != NULL)
  {
    printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "]\n", vma->vm_start, vma->vm_end);
    vma = vma->vm_next;
  }
  printf("\n");
  return 0;
}

int print_list_pgn(struct pgn_t *ip)
{
  printf("print_list_pgn: ");
  if (ip == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (ip != NULL)
  {
    printf("va[" FORMAT_ADDR "]-\n", ip->pgn);
    ip = ip->pg_next;
  }
  printf("\n");
  return 0;
}

int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
//addr_t pgn = ...
//addr_t pgit = ...
//struct mm_struct *mm = ...
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;

  get_pd_from_address(start, &pgd, &p4d, &pud, &pmd, &pt);

  /* TODO traverse the page map and dump the page directory entries */

  return 0;
}

/*
 * pte_clear_entry - reset the entry of @pgn in @mm to 0 (not mapped)
 * Does not return the frame or swap slot: the caller releases it first.
 * Clearing an entry whose tables do not exist succeeds without
 * allocating.  Return -1 only for an invalid mm/pgn.
 */
int pte_clear_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  (void)caller;
  struct pt_pgd *root = pt_root_for(mm, pgn);
  if (root == NULL)
    return -1;

  pthread_mutex_lock(&root->lock);
  mm_pte_t *pte = pt_walk_locked(root, pgn, 0);
  if (pte != NULL)
    *pte = 0;
  pthread_mutex_unlock(&root->lock);

  return 0;
}

#endif  //def MM64
