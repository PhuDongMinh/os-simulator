# 07 — Chuẩn bị bảo vệ đồ án

> Dùng file này **sau** khi đã học `01`–`06`. Phần câu hỏi (§6) và đáp án (§7) được tách riêng:
> tự trả lời thành tiếng trước, rồi mới xem đáp án. Ký hiệu [V]/[C]/[I]/[U] như `01`.

---

## 1. Trả lời các câu hỏi trong specification

Mỗi câu: **ý chính lý thuyết** → **liên hệ với chính simulator của nhóm** (giám khảo đánh giá cao phần này).

### Q1 (§1.6.2) — Ưu/nhược điểm của unified system call interface
**Ưu điểm**
- Một cổng vào duy nhất ⇒ dễ kiểm soát quyền, kiểm tra tham số, audit. Ở simulator: mọi truy cập MEMPHY
  từ thư viện user đi qua `_syscall` → `__sys_memmap`, và kernel luôn tra PCB **từ PID** (`get_pcb_by_pid`).
- API nhỏ, đồng nhất (read/write cho mọi loại tài nguyên) ⇒ portability, chương trình không phụ thuộc thiết bị.
- Dễ mở rộng: thêm syscall = thêm một dòng `syscall.tbl` (bảng sinh tự động bằng X-macro).

**Nhược điểm**
- **Hiệu năng:** mọi thao tác trả phí "vào kernel". Ở simulator, **mỗi byte** đọc/ghi = 1 syscall + quét
  registry 128 phần tử.
- **Mất ngữ nghĩa riêng:** một interface chung không diễn tả được thao tác đặc thù (cần `ioctl`-like). Ở đây
  `sys_memmap` phải ghép 5 thao tác khác hẳn nhau qua `a1`.
- **Xử lý lỗi phức tạp:** một mã lỗi `-1` cho mọi nguyên nhân (PID sai, op lạ, địa chỉ ngoài RAM). Tệ hơn,
  syscall không tồn tại trả **0** qua `__sys_ni_syscall` ⇒ lỗi bị nuốt. **[V]**
- **Tham số không định kiểu** (`a1..a3` là `uint64_t`) ⇒ kernel phải tự kiểm; thiếu kiểm là lỗ hổng: `IO_WRITE`
  gọi thẳng từ user ghi được địa chỉ vật lý bất kỳ. **[C]**
- Cân bằng portability ↔ efficiency: interface chung portable nhưng khó tối ưu (ví dụ không batch được nhiều byte).

### Q2 (§1.6.3) — Syscall chạy quá lâu: OS phát hiện và xử lý thế nào?
**OS thật:** timer interrupt vẫn đến; kernel preemptible cho phép scheduler giành CPU; syscall blocking đưa
process vào trạng thái ngủ; watchdog/soft-lockup detector (Linux: soft lockup, hung task) cảnh báo khi một CPU
kẹt trong kernel quá ngưỡng; signal ngắt syscall (`EINTR`); timeout cho các lời gọi chờ.

**Simulator:** **không** phát hiện. Syscall chạy đồng bộ trong `run()` trong một slot; timer chờ mọi thread
xong slot (barrier) ⇒ một syscall chạy mãi làm **toàn bộ simulator đứng**. Không có preemption trong kernel. **[C]**

**Hướng xử lý có trong code:** biến thao tác dài thành **bất đồng bộ** — chính là `dma_memcpy`: trả về ngay,
worker chạy RTL, callback báo xong; CPU chỉ chờ ở `dma_process_exit`. Có thể đề xuất: ngân sách bước mỗi slot,
chia syscall thành nhiều bước resumable, hoặc watchdog đếm slot.

### Q3 (§2.1) — Lợi ích của từng policy trong MLQ
| Policy | Lợi ích | Trong code |
|---|---|---|
| Mỗi priority một queue | Chọn process O(MAX_PRIO), không phải sắp xếp | `mlq_ready_queue[140]` |
| Priority cố định, không feedback | Đơn giản, dự đoán được, phù hợp tách system daemon / user job | `proc->prio` từ config |
| Slot = MAX_PRIO − prio | Queue cao được nhiều lượt hơn **nhưng có giới hạn** ⇒ queue thấp không chết đói hoàn toàn (khác strict priority) | `slot[]`, reset khi mọi queue không rỗng đã hết slot (`sched.c:80-92`) |
| Round-robin trong queue + time slice | Công bằng giữa các process cùng mức, response time bị chặn | `dequeue` lấy đầu, `put_proc` thêm cuối |
| Dual priority (default vs live) | Người dùng chạy chương trình với mức khác mặc định | `prio` từ config ghi đè `priority` |

Hạn chế: slot đếm theo lần dispatch; với tải nhỏ trông như strict priority (`02` §5.3); không có aging.

### Q4 (§2.2.2) — Vì sao kết hợp segmentation với paging?
- Segmentation: đơn vị logic (code/heap/stack), giới hạn và quyền theo segment, segment lớn lên được — nhưng
  **external fragmentation** và khó cấp vùng liên tục lớn.
- Paging: hết external fragmentation, swap theo page — nhưng không có cấu trúc logic, **internal fragmentation**,
  bảng trang lớn.
- Kết hợp: segment (VMA) xác định **vùng hợp lệ và giới hạn**; paging ánh xạ **bên trong** segment xuống frame rời rạc.
  Chỉ phần đã dùng của segment mới có page table.
- Trong simulator: `vm_area_struct` (vm_start/sbrk/vm_end/vm_limit) + region list là tầng "segment";
  `__read` kiểm region **trước** khi gọi paging; vma lớn lên bằng sbrk, frame được map từng page (`04` §1, §8).

### Q5 (§2.2.3) — Lợi ích của hierarchical paging N cấp
- **Sparse:** chỉ tạo bảng cho vùng dùng. Số liệu: 1 page = 18 472 B bảng; bảng phẳng cho 45-bit pgn = 128 TiB (`03` §5.5).
- Mỗi node vừa một page (4 KiB) ⇒ dễ cấp phát, có thể tự swap/page bảng (OS thật).
- Mở rộng không gian địa chỉ chỉ bằng thêm một cấp (4 cấp → 5 cấp: 256 TiB → 128 PiB).
- Chia sẻ cả cây con (ví dụ nửa kernel), huge page ở cấp cao (không có trong simulator).
- **Giá:** mỗi lần dịch 5 lần truy cập bộ nhớ thay vì 1 ⇒ cần TLB/page-walk cache (simulator không có).

### Q6 (§2.2.4) — Paging vs contiguous allocation
| | Paging | Contiguous |
|---|---|---|
| Fragmentation | Không external; internal ≤ 1 page/vùng (`alloc 300` chiếm cả page 4096) | External; cần compaction |
| Đặt vào RAM | Rời rạc, frame bất kỳ | Phải tìm khối đủ lớn |
| Swap / chia sẻ / bảo vệ | Theo page, linh hoạt | Cả khối |
| Dịch địa chỉ | Tra bảng nhiều cấp | base + limit, rất nhanh |
| Metadata | Page table | Hai thanh ghi |
| Phần cứng (DMA) | Phải cắt theo page | Thuận tiện |

Simulator có **cả hai**: user memory dùng paging; `kmalloc` dùng contiguous (`MEMPHY_get_freefp_range`) —
có thể thất bại dù tổng frame trống đủ, nếu chúng không liền nhau (`test_exhaustion_and_contiguous`). DMA phải
cắt copy tại mọi ranh giới page (`dma_build_runs`) vì trang ảo liên tục có thể ở frame xa nhau.

### Q7 (§2.4) — Nếu không đồng bộ hóa thì sao? Minh họa với kernel memory
**Tài nguyên dùng chung và khóa bảo vệ:**

| Tài nguyên | Khóa |
|---|---|
| ready queues, running_list | `queue_lock` (`sched.c:19`) |
| free/used frame list, storage | `memphy_lock` |
| cây page table mỗi mm | `pgd->lock` |
| user VMA/region, `krnl->fifo_pgn` | `mmvm_lock` |
| kernel symrgtbl, `kmap_slot` | `kmm_lock` |
| `proc_registry` | `registry_lock` |
| hàng đợi DMA, bộ đếm pending | `dma_lock` |
| pin count | `dma_pin_lock` |

**Ví dụ cụ thể (kernel memory):** hai CPU cùng chạy `__kmalloc` cho PID 2 và PID 3 mà không có `kmm_lock`:
1. Cả hai quét `os.mm->symrgtbl`, cùng thấy slot 0 trống.
2. Mỗi bên lấy dải frame khác nhau, cùng ghi `symrgtbl[0]`; bên ghi sau thắng.
3. `kmap_slot` của cả hai process trỏ slot 0 ⇒ `copy_from_user` của PID 2 ghi vào buffer của PID 3 (rò rỉ dữ liệu
   giữa process); frame của bên thua không còn ai trỏ tới ⇒ leak vĩnh viễn.
4. Khi cả hai exit, `__kfree` slot 0 hai lần ⇒ lần hai `MEMPHY_put_freefp` thất bại (`[kmem] WARNING …`).

Tương tự không có `memphy_lock`: hai CPU nhận **cùng một frame** ⇒ hai process ghi đè dữ liệu của nhau; không có
`queue_lock`: cùng một process được dispatch trên hai CPU.

Simulator cho thấy tính song song thật: trong một slot, log của các CPU xen kẽ khác nhau giữa các lần chạy
(`06` §6). Chỗ **chưa** đồng bộ hoàn toàn: RTL DMA ghi RAM không qua `memphy_lock`; `copy_*` khóa từng byte
nên không nguyên tử (`05` §A.5, §C.5).

---

## 2. Yêu cầu báo cáo (§3.3) — chuẩn bị gì

| Yêu cầu | Nguồn dữ liệu |
|---|---|
| Gantt chart scheduling | Log `sched_0`, `sched_1`, `os_1_singleCPU_mlq` (`02` §5.4 mẫu) |
| Trạng thái cấp phát trong data segment | Bảng `04` §4.3; dòng `print_pgtbl` sau mỗi alloc/free |
| Giao tiếp kernel/user bằng PID | `_syscall(krnl, pid, …)`, `get_pcb_by_pid` (`sys_mem.c:24`) |
| Sơ đồ dịch địa chỉ nhiều cấp | `03` §4, §5.4, §7 |
| Thống kê số lần truy cập + kích thước bảng trang | **Simulator không có bộ đếm** ⇒ trình bày số liệu tính từ `sizeof` (`03` §5.5) và nói rõ là phân tích |

---

## 3. Bảng đối chiếu requirement ↔ implementation

| Requirement (spec) | Trạng thái | Bằng chứng | Ghi chú |
|---|---|---|---|
| `enqueue`/`dequeue`/`get_proc` MLQ (§2.1) | ✅ | `queue.c`, `sched.c`; log `sched_*` [V] | slot theo dispatch |
| Paging, tách user/kernel space (§3.2) | ✅ | `proc->mm` riêng, `os.mm` chung; READ/WRITE chặn kernel [V] | Lỗ hổng `IO_WRITE` trực tiếp [C] |
| Kernel–user qua PID, không truyền PCB | ✅ | `get_pcb_by_pid` [V] | |
| Multi-level paging 64-bit (§3.2) | ✅ | Cây 5 cấp, 57-bit canonical, 6 test suite [V] | |
| Page replacement | ✅ | FIFO toàn cục + swap-in/out [V] | |
| Thống kê memory access / paging storage | ⚠️ | Không có counter | Tính tay |
| Co-processor bất đồng bộ (1đ) | ✅ | Hàng đợi + `dma_worker` + callback [V] | |
| Bus addressing: CPU qua MMU (1đ) | ✅ | `dma_vtop` dùng page table [V] | Chỉ page đang ở RAM |
| Bus addressing: co-processor truy cập RAM (1đ) | ✅ | `memory_bank_attach(mram.storage)`, DPI callbacks [V] | |
| Dùng chip DMA thật qua Verilator | ✅ | `Vdma_top*`, `dpi_rtl.cpp`; log `DMA completed` [V] | Đuôi 1–3 byte do CPU chép |
| Synchronization (§2.4) | ✅ | 8 khóa (bảng Q7) [C] | 2 chỗ chưa nguyên tử |
| Thêm syscall ví dụ (§1.6.3, `syscall 440`) | ❌ | Không có trong `syscall.tbl` [V] | Trả 0 lặng lẽ |
| Config `KERNEL_BASE_ADDR` (§2.2.5) | ❌ | `read_config` không đọc [C] | Base cố định |
| Layout kiểu macOS | ⚠️ | Chỉ có `__PAGEZERO` [C] | Không chia small/medium/large |
| Instructions mới (kmalloc, copy_*, dma_memcpy) | ✅ | `loader.c:44`, `cpu.c:99-112` [V] | |
| `MM_FIXED_MEMSZ` backward compat | ⚠️ | Có code, đặt RAM `0x100000000` > 256 MiB rồi bị kẹp [C] | Không bật mặc định |

---

## 4. Các quyết định thiết kế chính và trade-off

(Chi tiết Module 1 ở `03` §15. Dưới đây là phiên bản "nói trong 2 phút".)

1. **Page table là cây con trỏ trong heap host, cấp lười.** Đơn giản, đúng tinh thần "demand allocation"; đổi lại
   overhead bảng trang không xuất hiện trong RAM mô phỏng.
2. **PTE 32 bit, FPN mở rộng lên 16 bit.** Giữ đúng format spec, hỗ trợ RAM 256 MiB của config mẫu.
3. **Mọi thao tác đa bước là all-or-nothing** (`vm_map_ram`, `alloc_pages_range`, `__kmalloc`): thất bại không để
   lại frame mồ côi hay PTE nửa vời.
4. **FIFO toàn cục**, bỏ qua frame bị DMA ghim: RAM nhỏ vẫn chạy được nhiều process.
5. **Kernel memory liên tục, không swap**, ID qua `kmap_slot` như file descriptor.
6. **Syscall nhận PID**, kernel tự tra registry.
7. **DMA bất đồng bộ** với hàng đợi + worker riêng; CPU chỉ chờ khi process kết thúc.
8. **Thứ tự khóa cố định** `mmvm/kmm → pgd → memphy` (memphy là khóa lá).

---

## 5. Limitations, bugs và implementation gaps đã phát hiện

Không sửa trong phạm vi tài liệu này — chỉ liệt kê để chủ động trình bày.

| # | Mô tả | Vị trí | Mức | Bằng chứng |
|---|---|---|---|---|
| 1 | User gọi `syscall 17 5 paddr v` ghi được địa chỉ vật lý bất kỳ (bypass VM, chạm kernel) | `sys_mem.c:97-105` | Cao (bảo mật) | [C] |
| 2 | `inc_vma_limit` là stub; mở rộng vùng làm trong `__alloc` thay vì kernel | `mm-vm.c:136` | Trung bình (lệch kiến trúc) | [V] |
| 3 | `get_vma_by_num` dereference NULL khi `vmaid` vượt số VMA | `mm-vm.c:37-44` | Trung bình (tiềm ẩn) | [C] |
| 4 | Syscall không tồn tại trả 0 (nuốt lỗi) | `syscall.c:29-36` | Trung bình | [V] |
| 5 | Program mẫu `m2s` lẫn user ID/kernel ID ⇒ nhiều lệnh lỗi | `input/proc/m2s` | Trung bình (demo) | [V] |
| 6 | Config RAM 2048 B ⇒ 0 frame với page 4 KiB ⇒ mọi alloc user lỗi | `os_syscall`, `os_sc`, `os_syscall_list`, `small_1K` | Trung bình (output khác mẫu) | [V] |
| 7 | Swap > `INT_MAX` ⇒ `exit(1)` dù PTE cho phép 8 GiB | `mm-memphy.c:82` | Thấp | [C] |
| 8 | Không có counter thống kê memory access / storage | — | Trung bình (yêu cầu báo cáo) | [C] |
| 9 | Dirty bit không dùng ⇒ luôn ghi lại page khi evict | `mm64.c` | Thấp (hiệu năng) | [C] |
| 10 | Chọn victim O(k) lần walk 5 cấp | `mm64.c:601` | Thấp (hiệu năng) | [C] |
| 11 | Evict trong `alloc_pages_range` không qua `SYSMEM_SWP_OP` (khác `pg_getpage`) | `mm64.c:684` | Thấp (nhất quán) | [V] |
| 12 | `__free` không kiểm `malloc`; free region không coalesce | `libmem.c:199` | Thấp | [C] |
| 13 | `copy_*` khóa từng byte (không nguyên tử); RTL ghi RAM không qua `memphy_lock` | `libmem.c:1248`, `sysbus.c:211` | Thấp–TB (race) | [I] |
| 14 | `enqueue` bỏ process lặng lẽ khi queue đầy (50) | `queue.c:16` | Thấp | [C] |
| 15 | Không đọc `KERNEL_BASE_ADDR` | `os.c:563` | Thấp | [C] |
| 16 | `alloc` không ghi `regs[]`; register = region ID | `libmem.c:71` | Thấp (diễn giải) | [V] |
| 17 | Nhãn log `SWAP … pgn=` thực ra là victim FPN | `sys_mem.c:94` | Rất thấp | [V] |
| 18 | `printf` debug `libread:503`, `libfree:251` | `libmem.c` | Rất thấp | [V] |
| 19 | `Document/Memory_layout.md` (47-bit) và trạng thái trong `API_CONTRACT.md` đã cũ; sơ đồ thứ tự khóa registry ngược code | `Document/` | Rất thấp | [C] |
| 20 | Nhánh 32-bit (`mm.c`) không còn build được đúng | `mm.c` | Rất thấp (không dùng) | [C] |

---

## 6. Bộ câu hỏi vấn đáp (chỉ câu hỏi — tự trả lời trước)

### 6.1 Basic
- **B1.** Page và frame khác nhau thế nào? Kích thước trong simulator là bao nhiêu, định nghĩa ở đâu?
- **B2.** `memphy_struct` có những field nào? `free_fp_list` và `used_fp_list` dùng để làm gì?
- **B3.** RAM 1 MiB có bao nhiêu frame? RAM 2048 B có bao nhiêu? Hệ quả?
- **B4.** Tách `0x1014` thành pgn và offset.
- **B5.** Bit 31 và 30 của PTE nghĩa là gì? PTE = 0 nghĩa là gì?
- **B6.** Kể tên 5 cấp page table và dải bit của từng cấp.
- **B7.** Vì sao page 0 không được map?
- **B8.** RAM và SWAP khác nhau thế nào trong simulator?
- **B9.** Chính sách thay trang là gì?
- **B10.** Ai gọi `vm_map_ram`, khi nào?
- **B11.** Lệnh `free` có trả frame không? Frame được trả khi nào?
- **B12.** Dải địa chỉ user space và kernel space?
- **B13.** Scheduler dùng chính sách gì, "slot" nghĩa là gì?
- **B14.** Vì sao syscall truyền PID chứ không truyền PCB?

### 6.2 Intermediate
- **I1.** Chạy tay `MEMPHY_get_freefp_range` với free list `1,2,5,6,7`, `count = 3`.
- **I2.** Vì sao `free_fp_list` phải luôn được sắp xếp?
- **I3.** `pt_walk_locked` với `create = 0` và `create = 1` khác nhau thế nào? API nào dùng loại nào?
- **I4.** Tính bộ nhớ page table của process có page ở `0x1000` và `0x200000`.
- **I5.** Giải thích 3 pha của `vm_map_ram` và lý do thiết kế.
- **I6.** Vì sao `mm64_fifo_victim` duyệt hết danh sách thay vì lấy phần tử cuối?
- **I7.** Trace `pg_getpage` khi page đang ở swap và RAM đã đầy.
- **I8.** Vì sao FIFO là toàn cục? Cái giá là gì?
- **I9.** Khi swap-in, làm sao biết page nằm ở **thiết bị nào** và **slot nào**?
- **I10.** Vì sao FPN 16 bit? Hệ quả với config?
- **I11.** Kernel page khác user page ở 3 điểm nào?
- **I12.** `free_pcb_memph` làm sao biết phải trả frame/slot nào?
- **I13.** Thứ tự khóa là gì? Vì sao thread của process B sửa PTE của process A mà vẫn an toàn?
- **I14.** Đọc một page đang ở swap khi RAM đầy **và** swap cũng đầy thì sao?

### 6.3 Advanced
- **A1.** Hai CPU cùng `alloc` khi RAM còn đúng 1 frame trống — trace với các khóa.
- **A2.** Điều gì hỏng nếu PTE của victim được đổi sang SWAPPED **trước** khi đủ frame?
- **A3.** DMA đang ghim frame F; mọi page resident khác cũng bị ghim; một process khác `alloc`. Chuyện gì xảy ra?
- **A4.** Muốn đổi FIFO thành Clock/LRU, cần thêm gì và sửa ở đâu?
- **A5.** Belady's anomaly có thể xảy ra ở đây không? Vì sao?
- **A6.** Vì sao `pg_getpage` từ chối kernel mm? Nếu cho kernel page swap được thì rủi ro gì?
- **A7.** Thiết kế TLB cho simulator: đặt ở đâu, invalidate khi nào?
- **A8.** Page table nằm trong heap host — ảnh hưởng gì tới tính đúng của thống kê bộ nhớ? Làm sao đưa vào RAM mô phỏng?
- **A9.** Race nào còn lại giữa DMA và lệnh `write`? Đề xuất cách sửa.
- **A10.** Nếu thêm dirty bit, set ở đâu và dùng ở đâu?
- **A11.** `__alloc` giữ `mmvm_lock` rồi gọi syscall (lấy `registry_lock`). Có deadlock không?
- **A12.** Nếu copy swap-in thất bại **sau** khi victim đã bị đẩy ra, hệ thống còn nhất quán không?

---

## 7. Đáp án mẫu

### 7.1 Basic
- **B1.** Page = đơn vị của không gian **ảo**; frame = đơn vị của **RAM vật lý**; cùng kích thước 4096 B
  (`PAGING64_PAGESZ`, `include/mm64.h:10`). Page table ánh xạ page → frame.
- **B2.** `storage` (mảng byte), `maxsz`, `rdmflg`, `cursor`, `free_fp_list` (frame trống, tăng dần),
  `used_fp_list` (frame đã cấp). Mỗi frame ở đúng một danh sách; trả frame chưa cấp bị từ chối.
- **B3.** 1 MiB / 4096 = 256 frame. 2048 B ⇒ 0 frame ⇒ mọi `alloc` user thất bại (thấy ở `os_syscall`).
- **B4.** pgn = `0x1014 >> 12` = 1; offset = `0x1014 & 0xFFF` = `0x14` = 20.
- **B5.** Bit 31 PRESENT (ở RAM), bit 30 SWAPPED (ở swap). PTE 0 = chưa từng map; truy cập ⇒ `pg_getpage` trả -1.
- **B6.** PGD 56–48, P4D 47–39, PUD 38–30, PMD 29–21, PT 20–12, offset 11–0; mỗi cấp 9 bit = 512 mục.
- **B7.** Để NULL dereference luôn lỗi (macOS `__PAGEZERO`), và để `rg_start == 0` có thể dùng làm cờ "region trống".
- **B8.** RAM: CPU truy cập trực tiếp, chứa page đang dùng, ≤ 256 MiB, DMA thấy. SWAP: chỉ copy nguyên frame qua
  `__swap_cp_page`, tối đa 4 thiết bị, swap-out luôn vào `mswp[0]`.
- **B9.** FIFO toàn cục `krnl->fifo_pgn`, bỏ qua page không resident hoặc frame bị DMA ghim.
- **B10.** `__alloc` (đường B, khi `vm_end < next_brk`), để map các page mới ở cuối vma0.
- **B11.** Không; region về free list để tái dùng. Frame được trả khi process kết thúc (`free_pcb_memph`).
- **B12.** User `[0x1000, 0x00FF_FFFF_FFFF_FFFF]`; kernel `[0xFF00_0000_0000_0000, 0xFFFF_FFFF_FFFF_FFFF]`.
- **B13.** MLQ 140 queue; `slot[prio] = 140 − prio` lượt dispatch mỗi vòng; hết thì nhường queue sau; reset khi mọi
  queue không rỗng đều hết slot.
- **B14.** Kernel không tin con trỏ từ user; PID được tra trong `proc_registry` của kernel ⇒ process không thể
  đưa PCB giả hay PCB đã giải phóng; spec §3.3 yêu cầu.

### 7.2 Intermediate
- **I1.** Bắt đầu ở 1: `1,2` dài 2 < 3 ⇒ nhảy qua sau 2. Bắt đầu ở 5: `5,6,7` dài 3 ⇒ gỡ cả đoạn, đẩy lên đầu used
  list, trả `fpn = 5`. Free list còn `1,2`.
- **I2.** Để tìm dãy frame liên tiếp bằng một lần quét (so `fpn+1`), phục vụ `kmalloc`. `MEMPHY_put_freefp` chèn
  đúng vị trí để giữ bất biến này.
- **I3.** `create = 0`: chỉ tra, gặp NULL trả NULL ⇒ PTE đọc là 0, không cấp phát (`pte_get_entry`,
  `pte_clear_entry`). `create = 1`: `calloc` node thiếu (`pte_set_entry` và các `pte_set_*`, `pt_reserve`).
- **I4.** Root 4 136 + P4D + PUD + PMD (3×4 096) + 2 leaf (2×2 048) = **20 520 B**. `0x200000` có PMD index 1 nên cần
  leaf thứ hai; các cấp trên dùng chung.
- **I5.** (1) `prepare_pages`: tạo đường đi, kiểm page chưa map, tạo node FIFO — rẻ, hoàn tác được. (2)
  `alloc_pages_range`: lấy frame, victim chỉ được copy ra swap, **chưa** commit. (3) `map_prepared`: ghi PTE + nối
  FIFO. Lý do: thất bại ở bất kỳ pha nào không để lại trạng thái dở (đặc biệt không đẩy page của process khác ra
  vô ích).
- **I6.** Danh sách đơn, mới ở đầu, cũ ở cuối, và phải bỏ qua page không hợp lệ (đã swap, bị ghim) ⇒ phải xem hết để
  lấy phần tử **cũ nhất hợp lệ**. Trả `struct pgn_t **` để gỡ node O(1).
- **I7.** PTE SWAPPED → `src = mswp[type]`, `srcoff` → RAM hết → `mm64_fifo_victim` → slot trống trong active swap →
  syscall `SWP_OP` copy victim ra → gỡ victim khỏi FIFO, `pte_set_swap_mm(owner)` → copy page vào frame của victim →
  `pte_set_fpn` → trả slot cũ → thêm page vào đầu FIFO (`libmem.c:284-361`).
- **I8.** Để các process chia sẻ RAM nhỏ (config 1 frame). Giá: process gây nhiễu nhau (thrashing ở `06` §6), quét
  O(k) mọi page của mọi process, phải sửa PTE của mm khác (cần `mmvm_lock` chung).
- **I9.** Field *swap type* (bit 4–0) = chỉ số thiết bị (`swap_dev_of`), *swap offset* (bit 25–5) = slot trong thiết bị.
- **I10.** 13 bit của spec chỉ địa chỉ được 32 MiB với page 4 KiB; config mẫu dùng RAM 256 MiB = 65 536 frame ⇒ cần 16
  bit. Hệ quả: RAM > 256 MiB bị `os.c` kẹp; `dma_pin_count` có 65 536 phần tử.
- **I11.** Kernel: frame **liên tục** (`MEMPHY_get_freefp_range(n)`), map vào `os.mm` (cây chung, nửa kernel),
  **không** vào FIFO ⇒ không swap. User: frame lẻ, map vào `proc->mm`, vào FIFO, có thể bị swap.
- **I12.** Duyệt chính page table: `mm64_pt_foreach` gọi `release_page` cho mọi PTE ≠ 0; PRESENT ⇒ trả FPN về RAM;
  SWAPPED ⇒ trả slot về `mswp[type]`. Không cần danh sách phụ.
- **I13.** `mmvm_lock` / `kmm_lock` → `pgd->lock` → `memphy_lock`. Mọi đường chạm bộ nhớ user (alloc, read, write,
  copy, exit) đều giữ `mmvm_lock` chung, nên khi B đang sửa PTE của A thì A không thể đồng thời đọc/ghi/thoát.
- **I14.** `pg_getpage` cần một slot cho victim; `MEMPHY_get_freefp(active_mswp)` thất bại ⇒ trả -1, không đổi gì;
  lệnh read/write báo lỗi, process chạy tiếp.

### 7.3 Advanced
- **A1.** `__alloc` lấy `mmvm_lock` ngay đầu (`libmem.c:74`) ⇒ hai lệnh `alloc` bị tuần tự hóa. CPU đầu lấy frame
  trống; CPU sau thấy RAM hết ⇒ evict page cũ nhất (có thể là page vừa map của CPU đầu nếu FIFO chỉ có nó). Bên
  trong, `pgd->lock` và `memphy_lock` được lấy/nhả theo đúng thứ tự, không bao giờ ngược ⇒ không deadlock.
- **A2.** Nếu sau đó không đủ frame và phải hủy, victim đã bị đánh dấu SWAPPED dù frame của nó vẫn đang được dùng lại
  hoặc trả về RAM ⇒ hoặc mất dữ liệu (nếu frame bị cấp cho người khác) hoặc phải swap-in vô ích. Commit cuối cùng
  tránh cả hai (`mm64.c:704-739`).
- **A3.** `mm64_fifo_victim` bỏ qua mọi frame bị ghim ⇒ không có victim ⇒ `alloc_pages_range` trả -3000 ⇒ `alloc` thất
  bại, không đổi gì. Khi DMA xong, `dma_unpin_cb` bỏ ghim và lần thử sau có thể thành công. (`test_skip_pinned`.)
- **A4.** Thêm bit accessed (ví dụ dùng vùng USRNUM 16–27 hoặc bit 29), set trong `pg_getpage` mỗi lần hit; Clock: duyệt
  FIFO như vòng tròn, page có bit = 1 thì xóa bit và cho "cơ hội thứ hai". Sửa duy nhất `mm64_fifo_victim` vì mọi
  đường evict dùng chung nó.
- **A5.** Có thể: FIFO không phải stack algorithm, nên tăng số frame có thể làm tăng số lần swap với một số chuỗi truy
  cập. Simulator không đo page-fault count nên chưa minh họa được bằng số. **[I]**
- **A6.** Kernel page phải luôn truy cập được (kernel không thể "page fault" trong lúc giữ `kmm_lock`), cần liên tục vật
  lý cho DMA, và dùng chung cho mọi process. Nếu swap được: copy_* có thể phải swap-in trong khi giữ `kmm_lock` →
  phức tạp thứ tự khóa; DMA có thể ghi vào frame đã bị lấy.
- **A7.** Bảng nhỏ `(mm, pgn) → fpn` trong `pg_getpage` trước `pte_get_entry`. Invalidate khi: `pte_set_swap_mm` (victim,
  kể cả của mm khác), `pte_clear_entry`, `free_pcb_memph`. Vì victim có thể thuộc process khác, cần flush theo `(mm, pgn)`
  — tương tự TLB shootdown.
- **A8.** Bảng không chiếm frame ⇒ "RAM đã dùng" thấp hơn thực tế, và bảng không bao giờ bị swap. Để đưa vào RAM: cấp
  frame cho mỗi node qua `MEMPHY_get_freefp`, lưu FPN con trong PTE cấp trên thay cho con trỏ host, đọc/ghi node qua
  `MEMPHY_read/write`; cần frame dành riêng không bị evict.
- **A9.** Ghim chỉ ngăn frame bị thu hồi, không ngăn `write` của process vào cùng byte trong lúc RTL đang chép ⇒ kết quả
  phụ thuộc thứ tự. Cách sửa: cấm `write` vào frame đang ghim (kiểm `dma_frame_pinned` trong `pg_setval`), hoặc cho
  process chờ (`dma_pending_for`) trước khi chạm vùng nguồn/đích.
- **A10.** Set trong `pg_setval` (sau `pg_getpage`) bằng `pte_set_entry(…| DIRTY)`; khi evict, page không dirty **và** còn bản
  sao hợp lệ trong swap thì bỏ qua bước copy ra. Muốn vậy phải giữ slot swap sau swap-in (swap cache) thay vì trả ngay
  như hiện tại (`libmem.c:348`).
- **A11.** Không: không có đường nào giữ `registry_lock` rồi xin `mmvm_lock` (cleanup ở `os.c` nhả `registry_lock`
  trước khi gọi `free_pcb_memph`). Thứ tự thực tế là `mmvm_lock → registry_lock`, nhất quán.
- **A12.** Có: victim đã nằm an toàn trong swap với PTE SWAPPED đúng; frame được trả về RAM; page đích vẫn SWAPPED với
  slot cũ giữ nguyên. Chỉ mất công một lần evict. FIFO đã mất node victim — đúng vì victim không còn resident.

---

## 8. Chiến lược trả lời khi bảo vệ

1. Luôn trả lời theo khung **WHY → WHAT → HOW → WHERE (file:line) → bằng chứng (test/log)**.
2. Khi bị hỏi về gap, **chủ động thừa nhận** và nói cách sửa (đã có trong §5, §7.3).
3. Chuẩn bị sẵn 3 log để mở khi demo: `os_0_mlq_paging` (đọc/ghi + isolation), `os_1_mlq_paging_small_4K`
   (thay trang giữa process), `study_dma` (DMA qua RTL).
4. Chạy trước `tests/run_*.sh` để có thể nói "6 bộ test, tất cả pass, có AddressSanitizer".
5. Phân biệt rõ: "spec yêu cầu X", "code làm Y", "khác nhau vì Z".
