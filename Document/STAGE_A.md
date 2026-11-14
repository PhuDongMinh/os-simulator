# Stage A — Physical Memory Management

Phạm vi: `src/mm-memphy.c`, wrapper `MEMPHY_get_freefp_contig()` trong
`src/libmem.c`, declarations/comments trong hai header và isolated tests.
Không triển khai page tables, mapping, swapping hoặc process cleanup.
Thay đổi có sẵn trong `__alloc()` được giữ nguyên.

## API và initialization

- MM64 physical frame size là `PAGING64_PAGESZ = 4096` bytes. Không sửa
  `PAGE_SIZE`, `PAGING_PAGESZ`, PTE layout hoặc kernel allocation API.
- Các hàm trả `0` khi thành công, `-1` khi thất bại. Output parameter được
  giữ nguyên khi thất bại; failed I/O không đổi storage/cursor.
- `init_memphy()` chỉ gọi trên một struct mới, chưa sở hữu tài nguyên
  (hoặc đã được bên sở hữu dọn hoàn toàn). Không gọi lại trên live device.
  Struct có thể chưa được zero trước lần init đầu. Init failure để lại
  storage/list NULL, capacity/cursor 0 và mode đã normalize.
- `max_size` phải không vượt `INT_MAX` vì field `maxsz` vẫn là `int`.
  Device size 0 là disabled device hợp lệ. Size dưới một frame có byte
  storage nhưng không có frame để cấp. Phần dư cuối device dùng được
  cho byte I/O nhưng không được cấp thành frame.
- `MEMPHY_format()` nhận device có fields đã được khởi tạo, với frame size
  đúng active mode (4096 trong MM64). Reformat bị từ chối khi còn used
  frame. Tạo danh sách mới hoàn chỉnh trước khi thay danh sách cũ;
  host allocation failure không thay đổi state cũ.
- Initialization storage và từng node đều kiểm tra allocation failure;
  mọi allocation tạm được trả lại khi thất bại.

## Ownership và invariant

MEMPHY sở hữu metadata node suốt vòng đời device. Caller chỉ nhận FPN,
không sở hữu hoặc tự free node trong hai danh sách của device.

```text
init/format: mỗi complete frame -> free_fp_list
get single/range: free_fp_list -> used_fp_list
put: used_fp_list -> free_fp_list
```

- Mỗi FPN trong `[0, floor(maxsz / frame_size))` xuất hiện đúng một lần
  trong tổng hai danh sách, kể cả FPN 0.
- Free list tăng dần nghiêm ngặt; tail NULL; không duplicate hoặc cycle.
- Used list biểu diễn frame đã được checkout; không yêu cầu sắp xếp.
- `owner` vẫn NULL, dành cho integration sau. API hiện không nhận identity
  của process nên không xác thực process nào đang release. Nó kiểm tra
  frame đó có đang checkout trên device này hay không; không cung cấp
  generation token để phát hiện stale release sau một lần tái cấp phát.
- Release FPN ngoài phạm vi, chưa checkout hoặc đã trả lại bị từ chối.
- Allocation/release di chuyển node, không malloc/free node. Do đó runtime
  host allocation failure không ngăn trả frame về device.
- `MEMPHY_get_freefp_range()` là primitive dùng chung cho single allocation
  và contiguous wrapper. Nó chỉ cắt một run khi đã tìm đủ N frames liên
  tục; thất bại giữ nguyên danh sách và output.
- Wrapper trong `libmem.c` giữ nguyên signature và `int` output. FPN luôn
  fit `int` vì capacity bị giới hạn bởi `INT_MAX` bytes.

## Lock policy và integration points

Một static `memphy_lock` trong `mm-memphy.c` bảo vệ metadata, storage và
cursor của tất cả device. Đây là leaf lock: không callback lên VM/kernel,
không lấy `mmvm_lock`/`kmm_lock`. Public APIs chỉ lock một lần; private
`*_locked` helpers không tự lấy lock.

Caller hiện có có thể giữ `mmvm_lock` hoặc `kmm_lock` rồi gọi MEMPHY.
Contiguous wrapper gọi shared primitive thay vì tự sửa list. Không có
đường MEMPHY -> outer lock, nên không tạo lock-order inversion này.
Startup init phải hoàn tất trước khi publish device cho worker threads.

Những caller liên quan:

- `__kmalloc()` -> contiguous wrapper (đang giữ `kmm_lock`).
- `__kfree()` -> put (đang giữ `kmm_lock`).
- `free_pcb_memph()` -> put (đang giữ `mmvm_lock`).
- `pg_getpage()` -> get; user helpers thường giữ `mmvm_lock`.
- Syscall I/O, kernel I/O và page-copy helper -> physical read/write.

Các module sau phải tiếp tục dùng APIs, không sửa trực tiếp free/used list,
cursor hoặc storage khi có concurrent access. Byte I/O atomic theo từng
call, không có cam kết atomic cho cả page copy, allocation+mapping hay
DMA transaction. Những transaction đó cần policy riêng ở stage tương ứng.
Một global lock ưu tiên tính đúng và giữ ABI; nó serialize cả các device
khác nhau và có thể trở thành bottleneck nếu workload lớn.

## Physical I/O và dump

- Byte address phải thuộc `[0, maxsz)` và storage phải tồn tại.
- NULL device/output bị từ chối khi không hợp lệ.
- Random I/O không dịch cursor. Sequential I/O seek đến byte đang truy cập;
  cursor lưu địa chỉ byte vừa được truy cập, không phải byte kế tiếp.
- Direct sequential helpers từ chối random device. Generic read/write
  hỗ trợ cả hai mode. `MEMPHY_mv_csr()` chỉ seek tới byte hợp lệ và không
  wrap. Simulator biểu diễn seek trực tiếp, không tốn O(address) host steps.
- Dump không đổi state; in capacity, frame size, total/available frames,
  mode/cursor, tối đa 64 free FPN và 32 byte đầu. Available count được tính
  từ toàn bộ free list; kích thước output được giới hạn.

## Tests

Chạy từ repo root:

```sh
sh tests/run_memphy_tests.sh
SANITIZERS=thread sh tests/run_memphy_tests.sh
SANITIZERS=none sh tests/run_memphy_tests.sh
```

Default dùng ASan + UBSan. Trong môi trường ptrace/sandbox mà LeakSanitizer
không chạy được, có thể chạy:

```sh
ASAN_OPTIONS=detect_leaks=0 sh tests/run_memphy_tests.sh
```

Harness biên dịch source thực, gồm wrapper trong `libmem.c`. `objcopy`
localize các definition không liên quan trước link `--gc-sections`, tránh
`__read/__write` của simulator bị dùng làm libc/sanitizer symbol. Các
dependency Stage B–E không được link vào test. Chỉ compile `libmem.c` với
missing-declaration diagnostic đã biết được hạ mức; physical implementation
và test được compile `-Wall -Wextra -Werror`. Warning format có sẵn của
`__kmalloc()` vẫn có thể xuất hiện. Harness dùng `-iquote` để tránh collision
giữa project `sched.h` và system `<sched.h>`.

10 test groups bao phủ 14 nhóm yêu cầu trong `stageA.md`:

1. Disabled device và sub-page capacity.
2. Singleton, FPN 0, reuse, duplicate/invalid release.
3. Multi-frame exhaustion; fragmented/contiguous runs; arbitrary release order.
4. First/last byte, OOB, output preservation, random/sequential mode/cursor.
5. NULL arguments, negative/oversized counts, invalid format và size limits.
6. Inject malloc failure tại storage/từng node; rollback reformat; runtime
   get/put vẫn hoạt động khi host malloc bị buộc fail.
7. 10.000 operations so với bitmap model độc lập, kiểm tra partition,
   uniqueness và free-list ordering sau mỗi operation.
8. 8 pthreads: cấp đến exhaustion và 8.000 lượt thử mixed single/contiguous
   allocation + I/O + release; không cấp trùng frame đang được giữ.
9. Device isolation.
10. Dump content, bounded output và state preservation.

Test-only teardown giải phóng cả hai lists và storage sau khi threads join;
không thêm production process/device destructor của Stage E. Linker malloc/
free wrapping còn kiểm tra số allocation sống trở về 0 sau mỗi test group.

Kết quả trong môi trường hiện tại:

- ASan + UBSan, `detect_leaks=0`: 10/10 groups PASS.
- ThreadSanitizer: 10/10 groups PASS, không báo data race.
- `make obj/mm-memphy.o`: PASS; còn warning include `MLQ_SCHED` có sẵn.
- LeakSanitizer mặc định không hoàn tất: runtime báo không hoạt động dưới
  ptrace; không tuyên bố LSan pass.
- `make os` chưa build thành công: lỗi có sẵn thiếu declaration `__kfree`
  và `free_pcb_memph` ở `src/os.c`. Chưa sửa ngoài phạm vi Stage A.

## Các dependency còn lại

- Physical API dùng chung cho RAM và SWAP, không có field phân biệt vai
  trò. Vì vậy không áp giới hạn 13-bit RAM FPN lên mọi device (sẽ giới hạn
  sai SWAP). Integration phải validate RAM <= 8192 frames / 32 MiB hoặc
  thống nhất thay đổi PTE ở stage sau. Config RAM 256 MiB hiện không phù
  hợp PTE. Stage A không thay đổi PTE hoặc âm thầm truncate FPN.
- Các phép tính VM/kernel vẫn dùng 256 B/1 KiB ở nhiều nơi; Stage A chỉ
  thống nhất physical frame size 4 KiB, chưa làm end-to-end memory hoạt động.
- Kernel VMA/page alignment, allocation rollback và region-ID ownership
  vẫn là các lỗi integration đã ghi trong audit.
- Frame used nodes giữ đến device teardown. Stage E phải dọn cả hai lists
  và storage sau khi mọi caller đã dừng; không chỉ free storage.
- Process cleanup hiện vẫn sai với MM64. Duplicate/unowned put nay trả lỗi,
  nhưng không sửa được việc caller diễn giải sai PTE hoặc trả nhầm frame
  đang checkout. Không coi strict physical validation là thay thế PTE ownership.
