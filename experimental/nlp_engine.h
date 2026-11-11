/*
 * nlp_engine.h - He thong NLP phan loai y dinh cau hoi (Intent & Entity) tren ESP32-S3
 */
#ifndef NLP_ENGINE_H
#define NLP_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "intent_vocab.h"

typedef struct {
    qa_intent_t intent;
    float confidence;
    char target_entity[24];  /* vi du: "dong xu", "coin", "dien thoai", "xe may" */
    char normalized_q[128];
    uint32_t latency_ms;
} nlp_result_t;

/**
 * Khoi tao engine NLP (Load model intent vao PSRAM/DRAM)
 */
esp_err_t nlp_init(void);

/**
 * Phan tich cau hoi cua nguoi dung
 * @param question_text Chuoi cau hoi (tieng Viet hoac tieng Anh)
 * @param out_result Ket qua intent, entity va do tin cay
 */
esp_err_t nlp_classify_intent(const char *question_text, nlp_result_t *out_result);

/**
 * Giai phong tai nguyen NLP
 */
void nlp_deinit(void);

#endif /* NLP_ENGINE_H */
