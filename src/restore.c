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

/* (a, b). 처음 표는 절반을 「또렷하게」에 썼는데 여덟 장 어느 품질에서도
 * 한 번도 안 뽑혔고, 대신 「흐리게」 쪽 가장 센 값이 표 끝에 닿았다.
 * 그래서 흐린 쪽을 두텁게 다시 짰다 (2026-08-24). */
static const int8_t ingot_rest_tab[16][2] = {
    {  0,   0 },   /* 0 = 필터 없음 */
    {  0,   6 }, {  0,  12 }, {  0,  18 }, {  0,  24 },
    {  2,  14 }, {  4,  18 }, {  4,  24 },
    {  8,  24 }, {  8,  28 }, { 12,  28 }, { 12,  32 },
    { 16,  32 }, { 20,  36 },
    {  0, -12 }, { -4, -20 }
};

#define REST_SH 7                      /* 128 로 나눈다 */

/* src 의 i0 부터 i1 까지를 걸러 dst 의 같은 자리에 쓴다. 참조는 [0, n)
 * 안에서 자른다 -- 조각 가장자리에서도 이웃 조각의 화소를 그대로 본다.
 * 그래야 조각 경계에 줄이 안 생긴다. */
static void rest_line(const uint8_t *src, int n, int stride,
                      uint8_t *dst, int dstride, int i0, int i1, int a, int b)
{
    int c = 128 - 2 * a - 2 * b;
    int i;
    for (i = i0; i < i1; i++) {
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

/* src 의 한 조각만 걸러 dst 의 같은 자리에 쓴다. 참조는 언제나 src 이므로
 * 조각마다 다른 번호를 골라도 서로의 결과를 안 본다 -- 인코더와 디코더가
 * 같은 값을 얻는다. tmp 는 화소 수 * 3 바이트짜리 일감이다. */
void ingot_restore_region(uint8_t *dst, const uint8_t *src, int w, int h,
                          int ox, int oy, int rw, int rh, int idx, uint8_t *tmp)
{
    int a, b, y, x, ch;
    int y0, y1;
    if (idx <= 0 || idx > 15) return;
    a = ingot_rest_tab[idx][0];
    b = ingot_rest_tab[idx][1];
    /* 세로 걸기가 위아래 두 줄을 보므로 가로 걸기를 그만큼 넓게 한다. */
    y0 = oy - 2; if (y0 < 0) y0 = 0;
    y1 = oy + rh + 2; if (y1 > h) y1 = h;

    for (ch = 0; ch < 3; ch++) {
        for (y = y0; y < y1; y++)
            rest_line(src + ((size_t)y * w) * 3 + ch, w, 3,
                      tmp + ((size_t)y * w) * 3 + ch, 3, ox, ox + rw, a, b);
        for (x = ox; x < ox + rw; x++)
            rest_line(tmp + (size_t)x * 3 + ch, h, (size_t)w * 3,
                      dst + (size_t)x * 3 + ch, (size_t)w * 3,
                      oy, oy + rh, a, b);
    }
}

/* 한 조각에서 원본과 견주어 제곱오차가 가장 작은 번호를 고른다. 0 을
 * 포함하므로 어떤 조각에서도 안 거른 판보다 나빠지지 않는다. */
int ingot_restore_pick(const uint8_t *orig, const uint8_t *dec, int w, int h,
                       int ox, int oy, int rw, int rh,
                       uint8_t *work, uint8_t *tmp)
{
    int idx, best = 0, y, x, ch;
    int64_t bse = -1;
    for (idx = 0; idx < 16; idx++) {
        int64_t se = 0;
        if (idx) {
            ingot_restore_region(work, dec, w, h, ox, oy, rw, rh, idx, tmp);
        }
        for (y = oy; y < oy + rh; y++)
            for (x = ox; x < ox + rw; x++)
                for (ch = 0; ch < 3; ch++) {
                    size_t o = ((size_t)y * w + x) * 3 + ch;
                    int d = (int)(idx ? work[o] : dec[o]) - (int)orig[o];
                    se += (int64_t)d * d;
                }
        if (bse < 0 || se < bse) { bse = se; best = idx; }
    }
    return best;
}
