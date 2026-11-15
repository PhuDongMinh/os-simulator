# Task: Architecture Review — Five-Level Page Table Representation for OS Simulator

You are a senior operating systems engineer specializing in virtual memory, hierarchical page tables, memory ownership, and C programming.

I am responsible for **Module 1: Physical Memory & Paging** of an educational OS Simulator written in C.

The project has four modules:

1. Physical Memory & Paging — My responsibility.
2. Virtual Memory & User API — Another developer.
3. Kernel Memory & Syscalls — Already implemented.
4. DMA & Integration — Another developer.

Your task is to **review the current codebase and design a five-level page-table representation that best fits the existing architecture**.

This is an architecture review, NOT an implementation task.

**Do not modify any source file.**

## 1. Confirmed project decisions

Our team has agreed on the following constraints:

- MM64 is enabled.
- Virtual addresses use the five-level paging scheme described in the assignment specification.
- Page size is 4096 bytes (4 KiB).
- Each level uses a 9-bit index, giving 512 entries per table.
- The page-table levels are PGD → P4D → PUD → PMD → PT.
- Physical RAM is assumed to be at most 32 MiB.
- FPN occupies 13 bits, allowing 8192 physical frames.
- We will retain the existing 32-bit PTE format described in the assignment.
- We do not need to support RAM configurations exceeding 32 MiB.
- We do not intend to redesign PTE encoding.

Treat these as established design constraints.

Do not spend time proposing alternative PTE formats or extending RAM support.

If existing input configurations exceed 32 MiB, report them as unsupported under our chosen assumptions rather than modifying the PTE layout.

## 2. Understand the existing codebase

Inspect the actual repository, prioritizing:

- `include/os-mm.h`
- `include/mm.h`
- `include/mm64.h`
- `include/common.h`
- `src/mm64.c`
- `src/mm-memphy.c`
- `src/libmem.c`
- `src/mm-vm.c`
- `src/sys_mem.c`
- `src/syscall.c`
- `src/os.c`
- `src/loader.c`
- `src/cpu.c`

Also inspect any relevant documentation, including the assignment specification and existing memory-layout documents, if available.

Identify:

1. How `mm_struct` is currently defined.
2. What `pgd`, `p4d`, `pud`, `pmd`, and `pte` currently represent.
3. How the kernel page-table fields are organized.
4. How page tables are initialized.
5. How processes obtain their `mm_struct`.
6. How user and kernel address spaces are accessed.
7. How current PTE operations are called.
8. How process memory cleanup is performed.
9. Which functions already work and should be preserved.
10. Which incomplete functions prevent a working five-level page table.

Distinguish verified code behavior from assumptions and intended behavior.

Do not assume the current codebase follows Linux's page-table implementation.

## 3. Evaluate page-table representation strategies

Compare three possible designs:

### Option A — Pointer-based hierarchical tree

Each page-table level contains 512 entries.

Non-leaf entries reference dynamically allocated child tables.

Leaf entries store the existing PTE values.

Example conceptual hierarchy:

PGD → P4D → PUD → PMD → PT → PTE

### Option B — Dedicated page-table node structures

Introduce explicit structures representing:

- Page-table levels
- Child pointers
- Leaf entries
- Optional ownership metadata

Evaluate whether dedicated structs improve readability, safety, and cleanup.

### Option C — Physical-memory-backed page tables

Store the page-table hierarchy inside simulated physical RAM.

Evaluate whether this is suitable for an educational simulator where RAM is represented by `memphy_struct`.

For each option, evaluate:

- Compatibility with `mm_struct`
- Required source changes
- Implementation complexity
- Memory overhead
- Traversal cost
- Allocation failure handling
- Cleanup complexity
- Kernel/user isolation
- Compatibility with completed Module 3
- Integration with Modules 2 and 4
- Risk of introducing regressions

Recommend the simplest design that satisfies the specification and works naturally with the current codebase.

Do not prefer a more Linux-like design merely because Linux uses it.

## 4. Design the five-level hierarchy

Based on the recommended representation, specify the exact in-memory structure.

Address these questions:

### 4.1 Root representation

- Which field in `mm_struct` should be the authoritative root?
- Should `mm->pgd` be the only root pointer?
- What should happen to `mm->p4d`, `mm->pud`, `mm->pmd`, and `mm->pte`?
- Can these existing fields be reused safely?
- Should unused fields be retained for compatibility?

### 4.2 Non-leaf representation

Explain:

- What each non-leaf entry stores.
- Whether child references are pointers or encoded integer values.
- How to avoid invalid pointer-to-integer conversions.
- Whether `uintptr_t`, `uint64_t`, or explicit pointers should be used.
- How to distinguish missing child tables from valid children.
- How to ensure correct pointer alignment and memory lifetime.

### 4.3 Leaf representation

Explain:

- Where the actual PTE values are stored.
- How each virtual page maps to exactly one leaf PTE.
- How the existing 32-bit PTE encoding is preserved.
- How a PTE represents unmapped, resident, and swapped pages.
- How PTE getters/setters interact with the leaf.

Do not mix the representation of child-table pointers with the 32-bit leaf PTE format without explicitly explaining the type and semantics of each.

### 4.4 Address decomposition

Verify the five-level indices against the actual macros in `mm64.h`.

Use:

- PGD: bits 56–48
- P4D: bits 47–39
- PUD: bits 38–30
- PMD: bits 29–21
- PT: bits 20–12
- Offset: bits 11–0

Provide illustrative examples of traversing:

- Virtual page 0
- Virtual page 511
- Virtual page 512
- Addresses crossing PMD, PUD, P4D, and PGD boundaries

Identify potential integer-width and address-validation problems in the existing implementation.

## 5. Define page-table operations

Design the exact behavior of:

- `pte_get_entry()`
- `pte_set_entry()`
- `pte_set_fpn()`
- `pte_set_swap()`
- `pte_clear_entry()`
- `init_pte()`

For each function, explain:

1. Which `mm_struct` it operates on.
2. Whether it is permitted to allocate page tables.
3. What happens when an intermediate table is missing.
4. What happens when allocation fails.
5. How existing PTE flags are preserved or replaced.
6. How errors are reported.
7. Which callers depend on its behavior.

Propose a reusable internal page-table walker instead of duplicating traversal logic across multiple functions.

Define a concrete internal API, for example:

- `walk_page_table(mm, virtual_address, create)`
- `get_pte_slot(mm, pgn, create)`

These are conceptual names; choose actual signatures and types based on the repository.

The walker must:

- Traverse all five levels correctly.
- Never allocate during read-only lookup.
- Allocate missing tables only when requested.
- Zero-initialize new tables.
- Return a clear failure status.
- Avoid partial-allocation leaks.
- Preserve correct root ownership.

## 6. Kernel and user page-table ownership

This is a critical part of the review.

Inspect the relationships between:

- `os.mm`
- `os.krnl_pgd`
- `caller->mm`
- `caller->krnl->mm`

Determine how Module 3 currently selects and modifies kernel memory mappings.

Recommend a precise ownership model satisfying:

1. Every process has its own user address space.
2. Kernel mappings are logically shared and consistent across processes.
3. Module 3 can continue using its existing kernel-memory APIs.
4. User page-table cleanup never destroys shared kernel mappings.
5. Kernel page tables are destroyed only during system shutdown.
6. Process initialization cannot accidentally duplicate or disconnect the kernel root.

Compare these two approaches in the context of the actual code:

- Separate user roots with one shared kernel root.
- Per-process roots containing shared kernel branches.

Recommend one and justify it.

Do not require major changes to Module 3 unless unavoidable.

## 7. Memory ownership and cleanup design

Design the complete ownership lifecycle for page-table nodes.

### Initialization

- Root creation
- Intermediate table allocation
- Leaf initialization
- Kernel root initialization
- Process memory initialization

### Runtime

- Lookup
- Mapping
- Remapping if supported
- Unmapping
- Allocation failure rollback

### Process termination

Determine how to:

- Traverse all allocated page-table nodes.
- Release resident physical frames.
- Release SWAP slots.
- Clear leaf entries.
- Free intermediate page-table structures.
- Free the root.
- Avoid freeing shared kernel mappings.
- Avoid double-free and dangling pointers.

### System shutdown

Determine how the shared kernel page tables should be destroyed.

Clearly distinguish:

- Host heap memory used for page-table metadata.
- Simulated physical RAM frames.
- Simulated SWAP slots.

A page-table node allocated using host `calloc()` must not automatically be treated as a physical RAM frame.

Specify exact ownership responsibilities.

Review existing cleanup functions such as `free_pcb_memph()` and identify the minimum changes needed.

## 8. Cross-module compatibility

Inspect all usages of:

- `mm->pgd`
- `mm->p4d`
- `mm->pud`
- `mm->pmd`
- `mm->pte`
- `krnl_pgd`
- `pte_get_entry()`
- `pte_set_entry()`
- `pte_set_fpn()`
- `pte_set_swap()`
- `pte_clear_entry()`

Do not limit the search to Module 1 files.

For every relevant consumer, record:

- File and function
- Current assumptions
- Compatibility with the proposed design
- Required changes, if any
- Module owner
- Regression risk

Preserve the public API wherever possible.

Identify any existing Module 3 defects separately from defects introduced by the proposed design.

Do not modify other modules in this review.

## 9. Concurrency considerations

The simulator supports multiple CPU threads.

Analyze:

- Concurrent PTE lookup and modification
- Concurrent intermediate-table creation
- Kernel mapping changes shared by processes
- Process cleanup while another thread may access page tables
- Locking dependencies between paging and physical frame allocation

Recommend a minimal synchronization strategy compatible with the existing mutexes.

Define a consistent lock order.

Avoid introducing excessive complexity or unnecessary global serialization.

## 10. Required implementation blueprint

After completing the review, provide a concrete Stage B implementation plan.

Include:

### A. Recommended data structures

Show illustrative C declarations.

Explain which existing structures can remain unchanged and which need modification.

### B. Page-table walker

Provide precise pseudocode or C-like pseudocode for:

- Lookup without allocation
- Lookup with on-demand allocation
- Failure rollback

### C. Ownership model

Provide an ownership diagram explaining:

- Process-specific roots
- Shared kernel root
- Intermediate tables
- Leaf PTEs
- Resource cleanup responsibilities

### D. Function implementation order

List the exact functions to implement, in dependency order.

For each function, specify:

- Preconditions
- Postconditions
- Error handling
- Ownership
- Dependencies
- Tests

### E. Compatibility matrix

List all files and APIs affected by the recommended design.

### F. Test plan

Include:

- Single-page lookup and mapping
- Multiple pages in one leaf table
- PT boundary at page 511 → 512
- PMD/PUD/P4D/PGD boundary crossings
- Two processes using the same virtual address
- Shared kernel mapping visibility
- Missing page-table paths
- Failed intermediate-table allocation
- Repeated PTE updates
- Unmapping
- Process cleanup
- Shared kernel cleanup
- Allocation failure rollback
- Concurrent page-table access

## 11. Final report requirements

Write the report in Vietnamese, retaining English technical terminology.

Structure the result as:

1. Architecture findings from the current codebase
2. Comparison of representation options
3. Recommended page-table structure
4. Detailed traversal design
5. Kernel/user ownership architecture
6. Initialization and cleanup lifecycle
7. Cross-module compatibility matrix
8. Concurrency and locking strategy
9. Stage B implementation plan
10. Remaining risks and unresolved assumptions

For every major conclusion:

- Cite exact files, functions, and relevant line numbers.
- Separate verified facts from architectural recommendations.
- Explain why the proposed design fits this repository.
- Prefer minimum changes and maintainability over unnecessary abstraction.
- Avoid speculative redesigns.
- Identify any conflicts with the assignment specification.

Do not ask for unnecessary clarification. When the source is ambiguous, recommend a reasonable default and clearly label it as a design decision.

**IMPORTANT: READ-ONLY. Do not edit files, implement functions, run formatting tools, or create commits. Stop after delivering the design review.**