# README - Hệ thống giám sát bệnh nhân BLE → ESP32 → STM32 → CAN → Python Web

Tài liệu này dùng để giải thích tổng thể hệ thống, cách nạp code, cách nối dây, cách chạy Python Web Dashboard và cách demo.

---

## 1. Mục tiêu hệ thống

Hệ thống theo dõi nhiều bệnh nhân trong nhiều phòng. Mỗi bệnh nhân có node cảm biến gửi dữ liệu qua BLE. ESP32 gateway thu BLE theo từng dãy A/B rồi gửi dữ liệu qua UART về STM32. STM32 đóng gói thành CAN Extended Frame và gửi lên CAN bus. Máy tính dùng USB-CAN đọc dữ liệu, Python giải mã, xử lý cảnh báo và hiển thị web dashboard cho bác sĩ.

```text
Node cảm biến bệnh nhân
        ↓ BLE Advertising
ESP32 Gateway dãy A / dãy B
        ↓ UART text 115200
STM32F103
        ↓ CAN Extended 125 kbps
USB-CAN / CANable
        ↓ COM27
Python CAN Reader
        ↓ FastAPI Web
Web Dashboard cho bác sĩ
```

---

## 2. Thành phần chính

### 2.1 Node cảm biến BLE

Node cảm biến đo dữ liệu bệnh nhân và phát BLE Advertising.

Ví dụ node WATCH dùng MAX30100:

```text
MAX30100 → ESP32 node → BLE Advertising
```

Dữ liệu node phát:

```text
Wing ID       : A hoặc B
Room ID       : số phòng
Bed ID        : số giường
Patient ID    : mã bệnh nhân
Node type     : VITAL hoặc LOADCELL
HR            : nhịp tim
SpO2          : nồng độ oxy máu
Temp          : nhiệt độ
Battery       : pin
Seq           : số thứ tự gói
```

Ví dụ cấu hình node:

```c
#define WING_ID     'A'
#define ROOM_ID     1
#define BED_ID      2
#define PATIENT_ID  4
```

Nếu muốn node thuộc dãy B:

```c
#define WING_ID     'B'
```

---

### 2.2 ESP32 Gateway

ESP32 gateway nhận BLE từ node, lọc theo dãy A hoặc B, sau đó gửi chuỗi UART sang STM32.

Mô hình hiện tại dùng 2 ESP gateway:

```text
ESP Gateway A → nhận node dãy A → gửi UART sang STM32 UART2
ESP Gateway B → nhận node dãy B → gửi UART sang STM32 UART3
```

Cấu hình đề xuất:

```text
ESP A:
MY_WING_ID = 'A'
TX GPIO17  → STM32 PA3  USART2_RX

ESP B:
MY_WING_ID = 'B'
TX GPIO17  → STM32 PB11 USART3_RX
```

Format UART ESP phải gửi cho STM32:

```text
TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,SEQ=25\r\n
TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=25\r\n
```

Lưu ý:

```text
- Phải có \r\n hoặc ít nhất \n cuối dòng.
- Baudrate UART là 115200.
- ESP GND phải nối chung với STM32 GND.
- Không gửi format sai như TYR=..., BA=..., EQ=...
```

---

### 2.3 STM32F103 Gateway CAN

STM32 nhận UART từ ESP A/B, parse chuỗi, đóng gói thành CAN Extended ID và gửi lên CAN bus.

Kết nối UART:

```text
ESP A TX GPIO17 → STM32 PA3  USART2_RX
ESP B TX GPIO17 → STM32 PB11 USART3_RX
ESP GND         → STM32 GND
```

UART debug STM32:

```text
STM32 PA9 USART1_TX → RX USB-TTL
STM32 GND           → GND USB-TTL
Baudrate            → 115200
```

CAN:

```text
STM32 CAN_TX/CAN_RX → CAN Transceiver
CANH/CANL           → USB-CAN / CANable
Bitrate             → 125 kbps
```

STM32 nhận các dòng:

```text
TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,SEQ=25
TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=25
```

CAN Extended ID 29-bit:

```text
bit 28..26 : msg_type   1 = VITAL, 2 = LOADCELL
bit 25..24 : wing       0 = A, 1 = B
bit 23..16 : room
bit 15..8  : bed
bit 7..0   : patient
```

Ví dụ ID:

```text
VITAL A-R1-B2-P4    → ID = 0x04010204
VITAL B-R1-B2-P4    → ID = 0x05010204
LOADCELL A-R1-B2-P4 → ID = 0x08010204
LOADCELL B-R1-B2-P4 → ID = 0x09010204
```

CAN data VITAL:

```text
byte 0: HR low
byte 1: HR high
byte 2: SpO2
byte 3: Temp x10 low
byte 4: Temp x10 high
byte 5: Battery
byte 6: Seq
byte 7: Status
```

CAN data LOADCELL:

```text
byte 0: Volume ml low
byte 1: Volume ml high
byte 2: Drops/min low
byte 3: Drops/min high
byte 4: Seq
byte 5: Status
byte 6: 0
byte 7: 0
```

---

### 2.4 USB-CAN

USB-CAN đưa CAN bus vào máy tính.

Cấu hình:

```text
Interface   : CANable SLCAN
COM         : COM27
Bitrate     : 125000
Mode        : Normal
Listen only : OFF
```

Khi test bằng Cangaroo có thể thấy frame:

```text
ID   = 0x04010204
DLC  = 8
Data = 78 00 57 00 00 64 B7 00
```

---

### 2.5 Python Web Dashboard

Python có nhiệm vụ:

```text
1. Mở USB-CAN COM27.
2. Đọc CAN Extended Frame.
3. Giải mã ID bệnh nhân.
4. Giải mã data VITAL / LOADCELL.
5. Gom dữ liệu theo từng bệnh nhân.
6. Xử lý cảnh báo.
7. Tạo web dashboard realtime.
8. Vẽ biểu đồ trạng thái.
```

File Python đang dùng:

```text
patient_can_web_dashboard_charts_toggle_close_v2.py
```

Chạy:

```powershell
python patient_can_web_dashboard_charts_toggle_close_v2.py --channel COM27 --bitrate 125000 --web
```

Mở web:

```text
http://127.0.0.1:8000
```

---

## 3. Ngưỡng cảnh báo trong Python

```text
HR thấp critical      < 45 bpm
HR thấp warning       < 55 bpm
HR cao warning        > 110 bpm
HR cao critical       > 130 bpm

SpO2 warning          < 94%
SpO2 critical         < 90%

Sốt warning           >= 38.0°C
Sốt critical          >= 39.0°C

Pin warning           <= 20%
Pin critical          <= 10%

Dịch sắp hết warning  <= 80 ml
Dịch gần cạn critical <= 30 ml

Giọt/phút thấp        <= 5 DPM
Giọt/phút nhanh       >= 90 DPM
Giọt/phút quá nhanh   >= 120 DPM

Mất kết nối warning   sau 8 giây
Mất kết nối critical  sau 15 giây

Dịch ngưng chảy       DPM = 0 liên tục 20 giây
```

Xử lý tránh báo giả:

```text
- HR = 0 không báo nhịp tim thấp vì có thể cảm biến chưa đo được.
- SpO2 = 0 không báo SpO2 thấp vì có thể chưa có kết quả hợp lệ.
- Temp = 0 không báo sốt vì node hiện chưa có cảm biến nhiệt độ cơ thể thật.
- Cảnh báo cần lặp lại nhiều lần mới xác nhận, trừ mất kết nối.
```

---

## 4. Cách chạy toàn bộ hệ thống

### Bước 1: Nạp code node BLE

Nạp code WATCH hoặc LOADCELL.

Kiểm tra:

```text
WING_ID
ROOM_ID
BED_ID
PATIENT_ID
NODE_TYPE
```

Ví dụ node dãy A:

```c
#define WING_ID 'A'
```

Node dãy B:

```c
#define WING_ID 'B'
```

### Bước 2: Nạp code ESP Gateway A/B

ESP Gateway A:

```text
MY_WING_ID = 'A'
TX GPIO17  → STM32 PA3
```

ESP Gateway B:

```text
MY_WING_ID = 'B'
TX GPIO17  → STM32 PB11
```

ESP phải gửi UART đúng format:

```text
TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,SEQ=25\r\n
```

### Bước 3: Nạp code STM32

Dùng file STM32 gateway CAN:

```text
stm32_gateway_final_uart23_can_ext_125k.c
```

Mở UART1 debug 115200. Khi đúng sẽ thấy:

```text
CAN started
UART2/UART3 RX interrupt started
Ready. ESP lines must end with CRLF or LF.
```

Khi nhận đúng UART:

```text
UART2 RAW | TYPE=VITAL,W=A,R=1,B=2,P=4,HR=80,SPO2=97,T=36.80,BAT=100,SEQ=25
UART2 -> CAN VITAL OK | ID=0x04010204 ...
```

### Bước 4: Cắm USB-CAN

Cắm USB-CAN vào máy tính và kiểm tra COM, ví dụ:

```text
COM27
```

Không mở Cangaroo khi chạy Python vì COM27 chỉ nên cho một chương trình dùng.

### Bước 5: Chạy Python Web Dashboard

Trong thư mục:

```text
D:\nckh_test1\python_can
```

Chạy:

```powershell
python patient_can_web_dashboard_charts_toggle_close_v2.py --channel COM27 --bitrate 125000 --web
```

Mở:

```text
http://127.0.0.1:8000
```

Nếu muốn xem raw CAN:

```powershell
python patient_can_web_dashboard_charts_toggle_close_v2.py --channel COM27 --bitrate 125000 --web --print-raw
```

---

## 5. File bấm là chạy dashboard

Tạo file:

```text
run_dashboard.bat
```

Nội dung:

```bat
@echo off
title Patient CAN Dashboard

cd /d D:\nckh_test1\python_can

echo Dang kiem tra thu vien...
python -m pip install python-can fastapi uvicorn websockets wsproto

echo.
echo Dang mo web dashboard...
start http://127.0.0.1:8000

echo.
echo Dang chay dashboard...
python patient_can_web_dashboard_charts_toggle_close_v2.py --channel COM27 --bitrate 125000 --web

echo.
echo Neu thay dong nay la chuong trinh da dung hoac co loi.
pause
```

Sau này chỉ cần double click `run_dashboard.bat`.

Đưa ra Desktop:

```text
Right click run_dashboard.bat → Send to → Desktop
```

---

## 6. Tự chạy khi bật máy

Nhấn:

```text
Win + R
```

Gõ:

```text
shell:startup
```

Copy shortcut của `run_dashboard.bat` vào thư mục đó. Từ lần sau bật máy, dashboard sẽ tự chạy.

---

## 7. Lỗi thường gặp

### 7.1 Không thấy dữ liệu trên web

Kiểm tra:

```text
1. Terminal Python có in dữ liệu không?
2. API http://127.0.0.1:8000/api/patients có dữ liệu không?
3. Bấm Ctrl + F5 trên trình duyệt.
4. Đảm bảo Python đang chạy đúng file mới nhất.
```

### 7.2 Python báo thiếu uvicorn

Cài thư viện:

```powershell
python -m pip install python-can fastapi uvicorn websockets wsproto
```

### 7.3 Không mở được COM27

Nguyên nhân:

```text
- Cangaroo đang mở COM27.
- Python khác đang chiếm COM27.
- USB-CAN bị đổi COM.
```

Xử lý:

```text
1. Đóng Cangaroo.
2. Rút cắm lại USB-CAN.
3. Kiểm tra Device Manager xem COM mới là bao nhiêu.
4. Sửa trong lệnh Python hoặc file .bat.
```

Ví dụ đổi sang COM13:

```powershell
python patient_can_web_dashboard_charts_toggle_close_v2.py --channel COM13 --bitrate 125000 --web
```

### 7.4 STM32 không gửi CAN

Kiểm tra UART debug STM32:

```text
Không thấy UART2 RAW / UART3 RAW:
    ESP chưa gửi UART hoặc sai dây TX/RX/GND.

Thấy RAW nhưng PARSE SKIP:
    ESP gửi sai format.

Thấy CAN TX mailbox full:
    USB-CAN chưa ACK, sai bitrate, chưa mở thiết bị hoặc CAN wiring lỗi.
```

### 7.5 ESP gửi nhưng STM32 không nhận

Kiểm tra dây:

```text
ESP TX GPIO17 → STM32 PA3 hoặc PB11
ESP GND       → STM32 GND
Baudrate      → 115200
```

Nếu cần debug, bật echo byte trong STM32:

```c
#define ENABLE_UART_BYTE_ECHO 1
```

Khi nhận được UART, STM32 sẽ in:

```text
[UART2 RX] TYPE=VITAL,...
```

---

## 8. Ví dụ dữ liệu

### VITAL

UART ESP gửi:

```text
TYPE=VITAL,W=A,R=1,B=2,P=4,HR=120,SPO2=87,T=0.00,BAT=100,SEQ=183
```

STM32 gửi CAN:

```text
ID   = 0x04010204
Data = 78 00 57 00 00 64 B7 00
```

Python giải mã:

```text
A-R001-B002-P004
HR=120
SpO2=87
Temp=0.0
Battery=100
Seq=183
Alert=WARNING nhịp tim cao + CRITICAL SpO2 rất thấp
```

### LOADCELL

UART ESP gửi:

```text
TYPE=LOADCELL,W=B,R=1,B=2,P=4,ML=450,DPM=25,SEQ=101
```

STM32 gửi CAN:

```text
ID   = 0x09010204
Data = C2 01 19 00 65 00 00 00
```

Python giải mã:

```text
B-R001-B002-P004
Volume=450 ml
Drops/min=25
Seq=101
Alert=OK
```

---

## 9. Gợi ý phát triển thêm

```text
1. Lưu lịch sử vào SQLite hoặc PostgreSQL.
2. Thêm đăng nhập tài khoản bác sĩ.
3. Thêm nút xác nhận cảnh báo: Đã xử lý.
4. Thêm âm thanh khi critical.
5. Thêm phân trang theo phòng/dãy.
6. Thêm màn hình tổng quan bệnh viện.
7. Thêm biểu đồ lịch sử 1 giờ / 24 giờ.
8. Thêm xuất file Excel/PDF báo cáo theo ca trực.
9. Thêm cấu hình ngưỡng cảnh báo ngay trên web.
10. Thêm watchdog cho Python và STM32.
```

Kiến trúc nâng cấp:

```text
CAN Reader Python
        ↓
Database SQLite/PostgreSQL
        ↓
FastAPI Backend
        ↓
Web Dashboard
        ↓
Bác sĩ xác nhận cảnh báo
```

---

## 10. Tóm tắt vai trò từng phần

```text
Node BLE:
    Đo cảm biến và phát dữ liệu bệnh nhân.

ESP Gateway:
    Thu BLE theo dãy A/B và gửi UART text sang STM32.

STM32:
    Nhận UART, parse, đóng gói CAN Extended ID, gửi CAN bus.

USB-CAN:
    Chuyển dữ liệu CAN lên máy tính qua COM.

Python:
    Đọc CAN, giải mã bệnh nhân, xử lý cảnh báo, cấp dữ liệu cho web.

Web:
    Hiển thị bệnh nhân, cảnh báo, biểu đồ cho bác sĩ.
```

---

## 11. Checklist demo nhanh

```text
[ ] USB-CAN cắm vào máy và đúng COM27.
[ ] Cangaroo đã đóng.
[ ] STM32 đã nạp code gateway CAN.
[ ] ESP A nối PA3, ESP B nối PB11.
[ ] GND ESP và STM32 nối chung.
[ ] Node BLE đang phát dữ liệu.
[ ] run_dashboard.bat chạy được.
[ ] Web mở http://127.0.0.1:8000.
[ ] Dashboard có card bệnh nhân A/B.
[ ] Có cảnh báo khi HR/SpO2 vượt ngưỡng.
[ ] Biểu đồ bật/tắt được.
```
