/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "mm64.h"
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef MM64
#define MEMPHY_FRAME_SIZE PAGING64_PAGESZ
#else
#define MEMPHY_FRAME_SIZE PAGING_PAGESZ
#endif

/* Leaf lock: never acquire mmvm_lock/kmm_lock or call public MEMPHY APIs
 * while holding it. All live-device list, cursor and storage access goes
 * through this lock, including the contiguous allocator's shared primitive.
 * Initialization requires exclusive ownership of a fresh device. */
static pthread_mutex_t memphy_lock = PTHREAD_MUTEX_INITIALIZER;

static void free_frame_nodes(struct framephy_struct *head)
{
   while (head != NULL) {
      struct framephy_struct *next = head->fp_next;
      free(head);
      head = next;
   }
}

/* Build first, then publish: a failed reformat preserves the old free list.
 * Reformat is forbidden while any frame is checked out. */
static int format_locked(struct memphy_struct *mp, int pagesz)
{
   struct framephy_struct *head = NULL;
   struct framephy_struct **tail = &head;

   if (mp == NULL || pagesz != MEMPHY_FRAME_SIZE || mp->maxsz < 0 ||
       (mp->maxsz > 0 && mp->storage == NULL) || mp->used_fp_list != NULL)
      return -1;

   int count = mp->maxsz / pagesz;
   for (int i = 0; i < count; ++i) {
      struct framephy_struct *node = malloc(sizeof(*node));
      if (node == NULL) {
         free_frame_nodes(head);
         return -1;
      }
      *node = (struct framephy_struct){.fpn = (addr_t)i};
      *tail = node;
      tail = &node->fp_next;
   }

   free_frame_nodes(mp->free_fp_list);
   mp->free_fp_list = head;
   return 0;
}

int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   pthread_mutex_lock(&memphy_lock);
   int status = format_locked(mp, pagesz);
   pthread_mutex_unlock(&memphy_lock);
   return status;
}

int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   if (mp == NULL)
      return -1;

   /* Do not call init again on a live device: it owns storage and nodes.
    * Even a failed initialization leaves a deterministic empty device. */
   *mp = (struct memphy_struct){.rdmflg = randomflg != 0};
   if (max_size > (addr_t)INT_MAX)
      return -1;
   if (max_size == 0)
      return 0;

   mp->storage = malloc((size_t)max_size);
   if (mp->storage == NULL)
      return -1;
   memset(mp->storage, 0, (size_t)max_size);
   mp->maxsz = (int)max_size;

   if (MEMPHY_format(mp, MEMPHY_FRAME_SIZE) != 0) {
      free(mp->storage);
      mp->storage = NULL;
      mp->maxsz = 0;
      return -1;
   }
   return 0;
}

/* Atomically reserve a whole run. Nodes move free -> used without host
 * allocation, so ordinary allocation and release cannot fail due to malloc.
 * Failure does not modify either list or the caller's output. */
int MEMPHY_get_freefp_range(struct memphy_struct *mp, int count, addr_t *retfpn)
{
   if (mp == NULL || retfpn == NULL || count <= 0)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   if (mp->maxsz <= 0 || mp->storage == NULL ||
       count > mp->maxsz / MEMPHY_FRAME_SIZE) {
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   struct framephy_struct **link = &mp->free_fp_list;
   while (*link != NULL) {
      struct framephy_struct *first = *link;
      struct framephy_struct *last = first;
      int length = 1;
      while (length < count && last->fp_next != NULL &&
             last->fp_next->fpn == last->fpn + 1) {
         last = last->fp_next;
         ++length;
      }
      if (length == count) {
         *link = last->fp_next;
         last->fp_next = mp->used_fp_list;
         mp->used_fp_list = first;
         *retfpn = first->fpn;
         pthread_mutex_unlock(&memphy_lock);
         return 0;
      }
      /* No run can start inside the shorter consecutive prefix. */
      link = &last->fp_next;
   }

   pthread_mutex_unlock(&memphy_lock);
   return -1;
}

int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   return MEMPHY_get_freefp_range(mp, 1, retfpn);
}

int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   if (mp == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   if (mp->maxsz <= 0 || mp->storage == NULL ||
       fpn >= (addr_t)(mp->maxsz / MEMPHY_FRAME_SIZE)) {
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   struct framephy_struct **used = &mp->used_fp_list;
   while (*used != NULL && (*used)->fpn != fpn)
      used = &(*used)->fp_next;
   if (*used == NULL) {
      pthread_mutex_unlock(&memphy_lock);
      return -1; /* Not checked out: includes duplicate/unowned releases. */
   }

   struct framephy_struct *node = *used;
   *used = node->fp_next;
   node->owner = NULL;
   struct framephy_struct **available = &mp->free_fp_list;
   while (*available != NULL && (*available)->fpn < fpn)
      available = &(*available)->fp_next;
   node->fp_next = *available;
   *available = node;

   pthread_mutex_unlock(&memphy_lock);
   return 0;
}

static int valid_address(const struct memphy_struct *mp, addr_t addr)
{
   return mp != NULL && mp->storage != NULL && mp->maxsz > 0 &&
          addr < (addr_t)mp->maxsz;
}

int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   pthread_mutex_lock(&memphy_lock);
   if (!valid_address(mp, offset)) {
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }
   mp->cursor = (int)offset;
   pthread_mutex_unlock(&memphy_lock);
   return 0;
}

/* The cursor denotes the last addressed byte, not the next byte. A seek is
 * represented directly rather than spending O(address) host CPU steps. */
static int read_locked(struct memphy_struct *mp, addr_t addr, BYTE *value,
                       int sequential_only)
{
   if (value == NULL || !valid_address(mp, addr) ||
       (sequential_only && mp->rdmflg != 0))
      return -1;
   if (!mp->rdmflg)
      mp->cursor = (int)addr;
   *value = mp->storage[addr];
   return 0;
}

static int write_locked(struct memphy_struct *mp, addr_t addr, BYTE value,
                        int sequential_only)
{
   if (!valid_address(mp, addr) || (sequential_only && mp->rdmflg != 0))
      return -1;
   if (!mp->rdmflg)
      mp->cursor = (int)addr;
   mp->storage[addr] = value;
   return 0;
}

int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   pthread_mutex_lock(&memphy_lock);
   int status = read_locked(mp, addr, value, 1);
   pthread_mutex_unlock(&memphy_lock);
   return status;
}

int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   pthread_mutex_lock(&memphy_lock);
   int status = read_locked(mp, addr, value, 0);
   pthread_mutex_unlock(&memphy_lock);
   return status;
}

int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{
   pthread_mutex_lock(&memphy_lock);
   int status = write_locked(mp, addr, value, 1);
   pthread_mutex_unlock(&memphy_lock);
   return status;
}

int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{
   pthread_mutex_lock(&memphy_lock);
   int status = write_locked(mp, addr, value, 0);
   pthread_mutex_unlock(&memphy_lock);
   return status;
}

int MEMPHY_dump(struct memphy_struct *mp)
{
   if (mp == NULL)
      return -1;
   pthread_mutex_lock(&memphy_lock);
   if (mp->maxsz < 0 || (mp->maxsz > 0 && mp->storage == NULL)) {
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   size_t available = 0;
   for (struct framephy_struct *fp = mp->free_fp_list; fp; fp = fp->fp_next)
      ++available;
   printf("MEMPHY: capacity=%d bytes frame_size=%d total_frames=%d "
          "available_frames=%zu mode=%s cursor=%d\n",
          mp->maxsz, MEMPHY_FRAME_SIZE, mp->maxsz / MEMPHY_FRAME_SIZE,
          available, mp->rdmflg ? "random" : "sequential", mp->cursor);
   printf("  free frames (up to 64):");
   struct framephy_struct *fp = mp->free_fp_list;
   for (int i = 0; fp != NULL && i < 64; ++i, fp = fp->fp_next)
      printf(" %" PRIu64, (uint64_t)fp->fpn);
   if (fp != NULL)
      printf(" ...");
   printf("\n  first bytes (up to 32):");
   for (int i = 0; i < mp->maxsz && i < 32; ++i)
      printf(" %02x", (unsigned int)(unsigned char)mp->storage[i]);
   printf("\n");
   pthread_mutex_unlock(&memphy_lock);
   return 0;
}
