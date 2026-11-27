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
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#if defined(MM64)

static struct mm_struct *get_target_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  if (mm != NULL)
    return mm;
  if (caller != NULL)
  {
    if (caller->mm != NULL)
      return caller->mm;
    if (caller->krnl != NULL && caller->krnl->mm != NULL)
      return caller->krnl->mm;
  }
  return NULL;
}

/* Return the leaf slot, allocating a separate child table for each used branch. */
static addr_t *walk_page_tables(struct mm_struct *mm, addr_t pgn,
                                const addr_t index[5], int create)
{
  const unsigned int page_width = PAGING64_CPU_BUS_WIDTH - PAGING64_ADDR_PT_SHIFT;
  const addr_t sign_bit = BIT_ULL(page_width - 1);
  const addr_t upper = pgn >> page_width;
  /* Accept both 57-bit addresses and their sign-extended form. */
  if (mm == NULL || (upper != 0 &&
      (upper != (UINT64_MAX >> PAGING64_CPU_BUS_WIDTH) || !(pgn & sign_bit))))
    return NULL;

  if (mm->pgd == NULL && create)
    mm->pgd = calloc(PAGING64_MAX_PGN, sizeof(*mm->pgd));

  addr_t *table = mm->pgd;
  for (size_t level = 0; level < 4 && table != NULL; level++)
  {
    addr_t *entry = &table[index[level]];
    table = (addr_t *)(uintptr_t)*entry;
    if (table == NULL && create)
    {
      table = calloc(PAGING64_MAX_PGN, sizeof(*table));
      *entry = (addr_t)(uintptr_t)table;
    }
  }

  return table == NULL ? NULL : &table[index[4]];
}

static addr_t *lookup_pte(struct mm_struct *mm, addr_t pgn, int create)
{
  addr_t index[5];
  get_pd_from_pagenum(pgn, &index[0], &index[1], &index[2], &index[3], &index[4]);
  return walk_page_tables(mm, pgn, index, create);
}

/*
 * init_pte - Initialize PTE entry
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn,    // FPN
             int drt,    // dirty
             int swp,    // swap
             int swptyp, // swap type
             addr_t swpoff) // swap offset
{
  if (pre != 0) {
    if (swp == 0) { // Non swap ~ page online
      if (fpn == 0)
        return -1;  // Invalid setting

      /* Valid setting with FPN */
      SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
      CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
      CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

      SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT); //
    }
    else
    { // page swapped
      SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
      SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
      CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

      SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
      SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
    }
  }

  return 0;
}


/*
 * get_pd_from_pagenum - Parse address to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Extract page direactories */
	*pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
	*p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
	*pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
	*pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
	*pt = (addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT;

	/* TODO: implement the page direactories mapping */

	return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}


/*
 * pte_set_swap - Set PTE entry for swapped page
 * @pte    : target page table entry (PTE)
 * @swptyp : swap type
 * @swpoff : swap offset
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  struct krnl_t *krnl = caller->krnl;

  addr_t *pte;
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;
	
  // dummy pte alloc to avoid runtime error
  pte = malloc(sizeof(addr_t));
#ifdef MM64	
  /* Get value from the system */
  /* TODO Perform multi-level page mapping */
  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
  //... krnl->mm->pgd
  //... krnl->mm->pt
  //pte = &krnl->mm->pt;
  const addr_t index[] = {pgd, p4d, pud, pmd, pt};
  pte = walk_page_tables(get_target_mm(NULL, caller), pgn, index, 1);
  if (pte == NULL)
    return -1;
#else
  pte = &krnl->mm->pgd[pgn];
#endif
	
  CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  swptyp &= PAGING_PTE_SWPTYP_MASK >> PAGING_PTE_SWPTYP_LOBIT;
  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @pte   : target page table entry (PTE)
 * @fpn   : frame page number (FPN)
 * @caller: caller
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  addr_t *pte = lookup_pte(get_target_mm(mm, caller), pgn, 1);
  if (pte == NULL)
    return -1;

  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  return 0;
}


/* Get PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @caller: caller
 * @ret    : page table entry
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
#ifndef MM64
  struct krnl_t *krnl = caller->krnl;
#endif

  addr_t *pte = NULL;
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;
	
#if 0
  // dummy pte alloc to avoid runtime error
  pte = malloc(sizeof(addr_t));
#endif
#ifdef MM64	
  /* Get value from the system */
  /* TODO Perform multi-level page mapping */
  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
  //... krnl->mm->pgd
  //... krnl->mm->pt
  //pte = &krnl->mm->pt;
  const addr_t index[] = {pgd, p4d, pud, pmd, pt};
  pte = walk_page_tables(get_target_mm(mm, caller), pgn, index, 0);
  return pte == NULL ? 0 : *pte;
#else	
  pte = &krnl->mm->pgd[pgn];
#endif

#ifndef MM64
  return (addr_t) &pte;
#endif
}

/* Set PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @ret    : page table entry
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
#ifndef MM64
  struct krnl_t *krnl = caller->krnl;
#endif

  addr_t *pte = NULL;
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;
	
#if 0
  // dummy pte alloc to avoid runtime error
  pte = malloc(sizeof(addr_t));
#endif
#ifdef MM64	
  /* Get value from the system */
  /* TODO Perform multi-level page mapping */
  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
  //... krnl->mm->pgd
  //... krnl->mm->pt
  //pte = &krnl->mm->pt;
  const addr_t index[] = {pgd, p4d, pud, pmd, pt};
  pte = walk_page_tables(get_target_mm(mm, caller), pgn, index, 1);
  if (pte == NULL)
    return -1;
  *pte = pte_val;
#else	
  pte = pte_val;
#endif

	return 0;
}

static int valid_page_range(struct mm_struct *mm, addr_t addr, int pgnum)
{
  const struct vm_area_struct *vma = mm->mmap;
  const addr_t limit = vma == NULL ? UINT64_MAX : vma->vm_limit;
  return pgnum >= 0 && addr % PAGING64_PAGESZ == 0 &&
         (vma == NULL || addr >= vma->vm_start) && addr <= limit &&
         (pgnum == 0 || (addr_t)pgnum * PAGING64_PAGESZ - 1 <= limit - addr);
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  //int pgit = 0;
  //uint64_t pattern = 0xdeadbeef;

  /* TODO memset the page table with given pattern
   */

  struct mm_struct *mm = get_target_mm(NULL, caller);
  if (mm == NULL || !valid_page_range(mm, addr, pgnum))
    return -1;

  addr_t base_pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  for (int i = 0; i < pgnum; i++)
    if (lookup_pte(mm, base_pgn + i, 1) == NULL)
      return -1;

  for (int i = 0; i < pgnum; i++)
    *lookup_pte(mm, base_pgn + i, 0) = 0xdeadbeef;

  return 0;
}

static void free_page_list(struct pgn_t *pages)
{
  while (pages != NULL)
  {
    struct pgn_t *next = pages->pg_next;
    free(pages);
    pages = next;
  }
}

/* Reserve page tables and FIFO nodes before assigning any physical frames. */
static int prepare_page_range(struct mm_struct *mm, addr_t base_pgn,
                              int pgnum, struct pgn_t **pending)
{
  for (int i = 0; i < pgnum; i++)
  {
    addr_t *pte = lookup_pte(mm, base_pgn + i, 1);
    if (pte == NULL || *pte != 0 || enlist_pgn_node(pending, base_pgn + i) != 0)
    {
      free_page_list(*pending);
      *pending = NULL;
      return -1;
    }
  }

  return 0;
}

static void map_prepared_pages(struct mm_struct *mm, addr_t addr, int mapped,
                               struct framephy_struct *frame, struct pgn_t *pending,
                               struct vm_rg_struct *ret_rg)
{
  addr_t base_pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  for (int i = 0; i < mapped; i++, frame = frame->fp_next)
    pte_set_fpn(mm, base_pgn + i, frame->fpn, NULL);

  if (pending != NULL)
  {
    struct pgn_t *oldest = pending;
    while (oldest->pg_next != NULL)
      oldest = oldest->pg_next;
    oldest->pg_next = mm->fifo_pgn;
    mm->fifo_pgn = pending;
  }

  if (ret_rg != NULL)
  {
    ret_rg->vmaid = 0;
    ret_rg->rg_start = addr;
    ret_rg->rg_end = addr + (addr_t)mapped * PAGING64_PAGESZ;
    ret_rg->rg_next = NULL;
  }
}

/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                   // no guarantee all given pages are mapped
//struct framephy_struct *fpit;
//int pgit = 0;
//addr_t pgn;

  /* TODO: update the rg_end and rg_start of ret_rg 
  //ret_rg->rg_end =  ....
  //ret_rg->rg_start = ...
  //ret_rg->vmaid = ...
  */

  /* TODO map range of frame to address space
   *      [addr to addr + pgnum*PAGING_PAGESZ
   *      in page table caller->krnl->mm->pgd,
   *                    caller->krnl->mm->pud...
   *                    ...
   */

  /* Tracking for later page replacement activities (if needed)
   * Enqueue new usage page */
  //enlist_pgn_node(&caller->krnl->mm->fifo_pgn, pgn64 + pgit);

  struct mm_struct *mm = get_target_mm(NULL, caller);
  if (mm == NULL || !valid_page_range(mm, addr, pgnum))
    return (addr_t)-1;

  int mapped = 0;
  for (struct framephy_struct *frame = frames; frame != NULL && mapped < pgnum;
       frame = frame->fp_next)
    mapped++;

  struct pgn_t *pending = NULL;
  if (prepare_page_range(mm, addr >> PAGING64_ADDR_PT_SHIFT, mapped, &pending) != 0)
    return (addr_t)-1;

  map_prepared_pages(mm, addr, mapped, frames, pending, ret_rg);

  return 0;
}

/* Keep evictions pending until all requested RAM frames have been reserved. */
struct pending_eviction {
  struct pgn_t *victim;
  addr_t swap_frame;
  struct pending_eviction *next;
};

static void release_frame_list(struct framephy_struct **frames, struct memphy_struct *ram)
{
  while (*frames != NULL)
  {
    struct framephy_struct *frame = *frames;
    *frames = frame->fp_next;
    if (ram != NULL)
      MEMPHY_put_freefp(ram, frame->fpn);
    free(frame);
  }
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  //addr_t fpn;
  //int pgit;
  //struct framephy_struct *newfp_str = NULL;
  //struct framephy_struct **cur = frm_lst;

  /* TODO: allocate the page 
  //caller-> ...
  //frm_lst-> ...
  */


/*
  for (pgit = 0; pgit < req_pgnum; pgit++)
  {
    // TODO: allocate the page 
    if (MEMPHY_get_freefp(caller->mram, &fpn) == 0)
    {
      newfp_str->fpn = fpn;
    }
    else
    { // TODO: ERROR CODE of obtaining somes but not enough frames
    }
  }
*/


  if (frm_lst == NULL)
    return (addr_t)-1;
  *frm_lst = NULL;
  struct mm_struct *mm = get_target_mm(NULL, caller);
  if (mm == NULL || caller->krnl == NULL || caller->krnl->mram == NULL || req_pgnum < 0)
    return (addr_t)-1;

  struct memphy_struct *ram = caller->krnl->mram;
  struct memphy_struct *swap = caller->krnl->active_mswp;
  struct pending_eviction *evictions = NULL;
  struct framephy_struct **tail = frm_lst;
  int free_ram_frames = 0;
  int reserved;

  for (reserved = 0; reserved < req_pgnum; reserved++)
  {
    struct framephy_struct *frame = malloc(sizeof(*frame));
    if (frame == NULL)
      break;

    if (evictions == NULL && MEMPHY_get_freefp(ram, &frame->fpn) == 0)
    {
      free_ram_frames++;
    }
    else
    {
      if (swap == NULL || mm->fifo_pgn == NULL)
      {
        free(frame);
        break;
      }

      /* Detach the oldest FIFO entry so it can be restored on failure. */
      struct pgn_t **victim = &mm->fifo_pgn;
      while ((*victim)->pg_next != NULL)
        victim = &(*victim)->pg_next;

      addr_t vicpte = pte_get_entry(mm, (*victim)->pgn, caller);
      struct pending_eviction *eviction = malloc(sizeof(*eviction));
      if (!PAGING_PAGE_PRESENT(vicpte) || eviction == NULL ||
          MEMPHY_get_freefp(swap, &eviction->swap_frame) != 0)
      {
        free(eviction);
        free(frame);
        break;
      }

      frame->fpn = PAGING_FPN(vicpte);
      if (__swap_cp_page(ram, frame->fpn, swap, eviction->swap_frame) != 0)
      {
        MEMPHY_put_freefp(swap, eviction->swap_frame);
        free(eviction);
        free(frame);
        break;
      }

      eviction->victim = *victim;
      eviction->next = evictions;
      evictions = eviction;
      *victim = NULL;
    }

    frame->owner = mm;
    frame->fp_next = NULL;
    *tail = frame;
    tail = &frame->fp_next;
  }

  if (reserved != req_pgnum)
  {
    /* Free RAM reservations, while keeping the original pages resident. */
    struct pgn_t **restore = &mm->fifo_pgn;
    while (*restore != NULL)
      restore = &(*restore)->pg_next;
    while (evictions != NULL)
    {
      struct pending_eviction *eviction = evictions;
      evictions = eviction->next;
      MEMPHY_put_freefp(swap, eviction->swap_frame);
      *restore = eviction->victim;
      restore = &eviction->victim->pg_next;
      free(eviction);
    }

    /* Only the prefix obtained directly from RAM can be returned to its free list. */
    struct framephy_struct **evicted_frames = frm_lst;
    for (int i = 0; i < free_ram_frames; i++)
      evicted_frames = &(*evicted_frames)->fp_next;
    release_frame_list(evicted_frames, NULL);
    release_frame_list(frm_lst, ram);
    return (addr_t)-3000;
  }

  while (evictions != NULL)
  {
    struct pending_eviction *eviction = evictions;
    evictions = eviction->next;
    pte_set_swap(caller, eviction->victim->pgn, caller->krnl->active_mswp_id,
                 eviction->swap_frame);
    free(eviction->victim);
    free(eviction);
  }

  /* End TODO */

  return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL;
  addr_t ret_alloc = 0;

  /*@bksysnet: author provides a feasible solution of getting frames
   *FATAL logic in here, wrong behaviour if we have not enough page
   *i.e. we request 1000 frames meanwhile our RAM has size of 3 frames
   *Don't try to perform that case in this simple work, it will result
   *in endless procedure of swap-off to get frame and we have not provide
   *duplicate control mechanism, keep it simple
   */
  struct mm_struct *mm = get_target_mm(NULL, caller);
  if (mm == NULL || !valid_page_range(mm, mapstart, incpgnum) ||
      mapstart < astart || mapstart > aend ||
      (addr_t)incpgnum > (aend - mapstart) / PAGING64_PAGESZ)
    return (addr_t)-1;

  struct pgn_t *pending = NULL;
  if (prepare_page_range(mm, mapstart >> PAGING64_ADDR_PT_SHIFT, incpgnum, &pending) != 0)
    return (addr_t)-1;

  ret_alloc = alloc_pages_range(caller, incpgnum, &frm_lst);
  if (ret_alloc != 0)
  {
    free_page_list(pending);
    return (addr_t)-1;
  }

#if 0

  if (ret_alloc < 0 && ret_alloc != -3000)
    return -1;

  /* Out of memory */
  if (ret_alloc == -3000)
  {
    return -1;
  }

  /* it leaves the case of memory is enough but half in ram, half in swap
   * do the swaping all to swapper to get the all in ram */
   vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);
#endif

  map_prepared_pages(mm, mapstart, incpgnum, frm_lst, pending, ret_rg);
  release_frame_list(&frm_lst, NULL);

  return 0;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  if (mpsrc == NULL || mpdst == NULL ||
      srcfpn >= (addr_t)(mpsrc->maxsz / PAGING64_PAGESZ) ||
      dstfpn >= (addr_t)(mpdst->maxsz / PAGING64_PAGESZ))
    return -1;

  int cellidx;
  addr_t addrsrc, addrdst;
#ifdef MM64
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#else
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#endif

    BYTE data;
#if 0
    MEMPHY_read(mpsrc, addrsrc, &data);
    MEMPHY_write(mpdst, addrdst, data);
#endif
    if (MEMPHY_read(mpsrc, addrsrc, &data) != 0 ||
        MEMPHY_write(mpdst, addrdst, data) != 0)
      return -1;
  }

  return 0;
}

/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: process owning the user address space, or NULL for the kernel
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  if (mm == NULL)
    return -1;

  struct vm_area_struct *vma0 = malloc(sizeof(struct vm_area_struct));
  if (vma0 == NULL)
    return -1;

  /* TODO init page table directory */
   //mm->pgd = ...
   //mm->p4d = ...
   //mm->pud = ...
   //mm->pmd = ...
   //mm->pt = ...
  addr_t *pgd = calloc(PAGING64_MAX_PGN, sizeof(*pgd));
  if (pgd == NULL)
  {
    free(vma0);
    return -1;
  }


  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = caller == NULL ? PAGING64_KERNEL_START : PAGING64_USER_START;
  vma0->vm_limit = caller == NULL ? PAGING64_KERNEL_END : PAGING64_USER_END;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  if (first_rg == NULL)
  {
    free(pgd);
    free(vma0);
    return -1;
  }
  vma0->vm_freerg_list = NULL;
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  /* TODO update VMA0 next */
  // vma0->next = ...
  vma0->vm_next = NULL;

  /* Point vma owner backward */
  //vma0->vm_mm = mm; 
  vma0->vm_mm = mm;

  /* TODO: update mmap */
  //mm->mmap = ...
  //mm->symrgtbl = ...
  //mm->kcpooltbl
  /* Allocate the remaining levels when a page first uses them. */
  mm->pgd = pgd;
  mm->p4d = NULL;
  mm->pud = NULL;
  mm->pmd = NULL;
  mm->pt = NULL;
  mm->mmap = vma0;
  memset(mm->symrgtbl, 0, sizeof(mm->symrgtbl));
  mm->fifo_pgn = NULL;
  mm->kcpooltbl = NULL;

  return 0;
}

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

  if (pnode == NULL)
    return -1;
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

static void print_page_table(const addr_t *table, unsigned int shift,
                             addr_t prefix, addr_t first, addr_t last)
{
  for (addr_t index = 0; index < PAGING64_MAX_PGN; index++)
  {
    if (table[index] == 0)
      continue;

    addr_t addr = prefix | (index << shift);
    if (addr > last || (addr | (BIT_ULL(shift) - 1)) < first)
      continue;

    if (shift == PAGING64_ADDR_PT_SHIFT)
      printf("VA=%#" PRIx64 " PGD=%03x P4D=%03x PUD=%03x PMD=%03x PT=%03x PTE=%#" PRIx64 "\n",
             (uint64_t)addr, (unsigned int)PAGING64_ADDR_PGD(addr),
             (unsigned int)PAGING64_ADDR_P4D(addr), (unsigned int)PAGING64_ADDR_PUD(addr),
             (unsigned int)PAGING64_ADDR_PMD(addr), (unsigned int)index, (uint64_t)table[index]);
    else
      print_page_table((const addr_t *)(uintptr_t)table[index],
                       shift - (PAGING64_ADDR_PT_HIBIT - PAGING64_ADDR_PT_LOBIT + 1),
                       addr, first, last);
  }
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
  struct mm_struct *mm = get_target_mm(NULL, caller);
  if (mm == NULL || start > end)
    return -1;

  const addr_t mask = GENMASK64(PAGING64_CPU_BUS_WIDTH - 1, 0);
  if (mm->pgd != NULL)
    print_page_table(mm->pgd, PAGING64_ADDR_PGD_LOBIT, 0, start & mask, end & mask);

  return 0;
}

#endif  //def MM64
