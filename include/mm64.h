#ifndef MM64_H
#define MM64_H

#include "mm.h"
#define MM64_BITS_PER_LONG 64
#define PAGING64_PT_INDEX_BITS 9
#define PAGING64_PT_ENTRIES (1U << PAGING64_PT_INDEX_BITS)

#define PAGING64_CPU_BUS_WIDTH 57 /* Five 9-bit indices and a 12-bit offset */
#define PAGING64_PAGESZ  4096      /* 4 KiB; 12 offset bits */

#define PAGING64_USER_START ((addr_t)PAGING64_PAGESZ)
#define PAGING64_USER_END   ((addr_t)(BIT_ULL(PAGING64_CPU_BUS_WIDTH - 1) - 1))
#define PAGING64_KERNEL_START (~PAGING64_USER_END)
#define PAGING64_KERNEL_END   UINT64_MAX

#define GENMASK64(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (MM64_BITS_PER_LONG  - (h) - 1)))

#define PAGING64_MAX_PGN  (DIV_ROUND_UP(BIT_ULL(21),PAGING64_PAGESZ))

/* Physical capacity a PTE can address (derived from the mm.h PTE masks):
 * RAM  : 16-bit FPN        -> 65536 frames   = 256 MiB
 * SWAP : 21-bit swp offset -> 2097152 frames = 8 GiB per device
 * Devices larger than this cannot be fully referenced by page tables. */
#define PAGING64_MAX_RAM_FRAMES \
	(((unsigned long long)(PAGING_PTE_FPN_MASK >> PAGING_PTE_FPN_LOBIT)) + 1)
#define PAGING64_MAX_RAMSZ (PAGING64_MAX_RAM_FRAMES * PAGING64_PAGESZ)
#define PAGING64_MAX_SWP_FRAMES \
	(((unsigned long long)(PAGING_PTE_SWPOFF_MASK >> PAGING_PTE_SWPOFF_LOBIT)) + 1)
#define PAGING64_MAX_SWPSZ (PAGING64_MAX_SWP_FRAMES * PAGING64_PAGESZ)

/* Default base of vma0 created by init_mm() (Stage 2).
 * User: the first page is left unmapped (macOS-style __PAGEZERO), so no
 *       region starts at address 0 (libmem treats rg_start == 0 as "unset")
 *       and NULL dereferences fault.
 * Kernel: bottom of the canonical kernel half (VA bits 56..63 = 1).
 * The 57-bit layout is defined once, by PAGING64_USER_* / PAGING64_KERNEL_*
 * above; common.h derives USER_SPACE_* / KERNEL_SPACE_* from them. */
#define PAGING64_USER_VMA0_START   PAGING64_USER_START
#define PAGING64_KERNEL_VMA0_START PAGING64_KERNEL_START
#define PAGING64_PAGE_ALIGNSZ(sz) (DIV_ROUND_UP(sz,PAGING64_PAGESZ)*PAGING64_PAGESZ)


/* OFFSET */
#define PAGING64_ADDR_OFFST_HIBIT 11
#define PAGING64_ADDR_OFFST_LOBIT 0

/* PT */
#define PAGING64_ADDR_PT_HIBIT 20
#define PAGING64_ADDR_PT_LOBIT 12
#define PAGING64_ADDR_PT_SHIFT 12

/* PMD */
#define PAGING64_ADDR_PMD_HIBIT 29
#define PAGING64_ADDR_PMD_LOBIT 21

/* PUD */
#define PAGING64_ADDR_PUD_HIBIT 38
#define PAGING64_ADDR_PUD_LOBIT 30

/* P4D */
#define PAGING64_ADDR_P4D_HIBIT 47
#define PAGING64_ADDR_P4D_LOBIT 39

/* PGD */
#define PAGING64_ADDR_PGD_HIBIT 56
#define PAGING64_ADDR_PGD_LOBIT 48

/* Extract PGD Entry */
#define PAGING64_ADDR_OFFST(addr) GETVAL(addr,PAGING64_ADDR_OFFST_MASK,PAGING64_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT(addr)   ((addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PT_MASK,PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD(addr)   ((addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PMD_MASK,PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD(addr)   ((addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PUD_MASK,PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D(addr)   ((addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT)
//GETVAL(addr,PAGING64_ADDR_P4D_MASK,PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD(addr)   ((addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PGD_MASK,PAGING64_ADDR_PGD_LOBIT)


/* Masks */
#define PAGING64_ADDR_OFFST_MASK  GENMASK64(PAGING64_ADDR_OFFST_HIBIT,PAGING64_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT_MASK  GENMASK64(PAGING64_ADDR_PT_HIBIT,PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD_MASK  GENMASK64(PAGING64_ADDR_PMD_HIBIT,PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD_MASK  GENMASK64(PAGING64_ADDR_PUD_HIBIT,PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D_MASK  GENMASK64(PAGING64_ADDR_P4D_HIBIT,PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD_MASK  GENMASK64(PAGING64_ADDR_PGD_HIBIT,PAGING64_ADDR_PGD_LOBIT)

#ifdef MM64
#ifdef __cplusplus
extern "C" {
#endif

/* Page-table tree lifecycle (mm64.c). The pte_* API in mm.h requires a
 * root created by mm64_pgd_create(). Destroy frees table nodes only:
 * release frames/swap slots referenced by the PTEs first. */
int mm64_pgd_create(struct mm_struct *mm);
void mm64_pgd_destroy(struct mm_struct *mm);

/* Visit every non-zero PTE of @mm in increasing pgn order (pgn is the
 * canonical page number, sign-extended for the kernel half).  The tree
 * lock is held during the walk: @fn must not call pte_* on the same mm
 * (MEMPHY_* is fine: pgd->lock -> memphy_lock).
 * Return 0, or -1 if @mm/@fn is NULL or @mm has no root. */
typedef void (*mm64_pte_visit_fn)(addr_t pgn, addr_t pte, void *ctx);
int mm64_pt_foreach(struct mm_struct *mm, mm64_pte_visit_fn fn, void *ctx);

/* Oldest evictable page of @mm's FIFO (resident, not DMA-pinned):
 * returns the link to its node (not detached) and its frame in *vicfpn,
 * or NULL when no page can be evicted.  Caller holds mmvm_lock. */
struct pgn_t **mm64_fifo_victim(struct mm_struct *mm, addr_t *vicfpn);

#ifdef __cplusplus
}
#endif
#endif /* MM64 */


#endif
