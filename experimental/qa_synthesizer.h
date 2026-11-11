/*
 * qa_synthesizer.h - Bo tong hop cau tra loi thong minh ket hop Model 1 + Model 2
 */
#ifndef QA_SYNTHESIZER_H
#define QA_SYNTHESIZER_H

#include "detector_engine.h"
#include "nlp_engine.h"

typedef struct {
    char short_answer[64];   /* Dap an ngan gon: "Co", "Khong", "3", "O giua" */
    char full_answer[256];   /* Cau tra loi day du bang tieng Viet */
    char position_desc[64];  /* "O ben trai", "O ben phai", "Chinh giua", v.v. */
    int detected_count;
    qa_intent_t intent;
    uint32_t total_latency_ms;
} qa_answer_t;

/**
 * Tong hop cau tra loi dua tren ket qua phat hien cua Model 1 va y dinh cua Model 2
 */
void qa_synthesize_answer(const detector_result_t *det, const nlp_result_t *nlp, qa_answer_t *out_answer);

#endif /* QA_SYNTHESIZER_H */
