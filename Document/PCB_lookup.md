# Cơ chế tra cứu PCB an toàn (Process Registry)

## 1\. Bối cảnh

Trong kiến trúc dual-space, tiến trình ở User Space không thể truyền trực tiếp con trỏ cấu trúc `pcb_t` qua system call cho Kernel Space, mà chỉ có thể truyền các tham số cơ bản như `PID`. Do đó, hàm xử lý system call trong Kernel (`sys_mem.c`) cần một cơ chế an toàn để tìm lại `pcb_t` dựa trên `PID`.

Tránh race condition và dangling pointer giữa lúc tiến trình đang gọi system call và lúc Scheduler dọn dẹp tiến trình, cần thống nhất một cơ chế quản lý tập trung.

## 2\. Cấu trúc Process Registry tập trung

* **Sở hữu:** Kernel (struct `krnl_t` trong `common.h`) sẽ là chủ sở hữu duy nhất của một bảng ghi danh tiến trình (Process Registry). Các hàng đợi của Scheduler (`ready_queue`, `running_list`) chỉ "mượn" con trỏ, không sở hữu vùng nhớ.
* **Cập nhật `common.h`:** Thêm một mảng tĩnh và một con trỏ khóa (sử dụng `void *` để tránh lỗi vòng lặp include thư viện hệ thống `<pthread.h>`):

    ```c
    #define MAX_PROCESS_REGISTRY 128

    struct krnl_t {
        // ... các trường hiện có ...
        struct pcb_t *proc_registry[MAX_PROCESS_REGISTRY];
        void *registry_lock; 
    };
    ```

## 3\. API Tra cứu

Cung cấp một API chuẩn để sử dụng trong `sys_mem.c` (hoặc bất kỳ system call nào khác sau đây). Khi sử dụng khóa, ta thực hiện ép kiểu con trỏ `registry_lock` về `(pthread_mutex_t *)`:

```c
struct pcb_t *get_pcb_by_pid(struct krnl_t *krnl, uint32_t pid) {
    if (krnl == NULL) return NULL;
    
    struct pcb_t *target = NULL;
       
    pthread_mutex_lock((pthread_mutex_t *)krnl->registry_lock);
    for (int i = 0; i < MAX_PROCESS_REGISTRY; i++) {
        if (krnl->proc_registry[i] != NULL && krnl->proc_registry[i]->pid == pid) {
            target = krnl->proc_registry[i];
            break;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)krnl->registry_lock);
       
    return target;
}
```

## 4\. Thời điểm cấp phát và dọn dẹp

Để đảm bảo API tra cứu không bao giờ trả về một con trỏ trỏ tới vùng nhớ đã bị giải phóng, quy trình vòng đời của `pcb_t` phải tuân thủ nghiêm ngặt các mốc sau:

1. **Lúc khởi tạo (Initialization):** Cấp phát và khởi tạo khóa trong hàm main() của os.c:
    ```c
    os.registry_lock = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init((pthread_mutex_t *)os.registry_lock, NULL);
    ```

2. **Lúc thêm mới (Registration):** Ngay sau khi Loader (`ld_routine` trong `os.c`) gọi hàm `load()` để tạo PCB thành công, nó phải tìm một ô trống trong `proc_registry` để gán con trỏ này vào (được bảo vệ bằng khóa).
3. **Lúc dọn dẹp (Deregistration - Quan trọng nhất):** Trong `cpu_routine` (`os.c`), khi một tiến trình chạy xong (`proc->pc == proc->code->size`), hệ thống sẽ tiến hành gọi `free()`. **TRƯỚC KHI** gọi `free(proc)`, bắt buộc phải xóa tham chiếu của tiến trình đó trong `proc_registry` (gán lại bằng `NULL` bên trong block `registry_lock`).

## 5\. Hành động

* Cập nhật `common.h` và `os.c` để khai báo và khởi tạo Registry.
* Thêm logic cập nhật Registry vào hàm `ld_routine` và `cpu_routine`.
* Áp dụng `get_pcb_by_pid()` thay cho dummy `pcb_t` hiện tại trong hàm `__sys_memmap` của file `sys_mem.c`
