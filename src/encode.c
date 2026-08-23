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
/* encode.c - 규격 v0 인코더.
 *
 * 조각을 순서대로 쓰고 목차를 채운다. 조각 사이에 상태가 흐르지 않으므로
 * 나중에 이 반복문을 스레드로 나눠도 결과 비트스트림이 같다.
 *
 * 인코더는 디코더와 **같은 복원 화소**를 본다. 인트라 예측을 넣은 뒤로는
 * 그러지 않으면 다음 블록의 예측이 어긋난다.
 *
 * 16x16 묶음마다 "한 덩어리로 담을지, 8x8 넷으로 나눌지"를 고른다.
 * 고르는 잣대는 실제 비트 수와 왜곡을 함께 본 값이다.
 */
#include <stdlib.h>
#include "internal.h"

void ingot_encode_options_default(ingot_encode_options *opt)
{
    if (!opt) return;
    opt->quality = 20;
    opt->group_log2 = INGOT_GROUP_DEFAULT;
    opt->subsample = 0;
}

/* 평면에서 n x n 블록을 뽑는다. 평면 밖은 가장자리 화소를 복제한다. */
static void fetch_block(const uint8_t *plane, int pw, int ph,
                        int bx, int by, int n, int16_t *out)
{
    int y, x;
    for (y = 0; y < n; y++) {
        int sy = by + y;
        if (sy >= ph) sy = ph - 1;
        if (sy < 0) sy = 0;
        for (x = 0; x < n; x++) {
            int sx = bx + x;
            if (sx >= pw) sx = pw - 1;
            if (sx < 0) sx = 0;
            out[y * n + x] = (int16_t)plane[(size_t)sy * pw + sx];
        }
    }
}

/* 2x2 평균으로 절반 크기로 줄인다. 가장자리에서 짝이 없으면 있는 것만 쓴다. */
static void downsample(const uint8_t *src, int sw, int sh,
                       uint8_t *dst, int dw, int dh)
{
    int y, x;
    for (y = 0; y < dh; y++) {
        int y0 = y * 2, y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            int s = (int)src[(size_t)y0 * sw + x0] + (int)src[(size_t)y0 * sw + x1]
                  + (int)src[(size_t)y1 * sw + x0] + (int)src[(size_t)y1 * sw + x1];
            dst[(size_t)y * dw + x] = (uint8_t)((s + 2) >> 2);
        }
    }
}

/* 복원 블록을 평면에 쓴다. 평면 밖은 버린다. */
static void store_recon(uint8_t *recon, int pw, int ph,
                        int bx, int by, int n, const int16_t *blk)
{
    int y, x;
    for (y = 0; y < n; y++) {
        int dy = by + y;
        if (dy < 0 || dy >= ph) continue;
        for (x = 0; x < n; x++) {
            int dx = bx + x;
            if (dx < 0 || dx >= pw) continue;
            recon[(size_t)dy * pw + dx] =
                (uint8_t)ingot_clamp_u8((int)blk[y * n + x]);
        }
    }
}

/* 왜곡 쪽 눈금. λ 를 정수로 들고 다니면 고품질에서 qbase 가 작아
 * ((3*3*10)/1600) 처럼 0 으로 뭉개진다. 그러면 그 구간에서 율왜곡 판단이
 * 통째로 죽는다. 왜곡을 이 배수만큼 키워 λ 에 소수 자리를 만들어 준다
 * (2026-08-22 발견). */
/* 눈금이 고와지면(BIT_UNIT 이 커지면) 같은 배수만큼 같이 키운다.
 * 그래야 lambda 의 정수 자릿수가 그대로 남는다. */
#define INGOT_RD_SCALE (256 * (INGOT_BIT_UNIT / 16))

/* 양자화까지 한 계수를 담고, 같은 값으로 복원 블록을 만든다.
 * w 가 NULL 이면 비트를 세기만 하고 아무것도 쓰지 않는다(시험 인코딩). */
#if INGOT_SIGNHIDE
/* 계수 하나를 적는 값의 어림자. 1/INGOT_BIT_UNIT 비트 눈금이다.
 * 모델을 안 보는 어림이지만 후보끼리 견주는 데는 충분하다 -- 크기가
 * 커지면 지수 골롬 접두부가 길어지고, 0 이면 깃발 하나로 끝난다. */
static int64_t sh_bits(int v)
{
    int a = v < 0 ? -v : v, b = 0;
    if (a == 0) return 0;
    while (a >> (b + 1)) b++;
    return (int64_t)(2 * b + 3) * INGOT_BIT_UNIT;
}
#endif

static void code_residual(ingot_rc_enc *w, const int16_t *resid, int base, int n,
                          int plane, uint16_t *probs,
                          const int16_t *pred, int16_t *recon, int tx,
                          int64_t lambda)
{
    int16_t coef[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK], z[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK],
            deq[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK], back[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK];
    const uint16_t *zz = ingot_zigzag_of(n);
    int total = n * n, k, last = 0;

#if !INGOT_SIGNHIDE
    (void)lambda;      /* 부호 감추기를 꺼 두면 안 쓴다 */
#endif
    ingot_fdct(resid, n, coef, n, tx);

    for (k = 0; k < total; k++) {
        int idx = zz[k];
        int step = ingot_qstep_at(base, idx, n, plane);
        int level = ingot_quantize(coef[idx], step);
        z[k] = (int16_t)level;
        if (level != 0) last = k + 1;
    }

#if INGOT_SIGNHIDE
    /* 마지막 비영 계수의 부호를 감춘다. 절댓값 합의 홀짝이 그 부호와
     * 안 맞으면 계수 하나를 +-1 옮겨 맞춘다. 옮길 자리는 양자화 전 값에서
     * 덜 멀어지는 쪽으로 고른다. 반드시 last 를 적기 전에 끝낸다. */
    int sh_on = (last >= INGOT_SH_MIN);
    if (sh_on) {
        int sum = 0, j;
        for (j = 0; j < last; j++) sum += z[j] < 0 ? -z[j] : z[j];
        if ((sum & 1) != (z[last - 1] < 0 ? 1 : 0)) {
            int bestk = -1, bestd = 0;
            int64_t bestcost = 0;
            for (j = 0; j < last; j++) {
                int jdx = zz[j];
                int jstep = ingot_qstep_at(base, jdx, n, plane);
                int64_t e0 = (int64_t)coef[jdx] - ingot_dequantize(z[j], jstep);
                int d;
                for (d = -1; d <= 1; d += 2) {
                    int nv = z[j] + d;
                    int64_t e1, cost;
                    /* 마지막 비영 자리를 0 으로 만들면 last 의 뜻이 깨진다. */
                    if (j == last - 1 && nv == 0) continue;
                    if (nv > 32767 || nv < -32768) continue;
                    e1 = (int64_t)coef[jdx]
                       - ingot_dequantize((int16_t)nv, jstep);
                    /* 왜곡만 보면 안 된다. 크기를 키우면 비트가 늘고
                     * 줄이면 준다 -- 처음에 왜곡만 보고 골랐더니 파일이
                     * 오히려 커졌다(품질 63 에서 +3.2%). 계수 하나의 값을
                     * 대충 2*log2(|z|) + 2 로 보고, 0 에서 켜는 것은
                     * 「0 인가」 깃발까지 뒤집으므로 더 세게 친다. */
                    cost = e1 * e1 - e0 * e0
                         + lambda * (sh_bits(nv) - sh_bits(z[j]));
                    if (bestk < 0 || cost < bestcost) {
                        bestk = j; bestd = d; bestcost = cost;
                    }
                }
            }
            /* last >= 4 이므로 마지막 자리 말고도 후보가 남는다. */
            z[bestk] = (int16_t)(z[bestk] + bestd);
        }
    }
#endif
#ifdef INGOT_BIT_STATS
    ingot_bitplane = plane ? 1 : 0;
    ingot_bitcat = INGOT_BC_LAST;
#endif
    ingot_rc_put_uint(w, &probs[ingot_prob_of(ingot_ctx_last_n(plane, n))], (uint32_t)last);
#ifdef INGOT_BIT_STATS
    ingot_bitcat = INGOT_BC_ZERO;
#endif
    {
        int16_t placed[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK];
        for (k = 0; k < total; k++) placed[k] = 0;
        for (k = 0; k < last; k++) {
            int idx = zz[k];
            int lvl = ingot_ctx_level(ingot_nb2d(placed, idx, n));
#ifdef INGOT_BIT_STATS
            if (w && w->cap && k == last - 1) {
                ingot_certain[0]++;
                if (z[k] != 0) ingot_certain[1]++;
            }
#endif
            /* k == last-1 이면 이 계수가 0 이 아닌 것을 디코더도 안다.
             * last 가 「마지막 비영 계수의 다음 자리」이기 때문이다.
             * 그 깃발을 안 적는다 (2026-08-23). */
#if INGOT_SIGNHIDE
            if (sh_on && k == last - 1)
                /* 부호는 안 적는다. 디코더가 홀짝으로 안다. */
                ingot_rc_put_uint_from(w,
                    &probs[ingot_prob_of(ingot_ctx_index(k, n, plane, lvl))],
                    (uint32_t)(z[k] < 0 ? -z[k] : z[k]), 1);
            else
#endif
            ingot_rc_put_int_from(w,
                &probs[ingot_prob_of(ingot_ctx_index(k, n, plane, lvl))],
                z[k], (k == last - 1) ? 1 : 0);
            placed[idx] = z[k];
        }
    }

    for (k = 0; k < total; k++) {
        int idx = zz[k];
        int step = ingot_qstep_at(base, idx, n, plane);
        int v = (k < last) ? ingot_dequantize(z[k], step) : 0;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        deq[idx] = (int16_t)v;
    }
    ingot_idct(deq, back, n, n, tx);
    for (k = 0; k < total; k++)
        recon[k] = (int16_t)ingot_clamp_u8((int)pred[k] + (int)back[k]);
}

/* 복원과 원본의 차이를 값으로 매긴다. 모드·분할을 고를 때 쓴다.
 *
 * 블록의 마지막 열과 마지막 행은 다음 블록의 예측 이웃이 된다. 거기가
 * 틀리면 오차가 이 블록에서 끝나지 않고 옆으로 번지는데, 블록 하나만 보는
 * 자는 그 전파를 못 본다. 그래서 경계 화소를 더 비싸게 센다.
 *
 * INGOT_EDGE 는 16 분모다. 16 이면 예전과 같다. 규격이 아니라 인코더의
 * 판단이므로 이 값을 바꿔도 옛 파일이 그대로 읽힌다. */
#ifndef INGOT_EDGE
#define INGOT_EDGE 32   /* 재서 정했다: 32/16 에서 전 칸이 조금씩 좋아진다 (2026-08-22) */
#endif

static int64_t block_distortion_n(const int16_t *a, const int16_t *b,
                                  int total, int n, int bx, int by)
{
#if !INGOT_EDGE_GRID
    (void)bx; (void)by;
#endif
    int64_t s = 0;
#if INGOT_EDGE == 16
    int i;
    (void)n;
    for (i = 0; i < total; i++) {
        int d = (int)a[i] - (int)b[i];
        s += (int64_t)d * d;
    }
#else
    int y, x;
    (void)total;
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            int d = (int)a[y * n + x] - (int)b[y * n + x];
            int64_t e = (int64_t)d * d;
#if INGOT_EDGE_GRID
            /* 그림 안 16 격자의 끝. 후보를 어떻게 나누든 같은 화소가 걸린다. */
            if (((bx + x) & 15) == 15 || ((by + y) & 15) == 15)
#else
            if (x == n - 1 || y == n - 1)
#endif
                e = (e * INGOT_EDGE) / 16;
            s += e;
        }
    }
#endif
    return s;
}

/* ---- 모드 기호 ----
 * 담는 값을 재는 쪽과 실제로 쓰는 쪽이 한 문법을 보게 함수 둘로 묶어 둔다.
 * 둘이 어긋나면 인코더가 자기 값을 잘못 재고도 왕복은 통과해 안 잡힌다. */
/* 네 모드: 2비트 트리. 문맥은 앞 블록의 모드다. */
static int64_t mode_price(const uint16_t *probs, int pmode, int mode, int n)
{
    int mo = INGOT_PROB_MODE + (pmode & 3) * 3;
    int hi = (mode >> 1) & 1;
    int64_t c;
    (void)n;
    c  = (int64_t)ingot_rc_price(probs[mo + 0], hi);
    c += (int64_t)ingot_rc_price(probs[mo + 1 + hi], mode & 1);
    return c;
}

static void mode_write(ingot_rc_enc *w, uint16_t *probs, int pmode, int mode, int n)
{
    int mo = INGOT_PROB_MODE + (pmode & 3) * 3, hi = (mode >> 1) & 1;
    (void)n;
    ingot_rc_enc_bit(w, &probs[mo + 0], hi);
    ingot_rc_enc_bit(w, &probs[mo + 1 + hi], mode & 1);
}

/* 블록 하나를 네 모드로 시험해 가장 싼 것을 고르고 쓴다.
 * w 가 버리는 통로면 비용만 재는 셈이 된다.
 * cost 를 돌려준다 (왜곡 + lambda * 비트). */
static int64_t code_block(ingot_rc_enc *w, const uint8_t *orig, uint8_t *recon,
                          int pw, int ph, int bx, int by, int gx0, int gy0,
                          int n, int base, int plane, uint16_t *probs,
                          int *pmode, int64_t lambda)
{
    int16_t src[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK],
            pred[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK],
            best_pred[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK],
            resid[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK],
            out[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK];
    ingot_neighbors nb;
    uint16_t trial_p[INGOT_PROB_COUNT];
    ingot_rc_enc trial;
    uint8_t scratch[1];
    int total = n * n, m, k, best = INGOT_PRED_DC;
    int64_t best_cost = -1;
    int64_t rough[INGOT_PRED_COUNT];
    int order[INGOT_PRED_COUNT], t, ti, tries;

    fetch_block(orig, pw, ph, bx, by, n, src);
    ingot_gather_neighbors(recon, pw, ph, bx, by, gx0, gy0, n, &nb);


    /* 1단계: 잔차의 절대합으로 후보 순위를 매긴다. 담아 보지 않으므로 싸다. */
    for (m = 0; m < INGOT_PRED_COUNT; m++) {
        int64_t sad = 0;
        ingot_predict(&nb, m, pred);
        for (k = 0; k < total; k++) {
            int d = (int)src[k] - (int)pred[k];
            sad += (d < 0) ? -d : d;
        }
        rough[m] = sad;
        order[m] = m;
    }
    for (t = 1; t < INGOT_PRED_COUNT; t++) {      /* 삽입 정렬. 넷 또는 여덟이다 */
        int key = order[t];
        for (ti = t - 1; ti >= 0 && rough[order[ti]] > rough[key]; ti--)
            order[ti + 1] = order[ti];
        order[ti + 1] = key;
    }

    /* 2단계: 앞선 몇 개만 실제로 담아 보고 값과 비용으로 고른다. */
    tries = INGOT_MODE_TRIALS;
    if (tries > INGOT_PRED_COUNT) tries = INGOT_PRED_COUNT;
    for (t = 0; t < tries; t++) {
        int64_t bits, dist, cost;
        m = order[t];
        ingot_predict(&nb, m, pred);
        for (k = 0; k < total; k++)
            resid[k] = (int16_t)((int)src[k] - (int)pred[k]);

        /* 비트만 세는 시험 인코딩. 무리 상태는 사본으로 굴린다. */
        memcpy(trial_p, probs, sizeof(trial_p));
        ingot_rc_enc_init(&trial, scratch, 0);     /* 용량 0 = 세기만 한다 */
        code_residual(&trial, resid, base, n, plane, trial_p, pred, out,
                      ingot_tx_of_mode(m), lambda);
        bits = (int64_t)trial.bits;
        /* 모드를 적는 값도 후보마다 다르다. 앞 블록과 같은 모드는 확률 모델이
         * 이미 그쪽으로 기울어 있어 싸고, 드문 모드는 비싸다. 이것을 고른 뒤
         * 상수로 더하면 후보 사이의 차이가 통째로 사라져, 모드 선택이 왜곡만
         * 보고 결정된다 (2026-08-22). */
        bits += mode_price(probs, *pmode, m, n);
        dist = block_distortion_n(src, out, total, n, bx, by);
        cost = dist * INGOT_RD_SCALE + lambda * bits;

        if (best_cost < 0 || cost < best_cost) {
            best_cost = cost;
            best = m;
            for (k = 0; k < total; k++) best_pred[k] = pred[k];
        }
    }

    for (k = 0; k < total; k++)
        resid[k] = (int16_t)((int)src[k] - (int)best_pred[k]);

    /* 고른 모드로 쓴다. 모드 값은 위 시험 루프에서 이미 best_cost 에 들어갔다. */
    {
#ifdef INGOT_BIT_STATS
        ingot_bitplane = plane ? 1 : 0;
        ingot_bitcat = INGOT_BC_MODE;
#endif
        mode_write(w, probs, *pmode, best, n);
#ifdef INGOT_BIT_STATS
        ingot_bitcat = INGOT_BC_ZERO;
#endif
        *pmode = best;
    }
    code_residual(w, resid, base, n, plane, probs, best_pred, out,
                  ingot_tx_of_mode(best), lambda);
    store_recon(recon, pw, ph, bx, by, n, out);
    return best_cost;
}

/* 복원 평면에서 n x n 자리를 떠 두고, 되돌린다. 분할을 견주는 동안
 * 복원 화소가 바뀌므로 후보마다 원래대로 되돌려야 한다. */
static void save_patch(const uint8_t *recon, int pw, int ph,
                       int bx, int by, int n, uint8_t *patch)
{
    int y, x;
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            int dy = by + y, dx = bx + x;
            patch[y * n + x] =
                (dy >= 0 && dy < ph && dx >= 0 && dx < pw)
                ? recon[(size_t)dy * pw + dx] : 0;
        }
}

static void restore_patch(uint8_t *recon, int pw, int ph,
                          int bx, int by, int n, const uint8_t *patch)
{
    int y, x;
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            int dy = by + y, dx = bx + x;
            if (dy >= 0 && dy < ph && dx >= 0 && dx < pw)
                recon[(size_t)dy * pw + dx] = patch[y * n + x];
        }
}

/* 크기 n 덩어리 하나. 가장 작은 크기가 아니면, 통째로 담는 것과 넷으로
 * 나누는 것을 견주어 싼 쪽을 쓴다.
 *
 * 두 후보를 모두 버리는 통로에 써서 비용을 재고, 이긴 쪽만 진짜 통로에 쓴다.
 * 그 사이 무리 상태와 복원 화소는 매번 되돌린다. */
#ifdef INGOT_BIT_STATS
#define BS_SPLIT_ON()  (ingot_bitplane = plane ? 1 : 0, ingot_bitcat = INGOT_BC_SPLIT)
#define BS_SPLIT_OFF() (ingot_bitcat = INGOT_BC_ZERO)
#else
#define BS_SPLIT_ON()  ((void)0)
#define BS_SPLIT_OFF() ((void)0)
#endif

static int64_t code_quad(ingot_rc_enc *w, const uint8_t *orig, uint8_t *recon,
                         int pw, int ph, int bx, int by, int gx0, int gy0,
                         int n, int base, int plane, uint16_t *probs,
                         int *pmode, int64_t lambda)
{
    uint16_t save[INGOT_PROB_COUNT], pwhole[INGOT_PROB_COUNT];
    uint8_t patch[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK];
    uint8_t scratch[1];
    ingot_rc_enc trial;
    int64_t cost_whole, cost_split = 0;
    int i, h = n >> 1, sidx = INGOT_SPLIT_IDX(n);
    int save_pm, try_pm;


    if (n <= INGOT_MIN_BLOCK)
        return code_block(w, orig, recon, pw, ph, bx, by, gx0, gy0,
                          n, base, plane, probs, pmode, lambda);

    save_patch(recon, pw, ph, bx, by, n, patch);
    memcpy(save, probs, sizeof(save));
    save_pm = *pmode;

    /* 후보 1: 통째로 하나 */
    memcpy(pwhole, probs, sizeof(pwhole));
    try_pm = save_pm;
    ingot_rc_enc_init(&trial, scratch, 0);
    cost_whole = code_block(&trial, orig, recon, pw, ph, bx, by, gx0, gy0,
                            n, base, plane, pwhole, &try_pm, lambda)
               + lambda * INGOT_BIT_UNIT;
    restore_patch(recon, pw, ph, bx, by, n, patch);

    /* 통째로 담는 값이 이미 아주 작으면 나눠 봐야 이길 수 없다. 나누면
     * 나눔 비트와 블록 머리말이 넷으로 늘기 때문이다. 평탄한 자리가 많은
     * 이미지에서 이 한 줄이 시험 인코딩을 크게 줄인다. */
#if INGOT_SPLIT_SKIP_AREA
    if (cost_whole < lambda * INGOT_SPLIT_SKIP * INGOT_BIT_UNIT
                     * (n * n) / 256) {
#else
    if (cost_whole < lambda * INGOT_SPLIT_SKIP * INGOT_BIT_UNIT) {
#endif
        memcpy(probs, save, sizeof(save));
        *pmode = save_pm;
        BS_SPLIT_ON(); ingot_rc_enc_bit(w, &probs[INGOT_PROB_SPLIT + sidx], 0); BS_SPLIT_OFF();
        return code_block(w, orig, recon, pw, ph, bx, by, gx0, gy0,
                          n, base, plane, probs, pmode, lambda)
             + lambda * INGOT_BIT_UNIT;
    }

    /* 후보 2: 넷으로. 앞 블록의 복원이 뒤 블록의 이웃이라 순서대로 굴려야 한다. */
    memcpy(probs, save, sizeof(save));
    try_pm = save_pm;
    ingot_rc_enc_init(&trial, scratch, 0);
    for (i = 0; i < 4; i++)
        cost_split += code_quad(&trial, orig, recon, pw, ph,
                                bx + (i & 1) * h, by + (i >> 1) * h,
                                gx0, gy0, h, base, plane, probs, &try_pm, lambda);
    cost_split += lambda * INGOT_BIT_UNIT;
    restore_patch(recon, pw, ph, bx, by, n, patch);
    memcpy(probs, save, sizeof(save));
    *pmode = save_pm;

    /* 이긴 쪽을 진짜로 쓴다. */
    if (cost_whole <= cost_split) {
        BS_SPLIT_ON(); ingot_rc_enc_bit(w, &probs[INGOT_PROB_SPLIT + sidx], 0); BS_SPLIT_OFF();
        return code_block(w, orig, recon, pw, ph, bx, by, gx0, gy0,
                          n, base, plane, probs, pmode, lambda)
             + lambda * INGOT_BIT_UNIT;
    }
    BS_SPLIT_ON(); ingot_rc_enc_bit(w, &probs[INGOT_PROB_SPLIT + sidx], 1); BS_SPLIT_OFF();
    cost_split = lambda * INGOT_BIT_UNIT;
    for (i = 0; i < 4; i++)
        cost_split += code_quad(w, orig, recon, pw, ph,
                                bx + (i & 1) * h, by + (i >> 1) * h,
                                gx0, gy0, h, base, plane, probs, pmode, lambda);
    return cost_split;
}

#ifdef INGOT_PROB_DUMP
/* 확률표를 학습하려고 붙인 수집 장치. 평소 빌드에는 안 들어간다.
 * 조각 하나를 다 담은 시점의 확률을 모아 두었다가 파일로 낸다. */
#include <stdio.h>
static double g_prob_sum[INGOT_PROB_COUNT];
static long   g_prob_n;

static void ingot_prob_collect(const uint16_t *probs)
{
    int i;
    for (i = 0; i < INGOT_PROB_COUNT; i++) g_prob_sum[i] += probs[i];
    g_prob_n++;
}

void ingot_prob_dump(const char *path)
{
    FILE *f = fopen(path, "a");
    int i;
    if (!f) return;
    for (i = 0; i < INGOT_PROB_COUNT; i++)
        fprintf(f, "%d %.3f\n", i, g_prob_n ? g_prob_sum[i] / g_prob_n : 1024.0);
    fclose(f);
}
#endif

static void write_plane_group(ingot_rc_enc *w, const uint8_t *orig, uint8_t *recon,
                              int pw, int ph, int ox, int oy, int gw, int gh,
                              int base, int p, uint16_t *probs, int64_t lambda)
{
    int my, mx, pmode = INGOT_PRED_DC;   /* 조각·평면마다 DC 에서 시작한다 */
    for (my = 0; my < gh; my += INGOT_MAX_BLOCK)
        for (mx = 0; mx < gw; mx += INGOT_MAX_BLOCK)
            code_quad(w, orig, recon, pw, ph, ox + mx, oy + my, ox, oy,
                      INGOT_MAX_BLOCK, base, p, probs, &pmode, lambda);
}

ingot_status ingot_encode(const uint8_t *rgb, int width, int height,
                          const ingot_encode_options *opt,
                          uint8_t **out, size_t *out_size)
{
    ingot_encode_options def;
    uint8_t *full[3] = { NULL, NULL, NULL };
    uint8_t *small[2] = { NULL, NULL };
    uint8_t *recon[3] = { NULL, NULL, NULL };
    const uint8_t *plane[3];
    uint8_t *buf = NULL;
    size_t cap, need, toc_off, data_off = 0, slot = 0;
    uint32_t *glen = NULL;
    uint32_t gx_count, gy_count, group_count, gi;
    int gsize, quality, group_log2, sub, p;
    int cw, ch, qbase;
    int64_t lambda;
    ingot_status st = INGOT_OK;

    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (!rgb || !out || !out_size) return INGOT_ERR_ARG;
    if (width <= 0 || height <= 0) return INGOT_ERR_DIMENSION;
    if (width > INGOT_MAX_DIM || height > INGOT_MAX_DIM) return INGOT_ERR_DIMENSION;
    if ((uint64_t)width * (uint64_t)height > INGOT_MAX_PIXELS) return INGOT_ERR_DIMENSION;

    if (!opt) { ingot_encode_options_default(&def); opt = &def; }
    quality = opt->quality;
    if (quality < 0) quality = 0;
    if (quality > 63) quality = 63;
    group_log2 = opt->group_log2 ? opt->group_log2 : INGOT_GROUP_DEFAULT;
    if (group_log2 < INGOT_GROUP_LOG2_MIN) group_log2 = INGOT_GROUP_LOG2_MIN;
    if (group_log2 > INGOT_GROUP_LOG2_MAX) group_log2 = INGOT_GROUP_LOG2_MAX;
    gsize = 1 << group_log2;
    sub = opt->subsample ? 1 : 0;
    qbase = ingot_qstep(quality);

    /* 왜곡과 비트를 견주는 무게. 양자화가 거칠수록 비트가 비싸진다.
     * 계수는 관행값(0.85 * step^2)의 정수 근사다. */
    /* 비트 하나를 왜곡 얼마와 맞바꿀지 정하는 값. 100 분모라 3 은 0.03 이다.
     *
     * 10 이었는데 다섯 값을 다시 재서 3 으로 내렸다 (2026-08-23). 8/22 에
     * λ 산식의 정밀도 결함을 고친 뒤 한 번도 다시 안 정한 값이었다.
     *   6  -0.87 / -0.55 / -0.25      3  -2.91 / -2.22 / -0.36  <- 골랐다
     *   5  -1.39 / -0.89 / -0.36      2  -4.78 / -3.17 / +2.08
     *   4  -2.03 / -1.40 / -0.47      1  -5.91 / -3.52 / +7.48
     * 낮출수록 인코더가 비트를 더 쓰는 쪽으로 기운다. 제곱 오차는 1 까지
     * 계속 좋아지지만 **지각 지표가 4 에서 바닥을 치고 급격히 무너진다.**
     * 세 지표가 함께 오르는 마지막 지점이 3 이다. */
#ifndef INGOT_LAMBDA
#define INGOT_LAMBDA 3
#endif
    lambda = ((int64_t)qbase * qbase * INGOT_LAMBDA * INGOT_RD_SCALE)
           / (100 * INGOT_BIT_UNIT);
    if (lambda < 1) lambda = 1;

    gx_count = ingot_groups_across(width, gsize);
    gy_count = ingot_groups_across(height, gsize);
    group_count = gx_count * gy_count;

    cw = ingot_chroma_dim(width, sub);
    ch = ingot_chroma_dim(height, sub);

    {
        size_t n = (size_t)width * (size_t)height;
        size_t cn = (size_t)cw * (size_t)ch;
        for (p = 0; p < 3; p++) {
            full[p] = (uint8_t *)malloc(n);
            if (!full[p]) { st = INGOT_ERR_MEMORY; goto done; }
        }
        ingot_rgb_to_ycbcr(rgb, (int)n, full[0], full[1], full[2]);

        plane[0] = full[0];
        if (sub) {
            for (p = 0; p < 2; p++) {
                small[p] = (uint8_t *)malloc(cn);
                if (!small[p]) { st = INGOT_ERR_MEMORY; goto done; }
                downsample(full[p + 1], width, height, small[p], cw, ch);
                plane[p + 1] = small[p];
            }
        } else {
            plane[1] = full[1];
            plane[2] = full[2];
        }

        recon[0] = (uint8_t *)calloc(n, 1);
        recon[1] = (uint8_t *)calloc(cn, 1);
        recon[2] = (uint8_t *)calloc(cn, 1);
        if (!recon[0] || !recon[1] || !recon[2]) { st = INGOT_ERR_MEMORY; goto done; }
    }

    {
        uint32_t blocks = (uint32_t)((gsize / 8) * (gsize / 8)) * 3u;
        need = INGOT_HEADER_SIZE + (size_t)group_count * INGOT_TOC_ENTRY
             + (size_t)group_count * blocks * 24u + 1024u;
    }
    cap = need;

    glen = (uint32_t *)calloc(group_count, sizeof(uint32_t));
    if (!glen) { st = INGOT_ERR_MEMORY; goto done; }

    for (;;) {
        int retry = 0;
        buf = (uint8_t *)malloc(cap);
        if (!buf) { st = INGOT_ERR_MEMORY; goto done; }

        buf[0] = INGOT_SIG0; buf[1] = INGOT_SIG1;
        buf[2] = INGOT_SIG2; buf[3] = INGOT_SIG3;
        buf[4] = INGOT_VERSION;
        /* 비트 2 = 블록 경계 필터. 디코더가 켜고 끄는 신호일 뿐, 인코더는
         * 자기 복원 버퍼를 안 거른다 (예측이 필터 전 화소를 본다). */
        buf[5] = (uint8_t)((sub ? 0x02 : 0x00) | (INGOT_LF ? 0x04 : 0x00));
        buf[6] = (uint8_t)quality;
        buf[7] = (uint8_t)group_log2;
        ingot_put32(buf + 8,  (uint32_t)width);
        ingot_put32(buf + 12, (uint32_t)height);
        ingot_put32(buf + 16, group_count);
        ingot_put32(buf + 20, 0);

        toc_off  = INGOT_HEADER_SIZE;
        data_off = toc_off + (size_t)group_count * INGOT_TOC_ENTRY;

        /* 조각마다 쓸 자리를 미리 똑같이 잘라 둔다. 그래야 여러 조각을
         * 동시에 담아도 서로의 자리를 밟지 않는다. 다 담은 뒤 순서대로
         * 당겨 붙이면서 목차를 채운다. */
        slot = (cap - data_off) / group_count;
        if (slot < 64) { retry = 1; }

        if (!retry) {
            int gsi, gcount = (int)group_count;
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
            for (gsi = 0; gsi < gcount; gsi++) {
                int gx = gsi % (int)gx_count, gy = gsi / (int)gx_count;
                int ox = gx * gsize, oy = gy * gsize;
                int gw = width - ox, gh = height - oy;
                int csize = sub ? (gsize >> 1) : gsize;
                int cox = sub ? (ox >> 1) : ox, coy = sub ? (oy >> 1) : oy;
                int cgw, cgh, pp;
                ingot_rc_enc w;
                uint16_t probs[INGOT_PROB_COUNT];

                if (gw > gsize) gw = gsize;
                if (gh > gsize) gh = gsize;
                cgw = cw - cox; if (cgw > csize) cgw = csize;
                cgh = ch - coy; if (cgh > csize) cgh = csize;

                ingot_rc_enc_init(&w, buf + data_off + (size_t)gsi * slot, slot);
                ingot_prob_reset(probs, INGOT_PROB_COUNT);

                write_plane_group(&w, plane[0], recon[0], width, height,
                                  ox, oy, gw, gh, qbase, 0, probs, lambda);
                for (pp = 1; pp < 3; pp++)
                    write_plane_group(&w, plane[pp], recon[pp], cw, ch,
                                      cox, coy, cgw, cgh, qbase, pp, probs, lambda);

#ifdef INGOT_PROB_DUMP
                ingot_prob_collect(probs);
#endif
                glen[gsi] = (uint32_t)ingot_rc_enc_finish(&w);
                if (w.overflow) retry = 1;    /* 여럿이 써도 값이 같아 무해하다 */
            }
        }

        if (!retry) {
            size_t dst = data_off;
            for (gi = 0; gi < group_count; gi++) {
                size_t src = data_off + (size_t)gi * slot;
                if (dst != src) memmove(buf + dst, buf + src, glen[gi]);
#if INGOT_TOC4
                ingot_put32(buf + toc_off + (size_t)gi * INGOT_TOC_ENTRY,
                            (uint32_t)glen[gi]);
#else
                ingot_put32(buf + toc_off + (size_t)gi * INGOT_TOC_ENTRY,
                            (uint32_t)dst);
                ingot_put32(buf + toc_off + (size_t)gi * INGOT_TOC_ENTRY + 4,
                            glen[gi]);
#endif
                dst += glen[gi];
            }
            data_off = dst;
        }

        if (!retry) break;
        free(buf);
        buf = NULL;
        {
            size_t n = (size_t)width * (size_t)height;
            size_t cn = (size_t)cw * (size_t)ch;
            memset(recon[0], 0, n);
            memset(recon[1], 0, cn);
            memset(recon[2], 0, cn);
        }
        if (cap > (size_t)1 << 31) { st = INGOT_ERR_MEMORY; goto done; }
        cap *= 2;
    }

    /* 목차와 조각 데이터를 훑은 값을 머리말에 남긴다. 디코더가 이것으로
     * 손상을 잡는다. 머리말 자체는 빼고 계산해야 여기에 써 넣을 수 있다. */
    ingot_put32(buf + 20, ingot_hash32(buf + INGOT_HEADER_SIZE,
                                       data_off - INGOT_HEADER_SIZE));

    *out = buf;
    *out_size = data_off;
    buf = NULL;

done:
    free(glen);
    for (p = 0; p < 3; p++) { free(full[p]); free(recon[p]); }
    for (p = 0; p < 2; p++) free(small[p]);
    free(buf);
    return st;
}
