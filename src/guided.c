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
/* guided.c - 자기 유도 필터 두 장을 섞어 복원한다.
 *
 * 앞서 넣었던 복원 필터는 미리 정한 열여섯 후보 가운데 제곱오차가 가장 작은
 * 하나를 **골랐다**. 그러면 흐린 쪽이 늘 이겨서 지각을 판다(-0.40 / -0.83 /
 * +0.28 로 실측). 이 필터는 고르지 않는다 -- 두 장을 만들어 놓고 **섞는
 * 비율을 최소제곱으로 푼다.**
 *
 *   y = x + a*(r1 - x) + b*(r2 - x)
 *
 * r1 은 반지름 1, r2 는 반지름 2 의 자기 유도 필터 출력이다. 「자기 유도」는
 * 안내 그림이 자기 자신이라는 뜻이다. 상자 안의 평균과 분산으로
 *
 *   A = var / (var + e),   B = (1 - A) * mean,   출력 = A*x + B
 *
 * 를 만든다. 분산이 큰 자리(결이 있는 자리)는 A 가 1 에 가까워 원본이
 * 그대로 남고, 평탄한 자리는 평균 쪽으로 당겨진다.
 *
 * r1 과 r2 자체는 좋지 않을 수 있다. 인코더가 a 와 b 를 원본을 보고 풀면
 * 섞은 것이 원본에 훨씬 가까워진다 -- 이것이 원문이 강조하는 자리다.
 * (Valin 외, An Overview of Core Coding Tools in the AV1 Video Codec,
 *  Loop Restoration Filters 절의 dual self-guided filter)
 *
 * a 와 b 는 조각마다 4비트씩, 목차 항목의 위 여덟 비트에 담는다.
 * 잡음 값 e 는 양자화 스텝에서 유도하므로 신호하지 않는다.
 */
#include "internal.h"

#if INGOT_GUIDED

/* 상자 합. 적분 영상을 미리 만들어 O(1) 로 답한다. */
static void gd_integral(const uint8_t *p, int w, int h, int ch,
                        int64_t *s1, int64_t *s2)
{
    int y, x;
    for (x = 0; x <= w; x++) { s1[x] = 0; s2[x] = 0; }
    for (y = 0; y < h; y++) {
        int64_t r1 = 0, r2 = 0;
        s1[(size_t)(y + 1) * (w + 1)] = 0;
        s2[(size_t)(y + 1) * (w + 1)] = 0;
        for (x = 0; x < w; x++) {
            int v = p[((size_t)y * w + x) * 3 + ch];
            r1 += v; r2 += (int64_t)v * v;
            s1[(size_t)(y + 1) * (w + 1) + x + 1] =
                s1[(size_t)y * (w + 1) + x + 1] + r1;
            s2[(size_t)(y + 1) * (w + 1) + x + 1] =
                s2[(size_t)y * (w + 1) + x + 1] + r2;
        }
    }
}

static int64_t gd_box(const int64_t *s, int w, int x0, int y0, int x1, int y1)
{
    return s[(size_t)(y1 + 1) * (w + 1) + (x1 + 1)]
         - s[(size_t)y0 * (w + 1) + (x1 + 1)]
         - s[(size_t)(y1 + 1) * (w + 1) + x0]
         + s[(size_t)y0 * (w + 1) + x0];
}

/* 반지름 r 의 자기 유도 필터를 한 채널에 건다. 결과는 out 에 채널 간격으로
 * 쓴다. 계수는 1/256 눈금 정수로 다룬다. */
static void gd_filter(const uint8_t *src, uint8_t *out, int w, int h, int ch,
                      int r, int e, const int64_t *s1, const int64_t *s2)
{
    int y, x;
    for (y = 0; y < h; y++) {
        int y0 = y - r, y1 = y + r;
        if (y0 < 0) y0 = 0;
        if (y1 >= h) y1 = h - 1;
        for (x = 0; x < w; x++) {
            int x0 = x - r, x1 = x + r;
            int64_t n, sum, sq, mean, var, A, B;
            int v;
            if (x0 < 0) x0 = 0;
            if (x1 >= w) x1 = w - 1;
            n = (int64_t)(x1 - x0 + 1) * (y1 - y0 + 1);
            sum = gd_box(s1, w, x0, y0, x1, y1);
            sq  = gd_box(s2, w, x0, y0, x1, y1);
            mean = sum / n;
            /* 분산. 음수로 떨어지지 않게 자른다. */
            var = sq / n - mean * mean;
            if (var < 0) var = 0;
            A = (var * 256) / (var + e);
            B = ((256 - A) * mean);
            v = (int)((A * src[((size_t)y * w + x) * 3 + ch] + B + 128) >> 8);
            out[((size_t)y * w + x) * 3 + ch] = (uint8_t)ingot_clamp_u8(v);
        }
    }
}

/* 두 장을 만든다. e 는 양자화 스텝에서 유도하므로 신호가 없다. */
void ingot_guided_pair(const uint8_t *rgb, int w, int h, int base,
                       uint8_t *r1, uint8_t *r2, int64_t *work)
{
    int ch;
    int e1 = base * base * INGOT_GUIDED_E1 / 64;
    int e2 = base * base * INGOT_GUIDED_E2 / 64;
    int64_t *s1 = work, *s2 = work + (size_t)(w + 1) * (h + 1);
    if (e1 < 1) e1 = 1;
    if (e2 < 1) e2 = 1;
    for (ch = 0; ch < 3; ch++) {
        gd_integral(rgb, w, h, ch, s1, s2);
        gd_filter(rgb, r1, w, h, ch, 1, e1, s1, s2);
        gd_filter(rgb, r2, w, h, ch, 2, e2, s1, s2);
    }
}

/* 한 조각에 섞기를 건다. wa, wb 는 16 분모다. */
void ingot_guided_apply(uint8_t *rgb, const uint8_t *r1, const uint8_t *r2,
                        int w, int ox, int oy, int rw, int rh,
                        int wa, int wb)
{
    int y, x, ch;
    if (wa == 0 && wb == 0) return;
    for (y = oy; y < oy + rh; y++)
        for (x = ox; x < ox + rw; x++)
            for (ch = 0; ch < 3; ch++) {
                size_t o = ((size_t)y * w + x) * 3 + ch;
                int c = rgb[o];
                int v = c + (wa * ((int)r1[o] - c) + wb * ((int)r2[o] - c)
                             + 8) / 16;
                rgb[o] = (uint8_t)ingot_clamp_u8(v);
            }
}

/* 한 조각에서 원본에 가장 가까워지는 섞기 비율을 최소제곱으로 푼다.
 * 정규방정식을 그대로 풀고 16 분모 정수로 내린다. */
void ingot_guided_solve(const uint8_t *orig, const uint8_t *rgb,
                        const uint8_t *r1, const uint8_t *r2,
                        int w, int ox, int oy, int rw, int rh,
                        int *wa, int *wb)
{
    int64_t uu = 0, uv = 0, vv = 0, ud = 0, vd = 0, det;
    int y, x, ch, a, b;
    for (y = oy; y < oy + rh; y++)
        for (x = ox; x < ox + rw; x++)
            for (ch = 0; ch < 3; ch++) {
                size_t o = ((size_t)y * w + x) * 3 + ch;
                int c = rgb[o];
                int64_t u = (int)r1[o] - c;
                int64_t v = (int)r2[o] - c;
                int64_t d = (int)orig[o] - c;
                uu += u * u; uv += u * v; vv += v * v;
                ud += u * d; vd += v * d;
            }
    det = uu * vv - uv * uv;
    if (det == 0) { *wa = 0; *wb = 0; return; }
    /* 16 분모로 내린다. 0..15 만 담을 수 있으므로 그 밖은 자른다. */
    a = (int)((( ud * vv - vd * uv) * 16) / det);
    b = (int)((( vd * uu - ud * uv) * 16) / det);
    if (a < 0) a = 0;
    if (a > 15) a = 15;
    if (b < 0) b = 0;
    if (b > 15) b = 15;
    *wa = a; *wb = b;
}

#endif  /* INGOT_GUIDED */
