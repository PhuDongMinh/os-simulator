# Implement Stage A — Physical Memory Management

You have completed the codebase audit for Module 1 of the OS Simulator.

Use the previous audit as context. Your next task is to implement **Stage A only: Physical Memory Management**.

Do not proceed to Stage B, C, D, or E.

## 1. Scope

Primary file:
- `src/mm-memphy.c`

Related function:
- `MEMPHY_get_freefp_contig()` in `src/libmem.c`

Relevant headers may be modified only when necessary.

Module 3 is already implemented. Preserve its public APIs and behavior.

Important: The working tree contained pre-existing changes in `src/libmem.c`. Do not overwrite or revert them.

## 2. Architectural constraints

The audit identified:

- MM64 uses 4096-byte pages.
- Frame 0 is valid.
- RAM and SWAP use `memphy_struct`.
- The free-frame list currently contains pointer-management bugs.
- The contiguous allocator depends on frame-list ordering.
- Multiple CPU pthreads may access shared RAM.
- MM64's PTE currently supports only 13-bit FPNs.

Before implementation, inspect the relevant headers and verify these findings.

For Stage A, use 4096 bytes as the MM64 physical frame size.

Do not independently redesign the PTE layout, virtual-memory layout, or kernel memory API.

If a required change creates incompatibility with Module 3, document the issue rather than silently changing its behavior.

## 3. Implementation requirements

### A1. Memory initialization

Complete:
- `init_memphy()`
- `MEMPHY_format()`

Requirements:
- Initialize all fields deterministically.
- Handle zero-capacity devices safely.
- Handle devices containing exactly one frame.
- Allocate and initialize frame-list nodes correctly.
- Ensure the final node points to NULL.
- Handle host allocation failures.
- Prevent partial initialization leaks.
- Preserve valid device-mode behavior.
- Validate relevant size limits.

### A2. Frame allocation and release

Complete:
- `MEMPHY_get_freefp()`
- `MEMPHY_put_freefp()`

Requirements:
- Correctly pop the free-list head.
- Return the allocated FPN through the output parameter.
- Never allocate the same physical frame twice while it remains owned.
- Reject invalid or duplicate frame releases.
- Handle empty free lists safely.
- Preserve free-list invariants.
- Support frame 0.

Explicitly define the ownership and state transition of a frame.

### A3. Contiguous frame allocation

Review and fix:
- `MEMPHY_get_freefp_contig()`

Requirements:
- Correctly allocate a single frame.
- Correctly allocate N contiguous frames.
- Work after frames have been released in arbitrary order.
- Handle fragmented free lists.
- Avoid partial allocation on failure.
- Preserve the existing kernel allocation API.

Do not rewrite unrelated Module 3 functions.

### A4. Physical I/O

Complete:
- `MEMPHY_read()`
- `MEMPHY_write()`
- Sequential read/write helpers if required
- `MEMPHY_mv_csr()`

Requirements:
- Validate physical byte addresses.
- Reject NULL pointers where invalid.
- Never read or write outside device storage.
- Handle zero-size devices.
- Preserve output arguments on failure where appropriate.
- Maintain cursor consistency.

### A5. Debugging

Implement:
- `MEMPHY_dump()`

Provide useful, bounded debugging information:
- Device capacity
- Frame size
- Number of available frames
- Free-frame list
- Optional bounded memory contents

Do not mutate device state during dumping.

## 4. Concurrency and ownership

Inspect current locking conventions before introducing synchronization.

Determine which functions access shared physical frame metadata and whether their callers already hold locks.

Avoid:
- Double locking
- Deadlocks
- Unsynchronized frame allocation
- Duplicate ownership
- Breaking contiguous allocation

Document the lock ownership policy and the required integration points.

Do not introduce an incompatible locking protocol without identifying affected callers.

## 5. Testing

Create focused tests covering:

1. Zero-capacity device
2. Exactly one frame
3. Multiple frames
4. Full frame exhaustion
5. Frame release and reuse
6. Duplicate release
7. Invalid frame number
8. First and last valid physical addresses
9. Out-of-bounds read/write
10. Contiguous allocation of one and multiple frames
11. Fragmented frame lists
12. Repeated allocate/free cycles
13. Initialization and allocation failures
14. Concurrent allocation where supported

Test invariants, not just function return values.

Use sanitizers when possible.

The repository audit already identified unrelated compilation errors. Do not misrepresent them as Stage A regressions. Use an isolated test harness if necessary, while also reporting whole-project build status.

## 6. Deliverables

After implementation, provide:

1. Functions changed
2. Files modified
3. Important implementation decisions
4. Free-frame list invariants
5. Tests executed and results
6. Remaining bugs or integration dependencies
7. Git diff summary

Inspect the final diff and preserve all unrelated working-tree changes.

**Stop after Stage A. Do not implement page tables, mapping, swapping, or process cleanup yet.**

Explain results in Vietnamese and retain English technical terminology.