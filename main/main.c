/*
 * ESP32-S3 + OV7670 -> MJPEG webserver (ESP-IDF 5.x, esp32-camera)
 *
 * Wiring (OV7670 -> ESP32-S3):
 *   SDA=16 SCL=17 XCLK=12 PCLK=13 HREF=15 VSYNC=14
 *   D0..D7 = GPIO4..GPIO11
 *   RST -> 3.3V, PWDN -> GND  (khong dung chan GPIO)
 *
 * Endpoints:
 *   http://<ip>/          trang web
 *   http://<ip>/capture   1 anh JPEG
 *   http://<ip>:81/stream MJPEG stream
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

static const char *TAG = "ov7670_cam";

/* ---------------- Pin map ---------------- */
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

/* frame2jpg dung thang do 1..100 (so lon = chat luong cao) */
#define JPEG_QUALITY_PERCENT  (100 - (CONFIG_APP_JPEG_QUALITY * 100 / 63))

/* ---------------- Camera ---------------- */
static esp_err_t camera_init(void)
{
    bool psram = esp_psram_is_initialized();
    ESP_LOGI(TAG, "PSRAM: %s", psram ? "yes" : "no");

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
    cfg.jpeg_quality = CONFIG_APP_JPEG_QUALITY; /* khong dung voi OV7670, giu cho day du */

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
        ESP_LOGI(TAG, "Sensor PID=0x%02x VER=0x%02x MIDH=0x%02x MIDL=0x%02x",
                 s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL);
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

/* ---------------- Web server ---------------- */
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

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

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

#define STREAM_BOUNDARY "frameboundary1234567890"
static const char *STREAM_CT   = "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY;
static const char *STREAM_SEP  = "\r\n--" STREAM_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CT);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int64_t t0 = esp_timer_get_time();
    int frames = 0;

    while (true) {
        uint8_t *jpg = NULL;
        size_t len = 0;
        if (!grab_jpeg(&jpg, &len)) {
            res = ESP_FAIL;
            break;
        }

        char hdr[80];
        size_t hlen = snprintf(hdr, sizeof(hdr), STREAM_PART, (unsigned)len);

        res = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, hdr, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpg, len);
        free(jpg);
        if (res != ESP_OK) break;

        if (++frames % 60 == 0) {
            int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "stream: %.1f fps", 60.0 * 1e6 / (double)(now - t0));
            t0 = now;
        }
    }
    ESP_LOGI(TAG, "stream closed");
    return res;
}

static void start_webservers(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 4;

    httpd_uri_t index_uri   = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,   .user_ctx = NULL };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = NULL };

    httpd_handle_t main_srv = NULL, stream_srv = NULL;

    cfg.server_port = 80;
    cfg.ctrl_port   = 32768;
    if (httpd_start(&main_srv, &cfg) == ESP_OK) {
        httpd_register_uri_handler(main_srv, &index_uri);
        httpd_register_uri_handler(main_srv, &capture_uri);
        ESP_LOGI(TAG, "Web server on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start web server (80)");
    }

    cfg.server_port = 81;
    cfg.ctrl_port   = 32769;
    cfg.max_open_sockets = 3;
    if (httpd_start(&stream_srv, &cfg) == ESP_OK) {
        httpd_register_uri_handler(stream_srv, &stream_uri);
        ESP_LOGI(TAG, "Stream server on port 81");
    } else {
        ESP_LOGE(TAG, "Failed to start stream server (81)");
    }
}

/* ---------------- WiFi ---------------- */
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
        ESP_LOGI(TAG, "==> Mo trinh duyet: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
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
    ESP_LOGI(TAG, "SoftAP: SSID=%s PASS=%s", CONFIG_APP_AP_SSID, CONFIG_APP_AP_PASSWORD);
    ESP_LOGI(TAG, "==> Ket noi WiFi do roi mo: http://192.168.4.1/");
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
        ESP_LOGI(TAG, "Khong co SSID -> chay SoftAP");
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

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Ket noi '%s' that bai -> chuyen sang SoftAP", CONFIG_APP_WIFI_SSID);
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

    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera loi. Kiem tra day noi (dac biet SDA/SCL pull-up, XCLK, nguon 3.3V).");
        return;
    }

    wifi_start();
    start_webservers();
}
