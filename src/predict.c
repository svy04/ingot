/* ingot — a lossy still-image codec.
 * Copyright 2026 svy04
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* predict.c - 인트라 예측. 인코더와 디코더가 똑같이 쓴다.
 *
 * 이미 복원된 이웃 화소로 블록을 미리 그려 보고, 원본에서 그것을 뺀 나머지만
 * 변환한다. 이웃은 조각의 위·왼쪽 경계를 넘지 않는다 — 그래야 조각이 독립으로 남는다.
 * 평면 오른쪽·아래 밖으로 나가는 자리는 마지막 유효 화소를 복제한다.
 *
 * 인코더가 쓸 수 없는 모드를 골라도 디코더가 같은 조건으로 같은 판단(DC 로 떨어짐)을
 * 하므로 결과가 어긋나지 않는다.
 */
#include "internal.h"

void ingot_gather_neighbors(const uint8_t *recon, int pw, int ph,
                            int bx, int by, int gx0, int gy0, int n,
                            ingot_neighbors *nb)
{
    int i;

    nb->n = n;
    nb->has_top = (by > gy0) && (by >= 1) && (by <= ph);
    nb->has_left = (bx > gx0) && (bx >= 1) && (bx <= pw);
    nb->has_topleft = (nb->has_top && nb->has_left);

    for (i = 0; i < n; i++) {
        int tx = bx + i, ly = by + i;
        if (tx >= pw) tx = pw - 1;
        if (ly >= ph) ly = ph - 1;
        nb->top[i] = nb->has_top
            ? (int)recon[(size_t)(by - 1) * pw + tx] : 128;
        nb->left[i] = nb->has_left
            ? (int)recon[(size_t)ly * pw + (bx - 1)] : 128;
    }
    nb->topleft = nb->has_topleft
        ? (int)recon[(size_t)(by - 1) * pw + (bx - 1)] : 128;
}

/* 실제로 쓸 모드를 정한다. 쓸 수 없으면 DC 로 떨어진다. */
static int effective_mode(const ingot_neighbors *nb, int mode)
{
    if (mode == INGOT_PRED_V && !nb->has_top) return INGOT_PRED_DC;
    if (mode == INGOT_PRED_H && !nb->has_left) return INGOT_PRED_DC;
    if (mode == INGOT_PRED_PLANE && !(nb->has_top && nb->has_left))
        return INGOT_PRED_DC;
    return mode;
}

void ingot_predict(const ingot_neighbors *nb, int mode, int16_t *pred)
{
    int n = nb->n, x, y, i;

    mode = effective_mode(nb, mode);

    if (mode == INGOT_PRED_DC) {
        int sum = 0, cnt = 0, dc;
        if (nb->has_top)  { for (i = 0; i < n; i++) sum += nb->top[i];  cnt += n; }
        if (nb->has_left) { for (i = 0; i < n; i++) sum += nb->left[i]; cnt += n; }
        dc = cnt ? ((sum + (cnt >> 1)) / cnt) : 128;
        for (i = 0; i < n * n; i++) pred[i] = (int16_t)dc;
        return;
    }

    if (mode == INGOT_PRED_V) {
        for (y = 0; y < n; y++)
            for (x = 0; x < n; x++)
                pred[y * n + x] = (int16_t)nb->top[x];
        return;
    }

    if (mode == INGOT_PRED_H) {
        for (y = 0; y < n; y++)
            for (x = 0; x < n; x++)
                pred[y * n + x] = (int16_t)nb->left[y];
        return;
    }

    /* 평면 모드. 왼쪽과 위의 기울기를 이어 붙인다. */
    {
        int a = 0, b = 0, c, gx = 0, gy = 0;
        int half = n >> 1, shift = 0, t = n;
        int t_lo = 0, t_hi = 0, l_lo = 0, l_hi = 0;

        while (t > 1) { shift++; t >>= 1; }      /* log2(n) */

        for (i = 0; i < n; i++) { a += nb->top[i]; b += nb->left[i]; }
        a = (a + (n >> 1)) >> shift;
        b = (b + (n >> 1)) >> shift;
        c = nb->has_topleft ? nb->topleft : ((a + b + 1) >> 1);

        for (i = 0; i < half; i++)     { t_lo += nb->top[i]; l_lo += nb->left[i]; }
        for (i = half; i < n; i++)     { t_hi += nb->top[i]; l_hi += nb->left[i]; }
        gx = t_hi - t_lo;
        gy = l_hi - l_lo;

        for (y = 0; y < n; y++) {
            for (x = 0; x < n; x++) {
                /* (x - (n-1)/2) 를 2배해서 정수로 다룬다. */
                int dx = 2 * x - (n - 1), dy = 2 * y - (n - 1);
                int denom = n * n;      /* 기울기 합의 눈금 */
                int v = a + b - c + ((dx * gx + dy * gy + (denom >> 1)) / denom);
                pred[y * n + x] = (int16_t)ingot_clamp_u8(v);
            }
        }
    }
}
