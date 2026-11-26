/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * system bus - memory banks seen by the DMA RTL simulation.
 *
 * The RTL reaches memory only through the two DPI callbacks below, and they
 * always use bank 0. Bank 0 is either memory allocated here
 * (memory_bank_init, used by the standalone testbench) or memory that
 * already exists (memory_bank_attach, used by the OS to expose its RAM).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "sysbus.h"

// Memory bank structure
typedef struct {
    uint8_t* data;
    size_t size;
    int initialized;
    int owned;          // 1: allocated here and freed by cleanup, 0: attached
} memory_bank_t_struct;

// Array of memory banks
static memory_bank_t_struct g_banks[MAX_MEMORY_BANKS] = {0};
static int g_bank_count = 0;

// Number of invalid RTL accesses since the last memory_bank_take_bus_errors()
static atomic_int g_bus_errors = 0;

// Optional completion hook, see dma_set_done_hook()
static _Atomic(void (*)(void)) g_done_hook = NULL;

// Find the first free bank slot, or -1 if all are in use
static int find_free_slot(void) {
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (!g_banks[i].initialized) {
            return i;
        }
    }
    printf("Maximum number of memory banks (%d) reached\n", MAX_MEMORY_BANKS);
    return -1;
}

// Initialize a memory bank with given size in bytes
memory_bank_t memory_bank_init(size_t size_bytes) {
    int free_slot = find_free_slot();

    if (free_slot == -1) {
        return MEMORY_BANK_INVALID;
    }

    if (size_bytes == 0) {
        printf("Invalid bank size: 0\n");
        return MEMORY_BANK_INVALID;
    }

    uint8_t* data = (uint8_t*)malloc(size_bytes);
    if (!data) {
        printf("Failed to allocate memory bank of %zu bytes\n", size_bytes);
        return MEMORY_BANK_INVALID;
    }

    memset(data, 0, size_bytes);
    g_banks[free_slot].data = data;
    g_banks[free_slot].size = size_bytes;
    g_banks[free_slot].initialized = 1;
    g_banks[free_slot].owned = 1;
    g_bank_count++;

    printf("Memory bank [%d] initialized: %zu bytes at address %p\n", free_slot, size_bytes, (void*)data);
    return free_slot;
}

// Attach existing memory as a bank (no allocation, no copy)
memory_bank_t memory_bank_attach(void *base, size_t size_bytes) {
    if (base == NULL || size_bytes == 0) {
        printf("Invalid external memory bank (base=%p, size=%zu)\n", base, size_bytes);
        return MEMORY_BANK_INVALID;
    }

    int free_slot = find_free_slot();

    if (free_slot == -1) {
        return MEMORY_BANK_INVALID;
    }

    g_banks[free_slot].data = (uint8_t*)base;
    g_banks[free_slot].size = size_bytes;
    g_banks[free_slot].initialized = 1;
    g_banks[free_slot].owned = 0;
    g_bank_count++;

    printf("Memory bank [%d] attached: %zu bytes at address %p\n", free_slot, size_bytes, base);
    return free_slot;
}

// Cleanup memory bank
void memory_bank_cleanup(void) {
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            // Only free what this module allocated
            if (g_banks[i].owned && g_banks[i].data) {
                free(g_banks[i].data);
            }
            g_banks[i].data = NULL;
            g_banks[i].size = 0;
            g_banks[i].initialized = 0;
            g_banks[i].owned = 0;
            g_bank_count--;
            printf("Memory bank [%d] cleaned up\n", i);
        }
    }
}

int memory_bank_is_available(void) {
    // Check if at least one memory bank is initialized
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            return 1;
        }
    }
    return 0;
}

// Get pointer to locate data at offset
void* memory_bank_get_ptr(uint64_t offset) {
    if (!g_banks[0].initialized || offset >= g_banks[0].size) {
        return NULL;
    }

    return g_banks[0].data + offset;
}

// Get bank size in bytes
size_t memory_bank_get_size(void) {
    return g_banks[0].size;
}

// Is [offset, offset + length) completely inside bank 0 ?
int memory_bank_check_range(uint64_t offset, uint64_t length) {
    if (!g_banks[0].initialized) {
        return 0;
    }

    uint64_t size = g_banks[0].size;

    // "length <= size - offset" cannot overflow because offset <= size
    return offset <= size && length <= size - offset;
}

// Return and reset the number of invalid RTL accesses
int memory_bank_take_bus_errors(void) {
    return atomic_exchange(&g_bus_errors, 0);
}

// Install (or remove, with NULL) the DMA completion hook
void dma_set_done_hook(void (*hook)(void)) {
    atomic_store(&g_done_hook, hook);
}

// Count an invalid RTL access; only the first one is printed to avoid flooding
static void report_bus_error(const char *op, long long offset) {
    if (atomic_fetch_add(&g_bus_errors, 1) == 0) {
        printf("sysbus: invalid RTL %s at offset %lld (bank size %zu)\n",
               op, offset, g_banks[0].size);
    }
}

/**
 * DPI Callback: Called from RTL when DMA read access
 *
 * An access that does not fit completely inside bank 0 is not performed:
 * it is counted (see memory_bank_take_bus_errors) and 0 is returned.
 */
int dpi_bank_read32_legacy(long long offset) {
    uint32_t value = 0;

    if (offset < 0 || !memory_bank_check_range((uint64_t)offset, 4)) {
        report_bus_error("read", offset);
        return 0;
    }

    memcpy(&value, memory_bank_get_ptr((uint64_t)offset), 4);

    return (int)value;
}

/**
 * DPI Callback: Called from RTL when DMA write access
 *
 * An access that does not fit completely inside bank 0 is not performed
 * and is counted. The return value stays 0, as before, because the RTL's
 * reaction to a non-zero value is not known.
 */
int dpi_bank_write32_legacy(long long offset, int value) {
    if (offset < 0 || !memory_bank_check_range((uint64_t)offset, 4)) {
        report_bus_error("write", offset);
        return 0;
    }

    memcpy(memory_bank_get_ptr((uint64_t)offset), &value, 4);

    return 0;
}

/**
 * DPI Callback: Called from RTL when DMA transfer completes
 */
void dma_done_callback(void)
{
    printf("DMA completed\n");
    fflush(stdout);   // keep ordering vs. printf_IMPORTANT output

    void (*hook)(void) = atomic_load(&g_done_hook);
    if (hook != NULL) {
        hook();
    }
}
