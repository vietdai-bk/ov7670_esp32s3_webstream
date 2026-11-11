/*
 * main.c - ESP32-S3 + OV7670: chi stream camera len web
 *
 *  - Camera OV7670 (Pins: SDA=16, SCL=17, XCLK=12, PCLK=13, HREF=15, VSYNC=14, D0..D7=GPIO4..11)
 *  - Webserver:
 *      http://<ip>/            trang web xem stream
 *      http://<ip>/capture     1 anh JPEG
 *      http://<ip>:81/stream   MJPEG stream
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "esp_camera.h"
#include "img_converters.h"
#include "sdkconfig.h"

static const char *TAG = "cam_stream";

/* ---------------- Pin map OV7670 -> ESP32-S3 ---------------- */
#define CAM_PIN_PWDN   -1   /* PWDN noi GND */
#define CAM_PIN_RESET  -1   /* RST noi 3.3V */
#define CAM_PIN_XCLK   12
#define CAM_PIN_SIOD   16   /* SDA */
#define CAM_PIN_SIOC   17   /* SCL */
#define CAM_PIN_D0      4
#define CAM_PIN_D1      5
#define CAM_PIN_D2      6
#define CAM_PIN_D3      7
#define CAM_PIN_D4      8
#define CAM_PIN_D5      9
#define CAM_PIN_D6     10
#define CAM_PIN_D7     11
#define CAM_PIN_VSYNC  14
#define CAM_PIN_HREF   15
#define CAM_PIN_PCLK   13

#define JPEG_QUALITY_PERCENT  (100 - (CONFIG_APP_JPEG_QUALITY * 100 / 63))

/* Thanh ghi chat luong anh OV7670 (gamma, AEC/AGC, AWB, ma tran mau) lay tu driver Linux
 * drivers/media/i2c/ov7670.c. Driver esp32-camera chi ghi bo toi gian -> anh chay sang, am tim.
 * Bo qua clock/cua so/scaling de khong doi FPS va khung hinh. */
static const uint8_t OV7670_QUALITY_REGS[][2] = {
    /* Gamma */
    {0x7a,0x20},{0x7b,0x10},{0x7c,0x1e},{0x7d,0x35},{0x7e,0x5a},{0x7f,0x69},{0x80,0x76},{0x81,0x80},
    {0x82,0x88},{0x83,0x8f},{0x84,0x96},{0x85,0xa3},{0x86,0xaf},{0x87,0xc4},{0x88,0xd7},{0x89,0xe8},
    /* AGC/AEC: tat truoc khi chinh. AEC kieu trung binh (0xaa=0x14, Linux dung histogram 0x94 bo qua AEW/AEB), muc sang AEW/AEB/VPT */
    {0x13,0xe0},{0x00,0x00},{0x10,0x00},{0x0d,0x40},{0x14,0x18},{0xa5,0x05},{0xab,0x07},
    {0x24,0xa0},{0x25,0x90},{0x26,0xe5},{0x9f,0x78},{0xa0,0x68},{0xa1,0x03},{0xa6,0xd8},
    {0xa7,0xd8},{0xa8,0xf0},{0xa9,0x90},{0xaa,0x14},{0x13,0xe5},
    /* Reserved "magic" */
    {0x0e,0x61},{0x0f,0x4b},{0x16,0x02},{0x1e,0x07},{0x21,0x02},{0x22,0x91},{0x29,0x07},{0x33,0x0b},
    {0x35,0x0b},{0x37,0x1d},{0x38,0x71},{0x39,0x2a},{0x3c,0x78},{0x4d,0x40},{0x4e,0x20},{0x69,0x00},
    {0x74,0x10},{0x8d,0x4f},{0x8e,0x00},{0x8f,0x00},{0x90,0x00},{0x91,0x00},{0x96,0x00},{0x9a,0x00},
    {0xb0,0x84},{0xb1,0x0c},{0xb2,0x0e},{0xb3,0x82},{0xb8,0x0a},
    /* Can bang trang (AWB) */
    {0x43,0x0a},{0x44,0xf0},{0x45,0x34},{0x46,0x58},{0x47,0x28},{0x48,0x3a},{0x59,0x88},{0x5a,0x88},
    {0x5b,0x44},{0x5c,0x67},{0x5d,0x49},{0x5e,0x0e},{0x6c,0x0a},{0x6d,0x55},{0x6e,0x11},{0x6f,0x9f},
    {0x6a,0x40},{0x01,0x40},{0x02,0x60},{0x13,0xe7},
    /* Ma tran, edge, denoise, banding */
    {0x58,0x9e},{0x41,0x08},{0x3f,0x00},{0x75,0x05},{0x76,0xe1},{0x4c,0x00},{0x77,0x01},{0x4b,0x09},
    {0xc9,0x60},{0x41,0x38},{0x56,0x40},{0x34,0x11},{0x3b,0x12},{0xa4,0x88},{0x96,0x00},{0x97,0x30},
    {0x98,0x20},{0x99,0x30},{0x9a,0x84},{0x9b,0x29},{0x9c,0x03},{0x9d,0x4c},{0x9e,0x3f},{0x78,0x04},
    {0x79,0x01},{0xc8,0xf0},{0x79,0x0f},{0xc8,0x00},{0x79,0x10},{0xc8,0x7e},{0x79,0x0a},{0xc8,0x80},
    {0x79,0x0b},{0xc8,0x01},{0x79,0x0c},{0xc8,0x0f},{0x79,0x0d},{0xc8,0x20},{0x79,0x09},{0xc8,0x80},
    {0x79,0x02},{0xc8,0xc0},{0x79,0x03},{0xc8,0x40},{0x79,0x05},{0xc8,0x30},{0x79,0x26},
#if CONFIG_APP_PIXFMT_YUV422
    {0x14,0x18},{0x4f,0x80},{0x50,0x80},{0x51,0x00},{0x52,0x22},{0x53,0x5e},{0x54,0x80},{0x3d,0xc0},
#else
    {0x14,0x38},{0x4f,0xb3},{0x50,0xb3},{0x51,0x00},{0x52,0x3d},{0x53,0xa7},{0x54,0xe4},{0x3d,0xc0},
#endif
};

/* ---------------- Camera Initialization ---------------- */
static esp_err_t camera_init(void)
{
    bool psram = esp_psram_is_initialized();
    ESP_LOGI(TAG, "PSRAM Initialization: %s", psram ? "YES (Octal)" : "NO");

    framesize_t fs;
#if CONFIG_APP_FRAME_QQVGA
    fs = FRAMESIZE_QQVGA;
#elif CONFIG_APP_FRAME_VGA
    fs = FRAMESIZE_VGA;
#else
    fs = FRAMESIZE_QVGA;
#endif
    if (!psram && fs == FRAMESIZE_VGA) {
        ESP_LOGW(TAG, "Khong co PSRAM -> ha xuong QVGA");
        fs = FRAMESIZE_QVGA;
    }

    camera_config_t cfg = {0};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0 = CAM_PIN_D0;  cfg.pin_d1 = CAM_PIN_D1;
    cfg.pin_d2 = CAM_PIN_D2;  cfg.pin_d3 = CAM_PIN_D3;
    cfg.pin_d4 = CAM_PIN_D4;  cfg.pin_d5 = CAM_PIN_D5;
    cfg.pin_d6 = CAM_PIN_D6;  cfg.pin_d7 = CAM_PIN_D7;
    cfg.pin_xclk  = CAM_PIN_XCLK;
    cfg.pin_pclk  = CAM_PIN_PCLK;
    cfg.pin_vsync = CAM_PIN_VSYNC;
    cfg.pin_href  = CAM_PIN_HREF;
    cfg.pin_sccb_sda = CAM_PIN_SIOD;
    cfg.pin_sccb_scl = CAM_PIN_SIOC;
    cfg.sccb_i2c_port = 0;
    cfg.pin_pwdn  = CAM_PIN_PWDN;
    cfg.pin_reset = CAM_PIN_RESET;
    cfg.xclk_freq_hz = CONFIG_APP_XCLK_MHZ * 1000000;

#if CONFIG_APP_PIXFMT_YUV422
    cfg.pixel_format = PIXFORMAT_YUV422;
#else
    cfg.pixel_format = PIXFORMAT_RGB565;
#endif
    cfg.frame_size   = fs;
    cfg.jpeg_quality = CONFIG_APP_JPEG_QUALITY;

    if (psram) {
        cfg.fb_count    = 2;
        cfg.fb_location = CAMERA_FB_IN_PSRAM;
        cfg.grab_mode   = CAMERA_GRAB_LATEST;
    } else {
        cfg.fb_count    = 1;
        cfg.fb_location = CAMERA_FB_IN_DRAM;
        cfg.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "Sensor PID=0x%02x VER=0x%02x", s->id.PID, s->id.VER);
        /* Driver ov7670 doc CLKRC qua SCCB bi loi (tra ve 0xa1) roi ghi lai -> clock chia 34, ~1 FPS.
         * shortcut: ghi de CLKRC sau init, bo khi driver esp32-camera sua loi doc SCCB. */
        extern int SCCB_Write(uint8_t, uint8_t, uint8_t);
        SCCB_Write(s->slv_addr, 0x11, CONFIG_APP_OV7670_CLKRC);
        for (size_t i = 0; i < sizeof(OV7670_QUALITY_REGS) / sizeof(OV7670_QUALITY_REGS[0]); i++) {
            if (SCCB_Write(s->slv_addr, OV7670_QUALITY_REGS[i][0], OV7670_QUALITY_REGS[i][1]) != 0) {
                ESP_LOGW(TAG, "Ghi thanh ghi OV7670 0x%02x loi", OV7670_QUALITY_REGS[i][0]);
            }
        }
        /* Cua so anh: driver dung hstart=158 cho QVGA va xoa bit HREF[7:6] -> ~8 cot rac xanh/vang o
         * mep trai. Dung gia tri cua driver Linux, giu HREF edge offset = 0x80 (gia tri reset). */
        int hs = 172, he = 28, vs = 12, ve = 492;
        if (fs == FRAMESIZE_VGA) { hs = 158; he = 14; vs = 10; ve = 490; }
        SCCB_Write(s->slv_addr, 0x17, hs >> 3);                                  /* HSTART */
        SCCB_Write(s->slv_addr, 0x18, he >> 3);                                  /* HSTOP */
        SCCB_Write(s->slv_addr, 0x32, 0x80 | ((he & 7) << 3) | (hs & 7));        /* HREF */
        SCCB_Write(s->slv_addr, 0x19, vs >> 2);                                  /* VSTRT */
        SCCB_Write(s->slv_addr, 0x1a, ve >> 2);                                  /* VSTOP */
        SCCB_Write(s->slv_addr, 0x03, ((ve & 3) << 2) | (vs & 3));               /* VREF */
    }
    return ESP_OK;
}

/* Lay 1 frame va nen sang JPEG. Buffer tra ve phai free(). */
static bool grab_jpeg(uint8_t **out, size_t *out_len)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        return false;
    }
    bool ok = frame2jpg(fb, JPEG_QUALITY_PERCENT, out, out_len);
    esp_camera_fb_return(fb);
    if (!ok) {
        ESP_LOGE(TAG, "JPEG compression failed");
    }
    return ok;
}

/* ---------------- HTTP Handlers ---------------- */

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-S3 OV7670</title>"
"<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:16px}"
"img{max-width:100%;border:2px solid #444;border-radius:8px;background:#000;min-height:120px}"
"button{background:#2a7;border:0;color:#fff;padding:10px 18px;margin:8px 4px;border-radius:6px;font-size:16px}</style>"
"</head><body>"
"<h2>ESP32-S3 + OV7670</h2>"
"<img id='v' alt='stream'><br>"
"<button onclick='start()'>Stream</button>"
"<button onclick='stop()'>Stop</button>"
"<button onclick='snap()'>Snapshot</button>"
"<script>"
"var v=document.getElementById('v');"
"function start(){v.src='http://'+location.hostname+':81/stream?'+Date.now();}"
"function stop(){v.removeAttribute('src');}"
"function snap(){v.src='/capture?'+Date.now();}"
"window.onload=start;"
"</script></body></html>";

/* 1. Trang web */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* 2. Snapshot JPEG */
static esp_err_t capture_handler(httpd_req_t *req)
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    if (!grab_jpeg(&jpg, &len)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, (const char *)jpg, len);
    free(jpg);
    return res;
}

/* 3. MJPEG Stream */
#define STREAM_BOUNDARY "frameboundary1234567890"
static const char *STREAM_CT   = "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY;
static const char *STREAM_SEP  = "\r\n--" STREAM_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CT);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Thong ke FPS: cong don thoi gian tung buoc, in log moi giay */
    int frames = 0;
    int64_t t_cap = 0, t_enc = 0, t_send = 0, bytes = 0;
    int64_t t_win = esp_timer_get_time();

    while (true) {
        int64_t t0 = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        int64_t t1 = esp_timer_get_time();
        uint8_t *jpg = NULL;
        size_t len = 0;
        bool ok = frame2jpg(fb, JPEG_QUALITY_PERCENT, &jpg, &len);
        esp_camera_fb_return(fb);
        if (!ok) {
            ESP_LOGE(TAG, "JPEG compression failed");
            res = ESP_FAIL;
            break;
        }
        int64_t t2 = esp_timer_get_time();

        char hdr[80];
        size_t hlen = snprintf(hdr, sizeof(hdr), STREAM_PART, (unsigned)len);

        res = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpg, len);
        free(jpg);
        if (res != ESP_OK) break;
        int64_t t3 = esp_timer_get_time();

        frames++;
        t_cap += t1 - t0; t_enc += t2 - t1; t_send += t3 - t2; bytes += len;
        if (t3 - t_win >= 1000000) {
            ESP_LOGI(TAG, "FPS %.1f | cap %lldms enc %lldms send %lldms | %lldKB/frame",
                     frames * 1e6 / (t3 - t_win), t_cap / frames / 1000, t_enc / frames / 1000,
                     t_send / frames / 1000, bytes / frames / 1024);
            frames = 0; t_cap = t_enc = t_send = bytes = 0; t_win = t3;
        }
    }
    return res;
}

/* ---------------- Web Server Setup ---------------- */
static void start_webservers(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 5;

    httpd_uri_t index_uri   = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,   .user_ctx = NULL };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = NULL };

    httpd_handle_t main_srv = NULL, stream_srv = NULL;

    /* Main Server Port 80 */
    cfg.server_port = 80;
    cfg.ctrl_port   = 32768;
    if (httpd_start(&main_srv, &cfg) == ESP_OK) {
        httpd_register_uri_handler(main_srv, &index_uri);
        httpd_register_uri_handler(main_srv, &capture_uri);
        ESP_LOGI(TAG, "Web Server started on port 80");
    }

    /* Stream Server Port 81 */
    cfg.server_port = 81;
    cfg.ctrl_port   = 32769;
    cfg.max_open_sockets = 3;
    if (httpd_start(&stream_srv, &cfg) == ESP_OK) {
        httpd_register_uri_handler(stream_srv, &stream_uri);
        ESP_LOGI(TAG, "Camera Stream Server started on port 81");
    }
}

/* ---------------- WiFi Management ---------------- */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_eg;
static int s_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < CONFIG_APP_WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "Retry WiFi %d/%d", s_retry, CONFIG_APP_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "==> KET NOI THANH CONG! Mo trinh duyet: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void start_softap(void)
{
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_APP_AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_APP_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(CONFIG_APP_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(CONFIG_APP_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Khoi dong SoftAP: SSID='%s'", CONFIG_APP_AP_SSID);
    ESP_LOGI(TAG, "==> Ket noi WiFi do roi mo trinh duyet: http://192.168.4.1/");
}

static void wifi_start(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        ESP_LOGI(TAG, "SSID de trong -> khoi dong SoftAP");
        start_softap();
        return;
    }

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = strlen(CONFIG_APP_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); /* power save mac dinh lam stream giat, send 50-600ms */

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Ket noi '%s' that bai hoac timeout -> chuyen sang SoftAP", CONFIG_APP_WIFI_SSID);
        esp_wifi_stop();
        start_softap();
    }
}

/* ---------------- app_main ---------------- */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== ESP32-S3 + OV7670: STREAM CAMERA ===");

    /* 1. Khoi tao Camera OV7670 */
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera loi! Kiem tra cac chan cam OV7670 (SDA=16, SCL=17, XCLK=12, PCLK=13)");
        return;
    }

    /* 2. Ket noi WiFi / SoftAP */
    wifi_start();

    /* 3. Khoi dong Web Server */
    start_webservers();

    ESP_LOGI(TAG, "San sang! Mo trinh duyet de xem camera.");
}