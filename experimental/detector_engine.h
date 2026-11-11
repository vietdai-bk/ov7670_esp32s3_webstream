/*
 * detector_engine.h - He thong Object Detector phat hien dien thoai tren ESP32-S3
 */
#ifndef DETECTOR_ENGINE_H
#define DETECTOR_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"

#define MAX_DETECTED_BOXES 8
#define DETECTOR_INPUT_W   96
#define DETECTOR_INPUT_H   96

typedef struct {
    float xmin;     /* 0.0 .. 1.0 (ti le chieu ngang) */
    float ymin;     /* 0.0 .. 1.0 (ti le chieu doc) */
    float xmax;     /* 0.0 .. 1.0 */
    float ymax;     /* 0.0 .. 1.0 */
    float conf;     /* 0.0 .. 1.0 (do tin cay) */
    char label[16]; /* "coin" */
} bbox_t;

typedef struct {
    int count;
    bbox_t boxes[MAX_DETECTED_BOXES];
    uint32_t latency_ms;
    float threshold_used;
} detector_result_t;

/**
 * Khoi tao engine detector (khoi tao bo dem Tensor Arena trong PSRAM)
 */
esp_err_t detector_init(void);

/**
 * Chay detector tren frame camera hien tai
 * @param fb Frame buffer lay tu esp_camera_fb_get()
 * @param conf_threshold Nguong do tin cay (mac dinh 0.50f)
 * @param out_result Ket qua chua so luong box va toa do
 */
esp_err_t detector_run(camera_fb_t *fb, float conf_threshold, detector_result_t *out_result);

/**
 * Giai phong tai nguyen detector
 */
void detector_deinit(void);

#endif /* DETECTOR_ENGINE_H */
