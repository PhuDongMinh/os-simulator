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
#include <limits.h>
#include <unistd.h>
#include <pthread.h>

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
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
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

  //vma đó còn đủ chỗ để cấp phát (get trả về 0) -> cập nhật vma, trả về alloc_addr = start
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

  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
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

  uint64_t pte = pte_get_entry(caller->mm, pgn, caller);

  if (!PAGING_PAGE_PRESENT(pte))
  { /* Page is not online, make it actively living */
    addr_t vicpgn;
    addr_t vicpte;
    addr_t vicfpn, swpfpn;
    struct sc_regs regs;

    /* TODO Initialize the target frame storing our variable */
//  addr_t tgtfpn 

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

    /* TODO copy victim frame to swap 
     * SWP(vicfpn <--> swpfpn)
     * SYSCALL 1 sys_memmap
     */


    /* Update page table */
    //pte_set_swap(...);

    /* Update its online status of the target page */
    //pte_set_fpn(...);

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
  int fpn;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

//int phyaddr = ...

  /* TODO  
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */

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
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */


  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */

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
  if (caller == NULL || caller->mm == NULL || data == NULL) return -1;
  /* TODO Invalid memory identify */
  pthread_mutex_lock(&mmvm_lock);

  /* 1. NGaN TRUY CẬP KERNEL: Bắt buộc lấy thông tin vùng nhớ từ caller->mm (User Space) */
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* 2. Kiểm tra tính hợp lệ (Tránh con trỏ rác hoặc vùng nhớ chưa cấp phát) */
  if (currg == NULL || cur_vma == NULL || currg->rg_start == 0 || currg->rg_start >= currg->rg_end) {
    printf("[User Read] Lỗi Bảo Mật: Vùng nhớ (rgid=%d) không hợp lệ.\n", rgid);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 3. Kiểm tra tràn viền (Out-of-bounds) chống tràn số nguyen không dấu */
  if (offset >= (currg->rg_end - currg->rg_start)) {
    printf("[User Read] Lỗi: Offset vượt quá giới hạn vùng nhớ.\n");
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 4. Kiểm tra dải địa chỉ Canonical User Space (Chặn __PAGEZERO và Kernel Space) */
  addr_t vaddr = currg->rg_start + offset;
  if (vaddr < USER_SPACE_START || vaddr > USER_SPACE_END) {
    printf("[User Read] Lỗi Bảo Mật: Địa chỉ ảo không thuộc User Space.\n");
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 5. Truy cập vật lý thông qua bảng phân trang của User (caller->mm) */
  int ret = pg_getval(caller->mm, vaddr, data, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
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
  if (caller == NULL || caller->mm == NULL) return -1;
  pthread_mutex_lock(&mmvm_lock);
  
  /* 1. NGaN TRUY CẬP KERNEL: Bắt buộc lấy thông tin vùng nhớ từ caller->mm (User Space) */
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (currg == NULL || cur_vma == NULL || currg->rg_start == 0 || currg->rg_start >= currg->rg_end) /* Invalid memory identify */
  {
    printf("[User Write] Lỗi Bảo Mật: Vùng nhớ (rgid=%d) không hợp lệ.\n", rgid);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 3. Kiểm tra tràn viền */
  if (offset >= (currg->rg_end - currg->rg_start))
  {
    printf("[User Write] Lỗi: Offset vượt quá giới hạn vùng nhớ.\n");
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t vaddr = currg->rg_start + offset;

  /* 4. Chốt chặn phân quyền: Cấm truy cập __PAGEZERO (< 0x1000) và Kernel Space (>= 0xFF00...) */
  if (vaddr < USER_SPACE_START || vaddr > USER_SPACE_END)
  {
    printf("[User Write] Lỗi Bảo Mật: Địa chỉ 0x%lx nằm ngoài User Space.\n", (unsigned long)vaddr);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 5. Ghi vật lý thông qua bảng phân trang của User (caller->mm) */
  int ret = pg_setval(caller->mm, vaddr, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
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

/* Hàm hỗ trợ: Tìm và lấy N frame vật lý liên tục từ MEMPHY (RAM).
 * Đi qua MEMPHY_get_freefp_range: giữ memphy_lock và chuyển node sang
 * used_fp_list, nhờ vậy MEMPHY_put_freefp mới trả frame về được.
 * Không sửa free_fp_list trực tiếp. */
int MEMPHY_get_freefp_contig(struct memphy_struct *mp, int num_pages, int *ret_fpn)
{
  addr_t first;
  if (ret_fpn == NULL) return -1;

  /* maxsz is int, so every valid FPN also fits in int. */
  if (MEMPHY_get_freefp_range(mp, num_pages, &first) != 0) return -1;
  *ret_fpn = (int)first;
  return 0;
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
  addr_t  addr;
  int val = __kmalloc(caller, 0, reg_index, size, &addr);
  /* 
   * TODO: provide OS kmem allocation validation
   */
  if (val != 0) return -1; // Cấp phát thất bại
  return 0; 
}


/* Map one kernel page @pgn -> RAM frame @fpn. kmm_lock held. 0 / -1. */
static int kmem_map_page(struct pcb_t *caller, addr_t pgn, addr_t fpn)
{
#ifdef MM64
  return pte_set_fpn(caller->krnl->mm, pgn, fpn, caller);
#else
  if (pgn >= PAGING_MAX_PGN)
    return -1;
  caller->krnl->krnl_pgd[pgn] =
      ((uint32_t)fpn << PAGING_PTE_FPN_LOBIT) | PAGING_PTE_PRESENT_MASK;
  return 0;
#endif
}

/* Clear the kernel PTE of @pgn (does not release the frame). kmm_lock held. */
static void kmem_unmap_page(struct pcb_t *caller, addr_t pgn)
{
#ifdef MM64
  pte_clear_entry(caller->krnl->mm, pgn, caller);
#else
  if (pgn < PAGING_MAX_PGN)
    caller->krnl->krnl_pgd[pgn] = 0;
#endif
}

/* Return @fpn to RAM; a failure means the frame was not checked out, i.e.
 * a frame-accounting bug somewhere: report it instead of hiding it. */
static void kmem_put_frame(struct pcb_t *caller, addr_t fpn)
{
  if (MEMPHY_put_freefp(caller->krnl->mram, fpn) != 0)
    printf("[kmem] WARNING: pid %u cannot return frame %llu to RAM\n",
           caller->pid, (unsigned long long)fpn);
}

/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 *
 * Kernel memory: physically CONTIGUOUS frames (MEMPHY_get_freefp_range),
 * mapped in the shared kernel mm, never put in the FIFO (never swapped).
 * Either everything succeeds or nothing changes (frames, PTEs, vma, tables).
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      caller->krnl->mram == NULL || alloc_addr == NULL ||
      rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ || size == 0)
    return -1;

  struct mm_struct *kmm = caller->krnl->mm;

  /* Page-aligned size; reject sizes whose alignment overflows or whose page
   * count does not fit in int (MEMPHY_get_freefp_range takes an int). */
  addr_t aligned_sz = OS_PAGE_ALIGN(size);
  if (aligned_sz < size || aligned_sz / OS_PAGESZ > (addr_t)INT_MAX)
    return -1;
  int num_pages = (int)(aligned_sz / OS_PAGESZ);

  pthread_mutex_lock(&kmm_lock);

  /* 1. rgid of the process must not already hold a kernel region */
  if (caller->kmap_slot[rgid] != -1) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  /* 2. Free slot in the shared kernel symbol table */
  int kslot = -1;
  for (int s = 0; s < PAGING_MAX_SYMTBL_SZ; s++) {
    if (kmm->symrgtbl[s].rg_start == 0 && kmm->symrgtbl[s].rg_end == 0) {
      kslot = s;
      break;
    }
  }
  struct vm_area_struct *cur_vma = get_vma_by_num(kmm, vmaid);
  if (kslot == -1 || cur_vma == NULL) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  /* 3. Choose the virtual range, but do NOT commit it yet:
   *    a) first fit in the free-region list (space returned by __kfree);
   *    b) otherwise grow the kernel vma at its break. init_mm() leaves the
   *       kernel free list with one EMPTY region, so (b) is the normal path. */
  struct vm_rg_struct *hole = NULL;
  addr_t vaddr = 0;
  for (struct vm_rg_struct *rgit = cur_vma->vm_freerg_list; rgit != NULL;
       rgit = rgit->rg_next) {
    addr_t cand = OS_PAGE_ALIGN(rgit->rg_start);
    if (cand >= rgit->rg_start && cand >= KERNEL_SPACE_START &&
        cand <= rgit->rg_end && rgit->rg_end - cand >= aligned_sz) {
      hole = rgit;
      vaddr = cand;
      break;
    }
  }
  if (hole == NULL) {
    addr_t brk = OS_PAGE_ALIGN(cur_vma->sbrk);
    /* stay in kernel space; strict '<' keeps vaddr + aligned_sz from
     * wrapping to 0 at the very top of the address space */
    if (brk < cur_vma->sbrk || brk < KERNEL_SPACE_START ||
        KERNEL_SPACE_END - brk < aligned_sz) {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
    vaddr = brk;
  }

  /* 4. Contiguous physical frames (memphy moves them to used_fp_list) */
  addr_t start_fpn = 0;
  if (MEMPHY_get_freefp_range(caller->krnl->mram, num_pages, &start_fpn) != 0) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  /* 5. Map every page. pte_set_fpn can fail (lazy table allocation):
   *    then undo the PTEs already written and return all frames. */
  int mapped = 0;
  while (mapped < num_pages &&
         kmem_map_page(caller, OS_VADDR_PGN(vaddr + (addr_t)mapped * OS_PAGESZ),
                       start_fpn + (addr_t)mapped) == 0)
    mapped++;

  if (mapped < num_pages) {
    for (int i = 0; i < mapped; i++)
      kmem_unmap_page(caller, OS_VADDR_PGN(vaddr + (addr_t)i * OS_PAGESZ));
    for (int i = 0; i < num_pages; i++)
      kmem_put_frame(caller, start_fpn + (addr_t)i);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  /* 6. Commit: nothing can fail from here on */
  if (hole != NULL) {
    hole->rg_start = vaddr + aligned_sz;
  } else {
    cur_vma->sbrk = vaddr + aligned_sz;
    if (cur_vma->vm_end < cur_vma->sbrk)
      cur_vma->vm_end = cur_vma->sbrk;
  }

  /* Exact bounds [vaddr, vaddr + size) so copy_from/to_user check precisely */
  kmm->symrgtbl[kslot].rg_start = vaddr;
  kmm->symrgtbl[kslot].rg_end = vaddr + size;
  kmm->symrgtbl[kslot].rg_next = NULL;
  caller->kmap_slot[rgid] = kslot;
  *alloc_addr = vaddr;

  pthread_mutex_unlock(&kmm_lock);
  return 0;
}

int __kfree(struct pcb_t *caller, int vmaid, int rgid)
{
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&kmm_lock);

  int kslot = caller->kmap_slot[rgid];
  if (kslot < 0 || kslot >= PAGING_MAX_SYMTBL_SZ) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  struct vm_rg_struct *rgnode = get_symrg_byid(caller->krnl->mm, kslot);
  if (rgnode == NULL || rgnode->rg_start == 0 || rgnode->rg_start >= rgnode->rg_end) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  addr_t aligned_sz = OS_PAGE_ALIGN(rgnode->rg_end - rgnode->rg_start);
  int num_pages = (int)(aligned_sz / OS_PAGESZ);

  /* Return each frame (via MEMPHY_put_freefp only), then clear its PTE */
  for (int i = 0; i < num_pages; i++) {
    addr_t pgn = OS_VADDR_PGN(rgnode->rg_start + (addr_t)i * OS_PAGESZ);
#ifdef MM64
    addr_t pte = pte_get_entry(caller->krnl->mm, pgn, caller);
#else
    uint32_t pte = (pgn < PAGING_MAX_PGN) ? caller->krnl->krnl_pgd[pgn] : 0;
#endif
    if (PAGING_PAGE_PRESENT(pte))
      kmem_put_frame(caller, PAGING_FPN(pte));
    kmem_unmap_page(caller, pgn);
  }

  /* Give the virtual range back to the kernel vma for reuse by __kmalloc */
  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  if (freerg_node != NULL) {
    freerg_node->rg_start = rgnode->rg_start;
    freerg_node->rg_end = rgnode->rg_start + aligned_sz;
    freerg_node->rg_next = NULL;
    if (enlist_vm_freerg_list(caller->krnl->mm, freerg_node) != 0)
      free(freerg_node);
  } else {
    /* frames and PTEs are released; only this virtual range is lost */
    printf("[kmem] WARNING: cannot record the freed kernel range\n");
  }

  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;
  caller->kmap_slot[rgid] = -1;

  pthread_mutex_unlock(&kmm_lock);
  return 0;
}

/*libkmem_free - OS level management kmem free
 *@caller: caller
 *@reg_index: memory region ID
 */
int libkmem_free(struct pcb_t *caller, uint32_t reg_index)
{
  int val = __kfree(caller, 0, reg_index);
  if (val != 0) return -1;
  return 0; 
}

// Chính sách xóa PTE 64-bit: 
      // Gọi hàm cấu hình PTE (nếu có) để xóa cờ PRESENT hoặc gán toàn bộ bit = 0
      // Ví dụ: pte_set_fpn(caller->krnl->mm, pgn, 0, caller); // (Cần điều chỉnh bit PRESENT = 0 ben trong mm64.c)

/* ------------------------------------------------------------------------
 * DMA memcpy support
 *
 * libmem_dma_memcpy() turns "copy SIZE bytes at OFFSET from region SRC to
 * region DST" (regions of the calling process) into a list of PHYSICAL
 * copies and queues them for the DMA worker (dma_submit). It returns as
 * soon as they are queued: the process does not wait for the copy.
 *
 *  - Both regions must exist and OFFSET + SIZE must fit in both.
 *  - A copy is cut at every page boundary of the source and of the
 *    destination (consecutive virtual pages may sit in distant frames);
 *    cuts that turn out to be physically contiguous on both sides are
 *    merged back, so one RTL run covers as much as possible.
 *  - Every page involved must be in RAM. A page that is swapped out makes
 *    the instruction fail (swap-in is not done here).
 *  - The frames of every queued copy are PINNED until the DMA completes, so
 *    the page replacement code must not evict a frame for which
 *    dma_frame_pinned() is true.
 * ------------------------------------------------------------------------ */
#ifdef MM64
#define DMA_PAGESZ ((addr_t)PAGING64_PAGESZ)
#else
#define DMA_PAGESZ ((addr_t)PAGING_PAGESZ)
#endif

/* A 13-bit frame number addresses at most 8192 frames (32 MiB of 4 KiB frames) */
#define DMA_MAX_FRAMES 8192
/* More than the RAM can hold, so such a copy could not be fully resident */
#define DMA_MAX_COPY ((addr_t)32 << 20)

struct dma_run {
  addr_t src;                   /* physical */
  addr_t dst;                   /* physical */
  addr_t len;
};

struct dma_ctx {                /* frames pinned by one queued run */
  addr_t sfirst, slast;
  addr_t dfirst, dlast;
};

static unsigned dma_pin_count[DMA_MAX_FRAMES];
static pthread_mutex_t dma_pin_lock = PTHREAD_MUTEX_INITIALIZER;

static void dma_pin_range(addr_t first, addr_t last, int delta)
{
  addr_t f;

  pthread_mutex_lock(&dma_pin_lock);
  for (f = first; f <= last && f < DMA_MAX_FRAMES; f++)
  {
    if (delta > 0)
      dma_pin_count[f]++;
    else if (dma_pin_count[f] > 0)
      dma_pin_count[f]--;
  }
  pthread_mutex_unlock(&dma_pin_lock);
}

/* Is frame fpn in use by a queued or running DMA copy? (for page replacement) */
int dma_frame_pinned(uint64_t fpn)
{
  int pinned = 0;

  if (fpn < DMA_MAX_FRAMES)
  {
    pthread_mutex_lock(&dma_pin_lock);
    pinned = dma_pin_count[fpn] > 0;
    pthread_mutex_unlock(&dma_pin_lock);
  }
  return pinned;
}

static void dma_run_frames(const struct dma_run *r, struct dma_ctx *c)
{
  c->sfirst = r->src / DMA_PAGESZ;
  c->slast = (r->src + r->len - 1) / DMA_PAGESZ;
  c->dfirst = r->dst / DMA_PAGESZ;
  c->dlast = (r->dst + r->len - 1) / DMA_PAGESZ;
}

static void dma_pin_run(const struct dma_run *r, int delta)
{
  struct dma_ctx c;

  dma_run_frames(r, &c);
  dma_pin_range(c.sfirst, c.slast, delta);
  dma_pin_range(c.dfirst, c.dlast, delta);
}

/* Runs on the DMA worker once a queued run has been executed */
static void dma_unpin_cb(void *ctx, int status)
{
  struct dma_ctx *c = (struct dma_ctx *)ctx;

  (void)status;
  dma_pin_range(c->sfirst, c->slast, -1);
  dma_pin_range(c->dfirst, c->dlast, -1);
  free(c);
}

/*
 * Page table lookup used to translate addresses: returns the PTE of page
 * pgn of the caller. It is a hook because pte_get_entry() is still a stub in
 * mm64.c (it returns the address of a local variable). Build with
 * -DDMA_REAL_PTE once mm64.c really walks the page table; until then every
 * lookup fails and dma_memcpy is refused instead of copying garbage.
 */
static int dma_pte_default(struct pcb_t *caller, addr_t pgn, addr_t *pte)
{
#ifdef DMA_REAL_PTE
  *pte = pte_get_entry(caller->mm, pgn, caller);
  return 0;
#else
  (void)caller;
  (void)pgn;
  (void)pte;
  return -1;
#endif
}

static int (*dma_pte_lookup)(struct pcb_t *, addr_t, addr_t *) = dma_pte_default;

/* Replace the page table lookup (used by tests); NULL restores the default */
void dma_set_pte_lookup(int (*fn)(struct pcb_t *, addr_t, addr_t *))
{
  dma_pte_lookup = (fn != NULL) ? fn : dma_pte_default;
}

/* Virtual address of the caller -> physical offset in RAM. mmvm_lock held. */
static int dma_vtop(struct pcb_t *caller, addr_t va, addr_t *pa)
{
  addr_t pgn = va / DMA_PAGESZ;
  addr_t pte = 0;
  addr_t fpn;
  addr_t nframes;

  if (dma_pte_lookup(caller, pgn, &pte) != 0)
  {
    printf("libmem_dma_memcpy: page table lookup is not available\n");
    return -1;
  }
  if (!PAGING_PAGE_PRESENT(pte) || (pte & PAGING_PTE_SWAPPED_MASK))
  {
    printf("libmem_dma_memcpy: page %llu is not in RAM\n", (unsigned long long)pgn);
    return -1;
  }

  fpn = GETVAL(pte, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
  nframes = (addr_t)(caller->krnl->mram->maxsz / (int)DMA_PAGESZ);
  if (fpn >= nframes || fpn >= DMA_MAX_FRAMES)
  {
    printf("libmem_dma_memcpy: frame %llu of page %llu is outside the RAM\n",
           (unsigned long long)fpn, (unsigned long long)pgn);
    return -1;
  }

  *pa = fpn * DMA_PAGESZ + va % DMA_PAGESZ;
  return 0;
}

/* Append run r to the list and pin its frames */
static void dma_add_run(struct dma_run *runs, int *nruns, const struct dma_run *r)
{
  runs[*nruns] = *r;
  (*nruns)++;
  dma_pin_run(r, +1);
}

/*
 * Build the list of physical runs for the copy. On failure nothing stays
 * pinned. mmvm_lock must be held (page tables and symrgtbl are protected by it).
 */
static int dma_build_runs(struct pcb_t *caller, uint32_t src, uint32_t dst,
                          addr_t offset, addr_t size, struct dma_run *runs, int *nruns)
{
  struct vm_rg_struct *srg = &caller->mm->symrgtbl[src];
  struct vm_rg_struct *drg = &caller->mm->symrgtbl[dst];
  struct dma_run cur = {0, 0, 0};
  int have = 0;
  addr_t slen, dlen, sva, dva, left;
  int i;

  *nruns = 0;

  if (srg->rg_end <= srg->rg_start || drg->rg_end <= drg->rg_start)
  {
    printf("libmem_dma_memcpy: region %u or %u is not allocated\n", src, dst);
    return -1;
  }
  slen = srg->rg_end - srg->rg_start;
  dlen = drg->rg_end - drg->rg_start;
  if (offset > slen || size > slen - offset || offset > dlen || size > dlen - offset)
  {
    printf("libmem_dma_memcpy: offset %llu + size %llu does not fit in the regions\n",
           (unsigned long long)offset, (unsigned long long)size);
    return -1;
  }

  sva = srg->rg_start + offset;
  dva = drg->rg_start + offset;
  left = size;

  while (left > 0)
  {
    addr_t spa, dpa;
    addr_t seg = left;
    addr_t sroom = DMA_PAGESZ - sva % DMA_PAGESZ;
    addr_t droom = DMA_PAGESZ - dva % DMA_PAGESZ;

    if (seg > sroom)
      seg = sroom;
    if (seg > droom)
      seg = droom;

    if (dma_vtop(caller, sva, &spa) != 0 || dma_vtop(caller, dva, &dpa) != 0)
      goto fail;

    if (have && cur.src + cur.len == spa && cur.dst + cur.len == dpa)
    {
      cur.len += seg;                 /* contiguous on both sides: extend */
    }
    else
    {
      if (have)
        dma_add_run(runs, nruns, &cur);
      cur.src = spa;
      cur.dst = dpa;
      cur.len = seg;
      have = 1;
    }

    sva += seg;
    dva += seg;
    left -= seg;
  }
  if (have)
    dma_add_run(runs, nruns, &cur);

  return 0;

fail:
  for (i = 0; i < *nruns; i++)
    dma_pin_run(&runs[i], -1);
  *nruns = 0;
  return -1;
}

/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 *
 * Returns 0 once the copy is QUEUED (the data is not there yet), -1 if it
 * was refused: nothing is queued and nothing stays pinned.
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  struct dma_run *runs;
  int nruns = 0;
  int i, rc;
  addr_t maxruns;

  if (size == 0)
    return 0;

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mram == NULL)
    return -1;

  /* get_symrg_byid() lets rgid == PAGING_MAX_SYMTBL_SZ through, so check here */
  if (src >= PAGING_MAX_SYMTBL_SZ || dst >= PAGING_MAX_SYMTBL_SZ)
  {
    printf("libmem_dma_memcpy: invalid region id %u or %u\n", src, dst);
    return -1;
  }
  if (size > DMA_MAX_COPY)
  {
    printf("libmem_dma_memcpy: %u bytes is more than the RAM can hold\n", size);
    return -1;
  }

  /* cuts happen at the page boundaries of the source AND of the destination */
  maxruns = (addr_t)size / DMA_PAGESZ * 2 + 3;
  runs = (struct dma_run *)calloc(maxruns, sizeof(struct dma_run));
  if (runs == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  rc = dma_build_runs(caller, src, dst, offset, size, runs, &nruns);
  if (rc == 0 && src == dst)
  {
    /* same region, same offset: nothing to move */
    for (i = 0; i < nruns; i++)
      dma_pin_run(&runs[i], -1);
    nruns = 0;
  }
  pthread_mutex_unlock(&mmvm_lock);

  if (rc != 0)
  {
    free(runs);
    return -1;
  }

  for (i = 0; i < nruns; i++)
  {
    struct dma_ctx *ctx = (struct dma_ctx *)malloc(sizeof(struct dma_ctx));
    int j;

    if (ctx != NULL)
    {
      dma_run_frames(&runs[i], ctx);
      if (dma_submit(caller->pid, runs[i].src, runs[i].dst, runs[i].len,
                     dma_unpin_cb, ctx) == 0)
        continue;
      free(ctx);
    }

    /* could not queue run i: release the pins of the runs not queued */
    printf("libmem_dma_memcpy: cannot queue the copy\n");
    for (j = i; j < nruns; j++)
      dma_pin_run(&runs[j], -1);
    free(runs);
    return -1;
  }

  free(runs);
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

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mm == NULL || size == 0)
    return -1;

  if (source >= PAGING_MAX_SYMTBL_SZ || destination >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  // 1. Lấy thông tin vùng nhớ Nguồn (User Space) và Đích (Kernel Space)
  struct vm_rg_struct *src_rg = get_symrg_byid(caller->mm, source);
  int dst_kslot = caller->kmap_slot[destination];
  struct vm_rg_struct *dst_rg = (dst_kslot >= 0) ? get_symrg_byid(caller->krnl->mm, dst_kslot) : NULL;

  // Kiểm tra sự tồn tại của vùng nhớ
  if (src_rg == NULL || src_rg->rg_start == 0 || src_rg->rg_start >= src_rg->rg_end ||
      dst_rg == NULL || dst_rg->rg_start == 0 || dst_rg->rg_start >= dst_rg->rg_end) {
    printf("[Copy] Lỗi: Vùng nhớ Nguồn hoặc Đích chưa được cấp phát.\n");
    return -1;
  }

  if (src_rg->rg_start < USER_SPACE_START || src_rg->rg_end - 1 > USER_SPACE_END ||
      dst_rg->rg_start < KERNEL_SPACE_START || dst_rg->rg_end - 1 > KERNEL_SPACE_END) {
    printf("[Copy] Lỗi Bảo Mật: Địa chỉ Nguồn/Đích không thuộc đúng không gian User/Kernel.\n");
    return -1;
  }

  // Kiểm tra tràn viền (Out of bounds) trước khi thực sự copy
  addr_t src_len = src_rg->rg_end - src_rg->rg_start;
  addr_t dst_len = dst_rg->rg_end - dst_rg->rg_start;

  if (offset > src_len || size > src_len - offset) {
    printf("[Copy] Lỗi: Nguồn (User) không đủ dữ liệu theo size yêu cầu.\n");
    return -1;
  }
  if (offset > dst_len || size > dst_len - offset) {
    printf("[Copy] Lỗi: Đích (Kernel) không đủ sức chứa.\n");
    return -1;
  }

  // 2. Thực hiện copy byte-by-byte từ User sang Kernel
  BYTE data;
  for (uint32_t i = 0; i < size; i++) {
    // vmaid mặc định là 0
    if (__read_user_mem(caller, 0, source, offset + i, &data) != 0) return -1;
    if (__write_kernel_mem(caller, 0, destination, offset + i, data) != 0) return -1;
  }

  printf("[Copy] Thành công: Chép %u bytes từ User (rgid=%u) vào Kernel (rgid=%u)\n", size, source, destination);
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

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mm == NULL || size == 0)
    return -1;

  if (source >= PAGING_MAX_SYMTBL_SZ || destination >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  // 1. Lấy thông tin vùng nhớ Nguồn (Kernel Space) và Đích (User Space)
  int src_kslot = caller->kmap_slot[source];
  struct vm_rg_struct *src_rg = (src_kslot >= 0) ? get_symrg_byid(caller->krnl->mm, src_kslot) : NULL;
  struct vm_rg_struct *dst_rg = get_symrg_byid(caller->mm, destination);

  // Kiểm tra sự tồn tại
  if (src_rg == NULL || src_rg->rg_start == 0 || src_rg->rg_start >= src_rg->rg_end ||
      dst_rg == NULL || dst_rg->rg_start == 0 || dst_rg->rg_start >= dst_rg->rg_end) {
    printf("[Copy] Lỗi: Vùng nhớ Nguồn hoặc Đích chưa được cấp phát.\n");
    return -1;
  }

  if (src_rg->rg_start < KERNEL_SPACE_START || src_rg->rg_end - 1 > KERNEL_SPACE_END ||
      dst_rg->rg_start < USER_SPACE_START || dst_rg->rg_end - 1 > USER_SPACE_END) {
    printf("[Copy] Lỗi Bảo Mật: Địa chỉ Nguồn/Đích không thuộc đúng không gian Kernel/User.\n");
    return -1;
  }

  // Kiểm tra tràn viền 
  addr_t src_len = src_rg->rg_end - src_rg->rg_start;
  addr_t dst_len = dst_rg->rg_end - dst_rg->rg_start;

  if (offset > src_len || size > src_len - offset) {
    printf("[Copy] Lỗi: Nguồn (Kernel) không đủ dữ liệu theo size yêu cầu.\n");
    return -1;
  }
  if (offset > dst_len || size > dst_len - offset) {
    printf("[Copy] Lỗi: Đích (User) không đủ sức chứa.\n");
    return -1;
  }

  // 2. Thực hiện copy byte-by-byte từ Kernel sang User
  BYTE data;
  for (uint32_t i = 0; i < size; i++) {
    // Đọc từ Kernel
    if (__read_kernel_mem(caller, 0, source, offset + i, &data) != 0) return -1;
    // Ghi xuống User
    if (__write_user_mem(caller, 0, destination, offset + i, data) != 0) return -1;
  }

  printf("[Copy] Thành công: Chép %u bytes từ Kernel (rgid=%u) vào User (rgid=%u)\n", size, source, destination);
  return 0; // Thành công
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
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      data == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ) 
    return -1;

  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  pthread_mutex_lock(&kmm_lock);
  
  // 1. TTim vung nho ao Kernel
  int kslot = caller->kmap_slot[rgid];
  if (kslot < 0 || kslot >= PAGING_MAX_SYMTBL_SZ) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, kslot);
  if (currg == NULL || currg->rg_start == 0 || currg->rg_start >= currg->rg_end) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // 2. Kiem tra bien va trich xuat Page Number, Offset
  if (offset >= (currg->rg_end - currg->rg_start)) {
    pthread_mutex_unlock(&kmm_lock);
    return -1; 
  }

  addr_t vaddr = currg->rg_start + offset;
  if (vaddr < KERNEL_SPACE_START) {
    pthread_mutex_unlock(&kmm_lock);
    return -1; //Dia chi khong nam trong khong gian kernel
  }

  addr_t pgn = OS_VADDR_PGN(vaddr);
  addr_t off = OS_VADDR_OFF(vaddr);
  addr_t fpn;

  // 3. Dich Virtual Address -> Physical Address tu bang phan trang Kernel
#ifdef MM64
  addr_t pte = pte_get_entry(caller->krnl->mm, pgn, caller);
  if (!PAGING_PAGE_PRESENT(pte)) {
    pthread_mutex_unlock(&kmm_lock);
    return -1; 
  }
  fpn = PAGING_FPN(pte); 
#else
  uint32_t pte = caller->krnl->krnl_pgd[pgn];
  if (!PAGING_PAGE_PRESENT(pte)) { 
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  fpn = PAGING_FPN(pte);
#endif

  // 4. Truy xuat vao RAM vat ly
  addr_t phyaddr = (fpn * OS_PAGESZ) + off;
  int ret = MEMPHY_read(caller->krnl->mram, phyaddr, data);

  pthread_mutex_unlock(&kmm_lock);
  return ret;
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
  // krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&kmm_lock);

  int kslot = caller->kmap_slot[rgid];
  if (kslot < 0 || kslot >= PAGING_MAX_SYMTBL_SZ) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, kslot);
  if (currg == NULL || currg->rg_start == 0 || currg->rg_start >= currg->rg_end) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  if (offset >= currg->rg_end - currg->rg_start) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  addr_t vaddr = currg->rg_start + offset;
  if (vaddr < KERNEL_SPACE_START || vaddr > KERNEL_SPACE_END) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  addr_t pgn = OS_VADDR_PGN(vaddr);
  addr_t off = OS_VADDR_OFF(vaddr);
  addr_t fpn;

#ifdef MM64
  // Trích xuất PTE 64-bit bằng cách duyệt qua bảng phân trang của Kernel
  addr_t pte = pte_get_entry(caller->krnl->mm, pgn, caller);
  if (!PAGING_PAGE_PRESENT(pte)) {
    // Vùng nhớ Kernel luôn phải nằm trên RAM (được cấp phát cứng qua __kmalloc)
    // Nếu không có mặt (Present = 0), đây là lỗi truy cập bộ nhớ nghiêm trọng
    pthread_mutex_unlock(&kmm_lock);
    return -1; // Page của Kernel luôn phải nằm trên RAM, không được swap
  }
  fpn = PAGING_FPN(pte); // Lấy Frame Number từ 64-bit PTE (tự khớp FPN 16-bit)
#else
  uint32_t pte = caller->krnl->krnl_pgd[pgn];
  if (!PAGING_PAGE_PRESENT(pte)) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  fpn = PAGING_FPN(pte);
#endif

  addr_t phyaddr = (fpn * OS_PAGESZ) + off;
  int ret = MEMPHY_write(caller->krnl->mram, phyaddr, value);

  pthread_mutex_unlock(&kmm_lock);
  return ret;
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
  if (caller == NULL || caller->mm == NULL || data == NULL ||
      rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  
  // Trỏ về không gian mm của tiến trình User
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  if (currg == NULL || currg->rg_start == 0 || currg->rg_start >= currg->rg_end) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // Kiểm tra biên chống tràn số nguyên không dấu
  if (offset >= (currg->rg_end - currg->rg_start)) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // Kiểm tra dải địa chỉ Canonical User Space (Chặn __PAGEZERO và Kernel Space)
  addr_t vaddr = currg->rg_start + offset;
  if (vaddr < USER_SPACE_START || vaddr > USER_SPACE_END) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // pg_getval sẽ tự động đọc User Page Table và nạp Frame từ Swap lên nếu cần
  int ret = pg_getval(caller->mm, vaddr, data, caller);
  
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
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
  if (caller == NULL || caller->mm == NULL)
    return -1;

  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  if (currg == NULL || currg->rg_start == 0 || currg->rg_start >= currg->rg_end) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* Kiểm tra tràn viền chống tràn số nguyên không dấu */
  if (offset >= (currg->rg_end - currg->rg_start)) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t vaddr = currg->rg_start + offset;

  /* Chốt chặn dải địa chỉ Canonical User Space (chặn __PAGEZERO và Kernel Space) */
  if (vaddr < USER_SPACE_START || vaddr > USER_SPACE_END) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // pg_setval xử lý quá trình nạp trang/chuyển Swap trong không gian User
  int ret = pg_setval(caller->mm, vaddr, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


#ifdef MM64
struct release_ctx {
  struct krnl_t *krnl;
  int failed;                   /* frames/slots MEMPHY refused to take back */
};

/* Swap device a swapped PTE points to (its SWPTYP field) */
static struct memphy_struct *swap_dev_of(struct krnl_t *krnl, addr_t typ)
{
  if (typ >= PAGING_MAX_MMSWP)
    return NULL;
  if (krnl->mswp != NULL)
    return krnl->mswp[typ];
  return (typ == krnl->active_mswp_id) ? krnl->active_mswp : NULL;
}

/* mm64_pt_foreach callback: give one page's frame or swap slot back */
static void release_page(addr_t pgn, addr_t pte, void *arg)
{
  struct release_ctx *rc = (struct release_ctx *)arg;
  int r = 0;
  (void)pgn;

  if (pte & PAGING_PTE_SWAPPED_MASK) {
    struct memphy_struct *dev = swap_dev_of(
        rc->krnl, GETVAL(pte, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT));
    r = (dev != NULL) ? MEMPHY_put_freefp(dev, PAGING_PTE_SWP(pte)) : -1;
  } else if (PAGING_PAGE_PRESENT(pte)) {
    r = MEMPHY_put_freefp(rc->krnl->mram, PAGING_FPN(pte));
  }
  if (r != 0)
    rc->failed++;
}
#endif

/*free_pcb_memph - release the whole user address space of a process
 *@caller: finished process; its DMA requests must be done (os.c waits)
 *
 * MM64: every RAM frame and swap slot referenced by caller->mm's PTEs goes
 * back to its device (only through MEMPHY_put_freefp), then the page-table
 * tree, the FIFO, the vm areas and their free-region lists are freed.
 * The mm_struct itself stays (os.c frees it).  The kernel mm is never
 * touched: kernel regions are released by __kfree.
 * Return 0; -1 for invalid arguments or if some frame/slot was refused
 * (e.g. a vmap_pgd_memset debug pattern, which owns no frame): everything
 * is freed anyway.
 */
int free_pcb_memph(struct pcb_t *caller)
{
#ifdef MM64
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mram == NULL || caller->mm == caller->krnl->mm)
    return -1;

  struct mm_struct *mm = caller->mm;
  struct release_ctx rc = {caller->krnl, 0};

  pthread_mutex_lock(&mmvm_lock);

  /* 1. Frames and swap slots, then the tree itself */
  mm64_pt_foreach(mm, release_page, &rc);
  mm64_pgd_destroy(mm);

  /* 2. FIFO */
  while (mm->fifo_pgn != NULL) {
    struct pgn_t *next = mm->fifo_pgn->pg_next;
    free(mm->fifo_pgn);
    mm->fifo_pgn = next;
  }

  /* 3. vm areas and their free-region lists */
  while (mm->mmap != NULL) {
    struct vm_area_struct *vma = mm->mmap;
    while (vma->vm_freerg_list != NULL) {
      struct vm_rg_struct *next = vma->vm_freerg_list->rg_next;
      free(vma->vm_freerg_list);
      vma->vm_freerg_list = next;
    }
    mm->mmap = vma->vm_next;
    free(vma);
  }

  /* 4. Symbol table entries are embedded: just forget them */
  memset(mm->symrgtbl, 0, sizeof(mm->symrgtbl));

  pthread_mutex_unlock(&mmvm_lock);

  if (rc.failed > 0)
    printf("[mm] WARNING: pid %u: %d frame(s)/swap slot(s) could not be returned\n",
           caller->pid, rc.failed);
  return (rc.failed > 0) ? -1 : 0;
#else
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;

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
#endif
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
