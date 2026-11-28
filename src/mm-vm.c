/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "mm64.h"
#include <limits.h>
#include <stdlib.h>

/*get_vma_by_num - get vm area by numID
 *@mm: memory region
 *@vmaid: ID vm area to alloc memory region
 *
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *pvma = mm->mmap;

  if (mm->mmap == NULL)
    return NULL;

  int vmait = pvma->vm_id;

  while (vmait < vmaid)
  {
    if (pvma == NULL)
      return NULL;

    pvma = pvma->vm_next;
    if (pvma == NULL) return NULL;
    vmait = pvma->vm_id;
  }

  return pvma;
}

/* sys_memmap SYSMEM_SWP_OP: copy RAM frame @vicfpn to frame @swpfpn of the
 * active swap device.  Return 0, or -1 if a device is missing or a frame is
 * out of range (pg_getpage then keeps the victim resident). */
int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
    if (caller == NULL || caller->krnl == NULL)
        return -1;
    return __swap_cp_page(caller->krnl->mram, vicfpn,
                          caller->krnl->active_mswp, swpfpn);
}

/*get_vm_area_node - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct vm_rg_struct * newrg;
  /* TODO retrive current vma to obtain newrg, current comment out due to compiler redundant warning*/
  //struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  //newrg = malloc(sizeof(struct vm_rg_struct));

  /* TODO: update the newrg boundary
  // newrg->rg_start = ...
  // newrg->rg_end = ...
  */
  // struct vm_area_struct *cur_vma=....

  /* END TODO */
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  newrg = malloc(sizeof(struct vm_rg_struct));

  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = newrg->rg_start + alignedsz;

  return newrg;
}

/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct vm_area_struct *vma = caller->mm->mmap;

  /* TODO validate the planned memory area is not overlapped */
  if (vmastart >= vmaend)
  {
    return -1;
  }

  if (vma == NULL)
  {
    return -1;
  }

  /* TODO validate the planned memory area is not overlapped */

  struct vm_area_struct *cur_area = get_vma_by_num(caller->mm, vmaid);
  if (cur_area == NULL)
  {
    return -1;
  }

  while (vma != NULL)
  {
    if (vma != cur_area && OVERLAP(cur_area->vm_start, cur_area->vm_end, vma->vm_start, vma->vm_end))
    {
      return -1;
    }
    vma = vma->vm_next;
  }
  /* End TODO*/

  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  /* TOTO with new address scheme, the size need tobe aligned 
   *      the raw inc_sz maybe not fit pagesize
   */ 

  /* 
   * TODO Validate overlap of obtained region 
   *if (validate_overlap_vm_area(caller, vmaid, area->rg_start, area->rg_end) < 0)
   *   return -1;  //Overlap and failed allocation 
   */

  /* TODO: Obtain the new vm area based on vmaid */
    struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

    if (cur_vma == NULL || inc_sz == 0) return -1;

    addr_t old_sbrk = cur_vma->sbrk;
    addr_t old_end = cur_vma->vm_end;

    addr_t new_sbrk = old_sbrk + inc_sz;
    addr_t new_end = PAGING64_PAGE_ALIGNSZ(new_sbrk);

    if (new_end < old_end) new_end = old_end;

    addr_t inc_amt = new_end - old_end;
    addr_t incnumpage = inc_amt / PAGING64_PAGESZ;

    /* Validate overlap */
    if (validate_overlap_vm_area(caller, vmaid,
                                cur_vma->vm_start, new_end) < 0)
        return -1;

    /* Map additional pages to RAM */
    if (incnumpage > 0)
    {
        struct vm_rg_struct *newrg;

        if (vm_map_ram(caller,
                       cur_vma->vm_start,
                       new_end,
                       old_end,
                       (int)incnumpage,
                       newrg) != 0)
            return -1;
    }

    /* Update VMA after successful mapping */
    cur_vma->vm_end = new_end;
    cur_vma->sbrk = new_sbrk;

    return 0;
}

// #endif
