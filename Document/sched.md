## src/sched.c và include/sched.h
Chi tiết mô tả trang 12/33
### Scheduler có nhiệm vụ
-Chọn process trong ready_queue = /get_proc()/
-Đặt process load lần đầu vào ready_queue = /add_proc()/
-Đặt process hết time slice vào ready_queue =/put_proc()/

### Scheduler liên kết với include/queue.h
- get_proc() - dequeue() + enqueue()
- add_proc() - enqueue()
- put_proc() - purgequeue() + enqueue()

###  Scheduler có 2 loại
-MLQ (Multilevel queue): Ready_queue có 140 hàng queue, mỗi hàng queue có một priority (độ ưu tiên) + slot (số process được chọn tối đa mỗi lần) khác nhau
-Single: Ready_queue có 1 queue


### Scheduler thuật toán
- Chọn process ở queue có priority nhỏ (càng nhỏ càng ưu tiên)
- Chọn đến khi queue rỗng hoặc slot = 0
- Reset nếu tất cả slot = 0
### Cơ chế Lock và unlock ở 3 hàm chính
-Lock khi lấy process, unlock khi lấy xong process: đảm bảo 1 CPU tham gia lấy 1 lần



