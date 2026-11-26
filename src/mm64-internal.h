#ifndef MM64_INTERNAL_H
#define MM64_INTERNAL_H

/* Implementation-private host metadata. No allocation, traversal or PTE
 * operations are defined here. Public consumers use opaque root pointers. */
#include "mm64.h"

#ifdef MM64
struct pt_leaf {
   mm_pte_t entry[PAGING64_PT_ENTRIES];
};

struct pt_pmd {
   struct pt_leaf *child[PAGING64_PT_ENTRIES];
};

struct pt_pud {
   struct pt_pmd *child[PAGING64_PT_ENTRIES];
};

struct pt_p4d {
   struct pt_pud *child[PAGING64_PT_ENTRIES];
};

struct pt_pgd {
   /* Initialized by future root creation, destroyed only after quiescence.
    * A zeroed allocation alone does not initialize a pthread mutex. */
   pthread_mutex_t lock;
   struct pt_p4d *child[PAGING64_PT_ENTRIES];
};
#endif

#endif /* MM64_INTERNAL_H */
