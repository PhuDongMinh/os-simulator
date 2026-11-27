/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DMA Interface Header
 */

#ifndef __DMA_H__
#define __DMA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct pcb_t;

/*
 * do_dma - perform DMA function, synchronously (implemented in
 *          dma_c_testbench.cpp). Only the DMA worker calls it.
 * caller: process control block (not used)
 * source: PHYSICAL source offset, in the OS RAM
 * destination: PHYSICAL destination offset, in the OS RAM
 * length: number of bytes to copy
 * Returns 0 on success, -1 on failure.
 */
int do_dma(struct pcb_t *caller, uint64_t source, uint64_t destination, uint64_t length);

/*
 * Asynchronous DMA requests (implemented in os.c).
 *
 * A request is queued and executed later by the DMA worker thread, so the
 * CPU that submits it does not wait for the RTL simulation.
 */

/*
 * Called by the worker after a request ran, before the process is told it
 * is finished (status 0 = copied, -1 = failed). It is the place to unpin
 * the frames used by the request. It runs on the worker thread, so it must
 * be short.
 */
typedef void (*dma_done_fn)(void *ctx, int status);

/*
 * dma_submit - queue a copy of PHYSICAL offsets, on behalf of process pid.
 * done may be NULL. Returns 0 if queued, -1 if the DMA service is not
 * running or memory is short. Never blocks for the copy.
 */
int dma_submit(uint32_t pid, uint64_t source, uint64_t destination, uint64_t length,
               dma_done_fn done, void *ctx);

/* Number of requests of process pid that are queued or running. */
int dma_pending_for(uint32_t pid);

/*
 * dma_process_exit - called when process pid has run its last instruction.
 * Waits until all its requests completed (so its frames are no longer in use
 * by the DMA), drops its bookkeeping, and returns how many of them failed.
 */
int dma_process_exit(uint32_t pid);

#ifdef __cplusplus
}
#endif

#endif /* __DMA_H__ */
