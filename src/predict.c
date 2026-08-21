/* predict.c - 인트라 예측. 인코더와 디코더가 똑같이 쓴다.
 *
 * 이미 복원된 이웃 화소로 블록을 미리 그려 보고, 원본에서 그것을 뺀 나머지만
 * 변환한다. 이웃은 조각과 평면의 경계를 넘지 않는다 — 그래야 조각이 독립으로 남는다.
 *
 * 인코더가 쓸 수 없는 모드를 골라도 디코더가 같은 조건으로 같은 판단(DC 로 떨어짐)을
 * 하므로 결과가 어긋나지 않는다.
 */
#include "internal.h"

void ingot_gather_neighbors(const uint8_t *recon, int pw, int ph,
                            int bx, int by, int gx0, int gy0,
                            ingot_neighbors *n)
{
    int i;

    n->has_top = (by > gy0) && (by <= ph) && (by >= 1);
    n->has_left = (bx > gx0) && (bx <= pw) && (bx >= 1);
    n->has_topleft = (n->has_top && n->has_left);

    for (i = 0; i < 8; i++) {
        /* 평면 밖으로 나가면 마지막 유효 화소를 복제한다. 인코더와 디코더가
         * 같은 규칙을 써야 하므로 이것도 규격의 일부다. */
        int tx = bx + i, ly = by + i;
        if (tx >= pw) tx = pw - 1;
        if (ly >= ph) ly = ph - 1;
        n->top[i] = n->has_top
            ? (int)recon[(size_t)(by - 1) * pw + tx] : 128;
        n->left[i] = n->has_left
            ? (int)recon[(size_t)ly * pw + (bx - 1)] : 128;
    }
    n->topleft = n->has_topleft
        ? (int)recon[(size_t)(by - 1) * pw + (bx - 1)] : 128;
}

/* 실제로 쓸 모드를 정한다. 쓸 수 없으면 DC 로 떨어진다. */
static int effective_mode(const ingot_neighbors *n, int mode)
{
    if (mode == INGOT_PRED_V && !n->has_top) return INGOT_PRED_DC;
    if (mode == INGOT_PRED_H && !n->has_left) return INGOT_PRED_DC;
    if (mode == INGOT_PRED_PLANE && !(n->has_top && n->has_left))
        return INGOT_PRED_DC;
    return mode;
}

void ingot_predict(const ingot_neighbors *n, int mode, int16_t *pred)
{
    int x, y, i;

    mode = effective_mode(n, mode);

    if (mode == INGOT_PRED_DC) {
        int sum = 0, cnt = 0, dc;
        if (n->has_top)  { for (i = 0; i < 8; i++) sum += n->top[i];  cnt += 8; }
        if (n->has_left) { for (i = 0; i < 8; i++) sum += n->left[i]; cnt += 8; }
        dc = cnt ? ((sum + (cnt >> 1)) / cnt) : 128;
        for (i = 0; i < 64; i++) pred[i] = (int16_t)dc;
        return;
    }

    if (mode == INGOT_PRED_V) {
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                pred[y * 8 + x] = (int16_t)n->top[x];
        return;
    }

    if (mode == INGOT_PRED_H) {
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                pred[y * 8 + x] = (int16_t)n->left[y];
        return;
    }

    /* 평면 모드. 왼쪽과 위의 기울기를 이어 붙인다. */
    {
        int a = 0, b = 0, c, gx, gy;
        int t_lo = 0, t_hi = 0, l_lo = 0, l_hi = 0;

        for (i = 0; i < 8; i++) { a += n->top[i]; b += n->left[i]; }
        a = (a + 4) >> 3;
        b = (b + 4) >> 3;
        c = n->has_topleft ? n->topleft : ((a + b + 1) >> 1);

        for (i = 0; i < 4; i++) { t_lo += n->top[i]; l_lo += n->left[i]; }
        for (i = 4; i < 8; i++) { t_hi += n->top[i]; l_hi += n->left[i]; }
        gx = (t_hi - t_lo);       /* 16 배 기울기 */
        gy = (l_hi - l_lo);

        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                /* (x - 3.5) 와 (y - 3.5) 를 2 배해서 정수로 다룬다. */
                int dx = 2 * x - 7, dy = 2 * y - 7;
                int v = a + b - c + ((dx * gx + dy * gy + 32) >> 6);
                pred[y * 8 + x] = (int16_t)ingot_clamp_u8(v);
            }
        }
    }
}
