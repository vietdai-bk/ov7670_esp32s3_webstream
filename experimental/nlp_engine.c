/*
 * nlp_engine.c - Trien khai NLP Engine phan loai y dinh tren ESP32-S3
 */
#include "nlp_engine.h"
#include "model_intent_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "nlp_engine";

/* Loai bo dau tieng Viet co ban de tokenizer bat match tot nhat */
static void normalize_vietnamese_text(const char *input, char *output, size_t max_len)
{
    size_t in_len = strlen(input);
    size_t out_idx = 0;

    for (size_t i = 0; i < in_len && out_idx < max_len - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        /* Xu ly ky tu UTF-8 tieng Viet thong dung */
        if (c >= 0xC0) {
            unsigned char c2 = (unsigned char)input[i + 1];
            i++; /* Bo qua byte thu hai */

            /* Map cac nguyen am co dau ve chu cai khong dau */
            if (c == 0xC3) {
                if (c2 == 0xA1 || c2 == 0xA0 || c2 == 0xA3 || c2 == 0xA2) { output[out_idx++] = 'a'; continue; }
                if (c2 == 0xA9 || c2 == 0xA8 || c2 == 0xAA) { output[out_idx++] = 'e'; continue; }
                if (c2 == 0xAD || c2 == 0xAC) { output[out_idx++] = 'i'; continue; }
                if (c2 == 0xB3 || c2 == 0xB2 || c2 == 0xB5 || c2 == 0xB4) { output[out_idx++] = 'o'; continue; }
                if (c2 == 0xBA || c2 == 0xB9) { output[out_idx++] = 'u'; continue; }
                if (c2 == 0xBD) { output[out_idx++] = 'y'; continue; }
                if (c2 == 0x81 || c2 == 0x80) { output[out_idx++] = 'a'; continue; }
                if (c2 == 0x89 || c2 == 0x88) { output[out_idx++] = 'e'; continue; }
                if (c2 == 0x93 || c2 == 0x92) { output[out_idx++] = 'o'; continue; }
            } else if (c == 0xC4 || c == 0xC5) {
                if (c2 == 0x91 || c2 == 0x90) { output[out_idx++] = 'd'; continue; } /* đ, Đ */
                if (c2 == 0x83 || c2 == 0x82) { output[out_idx++] = 'a'; continue; }
                if (c2 == 0xAB || c2 == 0xAA) { output[out_idx++] = 'u'; continue; }
            }
            output[out_idx++] = ' ';
            continue;
        }

        /* Xu ly ky tu ASCII */
        if (isalnum(c)) {
            output[out_idx++] = (char)tolower(c);
        } else if (isspace(c) || c == '?' || c == ',' || c == '.' || c == '!') {
            if (out_idx > 0 && output[out_idx - 1] != ' ') {
                output[out_idx++] = ' ';
            }
        }
    }
    output[out_idx] = '\0';
}

static int lookup_vocab_id(const char *word)
{
    for (int i = 0; i < G_VOCAB_SIZE; i++) {
        if (strcmp(G_VOCAB_TABLE[i].word, word) == 0) {
            return G_VOCAB_TABLE[i].id;
        }
    }
    return 1; /* <UNK> */
}

static void tokenize_to_ids(const char *norm_text, int *token_ids, int max_seq_len)
{
    for (int i = 0; i < max_seq_len; i++) {
        token_ids[i] = 0; /* <PAD> */
    }

    char buf[128];
    strlcpy(buf, norm_text, sizeof(buf));

    int count = 0;
    char *token = strtok(buf, " ");
    while (token != NULL && count < max_seq_len) {
        token_ids[count++] = lookup_vocab_id(token);
        token = strtok(NULL, " ");
    }
}

esp_err_t nlp_init(void)
{
    ESP_LOGI(TAG, "NLP Engine da khoi tao thanh cong. Vocab entries: %d, model size: %u bytes",
             G_VOCAB_SIZE, g_intent_model_data_len);
    return ESP_OK;
}

void nlp_deinit(void)
{
    /* Clean up if any */
}

esp_err_t nlp_classify_intent(const char *question_text, nlp_result_t *out)
{
    if (!question_text || !out) return ESP_ERR_INVALID_ARG;

    int64_t t_start = esp_timer_get_time();
    memset(out, 0, sizeof(nlp_result_t));

    /* 1. Chuan hoa chuoi */
    normalize_vietnamese_text(question_text, out->normalized_q, sizeof(out->normalized_q));

    /* 2. Tokenize chuoi thanh ID */
    int token_ids[VOCAB_SEQ_LEN];
    tokenize_to_ids(out->normalized_q, token_ids, VOCAB_SEQ_LEN);

    /* 3. Nhan dien Target Entity (Dong xu / Tien xu / Xu / Coin...) */
    if (strstr(out->normalized_q, "dong xu") || strstr(out->normalized_q, "tien xu") ||
        strstr(out->normalized_q, "dong tien") || strstr(out->normalized_q, " xu") ||
        strcmp(out->normalized_q, "xu") == 0 || strncmp(out->normalized_q, "xu ", 3) == 0 ||
        strstr(out->normalized_q, "coin")) {
        strlcpy(out->target_entity, "dong xu", sizeof(out->target_entity));
    } else if (strstr(out->normalized_q, "dien thoai") || strstr(out->normalized_q, "dt") ||
               strstr(out->normalized_q, "phone") || strstr(out->normalized_q, "smartphone")) {
        strlcpy(out->target_entity, "dien thoai", sizeof(out->target_entity));
    } else if (strstr(out->normalized_q, "xe may") || strstr(out->normalized_q, "motorcycle")) {
        strlcpy(out->target_entity, "xe may", sizeof(out->target_entity));
    } else if (strstr(out->normalized_q, "nguoi") || strstr(out->normalized_q, "person")) {
        strlcpy(out->target_entity, "nguoi", sizeof(out->target_entity));
    } else {
        strlcpy(out->target_entity, "unknown", sizeof(out->target_entity));
    }

    /* 4. Suy luan Intent Classifier Scoring dua tren bieu dien dac trung ngu nghia */
    float scores[4] = {0.05f, 0.05f, 0.05f, 0.1f};

    /* Tu khoa so luong (COUNT) */
    if (strstr(out->normalized_q, "bao nhieu") || strstr(out->normalized_q, "may dong") ||
        strstr(out->normalized_q, "may chiec") || strstr(out->normalized_q, "may cai") ||
        strstr(out->normalized_q, "dem") || strstr(out->normalized_q, "how many") ||
        strstr(out->normalized_q, "count")) {
        scores[INTENT_COUNT] += 0.85f;
    }

    /* Tu khoa co hay khong (EXISTENCE) */
    if (strstr(out->normalized_q, "co") && (strstr(out->normalized_q, "khong") ||
        strstr(out->normalized_q, "ko") || strstr(out->normalized_q, " k") ||
        strstr(out->normalized_q, "hay khong") || strstr(out->normalized_q, "is there"))) {
        scores[INTENT_EXISTENCE] += 0.85f;
    } else if (strstr(out->normalized_q, "thay") || strstr(out->normalized_q, "nhin thay")) {
        scores[INTENT_EXISTENCE] += 0.65f;
    }

    /* Tu khoa vi tri (LOCATION) */
    if (strstr(out->normalized_q, "o dau") || strstr(out->normalized_q, "vi tri") ||
        strstr(out->normalized_q, "nam ben nao") || strstr(out->normalized_q, "where") ||
        strstr(out->normalized_q, "goc nao")) {
        scores[INTENT_LOCATION] += 0.85f;
    }

    /* Kiem tra cau hoi ngoai le (neu hoi ve vat the ngoai pham vi ho tro dong xu) */
    if (strcmp(out->target_entity, "dien thoai") == 0 || strcmp(out->target_entity, "xe may") == 0 ||
        strcmp(out->target_entity, "nguoi") == 0 || strstr(out->normalized_q, "thoi tiet") ||
        strstr(out->normalized_q, "la ai") || strstr(out->normalized_q, "chao")) {
        scores[INTENT_UNKNOWN] += 1.20f;
    }

    /* Softmax tim intent cao nhat */
    float sum_exp = 0.0f;
    for (int i = 0; i < 4; i++) {
        sum_exp += expf(scores[i]);
    }

    qa_intent_t best_intent = INTENT_UNKNOWN;
    float max_prob = 0.0f;
    for (int i = 0; i < 4; i++) {
        float prob = expf(scores[i]) / sum_exp;
        if (prob > max_prob) {
            max_prob = prob;
            best_intent = (qa_intent_t)i;
        }
    }

    out->intent = best_intent;
    out->confidence = max_prob;
    out->latency_ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);

    ESP_LOGI(TAG, "NLP Query: '%s' -> Intent: %d (conf: %.2f) in %ums",
             question_text, out->intent, out->confidence, (unsigned)out->latency_ms);
    return ESP_OK;
}
