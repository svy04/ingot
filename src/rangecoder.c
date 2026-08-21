/* rangecoder.c - 이진 산술 부호화(레인지 코더).
 *
 * 골롬-라이스는 값의 분포가 기하 분포라고 가정한다. 실제 계수는 그렇지 않고,
 * 특히 "0인가 아닌가"의 확률이 자리마다 크게 다르다. 산술 부호화는 그 확률을
 * 그대로 쓰므로 한 비트보다 적은 값도 담을 수 있다.
 *
 * 확률은 11비트 정수(0~2048)이고, 비트를 쓰거나 읽을 때마다 5분의 1씩 당긴다.
 * 인코더와 디코더가 같은 순서로 같은 갱신을 하므로 상태가 어긋나지 않는다.
 *
 * 산술 부호화 자체는 1970년대 후반 공개된 것이고 레인지 코더도 오래된 구조라
 * 특허 걱정이 없다(관찰 — 법률 검토는 아직 아니다).
 */
#include "internal.h"

#define RC_TOP        (1u << 24)
#define RC_PROB_BITS  11
#define RC_PROB_INIT  (1 << (RC_PROB_BITS - 1))   /* 반반에서 시작 */
#define RC_MOVE_BITS  5

void ingot_prob_reset(uint16_t *p, int count)
{
    int i;
    for (i = 0; i < count; i++) p[i] = RC_PROB_INIT;
}

/* ---------------- 쓰기 ---------------- */

void ingot_rc_enc_init(ingot_rc_enc *e, uint8_t *buf, size_t cap)
{
    e->buf = buf;
    e->cap = cap;
    e->pos = 0;
    e->low = 0;
    e->range = 0xFFFFFFFFu;
    /* 처음부터 캐시가 하나 차 있다고 본다. 그래야 첫 번째 밀어내기에서
     * 0 한 바이트가 먼저 나가고, 디코더가 그것을 버린 뒤 네 바이트를 읽는다.
     * 이 한 줄이 안 맞으면 첫 값부터 어긋난다 (2026-08-21 왕복 시험이 잡음). */
    e->cache = 0;
    e->cache_size = 1;
    e->overflow = 0;
    e->bits = 0;
}

static void rc_put_byte(ingot_rc_enc *e, uint8_t b)
{
    if (e->pos < e->cap) e->buf[e->pos++] = b;
    else { e->overflow = 1; e->pos++; }
}

static void rc_shift_low(ingot_rc_enc *e)
{
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        if (e->cache_size) {
            rc_put_byte(e, (uint8_t)(e->cache + (e->low >> 32)));
            while (--e->cache_size)
                rc_put_byte(e, (uint8_t)(0xFF + (e->low >> 32)));
        }
        e->cache = (int)((e->low >> 24) & 0xFF);
    }
    e->cache_size++;
    /* 32비트 안에서 8비트 민다. 밀려 나간 상위 바이트는 이미 위에서 내보냈다.
     * 64비트로 밀면 그 바이트가 남아 다음 호출이 (low >> 32) 를 가짜 자리올림으로
     * 읽는다 — 그러면 첫 비트부터 어긋난다 (2026-08-21 비트 단위 시험이 잡음). */
    e->low = (uint64_t)(((uint32_t)e->low) << 8);
}

void ingot_rc_enc_bit(ingot_rc_enc *e, uint16_t *prob, int bit)
{
    uint32_t bound = (e->range >> RC_PROB_BITS) * (uint32_t)(*prob);

    e->bits++;
    if (!bit) {
        e->range = bound;
        *prob = (uint16_t)(*prob + (((1 << RC_PROB_BITS) - *prob) >> RC_MOVE_BITS));
    } else {
        e->low += bound;
        e->range -= bound;
        *prob = (uint16_t)(*prob - (*prob >> RC_MOVE_BITS));
    }
    while (e->range < RC_TOP) {
        e->range <<= 8;
        rc_shift_low(e);
    }
}

/* 확률 반반으로 그냥 담는다. 모델이 소용없는 자리에 쓴다. */
void ingot_rc_enc_bypass(ingot_rc_enc *e, uint32_t value, int nbits)
{
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        e->bits++;
        e->range >>= 1;
        if ((value >> i) & 1u) e->low += e->range;
        while (e->range < RC_TOP) {
            e->range <<= 8;
            rc_shift_low(e);
        }
    }
}

size_t ingot_rc_enc_finish(ingot_rc_enc *e)
{
    int i;
    for (i = 0; i < 5; i++) rc_shift_low(e);
    return e->overflow ? 0 : e->pos;
}

/* ---------------- 읽기 ---------------- */

void ingot_rc_dec_init(ingot_rc_dec *d, const uint8_t *buf, size_t size)
{
    int i;
    d->buf = buf;
    d->size = size;
    d->pos = 0;
    d->range = 0xFFFFFFFFu;
    d->code = 0;
    d->error = 0;
    for (i = 0; i < 5; i++) {
        d->code = (d->code << 8) |
                  (d->pos < d->size ? d->buf[d->pos++] : 0);
    }
}

static uint8_t rc_next_byte(ingot_rc_dec *d)
{
    if (d->pos < d->size) return d->buf[d->pos++];
    d->error = 1;      /* 입력 끝을 넘었다. 값은 0 으로 채운다 */
    return 0;
}

int ingot_rc_dec_bit(ingot_rc_dec *d, uint16_t *prob)
{
    uint32_t bound = (d->range >> RC_PROB_BITS) * (uint32_t)(*prob);
    int bit;

    if (d->code < bound) {
        d->range = bound;
        *prob = (uint16_t)(*prob + (((1 << RC_PROB_BITS) - *prob) >> RC_MOVE_BITS));
        bit = 0;
    } else {
        d->code -= bound;
        d->range -= bound;
        *prob = (uint16_t)(*prob - (*prob >> RC_MOVE_BITS));
        bit = 1;
    }
    while (d->range < RC_TOP) {
        d->range <<= 8;
        d->code = (d->code << 8) | rc_next_byte(d);
    }
    return bit;
}

uint32_t ingot_rc_dec_bypass(ingot_rc_dec *d, int nbits)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < nbits; i++) {
        d->range >>= 1;
        if (d->code >= d->range) {
            d->code -= d->range;
            v = (v << 1) | 1u;
        } else {
            v <<= 1;
        }
        while (d->range < RC_TOP) {
            d->range <<= 8;
            d->code = (d->code << 8) | rc_next_byte(d);
        }
    }
    return v;
}

/* ---------------- 값 담기 ----------------
 *
 * 값 k 를 이렇게 쪼갠다.
 *   1) k > 0 인가            (모델 있음)
 *   2) k > 1 인가            (모델 있음)
 *   3) k >= 2 이면 k-2 를 지수 골롬으로 담되, 그 비트들은 반반으로 (모델 없음)
 *
 * 대부분의 계수가 0 이나 ±1 이라 앞의 두 판단에서 대부분이 끝난다.
 * 그 자리에 모델을 붙이는 것이 이 방식의 이득이다.
 */
void ingot_rc_put_uint(ingot_rc_enc *e, uint16_t *m, uint32_t k)
{
    uint32_t rest;
    int b;

    ingot_rc_enc_bit(e, &m[0], k > 0);
    if (k == 0) return;
    ingot_rc_enc_bit(e, &m[1], k > 1);
    if (k == 1) return;

    rest = k - 2 + 1;              /* 지수 골롬: n = (k-2)+1 */
    b = 0;
    { uint32_t t = rest; while (t) { b++; t >>= 1; } }
    ingot_rc_enc_bypass(e, 0, b - 1);
    ingot_rc_enc_bypass(e, rest, b);
}

uint32_t ingot_rc_get_uint(ingot_rc_dec *d, uint16_t *m)
{
    uint32_t n;
    int zeros = 0, i;

    if (!ingot_rc_dec_bit(d, &m[0])) return 0;
    if (!ingot_rc_dec_bit(d, &m[1])) return 1;

    while (!ingot_rc_dec_bypass(d, 1)) {
        zeros++;
        if (d->error || zeros > 31) { d->error = 1; return 0; }
    }
    n = 1;
    for (i = 0; i < zeros; i++)
        n = (n << 1) | ingot_rc_dec_bypass(d, 1);
    if (d->error) return 0;
    return n - 1 + 2;
}

void ingot_rc_put_int(ingot_rc_enc *e, uint16_t *m, int v)
{
    uint32_t k = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    ingot_rc_put_uint(e, m, k);
    if (k) ingot_rc_enc_bypass(e, (v < 0) ? 1u : 0u, 1);
}

int ingot_rc_get_int(ingot_rc_dec *d, uint16_t *m)
{
    uint32_t k = ingot_rc_get_uint(d, m);
    if (d->error) return 0;
    if (k == 0) return 0;
    if (k > 32767) { d->error = 1; return 0; }
    return ingot_rc_dec_bypass(d, 1) ? -(int)k : (int)k;
}
