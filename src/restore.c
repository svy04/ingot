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
/* restore.c - 복원 필터. 블록 경계 필터 다음에 오는 두 번째 필터다.
 *
 * 양자화는 화면 전체에 걸쳐 한쪽으로 치우친 자국을 남긴다. 어떤 그림은
 * 고주파가 깎여 흐려지고, 어떤 그림은 계단과 잔물결이 생겨 거칠어진다.
 * 이 필터는 그 치우침을 되돌린다.
 *
 * 계수를 파일에 실어 보내지 않는다. 대신 미리 정한 열다섯 개 중 하나를
 * 고른 번호만 깃발 바이트의 남는 비트 넷에 담는다 -- **비트 대가가 0** 이다.
 * 인코더는 자기 출력을 실제로 풀어서 열다섯 개를 다 걸어 보고 원본과의
 * 제곱오차가 가장 작은 것을 고른다. 0 번은 「필터 없음」이라 어느 그림에서도
 * 손해가 날 수 없다.
 *
 * 필터는 5탭 분리형 대칭이다: [a, b, 128-2a-2b, b, a] / 128. 가로로 한 번,
 * 세로로 한 번 건다. 가장자리 화소는 복제한다.
 */
#include "internal.h"

/* (a, b). 앞쪽 일곱은 흐리게, 뒤쪽 여덟은 또렷하게 만든다. */
static const int8_t ingot_rest_tab[16][2] = {
    {  0,   0 },   /* 0 = 필터 없음 */
    {  0,   8 }, {  0,  16 }, {  0,  24 },
    {  2,  12 }, {  4,  16 }, {  4,  24 }, {  8,  24 },
    {  0,  -8 }, {  0, -16 }, {  0, -24 },
    { -2, -12 }, { -4, -16 }, {  2, -16 },
    {  4, -20 }, { -4, -24 }
};

#define REST_SH 7                      /* 128 로 나눈다 */

static void rest_line(const uint8_t *src, int n, int stride,
                      uint8_t *dst, int dstride, int a, int b)
{
    int c = 128 - 2 * a - 2 * b;
    int i;
    for (i = 0; i < n; i++) {
        int m2 = i - 2, m1 = i - 1, p1 = i + 1, p2 = i + 2;
        int v;
        if (m2 < 0) m2 = 0;
        if (m1 < 0) m1 = 0;
        if (p1 >= n) p1 = n - 1;
        if (p2 >= n) p2 = n - 1;
        v = a * ((int)src[(size_t)m2 * stride] + (int)src[(size_t)p2 * stride])
          + b * ((int)src[(size_t)m1 * stride] + (int)src[(size_t)p1 * stride])
          + c * (int)src[(size_t)i * stride];
        v = (v + (1 << (REST_SH - 1))) >> REST_SH;
        dst[(size_t)i * dstride] = (uint8_t)ingot_clamp_u8(v);
    }
}

/* rgb 를 제자리에서 거른다. tmp 는 화소 수 * 3 바이트짜리 일감이다. */
void ingot_restore_rgb(uint8_t *rgb, int w, int h, int idx, uint8_t *tmp)
{
    int a, b, y, x, ch;
    if (idx <= 0 || idx > 15) return;
    a = ingot_rest_tab[idx][0];
    b = ingot_rest_tab[idx][1];

    for (ch = 0; ch < 3; ch++) {
        /* 가로 */
        for (y = 0; y < h; y++)
            rest_line(rgb + ((size_t)y * w) * 3 + ch, w, 3,
                      tmp + ((size_t)y * w) * 3 + ch, 3, a, b);
        /* 세로 */
        for (x = 0; x < w; x++)
            rest_line(tmp + (size_t)x * 3 + ch, h, (size_t)w * 3,
                      rgb + (size_t)x * 3 + ch, (size_t)w * 3, a, b);
    }
}

/* 원본과 견주어 제곱오차가 가장 작은 번호를 고른다. 0 을 포함하므로
 * 어떤 그림에서도 필터 없는 판보다 나빠지지 않는다. */
int ingot_restore_pick(const uint8_t *orig, const uint8_t *dec, int w, int h,
                       uint8_t *work, uint8_t *tmp)
{
    size_t n3 = (size_t)w * (size_t)h * 3;
    int idx, best = 0;
    int64_t bse = -1;
    for (idx = 0; idx < 16; idx++) {
        size_t i;
        int64_t se = 0;
        memcpy(work, dec, n3);
        if (idx) ingot_restore_rgb(work, w, h, idx, tmp);
        for (i = 0; i < n3; i++) {
            int d = (int)work[i] - (int)orig[i];
            se += (int64_t)d * d;
        }
        if (bse < 0 || se < bse) { bse = se; best = idx; }
    }
    return best;
}
