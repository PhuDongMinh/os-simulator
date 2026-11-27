# Stage B0 — Header & Type Consistency

Phạm vi: cấu hình/include order, typed declarations, public prototypes và
C/C++ compatibility. Chưa triển khai root creation, walker, PTE operations,
mapping, swapping hoặc cleanup. Không thay đổi thân hàm production `.c/.cpp`.

## Files và thay đổi

| File | Thay đổi |
|---|---|
| `include/os-mm.h` | Include `os-cfg.h` trước conditional types; thêm `mm_pte_t = uint32_t`, forward declarations năm mức; `mm->pgd` thành `struct pt_pgd *`. |
| `include/common.h` | Đọc config trước system headers; `krnl_pgd` cùng typed root, borrowed alias theo ownership contract. |
| `include/mm.h` | Hoàn thiện include guard; bổ sung declarations hiện thiếu của `pte_clear_entry`, `__kfree`, `free_pcb_memph`. Không đổi signatures đã có. |
| `include/mm64.h` | Thêm constants 9-bit/512 entries; sửa MM64 offset mask dùng đúng bits 11–0, không lấy constants legacy 256 B. |
| `include/libmem.h` | Include guard, C linkage cho public APIs; thêm declaration của `libkmem_free` đã có implementation. |
| `include/syscall.h` | Include guard và C linkage cho public functions/variables. |
| `src/mm64-internal.h` | Định nghĩa private typed nodes, không có implementation operations. |
| `Makefile` | C dùng `-iquote include` giống C++; tránh project `sched.h` che system `<sched.h>`. |
| `tests/check_mm64_types.py` | Focused compile, layout, linkage và negative type checks. |
| `Document/STAGE_B0.md` | Báo cáo và integration checklist này. |

## Configuration và type contract

`os-cfg.h` vẫn là nguồn cấu hình và vẫn bật `MM64`. Đọc trực tiếp `os-mm.h`
không còn phụ thuộc việc caller có include `common.h` trước hay không.

```text
os-mm.h -> os-cfg.h -> quyết định MM64 -> typedef addr_t
common.h -> os-cfg.h -> system headers -> os-mm.h -> typedef arg_t
```

Trong MM64, `addr_t` và `arg_t` đều là `uint64_t`. `mm_pte_t` luôn là
`uint32_t`; các public PTE APIs vẫn giữ return/argument containers hiện có
(`addr_t`, `uint64_t`, `addr_t *`). Không đổi `init_pte()` thành pointer 32-bit
khi callers còn dùng signature cũ. Việc validate/narrow PTE value về leaf
32-bit thuộc implementation tiếp theo, không được truncate âm thầm.

Giữ nguyên toàn bộ encoding masks trong `mm.h`, gồm FPN bits 12–0,
PRESENT/SWAPPED/DIRTY/RESERVE, USRNUM và SWAP payload. Không đổi RAM support
hoặc PTE format; RAM <=32 MiB là integration constraint đã chốt. B0 chưa
thêm runtime validation cho file config và chưa đổi legacy `PAGE_SIZE`.

Build C/C++ dùng `-iquote include` cho project headers có dấu ngoặc kép.
`-Iinclude` có thể khiến `<pthread.h>` đọc nhầm `include/sched.h`; với g++
điều này làm thiếu `cpu_set_t`. Không sửa scheduler functionality hoặc dùng
platform-specific `include_next` để che vấn đề filename collision này.

## Typed tree và ownership

Public headers chỉ forward-declare:

```text
pt_pgd -> pt_p4d -> pt_pud -> pt_pmd -> pt_leaf
```

Definitions ở `src/mm64-internal.h`: mỗi non-leaf có 512 typed child pointers;
leaf có 512 `mm_pte_t` entries. Root chứa mutex và 512 P4D child pointers.
Header này dành cho implementation MM64 và focused tests; các module khác
không cần biết layout node. Chỉ có declarations, chưa cấp phát hoặc init mutex.
`calloc()` một root không thay thế `pthread_mutex_init()` trong bước sau.

- `mm->pgd`: owning authoritative root.
- Mỗi process sẽ có user root riêng.
- `os.mm->pgd`: owning shared kernel root.
- `os.krnl_pgd`: borrowed alias cùng kiểu; bước init sau phải gán
  `os.krnl_pgd = os.mm->pgd`, không cấp/free root thứ hai.
- Các field `mm->p4d/pud/pmd/pt` và `krnl_p4d/pud/pmd/pt` được giữ kiểu cũ
  để tương thích tên field; initialization sau phải để NULL, không dùng làm
  roots hoặc current-table caches.
- Host child pointers tách biệt hoàn toàn với 32-bit PTE values. Không có
  pointer-to-integer cast nào được thêm.

Trên host hiện tại, layout C/C++ thống nhất:

| Type | Size (bytes) |
|---|---:|
| `addr_t`, `arg_t` | 8 |
| `mm_pte_t` | 4 |
| `vm_rg_struct` | 32 |
| `vm_area_struct` | 56 |
| `framephy_struct` | 24 |
| `memphy_struct` | 40 |
| `mm_struct` | 1024 |
| `pcb_t` | 280 |
| `krnl_t` | 1144 |
| `pt_leaf` | 2048 |
| `pt_pmd`, `pt_pud`, `pt_p4d` | 4096 mỗi loại |
| `pt_pgd` | 4136, gồm mutex 40 bytes trên host này |

Đây là host ABI quan sát được, không phải các size cần hardcode cho mọi
platform. Mọi translation unit phải rebuild; không tái dùng object có
`addr_t` 32-bit do include-order cũ.

## Verification

Chạy focused checks từ repo root:

```sh
python3 tests/check_mm64_types.py
```

Kết quả: PASS toàn bộ:

- 18 tổ hợp: 9 include orders x C gnu11/C++ gnu++14, với production include
  flags, `-Wall -Wextra -Werror`. Có repeated includes, pthread-first và
  headers nằm trong `extern "C"` như standalone DMA.
- Type checks cho `addr_t`, `arg_t`, root aliases, từng child level, leaf
  width, 512 entries, offset 12-bit, FPN 13-bit và PTE masks.
- So sánh 32 size/offset values giữa các orders/languages và executable
  linked từ C provider + C++ consumer.
- Function-pointer signature checks cho PTE, kernel allocation/free,
  kernel/user I/O, copy và syscall APIs. `nm -u` xác nhận cùng C symbols
  từ C/C++, không có C++ name mangling cho những APIs này.
- Negative compilation checks: reject flat `addr_t *` làm root, pointer
  làm PTE và child pointer sai level.

Compilation source thực:

- Syntax-check toàn bộ C/C++ sources, cả `DMA_MAIN=0` và `DMA_MAIN=1`:
  **37/39 configurations PASS**. Tất cả 21 C++ configurations PASS;
  C có 16/18 PASS, hai blockers ở dưới.
- Compile object thật: `mm64.o`, `mm-memphy.o`, `sys_mem.o`, `libstd.o`
  và C++ standalone `dma_c_testbench.o` PASS.
- Preprocessing `sys_mem.c` và các C translation units dùng memory headers
  xác nhận `ADDR_TYPE uint64_t`; các layout tests kiểm tra actual typedef.
- `git diff --check` PASS.

Syscall table phục vụ compilation được sinh vào `/tmp` bằng `sh`, không
sửa source. Lệnh `make os` mặc định trong sandbox hiện dừng ở `chmod` của
generator do filesystem không cho đổi permissions. Kiểm tra lại với table
đã sinh vào `/tmp`, bỏ qua riêng generator target và đặt objects vào `/tmp`
đã chạy tới compiler và dừng tại typed-root incompatibility ở `os.c:186`.
Không tuyên bố full build/link thành công.

## Remaining integration blockers

Trước B0, compilation đã lỗi thiếu prototypes của `pte_clear_entry`,
`__kfree`, `free_pcb_memph`. Những lỗi declaration đó đã được xử lý. Hai
blockers sau xuất hiện khi compiler áp dụng typed-root contract mới lên
consumer code cũ; không coi chúng là lỗi vô can với thay đổi B0:

1. **Module 4 / `src/os.c:186`:** còn cấp `krnl_pgd` bằng cast từ
   `addr_t *`. Cần thay initialization bằng một root do `init_mm()` sở hữu,
   rồi gán borrowed alias. Chưa sửa vì B0 không triển khai root creation.
2. **Module 1 lifecycle / `src/libmem.c:959`:** `free_pcb_memph()` còn
   đọc `mm->pgd[pagenum]` như bảng phẳng. Opaque root cố ý không cho phép
   cách truy cập đó. Cần leaf traversal/cleanup ở bước tương ứng; không
   thay expression bằng unsafe cast hoặc viết cleanup trong B0.

Public Module 3 function signatures vẫn tương thích. Tuy nhiên whole
`libmem.c` chưa compile vì chứa cleanup consumer trên; các signature/linkage
tests không được diễn giải thành Module 3 runtime/integration đã pass.

Những dependency còn giữ nguyên: MM64 page arithmetic trong Module 2/3,
PTE dummy implementations, RAM configuration validation <=32 MiB, kernel
region allocation/ownership, SWAP device pointer representation và process
lifetime. B0 chưa cấp tree, thiết lập root alias thực tế hay thực thi các
ownership/locking invariants ở runtime.
