# ESP32-S3 + OV7670 -> Webserver (ESP-IDF)

## So do chan
| OV7670 | ESP32-S3 |
|--------|----------|
| 3.3V / GND | 3.3V / GND |
| SDA / SCL | GPIO16 / GPIO17 |
| XCLK | GPIO12 |
| PCLK | GPIO13 |
| VSYNC / HREF | GPIO14 / GPIO15 |
| D0..D7 | GPIO4..GPIO11 |
| RST | 3.3V |
| PWDN | GND |

## Build & flash (ESP-IDF >= 5.0)
```bash
idf.py set-target esp32s3
idf.py menuconfig      # OV7670 Webcam -> WiFi SSID / password (de trong = SoftAP)
idf.py build flash monitor
```
Lan build dau can internet de tai component `espressif/esp32-camera`.

## Su dung
- Co WiFi: xem IP trong log, mo `http://<ip>/`
- SoftAP: ket noi WiFi `OV7670-CAM` / `12345678`, mo `http://192.168.4.1/`
- `/capture` = 1 anh JPEG, `:81/stream` = MJPEG

## Luu y
- PSRAM: mac dinh bat octal (N8R8/N16R8). Module quad PSRAM (N8R2/N4R2) doi
  `Component config -> ESP PSRAM -> Mode` sang Quad. Khong co PSRAM van chay (QVGA, 1 buffer).
- Hinh bi sai mau: menuconfig -> "Pixel format" -> YUV422.
- Khong len hinh / loi SCCB: OV7670 can pull-up 4.7k cho SDA/SCL (nhieu module da co san), day ngan, giam XCLK xuong 8-10MHz.
