/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#ifndef OSMM_H
#define OSMM_H

/* Configuration must precede every MM64-dependent type, even when this
 * header is the translation unit's first project include. */
#include "os-cfg.h"
#include <stdint.h>

#define MM_PAGING
#define PAGING_MAX_MMSWP 4 /* max number of supported swapped space */
#define PAGING_MAX_SYMTBL_SZ 30

/* 
 * @bksysnet: in long address mode of 64bit or original 32bit
 * the address type need to be redefined
 */

#ifdef MM64
#define ADDR_TYPE uint64_t
#else
#define ADDR_TYPE uint32_t
#endif

typedef char BYTE;
typedef ADDR_TYPE addr_t;
/* Leaf encoding is always 32 bits; addresses and child pointers are not PTEs. */
typedef uint32_t mm_pte_t;
//typedef unsigned int uint32_t;

#ifdef MM64
/* Host-heap table nodes. Definitions are private to the MM64 implementation. */
struct pt_pgd;
struct pt_p4d;
struct pt_pud;
struct pt_pmd;
struct pt_leaf;
#endif

/* 
 * @bksysnet: the format string need to be redefined
 *            based on the address mode
 */
#ifdef MM64
#define FORMAT_ADDR "%lu"
#define FORMATX_ADDR "%16p"
#else
#define FORMAT_ADDR "%d"
#define FORMATX_ADDR "%08x"
#endif

struct pgn_t{
   addr_t pgn;
   struct pgn_t *pg_next; 
};

/*
 *  Memory region struct
 */
struct vm_rg_struct {
   int vmaid;

   addr_t rg_start;
   addr_t rg_end;

   struct vm_rg_struct *rg_next;
};

/*
 *  Memory area struct
 */
struct vm_area_struct {
   unsigned long vm_id;
   addr_t vm_start;
   addr_t vm_end;

   addr_t sbrk;
/*
 * Derived field
 * unsigned long vm_limit = vm_end - vm_start
 */
   struct mm_struct *vm_mm;
   struct vm_rg_struct *vm_freerg_list;
   struct vm_area_struct *vm_next;
};


/* 
 * Kernel cache pool struct
 */
struct kcache_pool_struct {
   int size;
   int align;

#ifdef MM64
   addr_t storage;
#else
   uint32_t storage;
#endif
};


/* 
 * Memory management struct
 */
struct mm_struct {
#ifdef MM64
   /* Sole owning root: private per process, shared through kernel mm only. */
   struct pt_pgd *pgd;
   /* Compatibility placeholders, not roots or current-table caches.
    * Future MM64 initialization must leave these NULL. */
   addr_t *p4d;
   addr_t *pud;
   addr_t *pmd;
   addr_t *pt;
#else
   uint32_t *pgd;
#endif

   struct vm_area_struct *mmap;

   /* Currently we support a fixed number of symbol */
   struct vm_rg_struct symrgtbl[PAGING_MAX_SYMTBL_SZ];

   /* list of free page */
   struct pgn_t *fifo_pgn;

   /* kmem cache pool */
   struct kcache_pool_struct *kcpooltbl;

};


/*
 * FRAME/MEM PHY struct
 */
struct framephy_struct { 
   addr_t fpn;
   struct framephy_struct *fp_next;

   /* Resereed for tracking allocated framed */
   struct mm_struct* owner;
};

struct memphy_struct {
   /* Basic field of data and size */
   BYTE *storage;
   int maxsz;
   
   /* Sequential device fields */ 
   int rdmflg;
   int cursor;

   /* MEMPHY owns these nodes: sorted free list <-> checked-out used list.
    * Each complete physical frame occurs in exactly one list. Mutate only
    * through synchronized MEMPHY APIs; owner is reserved (NULL for now). */
   struct framephy_struct *free_fp_list;
   struct framephy_struct *used_fp_list;
};

#endif
