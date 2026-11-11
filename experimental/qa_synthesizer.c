/*
 * qa_synthesizer.c - Trien khai Engine tong hop cau tra loi truc quan tren ESP32-S3
 */
#include "qa_synthesizer.h"
#include <stdio.h>
#include <string.h>

static void determine_bbox_location(const bbox_t *box, char *buf, size_t max_len)
{
    float cx = (box->xmin + box->xmax) * 0.5f;
    float cy = (box->ymin + box->ymax) * 0.5f;

    const char *horiz = (cx < 0.35f) ? "ben trai" : (cx > 0.65f ? "ben phai" : "chinh giua");
    const char *vert  = (cy < 0.35f) ? "phia tren" : (cy > 0.65f ? "phia duoi" : "o giua");

    if (strcmp(horiz, "chinh giua") == 0 && strcmp(vert, "o giua") == 0) {
        snprintf(buf, max_len, "chinh giua khung hinh");
    } else {
        snprintf(buf, max_len, "%s, %s", horiz, vert);
    }
}

void qa_synthesize_answer(const detector_result_t *det, const nlp_result_t *nlp, qa_answer_t *out)
{
    if (!det || !nlp || !out) return;

    memset(out, 0, sizeof(qa_answer_t));
    out->detected_count = det->count;
    out->intent = nlp->intent;
    out->total_latency_ms = det->latency_ms + nlp->latency_ms;

    if (det->count > 0) {
        determine_bbox_location(&det->boxes[0], out->position_desc, sizeof(out->position_desc));
    } else {
        snprintf(out->position_desc, sizeof(out->position_desc), "khong xac dinh");
    }

    switch (nlp->intent) {
    case INTENT_EXISTENCE:
        if (det->count > 0) {
            snprintf(out->short_answer, sizeof(out->short_answer), "Co");
            snprintf(out->full_answer, sizeof(out->full_answer),
                     "Co! Phat hien %d dong xu trong khung hinh (do tin cay: %.0f%%).",
                     det->count, det->boxes[0].conf * 100.0f);
        } else {
            snprintf(out->short_answer, sizeof(out->short_answer), "Khong");
            snprintf(out->full_answer, sizeof(out->full_answer),
                     "Khong co dong xu nao trong khung hinh.");
        }
        break;

    case INTENT_COUNT:
        if (det->count > 0) {
            snprintf(out->short_answer, sizeof(out->short_answer), "%d", det->count);
            snprintf(out->full_answer, sizeof(out->full_answer),
                     "Trong khung hinh co %d dong xu.", det->count);
        } else {
            snprintf(out->short_answer, sizeof(out->short_answer), "0");
            snprintf(out->full_answer, sizeof(out->full_answer),
                     "Co 0 dong (Khong phat hien dong xu nao).");
        }
        break;

    case INTENT_LOCATION:
        if (det->count > 0) {
            strlcpy(out->short_answer, out->position_desc, sizeof(out->short_answer));
            snprintf(out->full_answer, sizeof(out->full_answer),
                     "Dong xu dang nam o %s cua khung hinh.", out->position_desc);
        } else {
            strlcpy(out->short_answer, "Khong tim thay", sizeof(out->short_answer));
            snprintf(out->full_answer, sizeof(out->full_answer),
                     "Khong tim thay dong xu de xac dinh vi tri.");
        }
        break;

    case INTENT_UNKNOWN:
    default:
        snprintf(out->short_answer, sizeof(out->short_answer), "Chua ho tro");
        snprintf(out->full_answer, sizeof(out->full_answer),
                 "Toi chi ho tro tra loi ve su hien dien, so luong va vi tri cua dong xu trong camera.");
        break;
    }
}
