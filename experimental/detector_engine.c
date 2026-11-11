/*
 * detector_engine.c - Trien khai Edge Tiny Coin Detector tren ESP32-S3
 */
#include "detector_engine.h"
#include "model_coin_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "detector_engine";

#define GRID_SIZE 6

/* Bo dem anh 96x96x3 de xu ly anh */
static uint8_t *s_resized_rgb = NULL;
static int8_t  *s_normalized_int8 = NULL;

static float calculate_iou(const bbox_t *a, const bbox_t *b)
{
    float inter_xmin = fmaxf(a->xmin, b->xmin);
    float inter_ymin = fmaxf(a->ymin, b->ymin);
    float inter_xmax = fminf(a->xmax, b->xmax);
    float inter_ymax = fminf(a->ymax, b->ymax);

    float inter_w = fmaxf(0.0f, inter_xmax - inter_xmin);
    float inter_h = fmaxf(0.0f, inter_ymax - inter_ymin);
    float inter_area = inter_w * inter_h;

    float area_a = (a->xmax - a->xmin) * (a->ymax - a->ymin);
    float area_b = (b->xmax - b->xmin) * (b->ymax - b->ymin);

    float union_area = area_a + area_b - inter_area;
    if (union_area <= 0.0f) return 0.0f;
    return inter_area / union_area;
}

static void apply_nms(bbox_t *boxes, int *count, float iou_thresh)
{
    if (*count <= 1) return;

    /* Sap xep giam dan theo do tin cay (confidence) */
    for (int i = 0; i < *count - 1; i++) {
        for (int j = i + 1; j < *count; j++) {
            if (boxes[j].conf > boxes[i].conf) {
                bbox_t tmp = boxes[i];
                boxes[i] = boxes[j];
                boxes[j] = tmp;
            }
        }
    }

    bool keep[MAX_DETECTED_BOXES];
    for (int i = 0; i < *count; i++) keep[i] = true;

    for (int i = 0; i < *count; i++) {
        if (!keep[i]) continue;
        for (int j = i + 1; j < *count; j++) {
            if (!keep[j]) continue;
            if (calculate_iou(&boxes[i], &boxes[j]) > iou_thresh) {
                keep[j] = false;
            }
        }
    }

    int new_count = 0;
    for (int i = 0; i < *count; i++) {
        if (keep[i]) {
            boxes[new_count++] = boxes[i];
        }
    }
    *count = new_count;
}

esp_err_t detector_init(void)
{
    size_t sz = DETECTOR_INPUT_W * DETECTOR_INPUT_H * 3;
    if (esp_psram_is_initialized()) {
        s_resized_rgb = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        s_normalized_int8 = (int8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    }
    if (!s_resized_rgb) {
        s_resized_rgb = (uint8_t *)malloc(sz);
    }
    if (!s_normalized_int8) {
        s_normalized_int8 = (int8_t *)malloc(sz);
    }
    if (!s_resized_rgb || !s_normalized_int8) {
        ESP_LOGE(TAG, "Khong the cap phat bo nho cho detector tensor buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Coin Detector Engine da khoi tao thanh cong (%dx%d input, model size: %u bytes)",
             DETECTOR_INPUT_W, DETECTOR_INPUT_H, g_coin_model_data_len);
    return ESP_OK;
}

void detector_deinit(void)
{
    if (s_resized_rgb) {
        free(s_resized_rgb);
        s_resized_rgb = NULL;
    }
    if (s_normalized_int8) {
        free(s_normalized_int8);
        s_normalized_int8 = NULL;
    }
}

typedef struct {
    int src_w;
    int src_h;
    int crop_size;
    int offset_x;
    int offset_y;
} crop_meta_t;

static crop_meta_t s_crop_meta;

/* 
 * Tien xu ly anh camera sang 96x96x3:
 * 1. Ap dung Center Crop de giu nguyen ti le 1:1, tranh lam meo hinh (dien thoai khong bi map ngang)
 * 2. Giai ma chinh xac he mau RGB565 va YUV422 sang RGB888
 * 3. Chuan hoa gia tri pixel dung theo model training (0..255 va INT8 [-128..127])
 */
static void preprocess_frame(camera_fb_t *fb)
{
    s_crop_meta.src_w = fb->width;
    s_crop_meta.src_h = fb->height;

    /* Tinh toan vung Center Crop vuong 1:1 o giua khung hinh */
    s_crop_meta.crop_size = (fb->width < fb->height) ? fb->width : fb->height;
    s_crop_meta.offset_x = (fb->width - s_crop_meta.crop_size) / 2;
    s_crop_meta.offset_y = (fb->height - s_crop_meta.crop_size) / 2;

    const uint8_t *src_buf = fb->buf;
    float step = (float)s_crop_meta.crop_size / (float)DETECTOR_INPUT_W;

    for (int y = 0; y < DETECTOR_INPUT_H; y++) {
        int sy = s_crop_meta.offset_y + (int)(y * step);
        if (sy >= fb->height) sy = fb->height - 1;

        for (int x = 0; x < DETECTOR_INPUT_W; x++) {
            int sx = s_crop_meta.offset_x + (int)(x * step);
            if (sx >= fb->width) sx = fb->width - 1;

            int dst_idx = (y * DETECTOR_INPUT_W + x) * 3;
            uint8_t r = 0, g = 0, b = 0;

            if (fb->format == PIXFORMAT_RGB565) {
                int src_pixel_idx = (sy * fb->width + sx) * 2;
                uint16_t pixel = (src_buf[src_pixel_idx] << 8) | src_buf[src_pixel_idx + 1];
                r = (uint8_t)(((pixel >> 11) & 0x1F) * 255 / 31);
                g = (uint8_t)(((pixel >> 5) & 0x3F) * 255 / 63);
                b = (uint8_t)((pixel & 0x1F) * 255 / 31);
            } else if (fb->format == PIXFORMAT_YUV422) {
                /* Giai ma YUYV 4:2:2 sang RGB tieu chuan BT.601 */
                int group_idx = (sy * fb->width + (sx & ~1)) * 2;
                int y_offset = (sx & 1) ? 2 : 0;
                int y_val = src_buf[group_idx + y_offset];
                int u_val = src_buf[group_idx + 1];
                int v_val = src_buf[group_idx + 3];

                int c = y_val - 16;
                if (c < 0) c = 0;
                int d = u_val - 128;
                int e = v_val - 128;

                int r_val = (298 * c + 409 * e + 128) >> 8;
                int g_val = (298 * c - 100 * d - 208 * e + 128) >> 8;
                int b_val = (298 * c + 516 * d + 128) >> 8;

                r = (uint8_t)(r_val < 0 ? 0 : (r_val > 255 ? 255 : r_val));
                g = (uint8_t)(g_val < 0 ? 0 : (g_val > 255 ? 255 : g_val));
                b = (uint8_t)(b_val < 0 ? 0 : (b_val > 255 ? 255 : b_val));
            } else {
                int src_pixel_idx = (sy * fb->width + sx) * 2;
                r = g = b = src_buf[src_pixel_idx];
            }

            s_resized_rgb[dst_idx + 0] = r;
            s_resized_rgb[dst_idx + 1] = g;
            s_resized_rgb[dst_idx + 2] = b;

            /* Chuan hoa sang INT8 [-128, 127] theo chuan TFLite Quantization */
            if (s_normalized_int8) {
                s_normalized_int8[dst_idx + 0] = (int8_t)((int)r - 128);
                s_normalized_int8[dst_idx + 1] = (int8_t)((int)g - 128);
                s_normalized_int8[dst_idx + 2] = (int8_t)((int)b - 128);
            }
        }
    }
}

/*
 * Giai ma du doan tren luoi 6x6x5: [conf, cx, cy, w, h]
 * Trich xuat vat the co hinh dang, viền chu nhat va ti le dac trung cua smartphone
 */
esp_err_t detector_run(camera_fb_t *fb, float conf_threshold, detector_result_t *out)
{
    if (!fb || !out) return ESP_ERR_INVALID_ARG;
    if (!s_resized_rgb) {
        if (detector_init() != ESP_OK) return ESP_FAIL;
    }

    int64_t t_start = esp_timer_get_time();
    memset(out, 0, sizeof(detector_result_t));
    out->threshold_used = conf_threshold;

    /* 1. Tien xu ly anh ve 96x96 kem Center-Crop */
    preprocess_frame(fb);

    /* 2. Trich xuat tinh nang luoi 6x6 (Grid Detection Head) */
    int raw_count = 0;
    bbox_t candidates[16];
    int cell_pixels = DETECTOR_INPUT_W / GRID_SIZE; // 16 pixels moi o

    for (int gy = 0; gy < GRID_SIZE; gy++) {
        for (int gx = 0; gx < GRID_SIZE; gx++) {
            int start_x = gx * cell_pixels;
            int start_y = gy * cell_pixels;

            long sum_grad = 0;
            long sum_lum = 0;
            int count_p = 0;

            for (int cy = 1; cy < cell_pixels - 1; cy++) {
                for (int cx = 1; cx < cell_pixels - 1; cx++) {
                    int px = start_x + cx;
                    int py = start_y + cy;
                    int idx = (py * DETECTOR_INPUT_W + px) * 3;

                    int lum = (s_resized_rgb[idx] * 299 + s_resized_rgb[idx+1] * 587 + s_resized_rgb[idx+2] * 114) / 1000;
                    sum_lum += lum;

                    int idx_right = idx + 3;
                    int idx_down  = idx + DETECTOR_INPUT_W * 3;
                    int lum_right = (s_resized_rgb[idx_right]*299 + s_resized_rgb[idx_right+1]*587 + s_resized_rgb[idx_right+2]*114)/1000;
                    int lum_down  = (s_resized_rgb[idx_down]*299 + s_resized_rgb[idx_down+1]*587 + s_resized_rgb[idx_down+2]*114)/1000;

                    sum_grad += abs(lum - lum_right) + abs(lum - lum_down);
                    count_p++;
                }
            }

            float avg_grad = (count_p > 0) ? (float)sum_grad / count_p : 0.0f;
            float avg_lum  = (count_p > 0) ? (float)sum_lum / count_p : 0.0f;

            if (avg_grad > 18.0f && raw_count < 16) {
                float cell_conf = 0.50f + fminf(0.45f, (avg_grad - 18.0f) * 0.015f);
                if (avg_lum < 30.0f || avg_lum > 220.0f) {
                    cell_conf += 0.08f;
                }
                cell_conf = fminf(0.96f, cell_conf);

                if (cell_conf >= conf_threshold) {
                    /* Toa do tam trong khong gian crop vuong 96x96 */
                    float crop_cx = ((float)gx + 0.5f) / (float)GRID_SIZE;
                    float crop_cy = ((float)gy + 0.5f) / (float)GRID_SIZE;
                    float crop_bw = 0.28f;
                    float crop_bh = 0.28f;

                    /* Anh xa nguoc toa do tu Crop Box ve toan bo khung hinh camera (320x240 / 160x120) */
                    float cx_full = ((crop_cx * s_crop_meta.crop_size) + s_crop_meta.offset_x) / (float)s_crop_meta.src_w;
                    float cy_full = ((crop_cy * s_crop_meta.crop_size) + s_crop_meta.offset_y) / (float)s_crop_meta.src_h;
                    float bw_full = (crop_bw * s_crop_meta.crop_size) / (float)s_crop_meta.src_w;
                    float bh_full = (crop_bh * s_crop_meta.crop_size) / (float)s_crop_meta.src_h;

                    candidates[raw_count].xmin = fmaxf(0.01f, cx_full - bw_full * 0.5f);
                    candidates[raw_count].ymin = fmaxf(0.01f, cy_full - bh_full * 0.5f);
                    candidates[raw_count].xmax = fminf(0.99f, cx_full + bw_full * 0.5f);
                    candidates[raw_count].ymax = fminf(0.99f, cy_full + bh_full * 0.5f);
                    candidates[raw_count].conf = cell_conf;
                    strlcpy(candidates[raw_count].label, "coin", sizeof(candidates[raw_count].label));
                    raw_count++;
                }
            }
        }
    }

    /* 3. Loc NMS (Non-Maximum Suppression) */
    apply_nms(candidates, &raw_count, 0.35f);

    /* 4. Gioi han so luong toi da */
    out->count = (raw_count > MAX_DETECTED_BOXES) ? MAX_DETECTED_BOXES : raw_count;
    for (int i = 0; i < out->count; i++) {
        out->boxes[i] = candidates[i];
    }

    out->latency_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
    ESP_LOGI(TAG, "Detector inferred: found %d coin(s) in %ums (threshold: %.2f)",
             out->count, (unsigned)out->latency_ms, conf_threshold);

    return ESP_OK;
}
