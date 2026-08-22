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
/* bitio.c - 비트 단위 읽기와 쓰기, 그리고 지수 골롬 부호.
 *
 * 조각마다 새로 초기화한다. 조각 사이에 비트 상태가 흐르지 않는 것이
 * 병렬성 축의 뿌리다 (SPEC.md 「조각 데이터」 절).
 *
 * 읽기 쪽은 적대적 입력을 전제한다. 입력 끝을 넘어가면 error 를 세우고
 * 0 을 돌려준다. 절대 abort 하지 않는다.
 */
#include "internal.h"

/* ---------------- 쓰기 ---------------- */

void ingot_bw_init(ingot_bw *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = 0;
    w->acc = 0;
    w->nbits = 0;
    w->overflow = 0;
    w->written_bits = 0;
}

static void ingot_bw_flush_byte(ingot_bw *w)
{
    if (w->pos < w->cap)
        w->buf[w->pos++] = (uint8_t)(w->acc >> 24);
    else
        w->overflow = 1;
    w->acc <<= 8;
    w->nbits -= 8;
}

void ingot_bw_put(ingot_bw *w, uint32_t value, int nbits)
{
    int i;
    if (nbits <= 0) return;
    w->written_bits += (uint32_t)nbits;
    for (i = nbits - 1; i >= 0; i--) {
        uint32_t bit = (value >> i) & 1u;
        w->acc |= bit << (31 - w->nbits);
        w->nbits++;
        if (w->nbits == 8)
            ingot_bw_flush_byte(w);
    }
}

void ingot_bw_put_ue(ingot_bw *w, uint32_t k)
{
    uint32_t n = k + 1;
    int b = 0;
    uint32_t t = n;
    while (t) { b++; t >>= 1; }        /* n 의 이진 자릿수 */
    ingot_bw_put(w, 0, b - 1);            /* 앞의 0 들 */
    ingot_bw_put(w, n, b);                /* n 자체 (맨 앞 비트가 1) */
}

void ingot_bw_put_se(ingot_bw *w, int v)
{
    uint32_t k = (v <= 0) ? (uint32_t)(-2 * (int64_t)v)
                          : (uint32_t)(2 * (int64_t)v - 1);
    ingot_bw_put_ue(w, k);
}

/* 부호 있는 값을 음이 아닌 정수로 접는다. 0->0, 1->1, -1->2, 2->3, -2->4 */
static uint32_t ingot_fold(int v)
{
    return (v <= 0) ? (uint32_t)(-2 * (int64_t)v)
                    : (uint32_t)(2 * (int64_t)v - 1);
}

static int ingot_unfold(uint32_t k)
{
    return (k & 1u) ? (int)((k + 1) >> 1) : -(int)(k >> 1);
}

void ingot_bw_put_rice_u(ingot_bw *w, uint32_t k, ingot_ctx *c)
{
    int m = ingot_rice_param(c);
    uint32_t q = k >> m;

    if (q < INGOT_ESCAPE_Q) {
        int i;
        for (i = 0; i < (int)q; i++) ingot_bw_put(w, 0, 1);
        ingot_bw_put(w, 1, 1);
        if (m) ingot_bw_put(w, k & ((1u << m) - 1u), m);
    } else {
        int i;
        for (i = 0; i < INGOT_ESCAPE_Q; i++) ingot_bw_put(w, 0, 1);
        ingot_bw_put_ue(w, k);
    }
    ingot_ctx_update(c, k);
}

uint32_t ingot_br_get_rice_u(ingot_br *r, ingot_ctx *c)
{
    int m = ingot_rice_param(c);
    uint32_t q = 0, k;

    while (q < INGOT_ESCAPE_Q) {
        int bit = ingot_br_bit_pub(r);
        if (r->error) return 0;
        if (bit) break;
        q++;
    }
    if (q >= INGOT_ESCAPE_Q) {
        k = ingot_br_get_ue(r);
    } else {
        uint32_t rem = m ? ingot_br_get(r, m) : 0;
        k = (q << m) | rem;
    }
    if (r->error) return 0;
    ingot_ctx_update(c, k);
    return k;
}

void ingot_bw_put_rice(ingot_bw *w, int v, ingot_ctx *c)
{
    uint32_t k = ingot_fold(v);
    int m = ingot_rice_param(c);
    uint32_t q = k >> m;

    if (q < INGOT_ESCAPE_Q) {
        int i;
        for (i = 0; i < (int)q; i++) ingot_bw_put(w, 0, 1);
        ingot_bw_put(w, 1, 1);
        if (m) ingot_bw_put(w, k & ((1u << m) - 1u), m);
    } else {
        int i;
        for (i = 0; i < INGOT_ESCAPE_Q; i++) ingot_bw_put(w, 0, 1);
        ingot_bw_put_ue(w, k);
    }
    ingot_ctx_update(c, k);
}

int ingot_br_get_rice(ingot_br *r, ingot_ctx *c)
{
    int m = ingot_rice_param(c);
    uint32_t q = 0, k;

    while (q < INGOT_ESCAPE_Q) {
        int bit = ingot_br_bit_pub(r);
        if (r->error) return 0;
        if (bit) break;
        q++;
    }
    if (q >= INGOT_ESCAPE_Q) {
        k = ingot_br_get_ue(r);
    } else {
        uint32_t rem = m ? ingot_br_get(r, m) : 0;
        k = (q << m) | rem;
    }
    if (r->error) return 0;
    ingot_ctx_update(c, k);
    return ingot_unfold(k);
}

size_t ingot_bw_finish(ingot_bw *w)
{
    while (w->nbits > 0)
        ingot_bw_flush_byte(w);
    return w->overflow ? 0 : w->pos;
}

/* ---------------- 읽기 ---------------- */

void ingot_br_init(ingot_br *r, const uint8_t *buf, size_t size)
{
    r->buf = buf;
    r->size = size;
    r->pos = 0;
    r->acc = 0;
    r->nbits = 0;
    r->error = 0;
}

int ingot_br_bit_pub(ingot_br *r);

static int ingot_br_bit(ingot_br *r)
{
    if (r->nbits == 0) {
        if (r->pos >= r->size) { r->error = 1; return 0; }
        r->acc = r->buf[r->pos++];
        r->nbits = 8;
    }
    r->nbits--;
    return (int)((r->acc >> r->nbits) & 1u);
}

int ingot_br_bit_pub(ingot_br *r)
{
    return ingot_br_bit(r);
}

uint32_t ingot_br_get(ingot_br *r, int nbits)
{
    uint32_t v = 0;
    int i;
    if (nbits <= 0 || nbits > 32) { r->error = 1; return 0; }
    for (i = 0; i < nbits; i++)
        v = (v << 1) | (uint32_t)ingot_br_bit(r);
    return v;
}

uint32_t ingot_br_get_ue(ingot_br *r)
{
    int zeros = 0;
    uint32_t n;
    int i;

    while (!ingot_br_bit(r)) {
        zeros++;
        if (r->error) return 0;
        if (zeros > 31) { r->error = 1; return 0; }  /* 망가진 입력 방어 */
    }
    if (r->error) return 0;

    n = 1;                                  /* 방금 읽은 1 */
    for (i = 0; i < zeros; i++)
        n = (n << 1) | (uint32_t)ingot_br_bit(r);
    if (r->error) return 0;
    return n - 1;
}

int ingot_br_get_se(ingot_br *r)
{
    uint32_t k = ingot_br_get_ue(r);
    if (r->error) return 0;
    if (k & 1u) return (int)((k + 1) >> 1);      /* 홀수 -> 양수 */
    return -(int)(k >> 1);                        /* 짝수 -> 0 또는 음수 */
}
