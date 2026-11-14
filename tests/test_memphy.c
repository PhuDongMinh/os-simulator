#define _POSIX_C_SOURCE 200809L
#include "mm64.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FRAME PAGING64_PAGESZ
#define MAX_TEST_FRAMES 128

/* Linker wrapping injects failure into the actual production allocations. */
void *__real_malloc(size_t size);
void __real_free(void *ptr);
static long fail_after = -1;
static long live_allocations;

void *__wrap_malloc(size_t size)
{
   if (fail_after == 0) return NULL;
   if (fail_after > 0) --fail_after;
   void *ptr = __real_malloc(size);
   if (ptr != NULL) ++live_allocations;
   return ptr;
}

void __wrap_free(void *ptr)
{
   if (ptr != NULL) --live_allocations;
   __real_free(ptr);
}

/* Test-only teardown: production process/device destruction is Stage E.
 * No workers may be accessing this device during inspection/teardown. */
static void dispose(struct memphy_struct *mp)
{
   struct framephy_struct *lists[] = {mp->free_fp_list, mp->used_fp_list};
   for (size_t i = 0; i < 2; ++i) {
      while (lists[i]) {
         struct framephy_struct *next = lists[i]->fp_next;
         free(lists[i]);
         lists[i] = next;
      }
   }
   free(mp->storage);
   *mp = (struct memphy_struct){0};
}

/* Check partition, uniqueness, ordering, complete-frame count and reserved
 * owner state against a separate model of outstanding allocations. */
static void check_state(struct memphy_struct *mp, const unsigned char *owned)
{
   int n = mp->maxsz / FRAME;
   unsigned char seen[MAX_TEST_FRAMES] = {0};
   assert(n <= MAX_TEST_FRAMES);
   struct framephy_struct *lists[] = {mp->free_fp_list, mp->used_fp_list};
   int total = 0;
   for (int used = 0; used < 2; ++used) {
      addr_t previous = 0;
      int first = 1;
      for (struct framephy_struct *fp = lists[used]; fp; fp = fp->fp_next) {
         assert(fp->fpn < (addr_t)n);
         assert(!seen[fp->fpn]); /* Also catches list cycles. */
         seen[fp->fpn] = 1;
         assert((owned[fp->fpn] != 0) == used);
         assert(fp->owner == NULL);
         if (!used && !first) assert(previous < fp->fpn);
         previous = fp->fpn;
         first = 0;
         ++total;
      }
   }
   assert(total == n);
   for (int i = 0; i < n; ++i) assert(seen[i]);
}

static void check_empty(struct memphy_struct *mp, int random)
{
   assert(mp->storage == NULL && mp->maxsz == 0);
   assert(mp->rdmflg == random && mp->cursor == 0);
   assert(mp->free_fp_list == NULL && mp->used_fp_list == NULL);
}

static void test_zero_and_subpage(void)
{
   struct memphy_struct mp;
   memset(&mp, 0xa5, sizeof(mp));
   assert(init_memphy(&mp, 0, 1) == 0);
   check_empty(&mp, 1);
   addr_t fpn = 77;
   int first = 77;
   BYTE byte = 42;
   assert(MEMPHY_get_freefp(&mp, &fpn) == -1 && fpn == 77);
   assert(MEMPHY_get_freefp_contig(&mp, 1, &first) == -1 && first == 77);
   assert(MEMPHY_put_freefp(&mp, 0) == -1);
   assert(MEMPHY_read(&mp, 0, &byte) == -1 && byte == 42);
   assert(MEMPHY_write(&mp, 0, 3) == -1);
   assert(MEMPHY_mv_csr(&mp, 0) == -1 && mp.cursor == 0);
   assert(MEMPHY_format(&mp, FRAME) == 0);
   dispose(&mp);

   assert(init_memphy(&mp, FRAME - 1, 0) == 0);
   assert(mp.free_fp_list == NULL && mp.used_fp_list == NULL);
   assert(MEMPHY_get_freefp(&mp, &fpn) == -1);
   assert(MEMPHY_write(&mp, FRAME - 2, 55) == 0);
   assert(MEMPHY_read(&mp, FRAME - 2, &byte) == 0 && byte == 55);
   dispose(&mp);
}

static void test_single_frame(void)
{
   struct memphy_struct mp;
   unsigned char owned[MAX_TEST_FRAMES] = {0};
   assert(init_memphy(&mp, FRAME, 1) == 0);
   check_state(&mp, owned);
   assert(mp.free_fp_list->fp_next == NULL);
   assert(MEMPHY_put_freefp(&mp, 0) == -1); /* Never allocated. */
   addr_t fpn = 91;
   assert(MEMPHY_get_freefp(&mp, &fpn) == 0 && fpn == 0);
   owned[0] = 1;
   check_state(&mp, owned);
   assert(MEMPHY_get_freefp(&mp, &fpn) == -1 && fpn == 0);
   assert(MEMPHY_format(&mp, FRAME) == -1); /* Cannot revoke ownership. */
   check_state(&mp, owned);
   assert(MEMPHY_put_freefp(&mp, 0) == 0);
   owned[0] = 0;
   assert(MEMPHY_put_freefp(&mp, 0) == -1);
   assert(MEMPHY_put_freefp(&mp, 1) == -1);
   assert(MEMPHY_put_freefp(&mp, (addr_t)-1) == -1);
   int first = -7;
   assert(MEMPHY_get_freefp_contig(&mp, 1, &first) == 0 && first == 0);
   owned[0] = 1;
   check_state(&mp, owned);
   assert(MEMPHY_put_freefp(&mp, 0) == 0);
   owned[0] = 0;
   check_state(&mp, owned);
   dispose(&mp);
}

static void test_exhaustion_and_contiguous(void)
{
   struct memphy_struct mp;
   unsigned char owned[MAX_TEST_FRAMES] = {0};
   assert(init_memphy(&mp, 8 * FRAME + 17, 1) == 0);
   for (int i = 0; i < mp.maxsz; ++i) assert(mp.storage[i] == 0);
   for (int i = 0; i < 8; ++i) {
      addr_t fpn = 100;
      assert(MEMPHY_get_freefp(&mp, &fpn) == 0 && fpn == (addr_t)i);
      owned[fpn] = 1;
      check_state(&mp, owned);
   }
   addr_t fpn = 123;
   assert(MEMPHY_get_freefp(&mp, &fpn) == -1 && fpn == 123);
   int release[] = {6, 2, 4, 0};
   for (int i = 0; i < 4; ++i) {
      assert(MEMPHY_put_freefp(&mp, release[i]) == 0);
      owned[release[i]] = 0;
   }
   check_state(&mp, owned);
   int first = -7;
   struct framephy_struct *free_head = mp.free_fp_list;
   struct framephy_struct *used_head = mp.used_fp_list;
   assert(MEMPHY_get_freefp_contig(&mp, 2, &first) == -1 && first == -7);
   assert(mp.free_fp_list == free_head && mp.used_fp_list == used_head);
   check_state(&mp, owned); /* Enough frames in total, no adjacent pair. */
   assert(MEMPHY_put_freefp(&mp, 3) == 0);
   owned[3] = 0;
   assert(MEMPHY_get_freefp_contig(&mp, 3, &first) == 0 && first == 2);
   for (int i = 2; i <= 4; ++i) owned[i] = 1;
   check_state(&mp, owned); /* Run begins after a nonmatching prefix. */
   for (int i = 7; i >= 0; --i) {
      if (owned[i]) assert(MEMPHY_put_freefp(&mp, i) == 0);
      owned[i] = 0;
   }
   assert(MEMPHY_get_freefp_contig(&mp, 9, &first) == -1 && first == 2);
   assert(MEMPHY_get_freefp_contig(&mp, 8, &first) == 0 && first == 0);
   memset(owned, 1, 8);
   check_state(&mp, owned);
   assert(MEMPHY_get_freefp(&mp, &fpn) == -1);
   dispose(&mp);
}

static void test_io_and_modes(void)
{
   for (int random = 0; random < 2; ++random) {
      struct memphy_struct mp;
      assert(init_memphy(&mp, FRAME, random ? 7 : 0) == 0);
      BYTE byte = 77;
      assert(MEMPHY_write(&mp, 0, (BYTE)0x81) == 0);
      assert(MEMPHY_write(&mp, FRAME - 1, 99) == 0);
      assert(MEMPHY_read(&mp, 0, &byte) == 0 && (unsigned char)byte == 0x81);
      assert(MEMPHY_read(&mp, FRAME - 1, &byte) == 0 && byte == 99);
      assert(mp.cursor == (random ? 0 : FRAME - 1));
      assert(MEMPHY_mv_csr(&mp, 13) == 0 && mp.cursor == 13);
      byte = 77;
      assert(MEMPHY_read(&mp, FRAME, &byte) == -1 && byte == 77);
      assert(MEMPHY_read(&mp, (addr_t)-1, &byte) == -1 && byte == 77);
      assert(MEMPHY_read(&mp, 0, NULL) == -1);
      assert(MEMPHY_write(&mp, FRAME, 4) == -1);
      assert(MEMPHY_write(&mp, (addr_t)-1, 4) == -1);
      assert(MEMPHY_mv_csr(&mp, FRAME) == -1 && mp.cursor == 13);
      assert(MEMPHY_seq_read(&mp, FRAME, &byte) == -1 && byte == 77);
      assert(MEMPHY_seq_write(&mp, FRAME, 4) == -1);
      assert(mp.cursor == 13);
      if (random) {
         assert(MEMPHY_seq_read(&mp, 3, &byte) == -1 && byte == 77);
         assert(MEMPHY_seq_write(&mp, 3, 88) == -1 && mp.cursor == 13);
      } else {
         assert(MEMPHY_seq_write(&mp, 3, 88) == 0 && mp.cursor == 3);
         assert(MEMPHY_seq_read(&mp, 3, &byte) == 0 && byte == 88);
         assert(MEMPHY_seq_read(&mp, 3, NULL) == -1 && mp.cursor == 3);
      }
      assert((unsigned char)mp.storage[0] == 0x81 && mp.storage[FRAME - 1] == 99);
      dispose(&mp);
   }
}

static void test_bad_arguments(void)
{
   struct memphy_struct mp;
   assert(init_memphy(NULL, FRAME, 1) == -1);
   assert(init_memphy(&mp, (addr_t)INT_MAX + 1, 0) == -1);
   check_empty(&mp, 0);
   assert(init_memphy(&mp, (addr_t)-1, 1) == -1);
   check_empty(&mp, 1);
   assert(init_memphy(&mp, FRAME, 1) == 0);
   addr_t fpn = 93;
   int first = 93;
   BYTE byte = 72;
   assert(MEMPHY_format(NULL, FRAME) == -1);
   assert(MEMPHY_format(&mp, 0) == -1);
   assert(MEMPHY_format(&mp, -1) == -1);
   assert(MEMPHY_format(&mp, FRAME / 2) == -1);
   assert(MEMPHY_get_freefp(NULL, &fpn) == -1 && fpn == 93);
   assert(MEMPHY_get_freefp(&mp, NULL) == -1);
   assert(MEMPHY_get_freefp_range(&mp, 0, &fpn) == -1);
   assert(MEMPHY_get_freefp_range(&mp, -1, &fpn) == -1);
   assert(MEMPHY_get_freefp_range(&mp, INT_MAX, &fpn) == -1 && fpn == 93);
   assert(MEMPHY_get_freefp_contig(NULL, 1, &first) == -1 && first == 93);
   assert(MEMPHY_get_freefp_contig(&mp, 1, NULL) == -1);
   assert(MEMPHY_get_freefp_contig(&mp, 0, &first) == -1 && first == 93);
   assert(MEMPHY_get_freefp_contig(&mp, -1, &first) == -1 && first == 93);
   assert(MEMPHY_put_freefp(NULL, 0) == -1);
   assert(MEMPHY_read(NULL, 0, &byte) == -1 && byte == 72);
   assert(MEMPHY_write(NULL, 0, 1) == -1);
   assert(MEMPHY_seq_read(NULL, 0, &byte) == -1 && byte == 72);
   assert(MEMPHY_seq_write(NULL, 0, 1) == -1);
   assert(MEMPHY_mv_csr(NULL, 0) == -1);
   assert(MEMPHY_dump(NULL) == -1);
   unsigned char owned[MAX_TEST_FRAMES] = {0};
   check_state(&mp, owned);
   dispose(&mp);
}

static void test_allocation_failures(void)
{
   struct memphy_struct mp;
   /* Storage plus four frame nodes: fail at every allocation position. */
   for (long position = 0; position < 5; ++position) {
      long before = live_allocations;
      fail_after = position;
      assert(init_memphy(&mp, 4 * FRAME, 1) == -1);
      fail_after = -1;
      check_empty(&mp, 1);
      assert(live_allocations == before);
   }
   assert(init_memphy(&mp, 4 * FRAME, 1) == 0);
   unsigned char owned[MAX_TEST_FRAMES] = {0};
   for (long position = 0; position < 4; ++position) {
      long before = live_allocations;
      struct framephy_struct *head = mp.free_fp_list;
      fail_after = position;
      assert(MEMPHY_format(&mp, FRAME) == -1);
      fail_after = -1;
      assert(mp.free_fp_list == head && live_allocations == before);
      check_state(&mp, owned);
   }
   assert(MEMPHY_format(&mp, FRAME) == 0);
   check_state(&mp, owned);
   /* Runtime allocation/release deliberately require no host allocation. */
   fail_after = 0;
   addr_t fpn;
   int first;
   assert(MEMPHY_get_freefp(&mp, &fpn) == 0 && fpn == 0);
   assert(MEMPHY_get_freefp_contig(&mp, 2, &first) == 0 && first == 1);
   assert(MEMPHY_put_freefp(&mp, fpn) == 0);
   assert(MEMPHY_put_freefp(&mp, 1) == 0);
   assert(MEMPHY_put_freefp(&mp, 2) == 0);
   fail_after = -1;
   check_state(&mp, owned);
   dispose(&mp);
}

static void test_model_cycles(void)
{
   struct memphy_struct mp;
   unsigned char owned[MAX_TEST_FRAMES] = {0};
   assert(init_memphy(&mp, 64 * FRAME, 1) == 0);
   unsigned int seed = 71;
   for (int step = 0; step < 10000; ++step) {
      seed = seed * 1664525u + 1013904223u;
      if (seed & 0x100u) {
         int count = (int)((seed >> 16) % 4) + 1;
         int expected = -1;
         for (int i = 0; i + count <= 64; ++i) {
            int j = 0;
            while (j < count && !owned[i + j]) ++j;
            if (j == count) { expected = i; break; }
         }
         int first = -9;
         int status = MEMPHY_get_freefp_contig(&mp, count, &first);
         if (expected < 0) assert(status == -1 && first == -9);
         else {
            assert(status == 0 && first == expected);
            for (int i = 0; i < count; ++i) owned[first + i] = 1;
         }
      } else {
         int fpn = (int)((seed >> 16) % 64);
         assert(MEMPHY_put_freefp(&mp, fpn) == (owned[fpn] ? 0 : -1));
         owned[fpn] = 0;
      }
      check_state(&mp, owned);
   }
   dispose(&mp);
}

struct worker_args {
   struct memphy_struct *mp;
   pthread_barrier_t *barrier;
   unsigned char *owned;
   pthread_mutex_t *model_lock;
   int id;
   int churn;
};

static void *allocate_worker(void *arg)
{
   struct worker_args *a = arg;
   pthread_barrier_wait(a->barrier);
   for (int iteration = 0; iteration < 1000; ++iteration) {
      int count = a->churn ? (iteration + a->id) % 3 + 1 : 1;
      int first = -1;
      int status;
      if (count == 1) {
         addr_t fpn = 0;
         status = MEMPHY_get_freefp(a->mp, &fpn);
         if (status == 0) first = (int)fpn;
      } else status = MEMPHY_get_freefp_contig(a->mp, count, &first);
      if (status != 0) {
         if (!a->churn) break;
         continue;
      }
      pthread_mutex_lock(a->model_lock);
      for (int i = 0; i < count; ++i) {
         assert(first + i < 64 && !a->owned[first + i]);
         a->owned[first + i] = 1;
      }
      pthread_mutex_unlock(a->model_lock);
      if (a->churn) {
         assert(MEMPHY_write(a->mp, (addr_t)first * FRAME, (BYTE)a->id) == 0);
         BYTE byte;
         assert(MEMPHY_read(a->mp, (addr_t)first * FRAME, &byte) == 0);
         assert(byte == (BYTE)a->id);
         pthread_mutex_lock(a->model_lock);
         for (int i = count - 1; i >= 0; --i) {
            a->owned[first + i] = 0;
            assert(MEMPHY_put_freefp(a->mp, first + i) == 0);
         }
         pthread_mutex_unlock(a->model_lock);
      }
   }
   return NULL;
}

static void test_concurrent(void)
{
   for (int churn = 0; churn < 2; ++churn) {
      struct memphy_struct mp;
      unsigned char owned[MAX_TEST_FRAMES] = {0};
      assert(init_memphy(&mp, 64 * FRAME, 0) == 0);
      pthread_barrier_t barrier;
      pthread_mutex_t model_lock;
      assert(pthread_barrier_init(&barrier, NULL, 8) == 0);
      assert(pthread_mutex_init(&model_lock, NULL) == 0);
      pthread_t threads[8];
      struct worker_args args[8];
      for (int i = 0; i < 8; ++i) {
         args[i] = (struct worker_args){&mp, &barrier, owned, &model_lock, i, churn};
         assert(pthread_create(&threads[i], NULL, allocate_worker, &args[i]) == 0);
      }
      for (int i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
      check_state(&mp, owned);
      for (int i = 0; i < 64; ++i) assert(owned[i] == !churn);
      assert(pthread_barrier_destroy(&barrier) == 0);
      assert(pthread_mutex_destroy(&model_lock) == 0);
      dispose(&mp);
   }
}

static void test_device_isolation(void)
{
   struct memphy_struct a, b;
   assert(init_memphy(&a, FRAME, 1) == 0);
   assert(init_memphy(&b, FRAME, 1) == 0);
   addr_t fpn;
   assert(MEMPHY_get_freefp(&a, &fpn) == 0 && fpn == 0);
   assert(MEMPHY_put_freefp(&b, fpn) == -1);
   assert(MEMPHY_write(&a, 0, 55) == 0);
   BYTE byte = 77;
   assert(MEMPHY_read(&b, 0, &byte) == 0 && byte == 0);
   dispose(&a);
   dispose(&b);
}

static void test_dump(void)
{
   struct memphy_struct mp;
   assert(init_memphy(&mp, 128 * FRAME, 0) == 0);
   addr_t fpn;
   assert(MEMPHY_get_freefp(&mp, &fpn) == 0);
   assert(MEMPHY_write(&mp, 29, 83) == 0);
   struct memphy_struct before = mp;
   struct framephy_struct *free_head = mp.free_fp_list;
   struct framephy_struct *used_head = mp.used_fp_list;
   /* Capture output to check its bounded size and contents. */
   FILE *capture = tmpfile();
   assert(capture != NULL);
   fflush(stdout);
   int saved = dup(STDOUT_FILENO);
   assert(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
   assert(MEMPHY_dump(&mp) == 0);
   fflush(stdout);
   assert(dup2(saved, STDOUT_FILENO) >= 0);
   close(saved);
   long size = ftell(capture);
   assert(size > 0 && size < 2048);
   rewind(capture);
   char output[2048] = {0};
   assert(fread(output, 1, (size_t)size, capture) == (size_t)size);
   assert(strstr(output, "capacity=524288") != NULL);
   assert(strstr(output, "frame_size=4096") != NULL);
   assert(strstr(output, "available_frames=127") != NULL);
   assert(strstr(output, " ...") != NULL);
   fclose(capture);
   assert(mp.storage == before.storage && mp.maxsz == before.maxsz);
   assert(mp.rdmflg == before.rdmflg && mp.cursor == before.cursor);
   assert(mp.free_fp_list == free_head && mp.used_fp_list == used_head);
   assert(mp.storage[29] == 83);
   unsigned char owned[MAX_TEST_FRAMES] = {1};
   check_state(&mp, owned);
   dispose(&mp);
}

#define RUN(test) do { test(); assert(live_allocations == 0); \
   printf("PASS %s\n", #test); } while (0)

int main(void)
{
   RUN(test_zero_and_subpage);
   RUN(test_single_frame);
   RUN(test_exhaustion_and_contiguous);
   RUN(test_io_and_modes);
   RUN(test_bad_arguments);
   RUN(test_allocation_failures);
   RUN(test_model_cycles);
   RUN(test_concurrent);
   RUN(test_device_isolation);
   RUN(test_dump);
   puts("Stage A: all 10 test groups passed.");
   return 0;
}
