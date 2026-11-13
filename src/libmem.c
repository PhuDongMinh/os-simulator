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
  /* TODO Invalid memory identify */
  pthread_mutex_lock(&mmvm_lock);

  /* 1. NGĂN TRUY CẬP KERNEL: Bắt buộc lấy thông tin vùng nhớ từ caller->mm (User Space) */
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* 2. Kiểm tra tính hợp lệ (Tránh con trỏ rác hoặc vùng nhớ chưa cấp phát) */
  if (currg == NULL || cur_vma == NULL || currg->rg_start == 0) {
    printf("[User Read] Lỗi Bảo Mật: Vùng nhớ (rgid=%d) không hợp lệ.\n", rgid);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 3. Kiểm tra tràn viền (Out-of-bounds) */
  addr_t vaddr = currg->rg_start + offset;
  if (vaddr >= currg->rg_end) {
    printf("[User Read] Lỗi: Offset vượt quá giới hạn vùng nhớ.\n");
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 4. Truy cập vật lý thông qua bảng phân trang của User (caller->mm) */
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
  pthread_mutex_lock(&mmvm_lock);
  
  /* 1. NGĂN TRUY CẬP KERNEL: Bắt buộc lấy thông tin vùng nhớ từ caller->mm (User Space) */
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (currg == NULL || cur_vma == NULL || currg->rg_start == 0) /* Invalid memory identify */
  {
    printf("[User Write] Lỗi Bảo Mật: Vùng nhớ (rgid=%d) không hợp lệ.\n", rgid);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 3. Kiểm tra tràn viền (Out-of-bounds) */
  addr_t vaddr = currg->rg_start + offset;
  if (vaddr >= currg->rg_end) {
    printf("[User Write] Lỗi: Offset vượt quá giới hạn vùng nhớ.\n");
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 4. Ghi vật lý thông qua bảng phân trang của User (caller->mm) */
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

/* Hàm hỗ trợ: Tìm và lấy N frame vật lý liên tục từ MEMPHY (RAM) */
int MEMPHY_get_freefp_contig(struct memphy_struct *mp, int num_pages, int *ret_fpn) {
  if (mp == NULL || num_pages <= 0) return -1;
    
  struct framephy_struct *curr = mp->free_fp_list;
  struct framephy_struct *start_node = curr;
  struct framephy_struct *prev = NULL;
  struct framephy_struct *start_prev = NULL;
  int count = 1;
    
  while (curr != NULL && curr->fp_next != NULL) {
    if (curr->fp_next->fpn == curr->fpn + 1) {
      count++;
      if (count == num_pages) {
        *ret_fpn = start_node->fpn;
        
        /* Cắt dải node liên tục này ra khỏi danh sách free_fp_list */
        if (start_prev == NULL) {
          mp->free_fp_list = curr->fp_next->fp_next;
        } else {
          start_prev->fp_next = curr->fp_next->fp_next;
        }
                
        /* Xóa các node đã lấy để tránh leak memory (nếu struct yêu cầu) */
        struct framephy_struct *temp = start_node;
        while (count > 0) {
          struct framephy_struct *next_temp = temp->fp_next;
          free(temp);
          temp = next_temp;
          count--;
        }
        return 0; // Thành công
      }
    } else {
      start_node = curr->fp_next;
      start_prev = curr;
      count = 1;
    }
    prev = curr;
    curr = curr->fp_next;
  }
  return -1; // Không đủ frame liên tục
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

  /* Bảo vệ vùng nhớ Kernel bằng khóa kmm_lock */
  pthread_mutex_lock(&kmm_lock);

  int num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  int start_fpn = -1;

  // 1. Cấp phát Vật Lý Liên Tục
  if (MEMPHY_get_freefp_contig(caller->krnl->mram, num_pages, &start_fpn) < 0) {
    printf("[Kernel __kmalloc] Lỗi: Không đủ %d frame vật lý liên tục.\n", num_pages);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // 2. Tìm Không gian Ảo trống trong os.mm (Kernel Space)
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);
  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;
  addr_t vaddr = -1;

  // Tìm vùng ảo vừa vặn (First-fit cơ bản cho Kernel)
  while (rgit != NULL) {
    if (rgit->rg_start + size <= rgit->rg_end) {
      vaddr = rgit->rg_start;
      rgit->rg_start += size; // Thu hẹp khoảng trống
      break;
    }
    rgit = rgit->rg_next;
  }

  if (vaddr == -1) {
    printf("[Kernel __kmalloc] Lỗi: Hết không gian nhớ ảo cho Kernel.\n");
    //Giải phóng lại start_fpn vào mram nếu cần xử lý lỗi chặt chẽ
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // Đăng ký vùng nhớ ảo mới vào Symbol Table của Kernel
  caller->krnl->mm->symrgtbl[rgid].rg_start = vaddr;
  caller->krnl->mm->symrgtbl[rgid].rg_end = vaddr + size;
  *alloc_addr = vaddr;

  // 3. Ánh xạ Địa chỉ Ảo -> Frame Vật Lý vào krnl_pgd
  for (int i = 0; i < num_pages; i++) {
    int current_fpn = start_fpn + i;
    addr_t current_vaddr = vaddr + i * PAGE_SIZE;
    int pgn = PAGING_PGN(current_vaddr);

#ifdef MM64
    // Hỗ trợ 64-bit Paging: Truyền trực tiếp mm của Kernel, PGN, FPN và caller
    // Hàm pte_set_fpn sẽ tự động rẽ nhánh đa mức (Multi-level) bên trong
    pte_set_fpn(caller->krnl->mm, pgn, current_fpn, caller);
#else
    // Hỗ trợ 32-bit Paging: Điền FPN và set cờ Present (bit 0 = 1)
    uint32_t pte = (current_fpn << PAGING_PTE_FPN_LOBIT) | PAGING_PTE_PRESENT_MASK;
    caller->krnl->krnl_pgd[pgn] = pte;
#endif
  }

  printf("[Kernel __kmalloc] Thành công: Size = %d bytes, VAddr = %lu, Physical Start Frame = %d\n", size, vaddr, start_fpn);

  /* Ghi nhận rgid vào danh sách quản lý của tiến trình */
  if (caller->num_kmaps < 10) {
      caller->kmap_rgid[caller->num_kmaps] = rgid;
      caller->num_kmaps++;
  }

  pthread_mutex_unlock(&kmm_lock);
  return 0; // 0 báo hiệu thành công
}

/*__kfree - free region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbol table)
 */
int __kfree(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&kmm_lock);

  // 1. Xác thực ID vùng nhớ
  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // 2. Lấy thông tin vùng nhớ từ Kernel Space
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->krnl->mm, rgid);
  if (rgnode == NULL || (rgnode->rg_start == 0 && rgnode->rg_end == 0)) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  int num_pages = (rgnode->rg_end - rgnode->rg_start + PAGE_SIZE - 1) / PAGE_SIZE;

  // 3. Thu hồi Frame Vật Lý và Xóa ánh xạ trong bảng phân trang Kernel
  for (int i = 0; i < num_pages; i++) {
    addr_t vaddr = rgnode->rg_start + i * PAGE_SIZE;
    int pgn = PAGING_PGN(vaddr);
    int fpn;

#ifdef MM64
    // Trích xuất PTE 64-bit
    addr_t pte = pte_get_entry(caller->krnl->mm, pgn, caller);
    if (PAGING_PAGE_PRESENT(pte)) {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn); // Trả frame về RAM
      
      // XÓA PTE 64-BIT: Gọi hàm chuyên dụng để reset entry này
      pte_clear_entry(caller->krnl->mm, pgn, caller);
    }
#else
    // Trích xuất và xóa PTE 32-bit
    uint32_t pte = caller->krnl->krnl_pgd[pgn];
    if (PAGING_PAGE_PRESENT(pte)) {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn); // Trả frame về RAM
    }
    caller->krnl->krnl_pgd[pgn] = 0; // Xóa sạch ánh xạ để vùng nhớ thực sự "chết"
#endif
  }

  // 4. Trả lại không gian ảo (Virtual Region) cho Kernel mmap
  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  enlist_vm_freerg_list(caller->krnl->mm, freerg_node);

  // 5. Xóa thông tin Symbol Table
  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  printf("[Kernel __kfree] Thành công: Đã thu hồi %d pages từ Kernel (rgid=%d).\n", num_pages, rgid);

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
      // Ví dụ: pte_set_fpn(caller->krnl->mm, pgn, 0, caller); // (Cần điều chỉnh bit PRESENT = 0 bên trong mm64.c)

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

  if (caller == NULL || size == 0) return -1;

  // 1. Lấy thông tin vùng nhớ Nguồn (User Space) và Đích (Kernel Space)
  struct vm_rg_struct *src_rg = get_symrg_byid(caller->mm, source);
  struct vm_rg_struct *dst_rg = get_symrg_byid(caller->krnl->mm, destination);

  // Kiểm tra sự tồn tại của vùng nhớ
  if (src_rg == NULL || src_rg->rg_start == 0 || 
      dst_rg == NULL || dst_rg->rg_start == 0) {
    printf("[Copy] Lỗi: Vùng nhớ Nguồn hoặc Đích chưa được cấp phát.\n");
    return -1; 
  }

  // Kiểm tra tràn viền (Out of bounds) trước khi thực sự copy
  if (src_rg->rg_start + offset + size > src_rg->rg_end) {
    printf("[Copy] Lỗi: Nguồn (User) không đủ dữ liệu theo size yêu cầu.\n");
    return -1;
  }
  if (dst_rg->rg_start + offset + size > dst_rg->rg_end) {
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

  if (caller == NULL || size == 0) return -1;

  // 1. Lấy thông tin vùng nhớ Nguồn (Kernel Space) và Đích (User Space)
  struct vm_rg_struct *src_rg = get_symrg_byid(caller->krnl->mm, source);
  struct vm_rg_struct *dst_rg = get_symrg_byid(caller->mm, destination);

  // Kiểm tra sự tồn tại
  if (src_rg == NULL || src_rg->rg_start == 0 || 
      dst_rg == NULL || dst_rg->rg_start == 0) {
    printf("[Copy] Lỗi: Vùng nhớ Nguồn hoặc Đích chưa được cấp phát.\n");
    return -1;
  }

  // Kiểm tra tràn viền 
  if (src_rg->rg_start + offset + size > src_rg->rg_end) {
    printf("[Copy] Lỗi: Nguồn (Kernel) không đủ dữ liệu theo size yêu cầu.\n");
    return -1;
  }
  if (dst_rg->rg_start + offset + size > dst_rg->rg_end) {
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
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  pthread_mutex_lock(&kmm_lock);
  
  // 1. Tìm vùng nhớ ảo Kernel
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);
  if (currg == NULL || currg->rg_start == 0) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // 2. Tính địa chỉ ảo và trích xuất Page Number (PGN), Offset
  addr_t vaddr = currg->rg_start + offset;
  if (vaddr >= currg->rg_end) {
    pthread_mutex_unlock(&kmm_lock);
    return -1; // Lỗi: Truy cập ngoài vùng cấp phát
  }

  int pgn = PAGING_PGN(vaddr);
  int off = vaddr % PAGE_SIZE;
  int fpn;

  // 3. Dịch Virtual Address -> Physical Address từ bảng phân trang Kernel
#ifdef MM64
  // Trích xuất PTE 64-bit bằng cách duyệt qua bảng phân trang của Kerne
  addr_t pte = pte_get_entry(caller->krnl->mm, pgn, caller);
  if (!PAGING_PAGE_PRESENT(pte)) {
    // Vùng nhớ Kernel luôn phải nằm trên RAM (được cấp phát cứng qua __kmalloc)
    // Nếu không có mặt (Present = 0), đây là lỗi truy cập bộ nhớ nghiêm trọng
    pthread_mutex_unlock(&kmm_lock);
    return -1; // Page của Kernel luôn phải nằm trên RAM, không được swap
  }
  fpn = PAGING_FPN(pte); // Lấy Frame Number từ 64-bit PTE
#else
  uint32_t pte = caller->krnl->krnl_pgd[pgn];
  if (!PAGING_PAGE_PRESENT(pte)) { // Page của Kernel luôn phải nằm trên RAM
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  fpn = PAGING_FPN(pte);
#endif

  // 4. Truy xuất vào RAM vật lý
  addr_t phyaddr = (fpn * PAGE_SIZE) + off;
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
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  pthread_mutex_lock(&kmm_lock);
  
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);
  if (currg == NULL || currg->rg_start == 0) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  addr_t vaddr = currg->rg_start + offset;
  if (vaddr >= currg->rg_end) {
    pthread_mutex_unlock(&kmm_lock);
    return -1; 
  }

  int pgn = PAGING_PGN(vaddr);
  int off = vaddr % PAGE_SIZE;
  int fpn;

#ifdef MM64
  // Trích xuất PTE 64-bit bằng cách duyệt qua bảng phân trang của Kerne
  addr_t pte = pte_get_entry(caller->krnl->mm, pgn, caller);
  if (!PAGING_PAGE_PRESENT(pte)) {
    // Vùng nhớ Kernel luôn phải nằm trên RAM (được cấp phát cứng qua __kmalloc)
    // Nếu không có mặt (Present = 0), đây là lỗi truy cập bộ nhớ nghiêm trọng
    pthread_mutex_unlock(&kmm_lock);
    return -1; // Page của Kernel luôn phải nằm trên RAM, không được swap
  }
  fpn = PAGING_FPN(pte); // Lấy Frame Number từ 64-bit PTE
#else
  uint32_t pte = caller->krnl->krnl_pgd[pgn];
  if (!PAGING_PAGE_PRESENT(pte)) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  fpn = PAGING_FPN(pte);
#endif

  addr_t phyaddr = (fpn * PAGE_SIZE) + off;
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
  pthread_mutex_lock(&mmvm_lock);
  
  // Trỏ về không gian mm của tiến trình User
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  if (currg == NULL || currg->rg_start == 0) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t vaddr = currg->rg_start + offset;
  if (vaddr >= currg->rg_end) {
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
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  pthread_mutex_lock(&mmvm_lock);
  
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  if (currg == NULL || currg->rg_start == 0) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t vaddr = currg->rg_start + offset;
  if (vaddr >= currg->rg_end) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // pg_setval xử lý quá trình nạp trang/chuyển Swap trong không gian User
  int ret = pg_setval(caller->mm, vaddr, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
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
