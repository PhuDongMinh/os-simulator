/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * system bus protoype header file
 */

#ifndef SYSTEM_BUS_H
#define SYSTEM_BUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of memory banks supported
#define MAX_MEMORY_BANKS 8

// Memory bank handle type
typedef int memory_bank_t;

// Invalid memory bank handle
#define MEMORY_BANK_INVALID (-1)

// Initialize a memory bank with given size in bytes
memory_bank_t memory_bank_init(size_t size_bytes);

/*
 * Attach EXISTING memory (e.g. the OS RAM, os.mram->storage) as a bank.
 * Nothing is allocated or copied, so the RTL reads/writes that memory
 * directly. The bank is not owned: memory_bank_cleanup() will not free it.
 * The RTL callbacks use bank 0, so attach before any memory_bank_init().
 */
memory_bank_t memory_bank_attach(void *base, size_t size_bytes);

// Cleanup all memory banks
void memory_bank_cleanup(void);

// Get pointer to locate data at offset
void* memory_bank_get_ptr(uint64_t offset);

// Get bank size in bytes
size_t memory_bank_get_size(void);

// Check if memory bank is initialized
int memory_bank_is_available(void);

/*
 * Return 1 if [offset, offset + length) lies completely inside bank 0,
 * 0 otherwise (also when no bank exists). Overflow safe.
 */
int memory_bank_check_range(uint64_t offset, uint64_t length);

/*
 * Return how many invalid RTL accesses (out of range / negative offset)
 * happened since the last call, and reset the counter. A non-zero value
 * after a DMA copy means the copied data cannot be trusted.
 */
int memory_bank_take_bus_errors(void);

/*
 * Install a function called from dma_done_callback() each time the RTL
 * reports that a transfer completed (NULL removes it). It runs inside the
 * RTL evaluation, on the thread that is driving the RTL, so it must be
 * short and must not block.
 */
void dma_set_done_hook(void (*hook)(void));

// DPI Callback: Called from RTL when DMA read access
int dpi_bank_read32_legacy(long long offset);

// DPI Callback: Called from RTL when DMA write access
int dpi_bank_write32_legacy(long long offset, int value);

// DPI Callback: Called from RTL when DMA transfer completes
void dma_done_callback(void);
#ifdef __cplusplus
}
#endif

#endif // SYSTEM_BUS_H
