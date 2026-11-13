# Phân bổ Không gian Bộ nhớ ảo 64-bit (User/Kernel)

## 1. Bối cảnh
Hệ thống phải quản lý không gian bộ nhớ ảo độc lập cho từng tiến trình (User Space) và một không gian dùng chung cho Kernel (Kernel Space). Để ngăn chặn tiến trình người dùng truy cập trái phép, hai không gian này phải được phân tách rạch ròi bằng cơ chế địa chỉ và bảng phân trang.

## 2. Thống nhất Memory Layout (Canonical 64-bit Address)
Áp dụng mô hình phân tách bộ nhớ Canonical Address (tương tự Linux/macOS):

*   **User Space (Không gian Người dùng):** 
    *   **Dải địa chỉ:** Bắt đầu từ `0x0000_0000_0000_0000` đến `0x0000_7FFF_FFFF_FFFF`.
    *   **Đặc điểm:** Bit thứ 47 luôn là `0`. Được quản lý bởi cấu trúc `proc->mm` của từng tiến trình riêng biệt. Chứa Code, Data, Heap, Stack của tiến trình.
*   **Kernel Space (Không gian HĐH):**
    *   **Dải địa chỉ:** Bắt đầu từ `0xFFFF_8000_0000_0000` đến `0xFFFF_FFFF_FFFF_FFFF`.
    *   **Đặc điểm:** Bit thứ 47 luôn là `1`. Là không gian **dùng chung** cho tất cả các tiến trình và HĐH. Được quản lý tập trung bởi `os.mm` và `os.krnl_pgd`. 

## 3. Quản lý cấu trúc dữ liệu Kernel (os.mm và krnl_pgd)
Để không gian Kernel có thể được quản lý tập trung và an toàn, HĐH sẽ sở hữu các cấu trúc riêng:

*   **`os.mm` (Kernel Memory Manager):**
    *   Đóng vai trò quản lý các vùng nhớ logic (Virtual Memory Area - VMA) của riêng Kernel (không liên quan đến người dùng).
    *   Chứa danh sách VMA (`mmap`) lưu các vùng nhớ đã được `kmalloc` cấp phát.
    *   Chứa `symrgtbl` nếu HĐH cần cấp phát các biến nội bộ (region ID) cho các tác vụ của chính nó.
*   **`os.krnl_pgd` (Kernel Page Global Directory):**
    *   Là gốc của bảng phân trang 5 mức (PGD -> P4D -> PUD -> PMD -> PT) dành riêng cho Kernel.
    *   Được cấp phát một lần duy nhất trong `ld_routine` của `os.c`.
    *   Khi tiến trình thực hiện System Call (ví dụ: `__kmalloc`), HĐH sẽ sử dụng `krnl->krnl_pgd` để cấp phát trang vật lý và ánh xạ vào dải địa chỉ của Kernel.

## 4. Giao tiếp User - Kernel
Vì User Space và Kernel Space dùng hai cây phân trang khác nhau (`proc->mm->pgd` và `krnl->krnl_pgd`), chúng không thể đọc ghi trực tiếp con trỏ của nhau.

*   **Tính dùng chung:** Khi System Call chạy, nó nhận biến `krnl_t *krnl`. Biến này chứa con trỏ tới HĐH gốc. Mọi tiến trình khi gọi syscall đều trỏ về cùng một `krnl`, do đó chúng dùng chung `krnl_pgd`.
*   **API `copy_from_user` và `copy_to_user` (trong `libmem.c`):**
    *   Đóng vai trò là cầu nối. HĐH sẽ "dịch" địa chỉ ảo của User (qua `caller->mm->pgd`) ra địa chỉ vật lý (Frame), đọc dữ liệu, sau đó "dịch" địa chỉ ảo của Kernel (qua `krnl->krnl_pgd`) ra Frame vật lý tương ứng và ghi dữ liệu sang.
    *   Tiến trình User không bao giờ được phép trực tiếp nhìn thấy hay truyền con trỏ vùng nhớ `0xFFFF...` vào lệnh đọc/ghi thông thường của nó.