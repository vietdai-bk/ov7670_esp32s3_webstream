# 🤖 ESP32-S3 + OV7670: Dual Edge AI Visual QA System

Dự án triển khai hệ thống **Edge AI Visual Question Answering (VQA)** chạy **100% trên chip ESP32-S3** kết hợp camera **OV7670**:
1. **Model 1 (Tiny Phone Detector)**: Phát hiện điện thoại trong khung hình camera, xuất tọa độ Bounding Box `[xmin, ymin, xmax, ymax]`, độ tin cậy và đếm số lượng.
2. **Model 2 (NLP Intent Classifier)**: Phân loại ý định câu hỏi của người dùng (Tiếng Việt & Tiếng Anh):
   - **`EXISTENCE`** ("Có hay không"): Ví dụ *"Trong frame có điện thoại không?"* ➔ Trả lời: **Có** hoặc **Không**.
   - **`COUNT`** ("Đếm số lượng"): Ví dụ *"Trong frame có bao nhiêu chiếc điện thoại?"* ➔ Trả lời: **1**, **2**, **3**...
   - **`LOCATION`** ("Vị trí"): Ví dụ *"Điện thoại nằm ở đâu?"* ➔ Trả lời: **Chính giữa**, **Bên trái**, **Bên phải**...
3. **Web Dashboard Glassmorphism**: Webserver phục vụ giao diện trực quan ngay trên ESP32-S3:
   - Hiển thị luồng camera thời gian thực hoặc Snapshot.
   - Lớp phủ HTML5 Canvas vẽ Bounding Box màu neon bao quanh điện thoại kèm nhãn và % tin cậy.
   - Khung nhập câu hỏi tương tác & nút bấm nhanh (*Quick Ask Chips*).
   - Thẻ hiển thị đáp án ngắn gọn & chi tiết do 2 mô hình cùng phối hợp suy luận.

---

## 📌 1. Sơ đồ nối chân phần cứng (OV7670 ➔ ESP32-S3)

| Chân OV7670 | Chân ESP32-S3 | Ghi chú |
|---|---|---|
| **3.3V** | **3.3V** | Cấp nguồn ổn định |
| **GND** | **GND** | Nối mass chung |
| **SIOD (SDA)** | **GPIO 16** | Giao tiếp SCCB/I2C (cần trở kéo pull-up 4.7k nếu module chưa có) |
| **SIOC (SCL)** | **GPIO 17** | Xung nhịp SCCB/I2C |
| **XCLK** | **GPIO 12** | Master clock cấp từ ESP32 (10 MHz) |
| **PCLK** | **GPIO 13** | Pixel clock |
| **VSYNC** | **GPIO 14** | Đồng bộ khung dọc (Frame Sync) |
| **HREF** | **GPIO 15** | Đồng bộ dòng ngang (Line Sync) |
| **D0** | **GPIO 4** | Dữ liệu bit 0 |
| **D1** | **GPIO 5** | Dữ liệu bit 1 |
| **D2** | **GPIO 6** | Dữ liệu bit 2 |
| **D3** | **GPIO 7** | Dữ liệu bit 3 |
| **D4** | **GPIO 8** | Dữ liệu bit 4 |
| **D5** | **GPIO 9** | Dữ liệu bit 5 |
| **D6** | **GPIO 10** | Dữ liệu bit 6 |
| **D7** | **GPIO 11** | Dữ liệu bit 7 |
| **RST** | **3.3V** | Nối trực tiếp lên 3.3V |
| **PWDN** | **GND** | Nối trực tiếp xuống GND |

---

## 📂 2. Cấu trúc Dự án

```
ov7670_webcam_esp32s3/
├── notebooks/
│   ├── 01_train_phone_detector.ipynb   # Train Model 1: Tiny-YOLO/MobileNet Phone Detector
│   ├── 02_train_intent_qa_model.ipynb   # Train Model 2: NLP Intent Classifier
│   └── requirements.txt                # Thư viện Python để train model
├── test_pipeline.py                    # Script kiểm thử logic VQA trên PC
└── ov7670_webcam/                      # Source Code ESP-IDF Deploy ESP32-S3
    ├── CMakeLists.txt
    ├── sdkconfig.defaults              # Cấu hình Octal PSRAM, Flash 4MB, WiFi
    └── main/
        ├── CMakeLists.txt
        ├── idf_component.yml
        ├── main.c                      # Khởi tạo Cam, Webserver REST API, WiFi
        ├── detector_engine.h/.c        # Preprocessing, Grid 6x6 Inference, NMS Bbox
        ├── nlp_engine.h/.c             # Tiền xử lý tiếng Việt, Tokenizer, Intent Classifier
        ├── qa_synthesizer.h/.c         # Hợp nhất Model 1 + Model 2 tạo câu trả lời
        ├── web_dashboard.h             # Giao diện Web HTML5 Canvas Glassmorphism
        ├── intent_vocab.h              # Bảng từ điển Tokenizer C-compatible
        ├── model_phone_data.h/.c       # Flatbuffer nhị phân Model 1
        └── model_intent_data.h/.c      # Flatbuffer nhị phân Model 2
```

---

## 🧠 3. Hướng dẫn Huấn luyện 2 Model (Jupyter Notebooks)

Cài đặt môi trường Python:
```bash
pip install -r notebooks/requirements.txt
```

### 📱 Notebook 1: `01_train_phone_detector.ipynb`
- **Bộ dữ liệu**: Tích hợp bộ sinh dữ liệu tổng hợp chân thực (*Photorealistic Synthetic Phone Generator*) tự động tạo hàng ngàn mẫu điện thoại đa dạng góc nghiêng, tỉ lệ viền màn hình, cụm camera trên các nền phòng làm việc, bàn gỗ phức tạp. Hỗ trợ trường hợp 0, 1 hoặc 2 điện thoại/ảnh để huấn luyện đếm số lượng.
- **Kiến trúc**: Depthwise Separable Convolutions siêu nhẹ (96x96x3 đầu vào ➔ Grid 6x6x5: `[conf, cx, cy, w, h]`, dung lượng < 90KB).
- **Hàm mất mát**: Focal Binary Cross-Entropy + Smooth L1 Box Regression.
- **Xuất file**: Xuất model ra TensorFlow Lite (Float32 & INT8 Quantization) và tạo file C header `model_phone_data.h`.

### 💬 Notebook 2: `02_train_intent_qa_model.ipynb`
- **Bộ dữ liệu**: Hơn 400 mẫu câu hỏi tiếng Việt và tiếng Anh đa dạng bao gồm các biến thể bỏ dấu, teencode, từ địa phương.
- **Kiến trúc**: Embedding + 1D Convolution + Global Max Pooling + Dense (< 15KB).
- **Xuất file**: Xuất mảng Flatbuffer C `model_intent_data.h` và bảng từ điển tra cứu `intent_vocab.h`.

---

## 🚀 4. Build & Nạp Code lên ESP32-S3 (ESP-IDF >= 5.0)

Mở terminal trong thư mục `ov7670_webcam`:

1. Thiết lập target sang ESP32-S3:
```bash
idf.py set-target esp32s3
```

2. Cấu hình WiFi (Tùy chọn):
```bash
idf.py menuconfig
# Vào mục "OV7670 Webcam" -> Nhập SSID và Password WiFi của bạn
# (Nếu để trống, ESP32-S3 sẽ tự phát WiFi SoftAP "OV7670-CAM", Pass: "12345678")
```

3. Biên dịch và nạp firmware:
```bash
idf.py build flash monitor
```

---

## 🌐 5. Trải nghiệm Giao diện Web Dashboard

Sau khi nạp xong:
- Nếu dùng WiFi gia đình: Xem địa chỉ IP trên màn hình Serial monitor, mở trình duyệt:
  ```
  http://<ESP32_IP>/
  ```
- Nếu dùng SoftAP: Kết nối WiFi `OV7670-CAM` (mật khẩu `12345678`), mở trình duyệt:
  ```
  http://192.168.4.1/
  ```

### Các tính năng trên Web:
- **Stream Camera**: Bấm `▶ Stream` để xem video MJPEG trực tiếp.
- **Vẽ Bounding Box**: Bấm `⚡ Detect Frame`, ô Canvas sẽ tự động vẽ khung viền cyan quanh điện thoại kèm nhãn `PHONE 93%`.
- **Hỏi đáp thông minh**: 
  - Nhập câu hỏi vào ô textbox (ví dụ: *"trong frame có bao nhiêu chiếc điện thoại?"*, *"có điện thoại không?"*, *"điện thoại ở đâu?"*).
  - Hoặc bấm các nút gợi ý nhanh (*Quick Ask Chips*).
  - ESP32-S3 sẽ phản hồi ngay lập tức:
    - **Đáp án ngắn**: `Có` / `Không` / `1` / `2`...
    - **Đáp án đầy đủ**: *"Có! Phát hiện 1 chiếc điện thoại trong khung hình (độ tin cậy: 93%)."*
    - **Thời gian suy luận**: Model 1 (Detector: ~45ms) | Model 2 (NLP: ~3ms).

---

## 📡 6. Các API REST Endpoints trên ESP32-S3

| Endpoint | Method | Chức năng | Phản hồi mẫu |
|---|---|---|---|
| `/` | GET | Trang chủ Web Dashboard | HTML/CSS/JS |
| `/capture` | GET | Chụp 1 ảnh tĩnh | Image (JPEG) |
| `:81/stream` | GET | Luồng video MJPEG | Multipart JPEG |
| `/api/detect?th=0.5` | GET | Chạy Model 1 phát hiện vật thể | `{"count":1,"time_ms":42,"boxes":[{"x":0.3,"y":0.2,"w":0.3,"h":0.5,"conf":0.93,"label":"phone"}]}` |
| `/api/ask?q=...` | GET/POST | Chạy Model 1 + Model 2 trả lời câu hỏi | `{"question":"co bao nhieu dien thoai","intent":"COUNT","count":1,"short_answer":"1","answer":"Trong khung hinh co 1 chiec dien thoai.","detector_ms":42,"nlp_ms":3}` |
