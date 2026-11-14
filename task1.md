# Task: OS Simulator — Architecture Audit & Physical Memory/Paging Implementation Plan

You are a senior systems programmer specializing in operating systems, virtual memory, paging, and C programming.

I am working on an educational OS Simulator. The project is divided into four modules:

1. Physical Memory & Paging — **My responsibility**
2. Virtual Memory & User API — Handled by another developer
3. Kernel Memory & System Calls — **Already implemented**
4. DMA & System Integration — Handled by another developer

Your task is to thoroughly investigate the existing codebase and prepare an accurate implementation plan for Module 1.

**IMPORTANT: This phase is READ-ONLY. Do not modify any source files. Do not implement anything yet.**

## 1. Understand the architecture

Inspect the entire repository, prioritizing:

- `mm-memphy.c`
- `mm64.c`
- `libmem.c`
- Relevant memory headers and data structures
- `mm-vm.c`
- `sys_mem.c`
- `syscall.c`
- `os.c`
- `cpu.c`
- Scheduler and process lifecycle code

Identify the actual relationships between:

- Physical RAM
- SWAP storage
- Frames and pages
- Five-level page tables
- Page Table Entries (PTE)
- Process memory management (`mm_struct`)
- Process Control Blocks (PCB)
- VMAs
- User-space allocation
- Kernel-space allocation
- System calls
- DMA

Explain how these components interact in the current implementation, not merely how a typical operating system works.

Trace at least these flows:

1. Process creation → memory initialization
2. User allocation → page mapping
3. Virtual memory read/write → physical memory access
4. Page fault or missing page → page resolution
5. RAM exhaustion → swapping
6. Process termination → resource cleanup

For every flow, identify the actual functions, structures, and relevant files.

Distinguish between implemented behavior, intended behavior, and missing behavior.

## 2. Audit Module 1

Inspect the following functions and their dependencies.

### Physical memory
- `init_memphy()`
- `MEMPHY_format()`
- `MEMPHY_get_freefp()`
- `MEMPHY_put_freefp()`
- `MEMPHY_read()`
- `MEMPHY_write()`
- Physical memory dump/debug helpers

### Five-level page tables and PTE
- Page table creation, traversal and destruction
- `pte_get_entry()`
- `pte_set_entry()`
- `pte_set_fpn()`
- `pte_set_swap()`
- `init_pte()`

### Allocation and mapping
- `alloc_pages_range()`
- `vmap_page_range()`
- `vm_map_ram()`

### Paging and swapping
- `pg_getpage()`
- `find_victim_page()`
- `enlist_pgn_node()`
- Related swap helpers

### Process lifecycle
- `init_mm()`
- Physical frame cleanup
- Swap-slot cleanup
- Page-table cleanup

For each function, report:

- File and location
- Current behavior
- Expected behavior inferred from the codebase
- Missing logic
- Bugs or suspicious assumptions
- Callers and dependencies
- Implementation priority

Do not assume every listed function is incomplete. Verify its actual state.

## 3. Investigate critical design contracts

Determine the following from the actual code:

1. Page size and page-table indexing scheme
2. Virtual and physical address representation
3. Physical frame numbering
4. Exact PTE bit layout and flag semantics
5. RAM and SWAP device representation
6. Free-frame list invariants
7. FIFO victim-selection mechanism
8. Page ownership and per-process isolation
9. Allocation rollback requirements
10. Process memory ownership and cleanup order
11. Error codes and return-value conventions
12. Any locking or concurrency assumptions

Pay special attention to the already completed Module 3.

Identify its dependencies on Module 1 and document the API contracts that **must remain compatible**.

Do not propose modifying Module 3 unless a concrete integration defect makes it unavoidable.

## 4. Build an implementation plan

Divide Module 1 into five implementation stages:

### Stage A — Physical Memory Management
Frame allocation/release, initialization, bounds checking, debugging.

### Stage B — Five-Level Page Tables & PTE
Hierarchical page tables, PTE operations, initialization.

### Stage C — Allocation & Mapping
Physical frame allocation, virtual-to-physical mapping, rollback on failure.

### Stage D — Paging & SWAP
Page lookup, FIFO replacement, swap-out, swap-in, PTE consistency.

### Stage E — Cleanup & Integration
Process termination, resource reclamation, module integration.

For each stage, provide:

- Exact functions to modify
- Prerequisites
- Expected behavior
- Important invariants
- Edge cases
- Unit/integration tests
- Acceptance criteria

## 5. Required output

Produce a technical audit containing:

1. **Architecture overview**
2. **Memory-management execution flows**
3. **Module 1 function audit table**
4. **Module 1 ↔ Module 2/3/4 dependency analysis**
5. **Current bugs and missing implementations**
6. **Implementation stages in dependency order**
7. **Testing strategy**
8. **Uncertainties requiring verification**

Cite concrete files, functions, and line locations whenever possible.

Do not invent implementation details or assume that this simulator behaves exactly like Linux.

Explain the architecture in Vietnamese while retaining English technical terminology.

The result must be sufficiently precise that another coding agent can implement Module 1 without having to rediscover the architecture.

**Stop after completing the audit. Do not modify source code.**