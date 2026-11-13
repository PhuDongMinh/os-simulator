# Báo cáo Tích hợp và Phân công Module MM64 (64-bit Paging)

**Người gửi:** `libmem.c`

**Người nhận:** `mm64.c`

---

## 1. Bối cảnh
Trong quá trình hoàn thiện các API cấp phát và thu hồi bộ nhớ Kernel (`__kmalloc` và `__kfree`) tại `libmem.c`, nhận thấy cần phải tương tác sâu với hệ thống phân trang đa mức của kiến trúc 64-bit.

Hiện tại, hàm `pte_set_fpn` trong `mm64.h` đang được gán cứng logic bật cờ `PRESENT`:
`SETBIT(*pte, PAGING_PTE_PRESENT_MASK);`

Do đó, **không thể sử dụng hàm này để thu hồi trang (free page)** vì khi truyền `fpn = 0`, trang đó vẫn bị đánh dấu là đang tồn tại (Present = 1), dẫn tới rò rỉ và lỗi logic bộ nhớ.

## 2. Nhiệm vụ cần làm cho `mm64.c`

Để hệ thống ghép nối thành công, `mm64` vui lòng thực hiện 3 task sau:

### Task 2.1: Thêm prototype `pte_clear_entry` vào `mm64.h`
Thêm dòng khai báo sau vào cuối file `mm64.h`:
```c
int pte_clear_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller);
```

### Task 2.2: Hiện thực `pte_clear_entry` trong `mm64.c`
Hàm này có nhiệm vụ duyệt qua 5 mức phân trang, tìm đúng con trỏ PTE và gán nó về `0` (nhằm xóa sạch `FPN` và cờ `PRESENT`).

**Mã nguồn tham khảo:**
```c
int pte_clear_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  addr_t *pte = NULL;
  
#ifdef MM64 
  addr_t pgd, p4d, pud, pmd, pt;
  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);

  /* TODO: Bổ sung logic trích xuất con trỏ PTE 5 mức tại đây */
  // pte = truy_vet_pointer_5_muc(mm, pgd, p4d, pud, pmd, pt);
   
#else
  // Fallback 32-bit
  if (mm == caller->krnl->mm) pte = &caller->krnl->krnl_pgd[pgn];
  else pte = &mm->pgd[pgn];
#endif

  if (pte != NULL) *pte = 0;
  return 0;
}
```

### Task 2.3: Hoàn thiện thuật toán cắt địa chỉ 5 mức (Cốt lõi)
Hiện tại hàm `get_pd_from_pagenum(pgn, ...)` vẫn đang trống logic. 
Yêu cầu nhóm sử dụng các phép toán dịch bit (`>>`) và mặt nạ (`&`) để cắt `pgn` (hoặc `vaddr`) thành 5 đoạn chỉ mục:
1. `PGD` (Page Global Directory)
2. `P4D` (Page 4-level Directory)
3. `PUD` (Page Upper Directory)
4. `PMD` (Page Middle Directory)
5. `PT`  (Page Table)

*Lưu ý: Độ dài của mỗi mức (bao nhiêu bit) cần bám sát vào các Macro cấu hình hệ thống trong `common.h` hoặc `mm64.h`.*

---
**Note:** Sau khi hoàn thành Task 2.3, hãy dùng thuật toán đó để gắn vào phần `TODO` của 2 hàm `pte_set_fpn` và `pte_clear_entry`. Khi đó, ghép nối giữa `libmem.c` và `mm64.c` sẽ hoàn hảo 100%.