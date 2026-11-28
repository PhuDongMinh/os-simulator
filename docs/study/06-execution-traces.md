# 06 — Execution Traces end-to-end

> Mọi kịch bản ở đây đã **chạy thật** (WSL Ubuntu, `make all`, build có `-DUSE_DMA`) trên một
> bản copy của repo trong `/tmp`, trừ khi ghi khác. Log được rút gọn: bỏ dòng `Time slot`
> không cần thiết và thay địa chỉ host trong dòng `PDG=…` bằng `<host>`.
> Ký hiệu [V]/[C]/[I]/[U] như `01`.

## 0. Cách tự chạy lại (không làm bẩn repo)

`./os` ghi đè `rtl_trace.vcd` (file đang được git theo dõi) và các program thử nghiệm không nên
nằm trong `input/`. Vì vậy luôn chạy trong bản copy:

```sh
# trong WSL
rm -rf /tmp/ossim && mkdir /tmp/ossim
cd "/mnt/d/01_University/Term 261/OS/hk261_ossim_demacOprx/hk261_ossim_demacOprx"
git archive HEAD | tar -x -C /tmp/ossim
cd /tmp/ossim && make all
./os os_0_mlq_paging | less
sh tests/run_swap_tests.sh          # tương tự cho 5 script test còn lại
```

Ba program tự viết dùng trong file này (tạo trong `/tmp/ossim/input/…`):

| Tên | Config (`input/<tên>`) | Program (`input/proc/<tên>`) |
|---|---|---|
| `study_swp` | `20 1 1` / `8192 16777216 0 0 0` / `0 study_swp 0` | `1 7` · `alloc 4096 1` · `alloc 4096 2` · `alloc 4096 3` · `write 7 1 0` · `read 1 0 0` · `write 9 3 5` · `read 3 5 1` |
| `study_kd` | `20 1 1` / `1048576 16777216 0 0 0` / `0 study_kd 0` | `1 12` · `alloc 100 1` · `alloc 100 2` · `write 65 1 0` · `write 66 1 1` · `dma_memcpy 1 2 0 2` · `calc` · `read 2 1 3` · `kmalloc 64 5` · `copy_from_user 1 5 0 2` · `alloc 50 6` · `copy_to_user 5 6 0 2` · `read 6 1 4` |
| `study_dma` | `20 1 1` / `1048576 16777216 0 0 0` / `0 study_dma 0` | `1 9` · `alloc 100 1` · `alloc 100 2` · `write 65 1 0` · `write 66 1 5` · `write 67 1 9` · `dma_memcpy 1 2 0 10` · `calc` · `read 2 5 3` · `read 2 9 4` |

(`·` = xuống dòng.) Time slice 20 để cả program chạy trong một lần dispatch, log dễ đọc.

---

## 1. Simulator startup — `os_syscall_list` **[V]**

1. **Initial state:** chưa có gì; config `2 1 1` / `2048 16777216 0 0 0` / `1 sc1 15`.
2. **Trigger:** `./os os_syscall_list`.
3. **Call chain:** `main` → `read_config` → `attach_event`×2 → `start_timer` → `init_memphy(mram, 2048)`,
   `init_memphy(mswp[0..3])` → `memory_bank_attach` → `dma_service_start` (`rtl_dma_init_simulation`,
   tạo `dma_worker`) → `init_scheduler` → tạo `ld_routine`, `cpu_routine`.
   Loader: `init_mm(os.mm, NULL)` → chờ t=1 → `load` → `init_mm(proc->mm, proc)` → registry → `add_proc`.
4. **State transitions:** RAM 2048 B ⇒ **0 frame**; SWP0 4096 slot; kernel vma0 tại `0xFF00…00`.
5. **Final state:** 1 process trong `mlq_ready_queue[15]`.
6. **Log:**
```
Time slot   0
Memory bank [0] attached: 2048 bytes at address <host>
Initialized dma_rtl simulation
ld_routine
Time slot   1
	Loaded a process at input/proc/sc1, PID: 1 PRIO: 15
	CPU 0: Dispatched process  1
0-sys_listsyscall
17-sys_memmap
Time slot   2
	CPU 0: Processed  1 has finished
	CPU 0 stopped
Clean up dma_rtl simulation
Memory bank [0] cleaned up
```

---

## 2. Process creation và scheduling — `sched_0` **[V]**

1. **Initial:** time slice 2, 1 CPU, 4 process (P1, P2 prio 4; P3, P4 prio 0).
2. **Trigger:** loader thêm process theo `start_time`.
3. **Call chain mỗi slot:** `cpu_routine` → [`put_proc` nếu hết slice] → `get_proc` → `get_mlq_proc`
   (quét prio 0..139, `slot[prio]--`) → `run` → `next_slot`.
4. **State transitions (queue sau mỗi dispatch):**

| Slot | Sự kiện | q[0] | q[4] | Running |
|---|---|---|---|---|
| 0 | load P1, dispatch P1 | ∅ | ∅ | P1 |
| 1 | load P2 | ∅ | P2 | P1 |
| 2 | put P1, dispatch P2 | ∅ | P1 | P2 |
| 4 | put P2, dispatch P1; load P3 | P3 | P2 | P1 |
| 5 | load P4 | P3, P4 | P2 | P1 |
| 6 | put P1, dispatch **P3** (prio 0 thắng) | P4 | P2, P1 | P3 |
| 8 | put P3, dispatch P4 | P3 | P2, P1 | P4 |
| 19 | P3 finished, dispatch P4 | ∅ | P2, P1 | P4 |

5. **Final:** mọi process xong, `CPU 0 stopped`.
6. **Kiểm chứng:** `./os sched_0`, so khớp với bảng Gantt ở `02` §5.4.

---

## 3. Memory allocation — `alloc 300 0` của `p0s` **[V]**

1. **Initial:** `mm` mới: vma0 `[0x1000,0x1000)`, sbrk 0x1000, free list `{[0x1000,0x1000)}`, cây chỉ có root, RAM 65 536 frame trống.
2. **Trigger:** `run` gặp `ALLOC 300 0`.
3. **Call chain:**
```mermaid
sequenceDiagram
    participant R as run
    participant A as __alloc
    participant S as _syscall/__sys_memmap
    participant M as vm_map_ram
    participant F as alloc_pages_range
    participant P as MEMPHY
    R->>A: liballoc(proc, 300, 0)
    A->>A: lock mmvm_lock; get_free_vmrg_area → thất bại (region rỗng)
    A->>S: syscall 17 {INC, 0, 300}
    S->>S: get_pcb_by_pid(1); inc_vma_limit → 0 (stub)
    A->>M: vm_map_ram(0x1000, 0x2000, 0x1000, 1)
    M->>M: prepare_pages: tạo P4D/PUD/PMD/leaf, PTE[1]==0, node FIFO
    M->>F: alloc_pages_range(1)
    F->>P: MEMPHY_get_freefp → fpn 0
    M->>M: map_prepared: PTE[1] = 0x80000000; FIFO = [p1]
    A->>A: vm_end=0x2000; free += [0x112C,0x2000); sbrk=0x112C; r0=[0x1000,0x112C)
    A->>R: 0 (rồi print_pgtbl)
```
4. **State transitions:** xem bảng `04` §4.3 dòng `alloc 300 0`.
5. **Final:** r0 = `[0x1000,0x112C)`, PTE[1] = F0, RAM free 65 535.
6. **Log:** `[SYSCALL] sys_memmap INC pid=1 a2=0 a3=300` rồi ` PDG=<host> P4D=<host> PUD=<host> PMD=<host>PT=<host>`.
   Dòng PDG có đủ 5 con trỏ khác NULL ⇒ đường đi tới page 1 đã được tạo.

---

## 4. Memory read/write — `p0s` trong `os_0_mlq_paging` **[V]**

1. **Initial:** r1 = `[0x1000,0x1064)`, PTE[1] = F0 (PID 1) / F1 (PID 4).
2. **Trigger:** `write 100 1 20`, sau đó `read 1 20 20`.
3. **Call chain:** `libwrite` → `__write` (4 kiểm tra) → `pg_setval(0x1014)` → `pg_getpage(pgn 1)` = hit
   → paddr = fpn·4096 + 0x14 → `_syscall(17, IO_WRITE)` → `MEMPHY_write`. READ tương tự rồi
   `regs[20]`? — `arg_2 = 20 ≥ 10` nên **không** ghi register (`cpu.c:132`).
4. **State:** `mram.storage[20] = 100` (PID 1), `mram.storage[4116] = 100` (PID 4).
5. **Log:**
```
[SYSCALL] sys_memmap IO_WRITE pid=1 addr=20 value=100
[SYSCALL] sys_memmap IO_WRITE pid=4 addr=4116 value=100
libread:503
[SYSCALL] sys_memmap IO_READ pid=1 addr=20 value=100
[User Write] Lỗi Bảo Mật: Vùng nhớ (rgid=2) không hợp lệ.
	CPU 0: PID  1: instruction at pc=7 failed
```
6. **Bài học:** cùng VA `0x1014`, PA khác nhau ⇒ isolation. Lệnh trên region 2, 3 lỗi vì program
   mẫu chưa từng `alloc` chúng — lỗi có chủ đích, không phải bug simulator.

---

## 5. Page-in (swap-in) — `study_swp`, RAM 2 frame **[V]**

1. **Initial:** RAM frame {0,1} trống; SWP0 4096 slot trống.
2. **Trigger:** `alloc` 3 page, rồi `write 7 1 0` vào page đã bị đẩy ra.
3. **Call chain (write 7 1 0):** `__write` → `pg_setval` → `pg_getpage`: PTE[1] = `0x40000000` (SWAPPED)
   → RAM hết → `mm64_fifo_victim` chọn p2 (F1) → `get_freefp(swap)` = slot 1 → **syscall SWP** →
   `__mm_swap_page` copy F1→slot1 → `pte_set_swap_mm(p2)` → `__swap_cp_page(slot0→F1)` →
   `pte_set_fpn(p1, F1)` → `put_freefp(swap, 0)` → FIFO thêm p1 → IO_WRITE paddr 4096.
4. **State transitions:** bảng đầy đủ ở `03` §10.
5. **Final:** p1→F1, p3→F0, p2→swap slot 1; FIFO `[p1, p3]`.
6. **Log:**
```
[SYSCALL] sys_memmap INC pid=1 a2=0 a3=4096      ← alloc r1 (F0)
[SYSCALL] sys_memmap INC pid=1 a2=0 a3=4096      ← alloc r2 (F1)
[SYSCALL] sys_memmap INC pid=1 a2=0 a3=4096      ← alloc r3: evict p1 → slot 0 (KHÔNG có dòng SWAP)
[SYSCALL] sys_memmap SWAP pid=1 pgn=1 swp=1      ← "pgn" thực ra là victim FPN 1
[SYSCALL] sys_memmap IO_WRITE pid=1 addr=4096 value=7
[SYSCALL] sys_memmap IO_READ pid=1 addr=4096 value=7
[SYSCALL] sys_memmap IO_WRITE pid=1 addr=5 value=9
[SYSCALL] sys_memmap IO_READ pid=1 addr=5 value=9
```

---

## 6. RAM hết frame và page replacement giữa nhiều process — `os_1_mlq_paging_small_4K` **[V]**

1. **Initial:** RAM 4096 B = **1 frame**, 4 CPU, 8 process (p0s PID 1, m0s PID 2 dùng bộ nhớ).
2. **Trigger:** PID 1 và PID 2 cùng `alloc` và `write`.
3. **Call chain:** lần `alloc` của PID 2 → `alloc_pages_range` evict page của **PID 1** (FIFO toàn cục,
   `owner = PID1->mm`) → `pte_set_swap_mm(PID1->mm, …)`. Khi PID 1 `write` → `pg_getpage` evict page của
   PID 2 qua syscall SWP, nạp lại page của mình. Lặp lại.
4. **State transitions (frame duy nhất F0):**

| Thời điểm (log) | F0 thuộc | Swap |
|---|---|---|
| PID 1 `alloc 300 0` | PID 1 p1 | — |
| PID 2 `alloc 300 0` | PID 2 p1 | PID 1 p1 → slot 0 |
| `SWAP pid=1 pgn=0 swp=1` | PID 1 p1 | PID 2 p1 → slot 1 |
| `SWAP pid=2 pgn=0 swp=0` | PID 2 p1 | PID 1 p1 → slot 0 |
| `SWAP pid=1 pgn=0 swp=1` | PID 1 p1 | PID 2 p1 → slot 1 |

5. **Final:** dữ liệu vẫn đúng: `IO_READ pid=1 addr=20 value=100` dù page của PID 1 đã bị đẩy ra
   swap 2 lần (do `alloc` rồi do `write` của PID 2) và nạp lại 2 lần.
6. **Kiểm chứng:** `./os os_1_mlq_paging_small_4K | grep -E "SWAP|IO_"`. Thứ tự giữa các CPU có thể khác
   giữa các lần chạy (song song trong slot). Đây là ví dụ **thrashing** để đưa vào báo cáo.

---

## 7. System call execution **[V]**

### 7a. `syscall 0` (`sc1`)
`run` → `libsyscall(proc, 0, 0, 0, 0)` → `_syscall(krnl, 1, 0, regs)` → `case 0` → `__sys_listsyscall`
→ in `sys_call_table[]`: `0-sys_listsyscall`, `17-sys_memmap` (log ở §1).

### 7b. Syscall không tồn tại (`sc2`: `syscall 101 1` trong `os_syscall`)
`_syscall` → `default` → `__sys_ni_syscall` → **return 0**. Không có output, không báo lỗi. Ngoài ra
`alloc 100 1` thất bại vì RAM 2048 B = 0 frame ⇒ 3 lệnh `write` sau cũng thất bại:
```
[SYSCALL] sys_memmap INC pid=1 a2=0 a3=100
	CPU 0: PID  1: instruction at pc=0 failed
[User Write] Lỗi Bảo Mật: Vùng nhớ (rgid=1) không hợp lệ.
	CPU 0: PID  1: instruction at pc=1 failed
```
**Unsupported:** ví dụ spec §1.6.3 (`syscall 440 1` in "The first system call parameter 1") không chạy
được vì repo chưa có `sys_xxxhandler` và dòng 440 trong `syscall.tbl`.

---

## 8. Kernel memory allocation + copy — `study_kd` **[V]**

1. **Initial:** r1, r2 user trong page 1 (F0). Kernel vma0 rỗng tại `0xFF00…00`.
2. **Trigger:** `kmalloc 64 5`, `copy_from_user 1 5 0 2`, `alloc 50 6`, `copy_to_user 5 6 0 2`, `read 6 1 4`.
3. **Call chain:**
   - `libkmem_malloc` → `__kmalloc`: `kmap_slot[5] == -1`, kslot 0 trống, VA = sbrk = `0xFF00…00`,
     `MEMPHY_get_freefp_range(1)` → F1 **[I]**, `pte_set_fpn(os.mm, 0xFF00000000000, 1)`, `kmap_slot[5] = 0`.
   - `copy_from_user`: kiểm tra → mỗi byte `__read_user_mem` (syscall IO_READ) + `__write_kernel_mem`
     (MEMPHY trực tiếp, **không** log).
   - `copy_to_user`: mỗi byte `__read_kernel_mem` + `__write_user_mem` (syscall IO_WRITE).
4. **State:** kernel region 5 = `[0xFF00…00, 0xFF00…40)` chứa "AB"; user r6 = `[0x10C8, 0x10FA)` chứa "AB".
5. **Final:** khi exit, `__kfree(…, 5)` trả F1 và xóa PTE kernel.
6. **Log:**
```
[SYSCALL] sys_memmap IO_READ pid=1 addr=0 value=65
[SYSCALL] sys_memmap IO_READ pid=1 addr=1 value=66
[Copy] Thành công: Chép 2 bytes từ User (rgid=1) vào Kernel (rgid=5)
[SYSCALL] sys_memmap IO_WRITE pid=1 addr=200 value=65
[SYSCALL] sys_memmap IO_WRITE pid=1 addr=201 value=66
[Copy] Thành công: Chép 2 bytes từ Kernel (rgid=5) vào User (rgid=6)
[SYSCALL] sys_memmap IO_READ pid=1 addr=201 value=66
```
   `addr=200` = 0x10C8 − 0x1000 ⇒ r6 bắt đầu ở 0x10C8 (first-fit sau r2 = `[0x1064,0x10C8)`).

---

## 9. DMA operation — `study_dma` (thành công) và `m2s` (thất bại có chủ đích)

### 9a. `study_dma` **[V]**
1. **Initial:** r1 = `[0x1000,0x1064)`, r2 = `[0x1064,0x10C8)`, cùng frame F0; r1[0]=65, r1[5]=66, r1[9]=67.
2. **Trigger:** `dma_memcpy 1 2 0 10`.
3. **Call chain:** `libmem_dma_memcpy` → lock `mmvm_lock` → `dma_build_runs`: `dma_vtop(0x1000)` = PA 0,
   `dma_vtop(0x1064)` = PA 100 ⇒ 1 run {src 0, dst 100, len 10}, ghim F0 → unlock → `dma_submit` → return 0.
   Worker: `do_dma(0, 100, 10)` → RTL chép 8 byte (`DMA completed`) → memcpy 2 byte đuôi → `dma_unpin_cb`.
4. **State:** `mram[100..109]` = `mram[0..9]`; pin count F0 về 0.
5. **Final/Log:**
```
[SYSCALL] sys_memmap IO_WRITE pid=1 addr=9 value=67
DMA completed
[SYSCALL] sys_memmap IO_READ pid=1 addr=105 value=66      ← byte trong phần RTL chép
[SYSCALL] sys_memmap IO_READ pid=1 addr=109 value=67      ← byte đuôi do memcpy chép
```
6. **Lưu ý:** với `dma_memcpy … 2` (trong `study_kd`) **không** có dòng `DMA completed` vì cả 2 byte là
   "đuôi" — RTL không chạy. Thứ tự `DMA completed` so với các dòng khác phụ thuộc lịch thread.

### 9b. `m2s` trong `os_2_singleCPU_mlq_paging` **[V]**
```
libmem_dma_memcpy: region 1 or 2 is not allocated        ← pc=4: "2" là kernel ID, không phải user region
[Copy] Lỗi: Vùng nhớ Nguồn hoặc Đích chưa được cấp phát. ← pc=5: kernel ID 4 chưa kmalloc (pc=6 mới cấp)
[Copy] Lỗi: …                                            ← pc=7: copy_to_user 3 4: kernel ID 3 chưa từng cấp
	CPU 0: PID  4: instruction at pc=8 failed           ← free 2: user region 2 chưa từng cấp
```
Kết luận: program mẫu `m2s` không khớp với quy ước ID của code (user ID ≠ kernel ID). Đây là chỗ cần
giải thích khi demo.

---

## 10. Process termination **[V]** (log) / **[C]** (thứ tự bên trong)

1. **Initial:** process có `pc == code->size`, có thể còn DMA pending, kernel region, page ở RAM/swap.
2. **Trigger:** đầu vòng lặp `cpu_routine` thấy `pc == size`.
3. **Call chain:** `dma_process_exit(pid)` (chờ `pending == 0`) → gỡ `proc_registry` → `__kfree` cho mỗi
   `kmap_slot[k] != -1` → `free_pcb_memph` (`mm64_pt_foreach(release_page)` → `mm64_pgd_destroy` →
   `mm64_fifo_drop` → free VMA) → `free(mm)` → `finish_proc` → free code, PCB.
4. **State (ví dụ `study_swp`):** RAM free {0,1} trở lại; swap slot 1 trả về; FIFO rỗng; cây bị hủy.
5. **Final:** `CPU 0: Processed 1 has finished`.
6. **Kiểm chứng:** `tests/run_mm_teardown_tests.sh` (`test_release_everything`: đếm frame trống trước/sau,
   ASan không báo leak); `test_kernel_memory_untouched`.

---

## 11. Kịch bản **không** được hỗ trợ (đừng demo như thể chạy được)

| Kịch bản | Lý do |
|---|---|
| Config có dòng `KERNEL_BASE_ADDR` | `read_config` không đọc dòng này ⇒ parse lệch **[C]** |
| `alloc` với RAM < 4096 B | 0 frame **[V]** |
| `syscall 440` in thông báo | chưa có handler **[V]** |
| DMA trên page đang ở swap | `dma_vtop` từ chối, không swap-in **[C]** |
| DMA giữa user và kernel region | chỉ nhận user region **[C]** |
| Nhiều VMA / `vmaid ≠ 0` | `get_vma_by_num` có thể crash **[C]** |
| Swap device > 2 GiB | `init_memphy` từ chối ⇒ `exit(1)` **[C]** |
| Build không có MM64 (32-bit) | `mm.c` deprecated, chữ ký hàm không khớp **[C]** |
| TLB, demand paging, page fault trap | không hiện thực |

---

## Active recall

1. Trong kịch bản §5, tại sao lần evict đầu tiên không in dòng `SWAP` mà lần thứ hai thì có?
2. Kịch bản §4: vì sao `read 1 20 20` không ghi `regs[20]`? Có crash không?
3. Ở §8, vì sao `copy_from_user` in 2 dòng `IO_READ` nhưng không in dòng nào cho phần ghi kernel?
4. Ở §9a, byte offset 9 được chép bởi ai: RTL hay CPU?
5. Ở §6, nếu FIFO là riêng từng process thì `alloc` của PID 2 có thành công không?
6. Liệt kê thứ tự 5 bước dọn dẹp ở §10 và cho một ví dụ sai thứ tự gây lỗi.

## Bài tập nhỏ

1. Chạy `./os os_1_mlq_paging_small_4K` 3 lần, so sánh thứ tự dòng `SWAP`. Giải thích vì sao khác.
2. Viết một program gây lỗi `page N is not in RAM` cho `dma_memcpy` (gợi ý: RAM 2 frame + 3 page).
3. Cho config `os_2_mlq_paging`, dự đoán số dòng `INC` trong log trước khi chạy; sau đó kiểm tra.
