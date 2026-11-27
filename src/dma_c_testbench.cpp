/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DMA C Testbench
 *
 * This file has two clearly separated paths:
 *
 *  1. do_dma()  (declared in dma.h) is the OS path. It receives PHYSICAL
 *     offsets into the memory bank (the bank is the OS RAM, attached with
 *     memory_bank_attach()), checks them, runs the RTL copy and reports
 *     success or failure. It copies the data that is already in memory:
 *     it NEVER generates data and never writes the source. It does not
 *     print anything unless built with -DDMA_TRACE=1.
 *
 *  2. dpi_dma_copy() and main() are the testbench path, built only with
 *     DMA_MAIN=1 ("make dma"). They generate a data pattern in the source,
 *     print it, run the copy and verify the result.
 *
 * Build modes:
 *   DMA_MAIN=1 - Build as standalone executable with main()
 *   DMA_MAIN=0 - Build as library module with do_dma()
 */

#ifndef DMA_MAIN
#define DMA_MAIN 0
#endif

#ifndef DMA_TRACE
#define DMA_TRACE 0
#endif

#include "dma.h"
#include "sysbus.h"
#include "dpi_rtl.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdbool>
#include <thread>
#include <atomic>

// Generated DPI functions from RTL
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vdma_top.h"
#include "Vdma_top__Dpi.h"

#if DMA_MAIN == 1
extern "C" {
#include "common.h"
#include "os-mm.h"
}
#endif

/* Do the byte ranges [a, a + len) and [b, b + len) share a byte? */
static bool ranges_overlap(uint64_t a, uint64_t b, uint64_t len)
{
    return a < b + len && b < a + len;
}

/*
 * do_dma - perform DMA function (OS path)
 * caller: process control block (not used, kept for the interface)
 * source: PHYSICAL source offset in the memory bank
 * destination: PHYSICAL destination offset in the memory bank
 * length: number of bytes to copy
 *
 * Returns 0 on success, -1 on failure (no memory bank, a range outside the
 * bank, overlapping ranges, RTL error or an invalid RTL bus access). On
 * failure the destination must not be trusted.
 */
extern "C" int do_dma(struct pcb_t *caller, uint64_t source, uint64_t destination, uint64_t length)
{
    (void)caller;

    if (length == 0) {
        return 0;
    }

    if (!memory_bank_is_available()) {
        printf("do_dma: no memory bank, call memory_bank_attach() first\n");
        return -1;
    }

    if (!memory_bank_check_range(source, length) ||
        !memory_bank_check_range(destination, length)) {
        printf("do_dma: range outside the memory bank: src=0x%llx dst=0x%llx len=%llu\n",
               (unsigned long long)source, (unsigned long long)destination,
               (unsigned long long)length);
        return -1;
    }

    /* Copying a range onto itself moves nothing */
    if (source == destination) {
        return 0;
    }

    /* The RTL copies forward and does not handle overlapping ranges */
    if (ranges_overlap(source, destination, length)) {
        printf("do_dma: source and destination overlap: src=0x%llx dst=0x%llx len=%llu\n",
               (unsigned long long)source, (unsigned long long)destination,
               (unsigned long long)length);
        return -1;
    }

#if DMA_TRACE
    printf("do_dma: src=0x%llx dst=0x%llx len=%llu\n",
           (unsigned long long)source, (unsigned long long)destination,
           (unsigned long long)length);
#endif

    memory_bank_take_bus_errors();      /* forget stale errors */

    /*
     * The RTL moves whole 32-bit words. Measured on the real RTL (M1): a
     * length that is not a multiple of 4 makes it write up to 3 bytes AFTER
     * the destination, bytes that may belong to somebody else. Restoring
     * them afterwards would be racy, so the RTL only copies the part whose
     * length is a multiple of 4 and the 1 to 3 trailing bytes are copied
     * here. Unaligned addresses are not a problem for the RTL.
     */
    uint64_t body = length & ~(uint64_t)3;
    uint64_t tail = length - body;
    int rc = 0;

    if (body != 0) {
        rc = dpi_rtl_dma_copy(source, destination, body);
    }

    int bus_errors = memory_bank_take_bus_errors();

    if (rc != 0 || bus_errors != 0) {
        printf("do_dma: copy failed (rtl=%d, invalid bus accesses=%d)\n", rc, bus_errors);
        return -1;
    }

    if (tail != 0) {
        memcpy((uint8_t *)memory_bank_get_ptr(destination + body),
               (const uint8_t *)memory_bank_get_ptr(source + body), (size_t)tail);
    }

    return 0;
}

#if DMA_MAIN == 1

/* ------------------------------------------------------------------------
 * Testbench path
 * ------------------------------------------------------------------------ */

static const uint64_t GUARD_BYTES = 8;   /* checked around the destination */
static const uint8_t GUARD_FILL = 0xEE;

/* Pattern cycles through 0x01 to min(length, 255) */
static uint8_t pattern_byte(uint64_t i, uint64_t length)
{
    uint64_t period = (length <= 255) ? length : 255;

    return (uint8_t)((i % period) + 1);
}

/* Print 8 bytes of the bank starting at offset (bytes outside it show "--") */
static void dump8(const char *name, uint64_t offset)
{
    printf("  %s[0..7] =", name);
    for (uint64_t k = 0; k < 8; k++) {
        if (memory_bank_check_range(offset + k, 1)) {
            printf(" %02x", *(uint8_t *)memory_bank_get_ptr(offset + k));
        } else {
            printf(" --");
        }
    }
    printf("\n");
}

/*
 * dpi_dma_copy - perform a DMA copy through the RTL and verify it
 * src_offset: source memory offset
 * dst_offset: destination memory offset
 * length: number of bytes to copy
 *
 * Fills the source with a pattern, runs the copy, then checks that the
 * destination holds the pattern, the source is unchanged, the bytes just
 * around the destination were not touched and the RTL made no invalid bus
 * access. Returns 0 only if everything is correct.
 */
int dpi_dma_copy(struct pcb_t *caller, uint64_t src_offset, uint64_t dst_offset, uint64_t length)
{
    (void)caller;

    if (!memory_bank_is_available()) {
        printf("dpi_dma_copy: no memory bank (call memory_bank_init first)\n");
        return -1;
    }
    if (length == 0) {
        printf("dpi_dma_copy: length is 0, nothing to do\n");
        return 0;
    }
    if (!memory_bank_check_range(src_offset, length) ||
        !memory_bank_check_range(dst_offset, length)) {
        printf("dpi_dma_copy: range outside the memory bank (size %zu)\n",
               memory_bank_get_size());
        return -1;
    }
    if (ranges_overlap(src_offset, dst_offset, length)) {
        printf("dpi_dma_copy: source and destination overlap\n");
        return -1;
    }

    printf("dma_testbench mode with dpi_dma_copy from memory bank\n");
    printf("dpi_dma_copy: src=%llu, dst=%llu, len=%llu\n",
           (unsigned long long)src_offset,
           (unsigned long long)dst_offset,
           (unsigned long long)length);

    uint8_t *src = (uint8_t *)memory_bank_get_ptr(src_offset);
    uint8_t *dst = (uint8_t *)memory_bank_get_ptr(dst_offset);

    /* Destination and the bytes right around it: known values */
    uint64_t bank_size = memory_bank_get_size();
    uint64_t after_off = dst_offset + length;
    uint64_t nbefore = (dst_offset < GUARD_BYTES) ? dst_offset : GUARD_BYTES;
    uint64_t nafter = (bank_size - after_off < GUARD_BYTES) ? (bank_size - after_off) : GUARD_BYTES;

    memset(dst - nbefore, GUARD_FILL, nbefore);
    memset(dst, 0, length);
    memset(dst + length, GUARD_FILL, nafter);

    /* Test data (testbench only, the OS path never does this) */
    for (uint64_t i = 0; i < length; i++) {
        src[i] = pattern_byte(i, length);
    }

    uint8_t before[GUARD_BYTES], after[GUARD_BYTES];

    memcpy(before, dst - nbefore, nbefore);
    memcpy(after, dst + length, nafter);

    printf("Before copy:\n");
    dump8("src", src_offset);
    dump8("dst", dst_offset);

    printf("\nCalling RTL DMA...\n");

    memory_bank_take_bus_errors();      /* forget stale errors */

    // Call the RTL DMA function directly with provided parameters
    int result = dpi_rtl_dma_copy(src_offset, dst_offset, length);

    int bus_errors = memory_bank_take_bus_errors();

    printf("\nAfter copy:\n");
    dump8("src", src_offset);
    dump8("dst", dst_offset);

    /* Verification */
    int bad = 0;
    uint64_t first_bad = length;

    for (uint64_t i = 0; i < length; i++) {
        if (dst[i] != pattern_byte(i, length)) {
            first_bad = i;
            break;
        }
    }

    bool src_changed = false;

    for (uint64_t i = 0; i < length; i++) {
        if (src[i] != pattern_byte(i, length)) {
            src_changed = true;
            break;
        }
    }

    /* How many guard bytes did the copy change, before and after the destination? */
    uint64_t over_before = 0;
    uint64_t over_after = 0;

    for (uint64_t k = 0; k < nbefore; k++) {
        if ((dst - nbefore)[k] != before[k]) {
            over_before++;
        }
    }
    for (uint64_t k = 0; k < nafter; k++) {
        if (dst[length + k] != after[k]) {
            over_after++;
        }
    }

    /*
     * Known RTL behaviour (M1): it moves whole 32-bit words, so when the
     * length is not a multiple of 4 it writes up to (4 - length % 4) bytes
     * after the destination. do_dma() avoids this, so here it is reported
     * as information, not as a failure. Anything else is a failure.
     */
    uint64_t max_word_over = (4 - length % 4) % 4;
    bool guards_intact = (over_before == 0 && over_after == 0);
    bool guards_word_tail = (over_before == 0 && over_after > 0 && over_after <= max_word_over);

    printf("\nVerification:\n");
    if (first_bad == length) {
        printf("  data   : PASS\n");
    } else {
        printf("  data   : FAIL, first mismatch at index %llu: got 0x%02x, expected 0x%02x\n",
               (unsigned long long)first_bad, dst[first_bad],
               pattern_byte(first_bad, length));
        bad = 1;
    }
    printf("  source : %s\n", src_changed ? "FAIL, modified by the copy" : "unchanged");
    if (src_changed) {
        bad = 1;
    }
    if (guards_intact) {
        printf("  guards : intact (bytes around the destination untouched)\n");
    } else if (guards_word_tail) {
        printf("  guards : INFO, raw RTL wrote %llu byte(s) after the destination "
               "(it moves whole 32-bit words; do_dma() copies the tail itself)\n",
               (unsigned long long)over_after);
    } else {
        printf("  guards : FAIL, %llu byte(s) changed before and %llu after the destination\n",
               (unsigned long long)over_before, (unsigned long long)over_after);
        bad = 1;
    }
    if (bus_errors == 0) {
        printf("  bus    : no invalid RTL access\n");
    } else {
        printf("  bus    : FAIL, %d invalid RTL access(es)\n", bus_errors);
        bad = 1;
    }
    printf("  rtl    : %s\n", result == 0 ? "done" : "FAIL, copy did not complete");
    printf("  RESULT : %s\n", (result == 0 && !bad) ? "PASS" : "FAIL");

    return (result != 0) ? result : (bad ? -1 : 0);
}

/*
 * Check the OS path: do_dma() must copy the data that is already there,
 * leave the source untouched and fail cleanly on bad arguments.
 */
static int check_os_path(struct pcb_t *caller, uint64_t src, uint64_t dst, uint64_t len)
{
    int fail = 0;

    printf("\n--- OS path: do_dma() must copy existing data ---\n");

    if (len == 0 || !memory_bank_check_range(src, len) ||
        !memory_bank_check_range(dst, len) || ranges_overlap(src, dst, len)) {
        printf("  skipped: arguments are not valid for this check\n");
        return -1;
    }

    uint8_t *ps = (uint8_t *)memory_bank_get_ptr(src);
    uint8_t *pd = (uint8_t *)memory_bank_get_ptr(dst);
    uint8_t *expect = (uint8_t *)malloc(len);

    if (expect == NULL) {
        printf("  skipped: out of memory\n");
        return -1;
    }

    /* "Existing data", different from the testbench pattern */
    for (uint64_t i = 0; i < len; i++) {
        expect[i] = (uint8_t)(0xA5 ^ (i * 7));
        ps[i] = expect[i];
        pd[i] = 0;
    }

    /* Bytes right around the destination: known values that must survive */
    uint64_t bank_size = memory_bank_get_size();
    uint64_t nbefore = (dst < GUARD_BYTES) ? dst : GUARD_BYTES;
    uint64_t nafter = (bank_size - (dst + len) < GUARD_BYTES) ? (bank_size - (dst + len)) : GUARD_BYTES;
    uint8_t before[GUARD_BYTES], after[GUARD_BYTES];

    memset(pd - nbefore, GUARD_FILL, nbefore);
    memset(pd + len, GUARD_FILL, nafter);
    memcpy(before, pd - nbefore, nbefore);
    memcpy(after, pd + len, nafter);

    int rc = do_dma(caller, src, dst, len);

    bool data_ok = (memcmp(pd, expect, len) == 0);
    bool src_ok = (memcmp(ps, expect, len) == 0);
    bool guards_ok = (memcmp(before, pd - nbefore, nbefore) == 0) &&
                     (memcmp(after, pd + len, nafter) == 0);

    printf("  copy existing data : %s\n", (rc == 0 && data_ok) ? "PASS" : "FAIL");
    printf("  source untouched   : %s\n", src_ok ? "PASS" : "FAIL");
    printf("  bytes around dst   : %s\n", guards_ok ? "PASS (untouched)" : "FAIL (written outside the destination)");
    if (rc != 0 || !data_ok || !src_ok || !guards_ok) {
        fail = 1;
    }
    free(expect);

    /* Error handling: nothing may crash and the result must be a failure */
    int rc_range = do_dma(caller, bank_size - 1, dst, 16);
    /* Two ranges shifted by one byte overlap only when len >= 2 */
    int rc_overlap = (len >= 2) ? do_dma(caller, src, src + 1, len) : -1;
    int rc_zero = do_dma(caller, src, dst, 0);

    printf("  range outside bank : %s\n", rc_range != 0 ? "PASS (rejected)" : "FAIL (accepted)");
    if (len >= 2) {
        printf("  overlapping ranges : %s\n", rc_overlap != 0 ? "PASS (rejected)" : "FAIL (accepted)");
    } else {
        printf("  overlapping ranges : skipped (needs length >= 2)\n");
    }
    printf("  length 0           : %s\n", rc_zero == 0 ? "PASS (no-op)" : "FAIL");
    if (rc_range == 0 || rc_overlap == 0 || rc_zero != 0) {
        fail = 1;
    }

    printf("  RESULT : %s\n", fail ? "FAIL" : "PASS");
    return fail ? -1 : 0;
}

/* main function for independent standalone dma_testbench build
 * [Usage]: ./dma_testbench [source_offset] [destination_offset] [length]
 * source: source memory offset
 * destination: destination memory offset
 * length: number of bytes to copy
 */
int main(int argc, char** argv) {
    // Default test parameters
    uint64_t source = 0x1000;
    uint64_t destination = 0x2000;
    uint64_t length = 512;

    /*
     *  Only accept command line arguments if provided
     *  [Usage]: ./dma_testbench [source_offset] [destination_offset] [length]
     */
    if (argc >= 4) {
        source = strtoull(argv[1], NULL, 0);
        destination = strtoull(argv[2], NULL, 0);
        length = strtoull(argv[3], NULL, 0);
    }

    printf("Running dma_testbench source=0x%llx, destination=0x%llx, length=%llu\n",
           (unsigned long long)source, (unsigned long long)destination, (unsigned long long)length);

    /*
     * Initialize the system memory bank
     * This is necessary to independent standalone dma_testbench build .
     */
    memory_bank_init(1024 * 1024);

    /* Initialize the internal Verilator RTL simulation
     * This is necessary to ensure that the RTL simulation is ready for DPI calls.
     */
    if (rtl_dma_init_simulation() != 0) {
        printf("Cannot initialize RTL simulation\n");
        return 1;
    }

    /*
    * Create process control block for the caller
    * Properly initialize all required fields
    */
    struct pcb_t *caller = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));
    caller->krnl = (struct krnl_t *)calloc(1, sizeof(struct krnl_t));
    caller->krnl->mm = (struct mm_struct *)calloc(1, sizeof(struct mm_struct));
    caller->krnl->mram = (struct memphy_struct *)calloc(1, sizeof(struct memphy_struct));
    caller->krnl->active_mswp = (struct memphy_struct *)calloc(1, sizeof(struct memphy_struct));
    caller->mm = caller->krnl->mm;
    caller->pid = 0;  // Set a default PID

    /* Testbench path: pattern, prints and verification */
    int result = dpi_dma_copy(caller, source, destination, length);

    /* OS path: copies the data that is already in memory */
    if (check_os_path(caller, source, destination, length) != 0) {
        result = -1;
    }

    // Cleanup
    free(caller->krnl->active_mswp);
    free(caller->krnl->mram);
    free(caller->krnl->mm);
    free(caller->krnl);
    free(caller);

    rtl_dma_cleanup_simulation();
    memory_bank_cleanup();

    return result != 0;
}
#endif
