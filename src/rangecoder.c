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

/* ---- 비트 값 표 ----
 *
 * 산술 부호화에서는 결정 하나가 1 비트가 아니다. 확률이 한쪽으로 쏠려 있으면
 * 0.05 비트일 수도 있고, 뜻밖의 값이면 5 비트일 수도 있다. 그래서 결정 개수를
 * 세면 인코더가 값과 비용을 잘못 저울질한다 (2026-08-21 실측: 개수로 세는
 * 동안 꼬리 자르기가 BD-rate 를 24 퍼센트포인트나 망쳤다).
 *
 * 아래 표는 확률마다 -log2(p) 를 1/16 비트 단위로 적어 둔 것이다.
 * 정수 제곱만으로 만들었으므로 어느 기계에서나 같다.
 */
static const uint16_t rc_price[128] = {
     128,  103,   91,   84,   78,   73,   69,   66,   63,   61,   58,   56,
      54,   52,   51,   49,   48,   46,   45,   44,   43,   42,   41,   40,
      39,   38,   37,   36,   35,   34,   34,   33,   32,   31,   31,   30,
      29,   29,   28,   28,   27,   26,   26,   25,   25,   24,   24,   23,
      23,   22,   22,   22,   21,   21,   20,   20,   19,   19,   19,   18,
      18,   17,   17,   17,   16,   16,   16,   15,   15,   15,   14,   14,
      14,   13,   13,   13,   12,   12,   12,   11,   11,   11,   11,   10,
      10,   10,   10,    9,    9,    9,    9,    8,    8,    8,    8,    7,
       7,    7,    7,    6,    6,    6,    6,    5,    5,    5,    5,    5,
       4,    4,    4,    4,    3,    3,    3,    3,    3,    2,    2,    2,
       2,    2,    2,    1,    1,    1,    1,    1
};

/* 확률 prob 인 자리에 bit 를 담을 때 드는 값. 1/16 비트 단위. */
uint32_t ingot_rc_price(uint16_t prob, int bit)
{
    uint32_t p = bit ? ((1u << 11) - prob) : prob;
    return rc_price[p >> 4];
}


/* ---- 시작 확률표 ----
 *
 * 조각마다 확률이 반반에서 출발하면, 조각 하나가 끝날 때쯤에야 자리를 잡고
 * 다음 조각에서 다시 반반으로 돌아간다. 조각을 잘게 나눌수록 이 낭비가 커진다.
 * 그래서 시험 이미지를 여러 품질로 담아 보고, 조각이 끝난 시점의 확률을 모아
 * 평균한 값을 시작값으로 깐다. 
 *
 * 이 표는 규격의 일부다. 디코더도 같은 값에서 출발해야 한다.
 * 다시 만들려면 `python tools/learn_probs.py` 를 쓴다.
 */
static const uint16_t ingot_prob_init[702] = {
      858,   917,   897,   896,   921,   998,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1041,  1048,  1017,  1016,  1018,  1026,  1024,  1024,
     1015,  1045,  1016,  1013,  1019,  1026,  1024,  1024,   981,  1014,   999,  1001,
     1018,  1027,  1024,  1024,   959,   983,   984,   991,  1013,  1029,  1024,  1024,
      875,   887,   887,   894,   917,  1004,  1024,  1024,  1111,  1090,  1013,  1015,
     1018,  1030,  1024,  1024,  1053,  1076,  1013,  1010,  1020,  1032,  1024,  1024,
      976,  1029,  1002,   997,  1017,  1033,  1024,  1024,   910,   960,   963,   974,
     1013,  1045,  1024,  1024,   811,   834,   850,   857,   922,  1053,  1024,  1024,
     1171,  1124,  1021,  1020,  1025,  1026,  1024,  1024,  1099,  1116,  1025,  1016,
     1024,  1030,  1024,  1024,   998,  1101,  1037,  1018,  1028,  1036,  1024,  1024,
      905,  1027,  1017,  1011,  1043,  1054,  1024,  1024,   827,   890,   911,   921,
      995,  1092,  1024,  1024,   491,   784,   578,   489,   658,  1082,  1024,  1024,
      836,  1146,   963,   905,  1009,  1101,  1024,  1024,   613,   947,   870,   824,
      993,  1146,  1024,  1024,   432,   640,   670,   696,   915,  1179,  1024,  1024,
      324,   414,   436,   448,   631,  1102,  1024,  1024,  1429,  1454,  1066,   934,
     1043,  1130,  1024,  1024,  1176,  1441,  1086,   932,  1054,  1156,  1024,  1024,
     1001,  1346,  1109,   936,  1082,  1211,  1024,  1024,   743,  1112,  1030,   920,
     1125,  1267,  1024,  1024,   481,   727,   723,   697,   960,  1301,  1024,  1024,
     1652,  1616,  1158,  1000,  1077,  1130,  1024,  1024,  1361,  1588,  1214,  1037,
     1112,  1161,  1024,  1024,  1142,  1473,  1268,  1083,  1171,  1221,  1024,  1024,
      794,  1165,  1133,  1061,  1229,  1280,  1024,  1024,   605,   804,   851,   864,
     1090,  1286,  1024,  1024,  1720,  1687,  1243,  1066,  1086,  1073,  1024,  1024,
     1456,  1641,  1314,  1107,  1141,  1103,  1024,  1024,  1259,  1515,  1336,  1162,
     1214,  1165,  1024,  1024,   901,  1222,  1183,  1116,  1239,  1230,  1024,  1024,
      756,   914,   947,   961,  1139,  1225,  1024,  1024,   979,  1004,   999,   997,
     1008,  1027,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1026,  1035,  1024,  1023,  1024,  1025,  1024,  1024,  1022,  1033,  1024,  1022,
     1024,  1024,  1024,  1024,  1016,  1026,  1021,  1022,  1024,  1026,  1024,  1024,
     1011,  1020,  1020,  1023,  1026,  1025,  1024,  1024,  1001,  1007,  1007,  1010,
     1016,  1030,  1024,  1024,  1052,  1034,  1024,  1024,  1024,  1024,  1024,  1024,
     1046,  1032,  1022,  1022,  1024,  1024,  1024,  1024,  1048,  1033,  1022,  1022,
     1024,  1025,  1024,  1024,  1034,  1027,  1021,  1018,  1026,  1026,  1024,  1024,
     1005,  1007,  1005,  1008,  1022,  1034,  1024,  1024,  1011,  1033,  1022,  1024,
     1028,  1030,  1024,  1024,  1025,  1034,  1026,  1024,  1024,  1024,  1024,  1024,
     1047,  1032,  1028,  1026,  1024,  1024,  1024,  1024,  1053,  1028,  1026,  1025,
     1024,  1024,  1024,  1024,  1050,  1025,  1024,  1024,  1024,  1024,  1024,  1024,
      965,  1534,  1133,   977,  1061,  1171,  1024,  1024,  1177,  1460,  1121,  1029,
     1056,  1066,  1024,  1024,   958,  1249,  1115,  1031,  1078,  1084,  1024,  1024,
      790,   938,   960,   964,  1074,  1112,  1024,  1024,   797,   854,   863,   881,
      965,  1102,  1024,  1024,  1589,  1565,  1113,  1042,  1044,  1031,  1024,  1024,
     1414,  1541,  1141,  1048,  1052,  1034,  1024,  1024,  1256,  1488,  1187,  1066,
     1064,  1047,  1024,  1024,  1005,  1293,  1162,  1075,  1092,  1061,  1024,  1024,
      858,  1012,  1017,  1007,  1081,  1123,  1024,  1024,  1520,  1396,  1060,  1028,
     1026,  1024,  1024,  1024,  1354,  1332,  1069,  1030,  1026,  1024,  1024,  1024,
     1201,  1260,  1098,  1035,  1028,  1024,  1024,  1024,  1024,  1123,  1085,  1042,
     1032,  1024,  1024,  1024,   991,  1024,  1030,  1026,  1032,  1028,  1024,  1024,
     1349,  1219,  1042,  1026,  1024,  1024,  1024,  1024,  1146,  1164,  1045,  1026,
     1024,  1024,  1024,  1024,  1095,  1108,  1038,  1026,  1024,  1024,  1024,  1024,
     1024,  1040,  1029,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,   877,   836,   819,   838,   852,  1053,  1024,  1024,
      979,   991,   982,   990,   993,  1032,  1024,  1024,   939,   925,   913,   921,
      927,   953,  1024,  1024,  1005,  1010,  1005,  1011,  1014,  1023,  1024,  1024,
      366,   290,   113,   235,   211,   646,  1024,  1024,  1247,  1044,   422,   706,
      664,  1051,  1024,  1024,  1984,  1087,  1939,  1984,   892,  1427,  1055,   952,
      773,  1358,  1314,  1211,  1535,   818
};

void ingot_prob_reset(uint16_t *p, int count)
{
#ifdef INGOT_PROB_LEARN
    /* 표를 학습하는 중에는 표를 안 쓴다. 안 그러면 이미 배운 값 위에서 또
     * 배우게 되어 같은 절차가 같은 표를 못 낸다 (2026-08-22 실측). */
    {
        int j;
        for (j = 0; j < count; j++) p[j] = RC_PROB_INIT;
        return;
    }
#endif
    int i, have = (int)(sizeof(ingot_prob_init) / sizeof(ingot_prob_init[0]));
    for (i = 0; i < count; i++)
        p[i] = (i < have) ? ingot_prob_init[i] : RC_PROB_INIT;
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

#ifdef INGOT_BIT_STATS
#include <stdio.h>
double ingot_bitstat[2][INGOT_BC_COUNT];
int ingot_bitcat = INGOT_BC_ZERO, ingot_bitplane = 0;

/* 시험 인코딩(cap 0)은 안 센다. 같은 계수를 여러 번 담아 보기 때문이다. */
static void bs_add(const ingot_rc_enc *e, uint32_t price)
{
    if (e->cap == 0) return;
    ingot_bitstat[ingot_bitplane][ingot_bitcat] += price / (double)INGOT_BIT_UNIT;
}

void ingot_bitstat_dump(const char *path)
{
    static const char *nm[INGOT_BC_COUNT] = {
        "last", "zero", "flag", "eg_prefix", "eg_suffix", "sign", "mode", "split"
    };
    FILE *f = fopen(path, "w");
    int pl, i;
    if (!f) return;
    for (pl = 0; pl < 2; pl++)
        for (i = 0; i < INGOT_BC_COUNT; i++)
            fprintf(f, "%s %s %.1f\n", pl ? "chroma" : "luma", nm[i],
                    ingot_bitstat[pl][i]);
    fclose(f);
}
#endif

void ingot_rc_enc_bit(ingot_rc_enc *e, uint16_t *prob, int bit)
{
    uint32_t bound = (e->range >> RC_PROB_BITS) * (uint32_t)(*prob);

    e->bits += ingot_rc_price(*prob, bit);
#ifdef INGOT_BIT_STATS
    bs_add(e, ingot_rc_price(*prob, bit));
#endif
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
    e->bits += (uint32_t)nbits * INGOT_BIT_UNIT;   /* 반반이니 정확히 nbits */
#ifdef INGOT_BIT_STATS
    bs_add(e, (uint32_t)nbits * INGOT_BIT_UNIT);
#endif
    for (i = nbits - 1; i >= 0; i--) {
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
/* 깃발을 몇 개까지 둘지. 이 수를 넘는 크기만 지수 골롬으로 넘어간다. */
#ifndef RC_FLAGS
#define RC_FLAGS 3
#endif

/* lo 는 「이 값이 lo 이상인 것을 디코더도 안다」는 뜻이다. 그만큼 깃발을
 * 건너뛴다. 건너뛴 뒤의 깃발은 뜻이 그대로라 같은 모델을 쓴다. */
void ingot_rc_put_uint_from(ingot_rc_enc *e, uint16_t *m, uint32_t k, int lo)
{
    uint32_t rest;
    int b, i;

    for (i = lo; i < RC_FLAGS; i++) {
#ifdef INGOT_BIT_STATS
        int sv = ingot_bitcat;
        if (ingot_bitcat != INGOT_BC_LAST)
            ingot_bitcat = i ? INGOT_BC_FLAG : INGOT_BC_ZERO;
#endif
        ingot_rc_enc_bit(e, &m[i], k > (uint32_t)i);
#ifdef INGOT_BIT_STATS
        ingot_bitcat = sv;
#endif
        if (k == (uint32_t)i) return;
    }

    /* k >= RC_FLAGS. 남은 크기를 지수 골롬으로 담되 접두부에 모델을 붙인다.
     * 접두부는 "자릿수가 몇인가"라 분포가 쏠려 있어 모델이 값을 하고,
     * 접미부는 거의 균등하므로 반반으로 둔다. */
    rest = k - RC_FLAGS + 1;
    b = 0;
    { uint32_t t = rest; while (t) { b++; t >>= 1; } }
#ifdef INGOT_BIT_STATS
    { int sv = ingot_bitcat;
      if (ingot_bitcat != INGOT_BC_LAST) ingot_bitcat = INGOT_BC_EGP;
#endif
    for (i = 0; i < b - 1; i++)
        ingot_rc_enc_bit(e, &m[RC_FLAGS + (i < 2 ? i : 2)], 1);
    ingot_rc_enc_bit(e, &m[RC_FLAGS + ((b - 1) < 2 ? (b - 1) : 2)], 0);
#ifdef INGOT_BIT_STATS
      if (ingot_bitcat != INGOT_BC_LAST) ingot_bitcat = INGOT_BC_EGS;
#endif
    if (b > 1) ingot_rc_enc_bypass(e, rest, b - 1);
#ifdef INGOT_BIT_STATS
      ingot_bitcat = sv; }
#endif
}

void ingot_rc_put_uint(ingot_rc_enc *e, uint16_t *m, uint32_t k)
{
    ingot_rc_put_uint_from(e, m, k, 0);
}

uint32_t ingot_rc_get_uint_from(ingot_rc_dec *d, uint16_t *m, int lo)
{
    uint32_t n;
    int ones = 0, i;

    for (i = lo; i < RC_FLAGS; i++)
        if (!ingot_rc_dec_bit(d, &m[i])) return (uint32_t)i;

    while (ingot_rc_dec_bit(d, &m[RC_FLAGS + (ones < 2 ? ones : 2)])) {
        ones++;
        if (d->error || ones > 31) { d->error = 1; return 0; }
    }
    n = 1;
    for (i = 0; i < ones; i++)
        n = (n << 1) | ingot_rc_dec_bypass(d, 1);
    if (d->error) return 0;
    return n - 1 + RC_FLAGS;
}

uint32_t ingot_rc_get_uint(ingot_rc_dec *d, uint16_t *m)
{
    return ingot_rc_get_uint_from(d, m, 0);
}

void ingot_rc_put_int_from(ingot_rc_enc *e, uint16_t *m, int v, int lo)
{
    uint32_t k = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    ingot_rc_put_uint_from(e, m, k, lo);
#ifdef INGOT_BIT_STATS
    ingot_bitcat = INGOT_BC_SIGN;
#endif
    if (k) ingot_rc_enc_bypass(e, (v < 0) ? 1u : 0u, 1);
#ifdef INGOT_BIT_STATS
    ingot_bitcat = INGOT_BC_ZERO;
#endif
}

int ingot_rc_get_int_from(ingot_rc_dec *d, uint16_t *m, int lo)
{
    uint32_t k = ingot_rc_get_uint_from(d, m, lo);
    if (d->error) return 0;
    if (k == 0) return 0;
    if (k > 32767) { d->error = 1; return 0; }
    return ingot_rc_dec_bypass(d, 1) ? -(int)k : (int)k;
}

int ingot_rc_get_int(ingot_rc_dec *d, uint16_t *m)
{
    return ingot_rc_get_int_from(d, m, 0);
}
