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

    /* 위 행 오른쪽 확장. 그 자리가 이미 복원됐을 때만 쓴다.
     *
     * 블록은 최상위 격자 안에서 Z 순서로 돈다. by 가 격자 경계에 있으면
     * 위 행은 한 줄 위의 격자 행이고 그 행은 통째로 끝나 있다 -- 오른쪽으로
     * 얼마든지 봐도 된다. 격자 안쪽이면 같은 격자의 위 절반만 끝나 있으므로
     * 그 격자의 오른쪽 끝까지만 본다. 못 보는 자리는 마지막 값을 되풀이한다.
     * 인코더와 디코더가 같은 규칙으로 판단하므로 어긋나지 않는다. */
    nb->top_n = n;
#if INGOT_TOPRIGHT
    if (nb->has_top) {
        int mb = INGOT_MAX_BLOCK;
        int lim;
        if (((by - gy0) & (mb - 1)) == 0) {
            lim = pw;                       /* 위 격자 행은 다 끝났다 */
        } else {
            int gcol = (bx - gx0) / mb;     /* 이 블록이 든 격자 칸 */
            lim = gx0 + (gcol + 1) * mb;
            if (lim > pw) lim = pw;
        }
        for (i = n; i < 2 * n; i++) {
            int tx = bx + i;
            nb->top[i] = (tx < lim && tx < pw)
                ? (int)recon[(size_t)(by - 1) * pw + tx]
                : nb->top[i - 1];
        }
        nb->top_n = 2 * n;
    } else {
        for (i = n; i < 2 * n; i++) nb->top[i] = 128;
    }
#else
    for (i = n; i < 2 * n; i++) nb->top[i] = nb->top[n - 1];
#endif
    nb->topleft = nb->has_topleft
        ? (int)recon[(size_t)(by - 1) * pw + (bx - 1)] : 128;
#if INGOT_CFL
    nb->luma = NULL;
    nb->luma_stride = 0;
    nb->bx = bx; nb->by = by; nb->pw = pw; nb->ph = ph;
#endif
}

/* 실제로 쓸 모드를 정한다. 쓸 수 없으면 DC 로 떨어진다. */
static int effective_mode(const ingot_neighbors *nb, int mode)
{
    if (mode == INGOT_PRED_V && !nb->has_top) return INGOT_PRED_DC;
    if (mode == INGOT_PRED_H && !nb->has_left) return INGOT_PRED_DC;
#if INGOT_MODES8
    /* 기울기가 양수인 각도는 위 행만 보므로 위가 있으면 쓸 수 있다.
     * 음수인 각도는 모서리를 지나므로 양쪽이 다 있어야 한다. */
    if (mode >= INGOT_PRED_D45) {
        int a = mode - INGOT_PRED_D45;
        if (a >= INGOT_ANGLE_N) a = INGOT_ANGLE_N - 1;
        if (ingot_angle_tab[a][0] > 0)
            return nb->has_top ? mode : INGOT_PRED_DC;
        return (nb->has_top && nb->has_left) ? mode : INGOT_PRED_DC;
    }
#endif
    if (mode >= INGOT_PRED_PLANE && !(nb->has_top && nb->has_left))
        return INGOT_PRED_DC;
    return mode;
}

#if INGOT_MODES8
/* 각도 예측. dx 는 위 행을 훑는 1/32 화소 기울기다. 화소 (y,x) 에서 그
 * 방향으로 거슬러 올라가 참조에 닿는 자리를 두 이웃으로 보간한다.
 *
 * dx 가 양수면 위 행 오른쪽으로 뻗어 위 행만 본다. 음수면 왼쪽으로 뻗어,
 * 위 행을 벗어나는 화소는 같은 직선이 왼쪽 열과 만나는 자리를 본다.
 * inv 는 그 역기울기로, dx 와 짝을 이뤄 미리 넘긴다 -- 나눗셈을 화소마다
 * 하지 않으려는 것이고, 규격에 박히는 값이므로 표로 고정한다. */
static void pred_angle(const ingot_neighbors *nb, int dx, int inv, int16_t *pred)
{
    int n = nb->n, y, x;
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            int idx = (x << 5) + (y + 1) * dx;
            int b, f, p0, p1, v;
            if (idx >= 0) {
                int tn = nb->top_n;
                b = idx >> 5; f = idx & 31;
                p0 = nb->top[b < tn ? b : tn - 1];
                p1 = nb->top[(b + 1) < tn ? (b + 1) : tn - 1];
            } else {
                int idy = (y << 5) + (x + 1) * inv;
                b = idy >> 5; f = idy & 31;
                if (b < 0) { b = 0; f = 0; }
                p0 = nb->left[b < n ? b : n - 1];
                p1 = nb->left[(b + 1) < n ? (b + 1) : n - 1];
            }
            v = (p0 * (32 - f) + p1 * f + 16) >> 5;
            pred[y * n + x] = (int16_t)ingot_clamp_u8(v);
        }
    }
}
#endif


#if INGOT_REFFILT
/* [1,2,1]/4 로 한 줄을 고른다. 양 끝은 그대로 둔다 -- 모서리 값이 흐려지면
 * 블록 경계가 어긋난다. */
static void ref_smooth(int *v, int n, int corner, int has_corner)
{
    int t[INGOT_MAX_BLOCK], i;
    if (n < 3) return;
    t[0] = has_corner ? ((corner + 2 * v[0] + v[1] + 2) >> 2) : v[0];
    for (i = 1; i < n - 1; i++)
        t[i] = (v[i - 1] + 2 * v[i] + v[i + 1] + 2) >> 2;
    t[n - 1] = v[n - 1];
    for (i = 0; i < n; i++) v[i] = t[i];
}
#endif

void ingot_predict(const ingot_neighbors *nb_in, int mode, int16_t *pred)
{
#if INGOT_REFFILT
    ingot_neighbors nbf;
    const ingot_neighbors *nb = nb_in;
#if INGOT_MODES8 && INGOT_REFFILT_ANGLE == 0
    /* 각도 예측은 이웃을 비스듬히 훑으며 이미 두 화소를 섞는다. 거기에
     * 평활화를 또 걸면 결이 두 번 뭉개진다. */
    int skip_f = (mode >= INGOT_PRED_D45);
#else
    int skip_f = 0;
#endif
    if (!skip_f && nb_in->n >= INGOT_REFFILT_MIN) {
        nbf = *nb_in;
        if (nbf.has_top)
            ref_smooth(nbf.top, nbf.n, nbf.topleft, nbf.has_topleft);
        if (nbf.has_left)
            ref_smooth(nbf.left, nbf.n, nbf.topleft, nbf.has_topleft);
        nb = &nbf;
    }
#else
    const ingot_neighbors *nb = nb_in;
#endif
    int n = nb->n, x, y, i;

    mode = effective_mode(nb, mode);


#if INGOT_MODES8
    if (mode >= INGOT_PRED_D45) {
        int a = mode - INGOT_PRED_D45;
        if (a >= INGOT_ANGLE_N) a = INGOT_ANGLE_N - 1;
        pred_angle(nb, ingot_angle_tab[a][0], ingot_angle_tab[a][1], pred);
        return;
    }
#endif

#if INGOT_CFL
    /* 색차이고 휘도를 받았으면 휘도에서 끌어온다. 기울기·절편은 위 행과
     * 왼쪽 열의 (휘도, 색차) 짝에서 최소제곱으로 뽑는다 -- 양쪽이 같은
     * 값을 보므로 신호할 것이 없다.
     *
     * 어느 모드 자리에 붙일지가 문제다. 넷째(평면) 자리에 붙였더니 색차
     * 블록이 DC 를 훨씬 자주 골라서 이 예측이 잘 안 쓰였다. DC 자리로
     * 옮기면 자주 쓰이지만, 이웃이 평평한 자리에서는 DC 가 더 나을 수 있다.
     * 재서 정한다. */
    if (nb->luma && INGOT_CFL_AT_DC == (mode == INGOT_PRED_DC ? 1 : 0)
        && (mode == INGOT_PRED_DC || mode == INGOT_PRED_PLANE)) {
        const uint8_t *L = nb->luma;
        int ls = nb->luma_stride, bx = nb->bx, by = nb->by;
        int cnt = 0;
        int64_t sx = 0, sy = 0, sxx = 0, sxy = 0;
        if (nb->has_top) {
            for (i = 0; i < n; i++) {
                int tx = bx + i;
                int lv, cv;
                if (tx >= nb->pw) tx = nb->pw - 1;
                lv = (int)L[(size_t)(by - 1) * ls + tx];
                cv = nb->top[i];
                sx += lv; sy += cv; sxx += (int64_t)lv * lv;
                sxy += (int64_t)lv * cv; cnt++;
            }
        }
        if (nb->has_left) {
            for (i = 0; i < n; i++) {
                int ly = by + i;
                int lv, cv;
                if (ly >= nb->ph) ly = nb->ph - 1;
                lv = (int)L[(size_t)ly * ls + (bx - 1)];
                cv = nb->left[i];
                sx += lv; sy += cv; sxx += (int64_t)lv * lv;
                sxy += (int64_t)lv * cv; cnt++;
            }
        }
        if (cnt >= 2) {
            int64_t den = (int64_t)cnt * sxx - sx * sx;
            /* 기울기를 1/64 눈금 정수로 들고 다닌다. 분모가 0 이면
             * 이웃 휘도가 평평하다는 뜻이라 기울기를 0 으로 둔다. */
            int64_t a64 = 0;
            int64_t b;
            if (den != 0) {
                a64 = (((int64_t)cnt * sxy - sx * sy) * 64) / den;
                if (a64 >  256) a64 =  256;      /* 4.0 배로 묶는다 */
                if (a64 < -256) a64 = -256;
            }
            b = (sy * 64 - a64 * sx) / cnt;
            for (y = 0; y < n; y++) {
                int sy2 = by + y;
                if (sy2 >= nb->ph) sy2 = nb->ph - 1;
                for (x = 0; x < n; x++) {
                    int sx2 = bx + x;
                    int v;
                    if (sx2 >= nb->pw) sx2 = nb->pw - 1;
                    v = (int)((a64 * (int64_t)L[(size_t)sy2 * ls + sx2]
                               + b + 32) >> 6);
                    pred[y * n + x] = (int16_t)ingot_clamp_u8(v);
                }
            }
            return;
        }
    }
#endif

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
#if INGOT_EDGEFIX
        /* 왼쪽 몇 열을 왼쪽 이웃 쪽으로 당긴다. 멀어질수록 덜 당긴다.
         * 위-왼쪽 모서리를 기준으로 잰 차이를 쓴다. */
        if (nb->has_top && nb->has_left && nb->has_topleft) {
            int lim = n < 4 ? n : 4;
            for (y = 0; y < n; y++) {
                int d = nb->left[y] - nb->topleft;
                for (x = 0; x < lim; x++)
                    pred[y * n + x] = (int16_t)ingot_clamp_u8(
                        pred[y * n + x] + (d >> (x + 1)));
            }
        }
#endif
        return;
    }

    if (mode == INGOT_PRED_H) {
        for (y = 0; y < n; y++)
            for (x = 0; x < n; x++)
                pred[y * n + x] = (int16_t)nb->left[y];
#if INGOT_EDGEFIX
        if (nb->has_top && nb->has_left && nb->has_topleft) {
            int lim = n < 4 ? n : 4;
            for (x = 0; x < n; x++) {
                int d = nb->top[x] - nb->topleft;
                for (y = 0; y < lim; y++)
                    pred[y * n + x] = (int16_t)ingot_clamp_u8(
                        pred[y * n + x] + (d >> (y + 1)));
            }
        }
#endif
        return;
    }


#if INGOT_SMOOTH
    /* 매끄러운 보간. 화소마다 위·왼쪽·오른쪽 끝·아래 끝을 거리로 섞는다.
     * 위에서 멀어질수록 아래 끝 값을, 왼쪽에서 멀어질수록 오른쪽 끝 값을
     * 더 크게 친다. 아래 왼쪽은 아직 안 담았으므로 왼쪽 열의 마지막 값을,
     * 위 오른쪽도 없으므로 위 행의 마지막 값을 그 자리로 쓴다. */
    {
        int tr = nb->has_top ? nb->top[n - 1] : 128;     /* 위 행의 오른쪽 끝 */
        int bl = nb->has_left ? nb->left[n - 1] : 128;   /* 왼쪽 열의 아래 끝 */
        for (y = 0; y < n; y++) {
            for (x = 0; x < n; x++) {
                int wv = n - 1 - y, wh = n - 1 - x;
                int v = wv * nb->top[x] + (y + 1) * bl
                      + wh * nb->left[y] + (x + 1) * tr;
                pred[y * n + x] = (int16_t)ingot_clamp_u8((v + n) / (2 * n));
            }
        }
        return;
    }
#endif

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
