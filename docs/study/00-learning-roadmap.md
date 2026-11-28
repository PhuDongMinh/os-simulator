# 00 — Lộ trình học 7 ngày (2–3 giờ/ngày)

> Mục tiêu cuối: sau 7 ngày, bạn tự giải thích được kiến trúc, trace được luồng thực thi, đọc
> được implementation, phân tích state transition và bảo vệ các quyết định thiết kế — **không cần AI**.
> Bạn phụ trách **Module 1 — Physical Memory & Paging**, nên ngày 3–5 dành trọn cho module này.

## Bộ tài liệu

| File | Nội dung | Dùng vào ngày |
|---|---|---|
| `01-system-architecture.md` | Mental model toàn hệ thống, struct trung tâm, startup/shutdown | 1 |
| `02-process-cpu-scheduler.md` | PCB, loader, CPU, timer, MLQ, lifecycle | 2 |
| `03-physical-memory-paging.md` | **MEMPHY, cây 5 cấp, PTE, mapping, FIFO, swap** | 3, 4, 5 |
| `04-virtual-memory.md` | VMA, region, alloc/free/read/write | 5 |
| `05-kernel-syscall-dma.md` | Kernel memory, syscall, DMA | 6 |
| `06-execution-traces.md` | 10 kịch bản end-to-end đã chạy thật | 5, 6, 7 |
| `07-spec-defense.md` | Trả lời câu hỏi spec, gap, bộ vấn đáp | 7 |

Ký hiệu bằng chứng **[V] [C] [I] [U]** giải thích ở đầu `01`.

## Vì sao học theo thứ tự này

Thứ tự đi theo **dependency thực tế trong code** (tầng dưới không gọi tầng trên):

```mermaid
flowchart LR
    D1["Ngày 1<br/>Bức tranh + build/run"] --> D2["Ngày 2<br/>Process / CPU / Scheduler"]
    D2 --> D3["Ngày 3<br/>MEMPHY + layout + PTE"]
    D3 --> D4["Ngày 4<br/>Cây 5 cấp + translation + mapping"]
    D4 --> D5["Ngày 5<br/>Replacement / swap + VM phía trên"]
    D5 --> D6["Ngày 6<br/>Kernel mem / syscall / DMA + traces"]
    D6 --> D7["Ngày 7<br/>Tổng ôn + bảo vệ thử"]
```
- Scheduler (ngày 2) không phụ thuộc bộ nhớ ⇒ học trước để hiểu "ai gọi `run()`".
- MEMPHY (ngày 3) là tầng thấp nhất của bộ nhớ ⇒ mọi thứ khác dựa vào nó.
- Page table (ngày 4) dựa trên MEMPHY; replacement (ngày 5) dựa trên cả hai; VM (ngày 5) gọi xuống paging.
- Kernel/syscall/DMA (ngày 6) dùng lại toàn bộ API của Module 1 ⇒ học sau cùng.

## Chuẩn bị môi trường (làm một lần, ~15 phút)

Máy có WSL Ubuntu với `gcc`, `make`, `verilator`. **Không chạy `./os` trong thư mục repo** (nó ghi đè
`rtl_trace.vcd` đang được git theo dõi). Làm trong bản copy:

```sh
# trong WSL
rm -rf /tmp/ossim && mkdir /tmp/ossim
cd "/mnt/d/01_University/Term 261/OS/hk261_ossim_demacOprx/hk261_ossim_demacOprx"
git archive HEAD | tar -x -C /tmp/ossim
cd /tmp/ossim && make all
for t in tests/run_*.sh; do sh "$t" | tail -3; done   # 6 bộ test phải PASS
```
Spec: `d:\01_University\Term 261\OS\hk261_ossim_demacOprx.pdf` (thư mục cha của repo).

---

## Ngày 1 — Bức tranh tổng thể và chạy được simulator

**Learning objectives**
- Giải thích simulator mô phỏng gì và **không** mô phỏng gì.
- Vẽ lại sơ đồ tầng và kể tên 5 loại thread.
- Build, chạy test và chạy một config; đọc được log ở mức "ai làm gì".

**Kiến thức tiên quyết:** process, thread, mutex ở mức khái niệm; C pointer và linked list.

**Tài liệu (≈60')**
- `01-system-architecture.md` toàn bộ.
- Spec §1.1–1.2 (tr. 3–5), §2.4 (tr. 24–25).

**Source cần mở (≈45')**
- `include/os-cfg.h` (cấu hình nào đang bật).
- `include/common.h:135-196` (`pcb_t`, `krnl_t`), `include/os-mm.h` (mọi struct bộ nhớ).
- `src/os.c:622-759` (`main`) — chỉ đọc lướt thứ tự khởi tạo.

**Thực hành (≈45')**
1. Làm phần "Chuẩn bị môi trường".
2. Chạy `./os os_syscall_list` và `./os os_0_mlq_paging > a.log`. Với mỗi dòng trong log, ghi ra *thread nào* in nó.
3. Trên giấy: vẽ class diagram 9 struct ở `01` §4, không nhìn tài liệu.

**Active recall**
1. Page table nằm ở đâu: RAM mô phỏng hay heap host?
2. Trong chế độ paging, `alloc 300 4` ghi kết quả vào `regs[4]` hay `symrgtbl[4]`?
3. Vì sao config `os_syscall` không `alloc` được?
4. Kể 5 bước dọn dẹp khi process kết thúc.

**Tiêu chí đã hiểu:** giải thích sơ đồ `01` §2.1 trong 3 phút cho một người khác; trả lời đúng 4 câu trên không mở tài liệu.

---

## Ngày 2 — Process, CPU, timer, scheduler

**Learning objectives**
- Trace một process từ file trên đĩa tới `Processed X has finished`.
- Giải thích thuật toán MLQ đúng như code (slot theo dispatch, reset).
- Vẽ Gantt chart từ log.

**Tiên quyết:** ngày 1; khái niệm round-robin, priority scheduling, time slice.

**Tài liệu (≈50')**
- `02-process-cpu-scheduler.md`.
- Spec §1.3–1.5 (tr. 5–8), §2.1 (tr. 12–13).

**Source (≈50')**
- `src/loader.c:44-57` (`op_table`), `:202-301` (`load`).
- `src/cpu.c:76-153` (`run`).
- `src/timer.c:21-77` (`timer_routine`, `next_slot`).
- `src/sched.c:44-142`, `src/queue.c`.
- `src/os.c:322-420` (`cpu_routine`), `:422-516` (`ld_routine`).

**Thực hành (≈45')**
1. Chạy `./os sched_0`, vẽ Gantt đầy đủ; so với `02` §5.4.
2. Chạy `./os sched_1` và dự đoán trước thứ tự dispatch.
3. Trong `/tmp/ossim`, đổi `input/sched_0` sang 2 CPU, chạy 2 lần, giải thích vì sao log khác nhau.

**Active recall**
1. `slot[prio]` giảm mỗi instruction hay mỗi dispatch?
2. Khi nào reset toàn bộ `slot[]`?
3. Lệnh lỗi có làm process dừng không? Vì sao (`cpu.c` dòng nào)?
4. Vì sao trong một slot, log của các CPU có thể xen kẽ khác nhau?

**Tiêu chí:** vẽ được flowchart `cpu_routine` (`02` §7) không nhìn; giải thích đúng Gantt của `sched_1`.

---

## Ngày 3 — Physical memory, layout địa chỉ, PTE (Module 1, phần 1)

**Learning objectives**
- Mô tả `memphy_struct` và 5 invariant của nó.
- Chạy tay `MEMPHY_get_freefp_range` / `MEMPHY_put_freefp`.
- Cắt mọi địa chỉ 57-bit thành 5 chỉ số + offset; đọc/viết PTE ở hex.

**Tiên quyết:** ngày 1; phép dịch bit và mask; khái niệm frame/page.

**Tài liệu (≈50')**
- `03` §0–4 và §6.
- Spec §2.2.2–2.2.4 (tr. 15–19): Table 2, ví dụ `0x39ffe1d9c9000`, Figure 6 PTE.

**Source (≈60')**
- `src/mm-memphy.c` **toàn bộ** (285 dòng) — đây là file của bạn, đọc từng dòng.
- `include/mm.h:11-71` (bit PTE), `include/mm64.h` (layout 57-bit, giới hạn).
- `src/mm64.c:58-62` (`pgn_is_canonical`), `:244-300` (`init_pte`, `get_pd_from_*`).
- `src/os.c:648-689` (tạo RAM/SWAP, kẹp kích thước).

**Thực hành (≈45')**
1. `sh tests/run_memphy_tests.sh`; mở `tests/test_memphy.c`, đọc `test_exhaustion_and_contiguous` và `test_model_cycles` — mỗi test kiểm invariant nào?
2. Bài tập `03` cuối file: free list `0,1,2,4,5,8,9,10,11`, `count = 4`.
3. Cắt tay: `0x1014`, `0x200000`, `0x40000000`, `0xFF00000000001000`. Kiểm bằng Python.
4. Viết PTE hex cho: FPN 7; swap device 0 slot 9; swap device 1 slot 0.

**Active recall**
1. Vì sao free list phải tăng dần?
2. `MEMPHY_get_freefp_range` có `malloc` không? Tại sao quan trọng?
3. Vì sao FPN là 16 bit? RAM tối đa?
4. Một `pgn` hợp lệ phải thỏa điều kiện gì ở bit cao?
5. PTE `0xDEADBEEF` thuộc trạng thái nào và từ đâu ra?

**Tiêu chí:** giải thích `MEMPHY_get_freefp_range` bằng ví dụ trên bảng trắng; cắt địa chỉ và viết PTE không sai lần nào.

---

## Ngày 4 — Cây 5 cấp, translation, mapping (Module 1, phần 2)

**Learning objectives**
- Giải thích `pt_walk_locked` và demand allocation; vẽ cây trước/sau một lần map.
- Tính kích thước page table và số lần truy cập bộ nhớ (số liệu cho báo cáo).
- Giải thích 3 pha all-or-nothing của `vm_map_ram`.

**Tiên quyết:** ngày 3.

**Tài liệu (≈50')**
- `03` §5, §7, §8.
- Spec §2.2.2 "Multi-level page table size" (tr. 17), §3.2 "Multi-level paging".

**Source (≈60')**
- `src/mm64-internal.h` (5 struct).
- `src/mm64.c:75-229` (walker, create/destroy, foreach), `:302-387` (pte API), `:1055-1069` (`pte_clear_entry`).
- `src/mm64.c:420-586` (`pt_reserve`, `range_in_vma`, `prepare_pages`, `map_prepared`, `vmap_*`).
- `src/mm64.c:756-794` (`vm_map_ram`), `:843-879` (`init_mm`), `:997-1047` (`print_pgtbl`).

**Thực hành (≈45')**
1. `sh tests/run_mm64_pt_tests.sh` và `run_mm64_map_tests.sh`. Đọc `test_boundaries`, `test_out_of_memory_rollback`.
2. Từ một dòng `PDG=… P4D=… PUD=… PMD=…PT=…` trong log, giải thích vì sao các địa chỉ cách nhau `0x1010`.
3. Tính page table cho process có page ở `0x1000`, `0x200000`, `0x40000000`.
4. Viết lại pseudocode `vm_map_ram` trên giấy, đánh dấu chỗ nào hoàn tác được.

**Active recall**
1. `create = 0` có bao giờ cấp phát? API nào dùng `create = 0`?
2. Nếu `calloc` thất bại giữa đường walk thì node đã tạo bị xử lý thế nào? Vì sao chấp nhận được?
3. Vì sao `pte_*` nhận `mm` tường minh thay vì tự lấy `caller->mm`?
4. 3 pha của `vm_map_ram` là gì?

**Tiêu chí:** vẽ được cây `03` §5.4 và trình bày số liệu `03` §5.5 trong 2 phút; giải thích vì sao thất bại không để lại trạng thái dở.

---

## Ngày 5 — Page replacement, swap, teardown và VM phía trên (Module 1, phần 3)

**Learning objectives**
- Trace `alloc_pages_range` và `pg_getpage` khi RAM đầy, với bảng trạng thái.
- Giải thích FIFO toàn cục, ghim DMA, và vì sao victim chỉ commit cuối cùng.
- Hiểu tầng VM gọi xuống paging ở đâu (`__alloc`, `__read`, `__write`).

**Tiên quyết:** ngày 3–4; khái niệm FIFO replacement, swap-in/out.

**Tài liệu (≈60')**
- `03` §9–14 (đặc biệt worked example §10).
- `04` toàn bộ.
- `06` §3, §4, §5, §6.

**Source (≈60')**
- `src/mm64.c:588-741` (`mm64_fifo_victim`, `mm64_fifo_drop`, `alloc_pages_range`), `:804-817` (`__swap_cp_page`).
- `src/libmem.c:284-361` (`pg_getpage`), `:369-446` (`pg_getval/setval`), `:1549-1693` (`swap_dev_of`, `free_pcb_memph`, `find_victim_page`).
- `src/libmem.c:71-212` (`__alloc`, `__free`), `:456-565` (`__read`, `__write`), `:1701-1759` (`get_free_vmrg_area`).
- `src/mm-vm.c` (để thấy `inc_vma_limit` là stub, `get_vma_by_num` có bug).

**Thực hành (≈45')**
1. `sh tests/run_swap_tests.sh`, `run_mm_teardown_tests.sh`. Đọc `test_processes_share_one_frame`.
2. Tạo `study_swp` theo `06` §0, chạy, tự điền lại bảng `03` §10 **trước khi** xem.
3. `./os os_1_mlq_paging_small_4K | grep -E "SWAP|IO_"` — xác định page của ai bị đẩy ra ở mỗi dòng.
4. Làm bài tập `04` (tiếp bảng `p0s`, bảng `m1s`).

**Active recall**
1. Vì sao `mm64_fifo_victim` duyệt hết danh sách? Trả về kiểu gì, vì sao?
2. Eviction nào đi qua syscall `SWP_OP`, eviction nào không?
3. Khi swap-in, thiết bị và slot lấy từ đâu? Slot cũ có được giữ không?
4. `free` có unmap không? Frame trả khi nào?
5. Vì sao thread của process B được phép sửa PTE của process A?

**Tiêu chí:** trình bày worked example `03` §10 từ trí nhớ, kể cả giá trị hex của PTE; giải thích thrashing ở `small_4K`.

---

## Ngày 6 — Kernel memory, system call, DMA và tích hợp

**Learning objectives**
- Giải thích kernel address space, `kmap_slot`, `__kmalloc` (contiguous, không swap).
- Giải thích cơ chế sinh bảng syscall và mọi op của `sys_memmap`.
- Trace một `dma_memcpy` từ CPU tới RTL và callback.

**Tiên quyết:** ngày 1–5.

**Tài liệu (≈60')**
- `05` toàn bộ.
- `06` §7, §8, §9, §10, §11.
- Spec §1.6 (tr. 8–11), §2.3 (tr. 21–24), phụ lục DMA (tr. 33).

**Source (≈50')**
- `src/libmem.c:604-847` (kmalloc/kfree), `:1198-1546` (copy_*, `__read/__write_*_mem`), `:853-1195` (DMA).
- `src/syscall.c`, `src/syscall.tbl`, `src/syscalltbl.sh`, `src/libstd.c`, `src/sys_mem.c`.
- `src/os.c:73-315` (dịch vụ DMA), `src/dma_c_testbench.cpp:78-149` (`do_dma`), `src/sysbus.c:179-228`.

**Thực hành (≈45')**
1. `sh tests/run_kmem_tests.sh`.
2. Tạo và chạy `study_kd`, `study_dma` (`06` §0). Giải thích vì sao `dma_memcpy … 2` không in `DMA completed`.
3. Chạy `./os os_2_singleCPU_mlq_paging`, giải thích từng lệnh lỗi của `m2s`; viết lại `m2s` cho đúng (trong `/tmp`).

**Active recall**
1. Kernel memory có chứa PCB/page table không?
2. Ba khác biệt giữa frame của `kmalloc` và frame user.
3. `syscall 440 1` xảy ra gì?
4. Vì sao phải ghim frame khi DMA? CPU chờ DMA ở đâu?
5. Lỗ hổng `SYSMEM_IO_WRITE` là gì?

**Tiêu chí:** vẽ sequence diagram DMA `05` §C.3 không nhìn; trace đúng `copy_from_user` qua các khóa.

---

## Ngày 7 — Tổng ôn và bảo vệ thử

**Learning objectives**
- Trả lời 7 câu hỏi spec bằng lời của mình, có liên hệ code.
- Trình bày requirement matrix và gap một cách chủ động.
- Chuẩn bị số liệu và hình cho báo cáo.

**Tài liệu (≈40')**
- `07` toàn bộ; xem lại `03` §15–16.

**Thực hành (≈100')**
1. **Bảo vệ thử 30'**: nhờ bạn cùng nhóm hỏi ngẫu nhiên từ `07` §6 (ưu tiên Intermediate/Advanced phần Module 1). Chỉ xem `07` §7 sau khi trả lời.
2. **Chuẩn bị báo cáo 40'**: Gantt chart (`sched_0`, `os_1_singleCPU_mlq`); sơ đồ dịch địa chỉ 5 cấp với ví dụ `0x1014` và ví dụ spec; bảng storage/memory access (`03` §5.5); bảng state `03` §10.
3. **Demo khô 30'**: chạy `os_0_mlq_paging`, `os_1_mlq_paging_small_4K`, `study_dma` và thuyết minh từng dòng log quan trọng.

**Active recall (tổng)**
1. Vẽ toàn bộ đường đi của `write 100 1 20` từ `run()` tới `mram.storage[20]`.
2. Kể các invariant của Module 1 (`03` §13).
3. 3 trade-off lớn nhất của thiết kế paging và lý do chọn.
4. 3 gap đáng nói nhất và cách sửa.

**Tiêu chí hoàn thành lộ trình**
- Trả lời đúng ≥ 90% Basic, ≥ 75% Intermediate, ≥ 50% Advanced của `07` §6 mà không xem đáp án.
- Tự trace một kịch bản mới (ví dụ RAM 3 frame, 2 process, 2 page mỗi process) ra bảng trạng thái, rồi chạy simulator xác nhận.
- Giải thích được mọi dòng log của `06` §5 và §6.

---

## Gợi ý phương pháp mỗi ngày

1. **10' recall đầu buổi**: trả lời lại câu hỏi active recall của hôm trước, không mở tài liệu.
2. **Đọc tài liệu → mở code → chạy**: luôn theo thứ tự này; đừng đọc code trước khi biết nó trả lời câu hỏi gì.
3. **Viết tay** bảng trạng thái; đây là kỹ năng được kiểm tra nhiều nhất khi vấn đáp.
4. Mỗi khi gặp chỗ tài liệu ghi **[I]** hoặc **[U]**, coi đó là bài tập: tìm cách biến nó thành **[V]**.
