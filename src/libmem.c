/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "sysbus.h"

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  struct vm_rg_struct *rg_node = mm->mmap->vm_freerg_list;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  if (rg_node != NULL)
    rg_elmt->rg_next = rg_node;

  /* Enlist the new region */
  mm->mmap->vm_freerg_list = rg_elmt;

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  int inc_sz=0;

  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
 
    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/

  /*Attempt to increate limit to get space */
#ifdef MM64
  inc_sz = (uint32_t)(size/(int)PAGING64_PAGESZ);
  inc_sz = inc_sz + 1;
#else
  inc_sz = PAGING_PAGE_ALIGNSZ(size);
#endif
  int old_sbrk;
  inc_sz = inc_sz + 1;

  old_sbrk = cur_vma->sbrk;

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
#ifdef MM64
  regs.a3 = size;
#else
  regs.a3 = PAGING_PAGE_ALIGNSZ(size);
#endif  
  _syscall(caller->krnl, caller->pid, 17, &regs); /* SYSCALL 17 sys_memmap */
  if (cur_vma->vm_end < (addr_t)old_sbrk + size)
  {
#ifdef MM64
    addr_t align_sz = PAGING64_PAGE_ALIGNSZ(size);
    int incpgnum = align_sz / PAGING64_PAGESZ;
#else
    addr_t align_sz = PAGING_PAGE_ALIGNSZ(size);
    int incpgnum = align_sz / PAGING_PAGESZ;
#endif
    struct vm_rg_struct ret_rg;
    if ((int64_t)vm_map_ram(caller, cur_vma->vm_start, cur_vma->vm_end + align_sz, cur_vma->vm_end, incpgnum, &ret_rg) < 0)
    {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
    cur_vma->vm_end += align_sz;
    cur_vma->sbrk = old_sbrk + size;
    if ((addr_t)old_sbrk + size < cur_vma->vm_end)
    {
      struct vm_rg_struct *left_rg = init_vm_rg(old_sbrk + size, cur_vma->vm_end);
      enlist_vm_freerg_list(caller->mm, left_rg);
    }
  }
  else if (cur_vma->sbrk == (addr_t)old_sbrk)
  {
    cur_vma->sbrk = old_sbrk + size;
  }
  /*Successful increase limit */
  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;

  *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode->rg_start == 0 && rgnode->rg_end == 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->mm, freerg_node);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t  addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  /* By default using vmaid = 0 */
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val == -1)
  {
    return -1;
  }
printf("%s:%d\n",__func__,__LINE__);
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0;//val;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller)
{
  if (mm != NULL && mm != caller->mm)
  {
    uint64_t kpte = pte_get_entry(mm, pgn, caller);
    if (!PAGING_PAGE_PRESENT(kpte))
    {
      addr_t kfpn = 0;
      if (MEMPHY_get_freefp(caller->krnl->mram, &kfpn) == 0)
      {
        pte_set_fpn(mm, pgn, kfpn, caller);
        enlist_pgn_node(&mm->fifo_pgn, pgn);
      }
      else
      {
        return -1;
      }
    }
    *fpn = PAGING_FPN(pte_get_entry(mm, pgn, caller));
    return 0;
  }

  uint64_t pte = pte_get_entry(caller->mm, pgn, caller);

  if (!PAGING_PAGE_PRESENT(pte))
  { /* Page is not online, make it actively living */
    addr_t vicpgn;
    addr_t vicpte;
    addr_t vicfpn, swpfpn;
    struct sc_regs regs;

    /* TODO Initialize the target frame storing our variable */
//  addr_t tgtfpn 
    addr_t tgtfpn = PAGING_SWP(pte);

    /* TODO: Play with your paging theory here */
    /* Find victim page */
    if (find_victim_page(caller->mm, &vicpgn) == -1)
    {
      return -1;
    }

    /* Get free frame in MEMSWP */
    if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == -1)
    {
      return -1;
    }

    /* TODO: Implement swap frame from MEMRAM to MEMSWP and vice versa*/
    vicpte = pte_get_entry(caller->mm, vicpgn, caller);
    vicfpn = PAGING_FPN(vicpte);

    /* TODO copy victim frame to swap 
     * SWP(vicfpn <--> swpfpn)
     * SYSCALL 1 sys_memmap
     */
    regs.a1 = SYSMEM_SWP_OP;
    regs.a2 = vicfpn;
    regs.a3 = swpfpn;
    _syscall(caller->krnl, caller->pid, 17, &regs);

    if (pte & PAGING_PTE_SWAPPED_MASK)
    {
      __swap_cp_page(caller->krnl->active_mswp, tgtfpn, caller->krnl->mram, vicfpn);
      MEMPHY_put_freefp(caller->krnl->active_mswp, tgtfpn);
    }


    /* Update page table */
    //pte_set_swap(...);
    pte_set_swap(caller, vicpgn, caller->krnl->active_mswp_id, swpfpn);

    /* Update its online status of the target page */
    //pte_set_fpn(...);
    pte_set_fpn(caller->mm, pgn, vicfpn, caller);

    enlist_pgn_node(&caller->mm->fifo_pgn, pgn);
  }

  *fpn = PAGING_FPN(pte_get_entry(caller->mm,pgn, caller));

  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, int addr, BYTE *data, struct pcb_t *caller)
{
  int pgn = PAGING_PGN(addr);
//int off = ...
#ifdef MM64
  pgn = (addr_t)addr >> PAGING64_ADDR_PT_SHIFT;
  int off = (addr_t)addr & (PAGING64_PAGESZ - 1);
#else
  int off = PAGING_OFFST(addr);
#endif
  int fpn;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

//int phyaddr = ...
#ifdef MM64
  int phyaddr = (fpn << PAGING64_ADDR_PT_SHIFT) + off;
#else
  int phyaddr = (fpn << PAGING_ADDR_FPN_LOBIT) + off;
#endif

  /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */
  struct sc_regs regs;
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  regs.a3 = 0;
  _syscall(caller->krnl, caller->pid, 17, &regs);
  *data = (BYTE)regs.a3;

  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, int addr, BYTE value, struct pcb_t *caller)
{
  int pgn = PAGING_PGN(addr);
//int off = ...
#ifdef MM64
  pgn = (addr_t)addr >> PAGING64_ADDR_PT_SHIFT;
  int off = (addr_t)addr & (PAGING64_PAGESZ - 1);
#else
  int off = PAGING_OFFST(addr);
#endif
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */


  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */
#ifdef MM64
  int phyaddr = (fpn << PAGING64_ADDR_PT_SHIFT) + off;
#else
  int phyaddr = (fpn << PAGING_ADDR_FPN_LOBIT) + off;
#endif
  struct sc_regs regs;
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = value;
  _syscall(caller->krnl, caller->pid, 17, &regs);

  return 0;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);

//struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  /* TODO Invalid memory identify */
  pthread_mutex_lock(&mmvm_lock);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (currg == NULL || cur_vma == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  pg_getval(caller->mm, currg->rg_start + offset, data, caller);
  pthread_mutex_unlock(&mmvm_lock);
  return 0;

  pg_getval(caller->krnl->mm, currg->rg_start + offset, data, caller);

  return 0;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
  BYTE data;
printf("%s:%d\n",__func__,__LINE__);
  int val = __read(proc, 0, source, offset, &data);

  *destination = data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);
  currg = get_symrg_byid(caller->mm, rgid);
  cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (currg == NULL || cur_vma == NULL) /* Invalid memory identify */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pg_setval(caller->mm, currg->rg_start + offset, value, caller);
  pthread_mutex_unlock(&mmvm_lock);
  return 0;

  pg_setval(caller->krnl->mm, currg->rg_start + offset, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /* 
   * TODO: provide OS level management
   *       and forward the request to helper
   */
//addr_t  addr;
//int val = ...
  addr_t addr = 0;
  int val = (int)__kmalloc(caller, 0, reg_index, size, &addr);

  /* 
   * TODO: provide OS kmem allocation validation
   */
  if (val == -1)
  {
    return -1;
  }

  return 0;
}


/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /* TODO: provide OS kernel memory allocation
   *       update krnl_pgd for OS kernel level management */

  //struct krnl_t *krnl = caller->krnl;
  //krnl->symrgtbl...
  //krnl->krnl_pgd ...
  pthread_mutex_lock(&kmm_lock);
  if (caller == NULL || caller->krnl == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ || size == 0)
  {
    pthread_mutex_unlock(&kmm_lock);
    return (addr_t)-1;
  }

  struct krnl_t *krnl = caller->krnl;
  if (krnl->mm == NULL)
  {
    krnl->mm = (struct mm_struct *)malloc(sizeof(struct mm_struct));
    init_mm(krnl->mm, caller);
  }

  struct vm_area_struct *cur_vma = get_vma_by_num(krnl->mm, vmaid);
  if (cur_vma == NULL)
  {
    pthread_mutex_unlock(&kmm_lock);
    return (addr_t)-1;
  }

  addr_t old_sbrk = cur_vma->sbrk;
  if (old_sbrk + size > cur_vma->vm_end)
  {
#ifdef MM64
    addr_t inc_sz = PAGING64_PAGE_ALIGNSZ((old_sbrk + size) - cur_vma->vm_end);
    int incpgnum = inc_sz / PAGING64_PAGESZ;
#else
    addr_t inc_sz = PAGING_PAGE_ALIGNSZ((old_sbrk + size) - cur_vma->vm_end);
    int incpgnum = inc_sz / PAGING_PAGESZ;
#endif
    struct framephy_struct *frm_lst = NULL;
    if ((int64_t)alloc_pages_range(caller, incpgnum, &frm_lst) < 0)
    {
      pthread_mutex_unlock(&kmm_lock);
      return (addr_t)-1;
    }

#ifdef MM64
    if (krnl->krnl_pgd == NULL)
      krnl->krnl_pgd = (addr_t *)calloc(PAGING64_MAX_PGN, sizeof(addr_t));
    if (krnl->krnl_p4d == NULL)
      krnl->krnl_p4d = (addr_t *)calloc(PAGING64_MAX_PGN, sizeof(addr_t));
    if (krnl->krnl_pud == NULL)
      krnl->krnl_pud = (addr_t *)calloc(PAGING64_MAX_PGN, sizeof(addr_t));
    if (krnl->krnl_pmd == NULL)
      krnl->krnl_pmd = (addr_t *)calloc(PAGING64_MAX_PGN, sizeof(addr_t));
    if (krnl->krnl_pt == NULL)
      krnl->krnl_pt = (addr_t *)calloc(PAGING64_MAX_PGN, sizeof(addr_t));

    addr_t base_pgn = cur_vma->vm_end >> PAGING64_ADDR_PT_SHIFT;
#else
    if (krnl->krnl_pgd == NULL)
      krnl->krnl_pgd = (uint32_t *)calloc(PAGING_MAX_PGN, sizeof(uint32_t));
    addr_t base_pgn = PAGING_PGN(cur_vma->vm_end);
#endif

    struct framephy_struct *fpit = frm_lst;
    for (int pgit = 0; pgit < incpgnum && fpit != NULL; pgit++)
    {
      addr_t pgn = base_pgn + pgit;
      pte_set_fpn(krnl->mm, pgn, fpit->fpn, caller);
      enlist_pgn_node(&krnl->mm->fifo_pgn, pgn);

#ifdef MM64
      addr_t pgd = 0, p4d = 0, pud = 0, pmd = 0, pt = 0;
      get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
      krnl->krnl_pgd[pgd] = (addr_t)&krnl->krnl_p4d[p4d];
      krnl->krnl_p4d[p4d] = (addr_t)&krnl->krnl_pud[pud];
      krnl->krnl_pud[pud] = (addr_t)&krnl->krnl_pmd[pmd];
      krnl->krnl_pmd[pmd] = (addr_t)&krnl->krnl_pt[pt];
      krnl->krnl_pt[pt] = pte_get_entry(krnl->mm, pgn, caller);
#else
      krnl->krnl_pgd[pgn] = pte_get_entry(krnl->mm, pgn, caller);
#endif
      fpit = fpit->fp_next;
    }

    while (frm_lst != NULL)
    {
      struct framephy_struct *next = frm_lst->fp_next;
      free(frm_lst);
      frm_lst = next;
    }

    cur_vma->vm_end += inc_sz;
  }

  cur_vma->sbrk = old_sbrk + size;
  krnl->mm->symrgtbl[rgid].rg_start = old_sbrk;
  krnl->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
  krnl->mm->symrgtbl[rgid].vmaid = vmaid;

  if (alloc_addr != NULL)
    *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&kmm_lock);

  return 0;

}



/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  /* TODO: provide DMA mempy */

  //struct krnl_t *krnl = caller->krnl;

  /*
   *  TODO: provide asynchronous dma memcpy
   *        handling the dma_done_callback
   */
  //do_dma(caller, (uint64_t) src, (uint64_t) dst, (uint64_t) size);
  //krnl->...
  //krnl->...
  if (caller == NULL || caller->krnl == NULL)
    return -1;

  struct krnl_t *krnl = caller->krnl;
  struct mm_struct *src_mm = caller->mm;
  struct vm_rg_struct *src_rg = get_symrg_byid(caller->mm, src);
  if ((src_rg == NULL || (src_rg->rg_start == 0 && src_rg->rg_end == 0)) && krnl->mm != NULL)
  {
    struct vm_rg_struct *krg = get_symrg_byid(krnl->mm, src);
    if (krg != NULL && krg->rg_end > krg->rg_start)
    {
      src_mm = krnl->mm;
      src_rg = krg;
    }
  }

  struct mm_struct *dst_mm = caller->mm;
  struct vm_rg_struct *dst_rg = get_symrg_byid(caller->mm, dst);
  if ((dst_rg == NULL || (dst_rg->rg_start == 0 && dst_rg->rg_end == 0)) && krnl->mm != NULL)
  {
    struct vm_rg_struct *krg = get_symrg_byid(krnl->mm, dst);
    if (krg != NULL && krg->rg_end > krg->rg_start)
    {
      dst_mm = krnl->mm;
      dst_rg = krg;
    }
  }

  if (src_rg == NULL || dst_rg == NULL)
    return -1;

  addr_t src_vaddr = src_rg->rg_start + offset;
  addr_t dst_vaddr = dst_rg->rg_start + offset;
  int src_fpn = 0, dst_fpn = 0;

#ifdef MM64
  int src_pgn = src_vaddr >> PAGING64_ADDR_PT_SHIFT;
  int src_off = src_vaddr & (PAGING64_PAGESZ - 1);
  int dst_pgn = dst_vaddr >> PAGING64_ADDR_PT_SHIFT;
  int dst_off = dst_vaddr & (PAGING64_PAGESZ - 1);
#else
  int src_pgn = PAGING_PGN(src_vaddr);
  int src_off = PAGING_OFFST(src_vaddr);
  int dst_pgn = PAGING_PGN(dst_vaddr);
  int dst_off = PAGING_OFFST(dst_vaddr);
#endif

  if (pg_getpage(src_mm, src_pgn, &src_fpn, caller) != 0)
    return -1;
  if (pg_getpage(dst_mm, dst_pgn, &dst_fpn, caller) != 0)
    return -1;

#ifdef MM64
  uint64_t src_phyaddr = ((uint64_t)src_fpn << PAGING64_ADDR_PT_SHIFT) + src_off;
  uint64_t dst_phyaddr = ((uint64_t)dst_fpn << PAGING64_ADDR_PT_SHIFT) + dst_off;
#else
  uint64_t src_phyaddr = ((uint64_t)src_fpn << PAGING_ADDR_FPN_LOBIT) + src_off;
  uint64_t dst_phyaddr = ((uint64_t)dst_fpn << PAGING_ADDR_FPN_LOBIT) + dst_off;
#endif

  for (uint32_t i = 0; i < size; i++)
  {
    BYTE data = 0;
    if (src_mm == krnl->mm)
      __read_kernel_mem(caller, 0, src, offset + i, &data);
    else
      __read_user_mem(caller, 0, src, offset + i, &data);

    if (dst_mm == krnl->mm)
      __write_kernel_mem(caller, 0, dst, offset + i, data);
    else
      __write_user_mem(caller, 0, dst, offset + i, data);
  }

  if (!memory_bank_is_available())
  {
    size_t bank_sz = (krnl->mram != NULL && krnl->mram->maxsz > 0) ? (size_t)krnl->mram->maxsz : (1024 * 1024);
    memory_bank_init(bank_sz);
  }

  do_dma(caller, src_phyaddr, dst_phyaddr, (uint64_t)size);

  return 0;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_user_mem(...)
  //__write_kernel_mem(...);
  if (caller == NULL || caller->krnl == NULL)
    return -1;

  struct vm_rg_struct *krg = get_symrg_byid(caller->krnl->mm, destination);
  if (krg != NULL && krg->rg_start == 0 && krg->rg_end == 0)
  {
    addr_t kaddr = 0;
    __kmalloc(caller, 0, destination, offset + size, &kaddr);
  }

  for (uint32_t i = 0; i < size; i++)
  {
    BYTE data = 0;
    if (__read_user_mem(caller, 0, source, offset + i, &data) != 0)
      return -1;
    if (__write_kernel_mem(caller, 0, destination, offset + i, data) != 0)
      return -1;
  }

  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_kernel_mem(...)
  //__write_user_mem(...);
  if (caller == NULL || caller->krnl == NULL)
    return -1;

  struct vm_rg_struct *urg = get_symrg_byid(caller->mm, destination);
  if (urg != NULL && urg->rg_start == 0 && urg->rg_end == 0)
  {
    addr_t uaddr = 0;
    __alloc(caller, 0, destination, offset + size, &uaddr);
  }

  for (uint32_t i = 0; i < size; i++)
  {
    BYTE data = 0;
    if (__read_kernel_mem(caller, 0, source, offset + i, &data) != 0)
      return -1;
    if (__write_user_mem(caller, 0, destination, offset + i, data) != 0)
      return -1;
  }

  return 0;

  return 1;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  pthread_mutex_lock(&kmm_lock);
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL)
  {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);
  if (currg == NULL || cur_vma == NULL)
  {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  pg_getval(caller->krnl->mm, currg->rg_start + offset, data, caller);
  pthread_mutex_unlock(&kmm_lock);

  return 0;
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  pthread_mutex_lock(&kmm_lock);
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL)
  {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);
  if (currg == NULL || cur_vma == NULL)
  {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  pg_setval(caller->krnl->mm, currg->rg_start + offset, value, caller);
  pthread_mutex_unlock(&kmm_lock);

  return 0;
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  return __read(caller, vmaid, rgid, offset, data);

   return 0;
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  return __write(caller, vmaid, rgid, offset, value);

  return 0;
}


/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;
#ifdef MM64
  for (pagenum = 0; pagenum < PAGING64_MAX_PGN; pagenum++)
  {
    addr_t pte64 = pte_get_entry(caller->mm, pagenum, caller);
    if (PAGING_PAGE_PRESENT(pte64))
    {
      fpn = PAGING_FPN(pte64);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else if (pte64 & PAGING_PTE_SWAPPED_MASK)
    {
      fpn = PAGING_SWP(pte64);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }
  pthread_mutex_unlock(&mmvm_lock);
  return 0;
#endif

  for (pagenum = 0; pagenum < PAGING_MAX_PGN; pagenum++)
  {
    pte = caller->mm->pgd[pagenum];

    if (PAGING_PAGE_PRESENT(pte))
    {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else
    {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;

  /* TODO: Implement the theorical mechanism to find the victim page */
  if (!pg)
  {
    return -1;
  }
  struct pgn_t *prev = NULL;
  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;
  if (prev == NULL)
  {
    mm->fifo_pgn = NULL;
    free(pg);
    return 0;
  }
  prev->pg_next = NULL;

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
