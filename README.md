# ESP32-S3 + OV7670 Webcam (ESP-IDF)

Đọc camera **OV7670** (không có FIFO) bằng **ESP32-S3** và phát hình lên trình duyệt qua WiFi: xem trực tiếp (MJPEG) hoặc chụp ảnh JPEG.

- Driver camera: [`espressif/esp32-camera`](https://components.espressif.com/components/espressif/esp32-camera) (tự tải khi build)
- Webserver: `esp_http_server`, hai server riêng (trang web/ảnh tĩnh ở cổng 80, stream ở cổng 81 để stream không chặn trang chính)
- Cấu hình WiFi, độ phân giải, XCLK qua `idf.py menuconfig`
- Không có WiFi hoặc nhập sai SSID thì tự chuyển sang SoftAP

> Đã thử trên ESP-IDF v5.2.6, ESP32-S3 module N16R8 (PSRAM octal 8MB).

## Phần cứng

### Sơ đồ nối chân

| OV7670 | ESP32-S3 | Ghi chú |
|--------|----------|---------|
| 3.3V   | 3V3      | |
| GND    | GND      | |
| SDA    | GPIO16   | SCCB data |
| SCL    | GPIO17   | SCCB clock |
| XCLK   | GPIO12   | ESP32-S3 cấp clock cho camera |
| PCLK   | GPIO13   | |
| HREF   | GPIO15   | |
| VSYNC  | GPIO14   | |
| D0     | GPIO4    | |
| D1     | GPIO5    | |
| D2     | GPIO6    | |
| D3     | GPIO7    | |
| D4     | GPIO8    | |
| D5     | GPIO9    | |
| D6     | GPIO10   | |
| D7     | GPIO11   | |
| RST    | 3.3V     | nối cứng, không dùng GPIO |
| PWDN   | GND      | nối cứng, không dùng GPIO |

Đổi chân thì sửa các `#define CAM_PIN_*` ở đầu `main/main.c`.

### Lưu ý khi đấu dây

- Dùng dây **ngắn**, nối thêm GND gần nhóm dây XCLK/PCLK.
- SDA/SCL cần pull-up (thường 4.7k). Nhiều module đã có sẵn, nếu không thì thêm bên ngoài.
- OV7670 dùng nguồn **3.3V**.

## Yêu cầu

- ESP-IDF **5.0 trở lên**
- Kết nối internet ở lần build đầu (để tải component `esp32-camera`)
- Board ESP32-S3, nên có PSRAM

## Build và flash

```bash
idf.py set-target esp32s3
idf.py menuconfig          # mục "OV7670 Webcam" -> nhập WiFi SSID / password
idf.py build flash monitor
```

Sau khi kết nối WiFi, IP sẽ được in ra trong log:

```
I (4934) cam_stream: ==> KET NOI THANH CONG! Mo trinh duyet: http://192.168.110.25/
```

Nếu không đặt SSID (hoặc kết nối thất bại), board phát WiFi **`OV7670-CAM`** (mật khẩu `12345678`), mở `http://192.168.4.1/`.

> Nếu đã build trước đó rồi sửa `sdkconfig.defaults`, cần `rm sdkconfig` (Windows: `del sdkconfig`) rồi `idf.py fullclean` để các giá trị mặc định mới có hiệu lực.

## Sử dụng

| URL | Chức năng |
|-----|-----------|
| `http://<ip>/` | Trang web: Stream / Stop / Snapshot |
| `http://<ip>/capture` | Một ảnh JPEG |
| `http://<ip>:81/stream` | MJPEG stream (dùng được với `<img>`, VLC, OpenCV...) |

Xem stream bằng OpenCV:

```bash
python read_camera.py 192.168.110.25
```

## Cấu hình (`idf.py menuconfig` -> OV7670 Webcam)

| Tùy chọn | Mặc định | Ý nghĩa |
|----------|----------|---------|
| WiFi SSID / password | trống | Để trống SSID thì chạy SoftAP |
| Số lần thử kết nối WiFi | 5 | Quá số lần này thì chuyển sang SoftAP |
| SoftAP SSID / password | `OV7670-CAM` / `12345678` | |
| XCLK (MHz) | 10 | 5 đến 20. Giảm xuống nếu hình bị nhiễu |
| OV7670 CLKRC prescaler | 1 | FPS cảm biến = max / (giá trị + 1). 1 = ~10 FPS, 0 = ~13 FPS nhưng dễ sọc nhiễu nếu dây dài |
| JPEG quality | 12 | 5 đến 63, số nhỏ thì chất lượng cao |
| Frame size | QVGA 320x240 | QQVGA 160x120, QVGA, VGA 640x480 (cần PSRAM) |
| Pixel format | RGB565 | Có thể đổi sang YUV422 |

Cấu hình mặc định trong `sdkconfig.defaults`: bật PSRAM octal 80MHz (vẫn boot được nếu board không có PSRAM), flash 4MB với partition single-app lớn, `CONFIG_OV7670_SUPPORT=y`. Để tăng tốc: build `-O2`, camera DMA thẳng vào PSRAM, task camera chạy trên core 1, bộ đệm TCP lớn hơn.

## Cách hoạt động

```
OV7670 --DVP--> ESP32-S3 (LCD_CAM + GDMA) --> frame RGB565 (PSRAM)
                                                  |
                                       frame2jpg (nén phần mềm)
                                                  |
                              esp_http_server --> /capture, :81/stream --> trình duyệt
```

OV7670 không có bộ nén JPEG phần cứng, nên mỗi frame được nén JPEG bằng phần mềm trước khi gửi. Vì vậy tốc độ khung hình bị giới hạn bởi CPU và độ phân giải. Mỗi giây log in tốc độ stream và thời gian từng bước:

```
I (13331) cam_stream: FPS 10.0 | cap 25ms enc 70ms send 4ms | 20KB/frame
```

`cap` = chờ frame từ camera, `enc` = nén JPEG, `send` = gửi qua WiFi.

### Sửa lỗi driver OV7670 trong `main.c`

Driver `esp32-camera` có vài lỗi với OV7670, `camera_init()` ghi đè lại sau khi khởi tạo:
- **FPS ~1:** driver đọc thanh ghi `CLKRC` qua SCCB bị sai (trả về `0xa1`) rồi ghi lại, làm clock bị chia 34. `main.c` ghi lại `CLKRC` theo menuconfig.
- **Ảnh ám tím, cháy sáng:** driver chỉ ghi bộ thanh ghi tối giản. `main.c` ghi thêm bộ thanh ghi chất lượng ảnh (gamma, AWB, ma trận màu, AEC) lấy từ driver Linux `ov7670.c`.
- **Vạch xanh ở mép trái:** cửa sổ ảnh của driver lệch. `main.c` ghi lại HSTART/HSTOP/VSTART/VSTOP.

## Xử lý sự cố

**Log báo `cam_hal: FB-SIZE: ... != 153600` hoặc hình có dải nhiễu ngang**
Frame bị mất dữ liệu trên đường DVP. Thử lần lượt:
1. Giảm XCLK xuống 8, 6 hoặc 5 MHz trong menuconfig (PCLK bám theo XCLK).
2. Tăng `OV7670 CLKRC prescaler` (0 -> 1 -> 2) để giảm PCLK.
3. Dây ngắn hơn, thêm GND, kiểm tra kỹ PCLK/HREF/VSYNC và D0 đến D7.
4. Đổi frame size sang QQVGA. Nếu hết lỗi thì nghi ngờ băng thông (PSRAM/WiFi) hơn là dây.

> Dòng `FB-SIZE: 0 != ...` xuất hiện **một lần** lúc khởi tạo là bình thường.

**Crash `InstrFetchProhibited`, `PC: 0x00000000` sau khi nhận diện sensor**
Không gọi `s->set_reg(...)` / `s->get_reg(...)` với driver OV7670 của `esp32-camera`, hàm này không được cài đặt (con trỏ NULL). Muốn giảm PCLK thì tăng `OV7670 CLKRC prescaler` hoặc giảm XCLK qua menuconfig. Các hàm như `set_hmirror`, `set_whitebal` đọc-sửa-ghi thanh ghi qua SCCB (đọc bị sai) nên sẽ ghi giá trị rác.

**`ov7725: Mismatch PID=0x76` trong log**
Bình thường. Driver dò lần lượt các loại cảm biến, sau đó vẫn báo `Detected OV7670 camera`.

**Màu sai / ngả màu lạ**
Đã sửa bằng bộ thanh ghi chất lượng ảnh trong `main.c`. Kiểm tra dây bằng mẫu sọc màu: ghi `0x70=0xBA`, `0x71=0xB5` sau init; nếu 8 sọc đúng màu (trắng, vàng, xanh lơ, xanh lá, tím hồng, đỏ, xanh dương, đen) thì dây D0..D7 đúng. RGB565 cho màu tốt hơn YUV422.

**Ảnh quá tối / quá sáng**
Chỉnh mức sáng mục tiêu `{0x24, ...}` (AEW) và `{0x25, ...}` (AEB) trong `OV7670_QUALITY_REGS`, `main/main.c`.

**`esp_camera_init failed` / không nhận camera (SCCB)**
Kiểm tra SDA/SCL, nguồn 3.3V, pull-up 4.7k, XCLK có ra không, RST đã nối 3.3V và PWDN đã nối GND chưa.

**Không có PSRAM hoặc PSRAM quad (N8R2, N4R2)**
Với PSRAM quad: `Component config -> ESP PSRAM -> Mode` chọn Quad. Không có PSRAM thì vẫn chạy nhưng bị hạ xuống QVGA, 1 frame buffer.

**Cảnh báo `Detected size(16384k) larger than ... image header(4096k)`**
Chỉ là cảnh báo: board có flash 16MB nhưng project đang khai báo 4MB. Muốn dùng hết thì đổi `Serial flasher config -> Flash size`.

## Cấu trúc project

```
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── read_camera.py        # xem stream bằng OpenCV
├── experimental/         # code VQA thử nghiệm, KHÔNG build vào firmware
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml     # phụ thuộc espressif/esp32-camera
    ├── Kconfig.projbuild     # menu "OV7670 Webcam"
    └── main.c                # camera, WiFi, webserver
```

## Hạn chế đã biết

- OV7670 cho chất lượng hình trung bình (nhiễu, màu hơi lệch), phù hợp thử nghiệm và học tập.
- Tốc độ khoảng 10 FPS ở QVGA (13 FPS với CLKRC=0), giới hạn bởi nén JPEG bằng phần mềm (~70ms/frame).
- Code trong `experimental/` (phát hiện vật thể, NLP hỏi đáp) chưa chạy model TFLite thật và chưa được build. Mô tả ý tưởng: `experimental/README_vqa.md`.
- Mỗi lúc chỉ nên mở một luồng `/stream`.

## Giấy phép

Chưa chọn giấy phép. Thêm file `LICENSE` (ví dụ MIT) theo nhu cầu.
