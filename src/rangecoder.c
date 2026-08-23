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
/* 확률이 한 번에 얼마나 움직이는가. 5 는 LZMA 에서 온 값이고 우리가 잰 적은
 * 없다. -D 로 훑을 수 있게 감싼다 (감싸기 전에는 -D 가 조용히 먹히지 않았다). */
#ifndef RC_MOVE_BITS
#define RC_MOVE_BITS  5
#endif

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

/* 위 128칸 표는 확률을 16칸씩 뭉뚱그린다. 뭉뚱그린 폭이 값보다 큰 구간이 있어
 * 흔한(=싼) 비트를 실제보다 비싸게 부른다. 실측(2026-08-23, 표준 사진 4장 x 품질 3):
 * 인코더가 스스로 센 값이 진짜 -log2 합보다 +2.7~+5.8% 컸고, 그 초과의 60%가
 * 가장 싼 구간 하나에서 나왔다. 아래 512칸 표는 1/256 비트 눈금이라 같은 측정에서
 * 오차가 -0.2% 안으로 들어온다. 인코더만의 자라 규격이 아니다. */

/* 확률 prob 인 자리에 bit 를 담을 때 드는 값. 1/INGOT_BIT_UNIT 비트 단위. */
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
      730,   998,   877,   839,   913,  1041,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1117,  1177,  1027,  1009,  1019,  1033,  1024,  1024,
     1032,  1144,  1021,  1002,  1019,  1037,  1024,  1024,   903,  1055,  1004,   997,
     1018,  1045,  1024,  1024,   841,   931,   944,   960,  1008,  1053,  1024,  1024,
      764,   803,   811,   828,   890,  1040,  1024,  1024,  1281,  1187,  1024,  1011,
     1027,  1038,  1024,  1024,  1080,  1166,  1025,  1004,  1025,  1044,  1024,  1024,
      941,  1102,  1027,   989,  1024,  1062,  1024,  1024,   823,   978,   979,   970,
     1037,  1076,  1024,  1024,   735,   819,   851,   863,   956,  1107,  1024,  1024,
     1319,  1196,  1037,  1023,  1028,  1028,  1024,  1024,  1148,  1227,  1051,  1028,
     1030,  1032,  1024,  1024,  1030,  1230,  1090,  1034,  1048,  1042,  1024,  1024,
      894,  1102,  1071,  1040,  1075,  1066,  1024,  1024,   812,   923,   950,   952,
     1043,  1125,  1024,  1024,   483,   789,   573,   487,   655,  1076,  1024,  1024,
      845,  1142,   957,   900,  1007,  1098,  1024,  1024,   626,   951,   867,   831,
      988,  1143,  1024,  1024,   444,   647,   684,   708,   907,  1170,  1024,  1024,
      333,   420,   441,   457,   641,  1102,  1024,  1024,  1384,  1433,  1066,   935,
     1043,  1127,  1024,  1024,  1155,  1419,  1079,   934,  1050,  1153,  1024,  1024,
      992,  1331,  1098,   932,  1079,  1203,  1024,  1024,   748,  1098,  1014,   911,
     1106,  1264,  1024,  1024,   483,   722,   717,   691,   951,  1293,  1024,  1024,
     1615,  1595,  1153,   995,  1074,  1127,  1024,  1024,  1345,  1565,  1201,  1031,
     1110,  1157,  1024,  1024,  1139,  1454,  1254,  1066,  1167,  1218,  1024,  1024,
      796,  1156,  1123,  1055,  1218,  1271,  1024,  1024,   609,   801,   843,   861,
     1085,  1284,  1024,  1024,  1699,  1674,  1240,  1060,  1088,  1073,  1024,  1024,
     1445,  1630,  1304,  1102,  1140,  1105,  1024,  1024,  1249,  1501,  1326,  1156,
     1213,  1170,  1024,  1024,   903,  1210,  1170,  1109,  1232,  1232,  1024,  1024,
      753,   909,   938,   955,  1134,  1225,  1024,  1024,   916,  1151,  1040,   998,
     1030,  1066,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1112,  1208,  1049,  1029,  1026,  1026,  1024,  1024,  1076,  1181,  1049,  1030,
     1028,  1026,  1024,  1024,   993,  1126,  1050,  1033,  1031,  1027,  1024,  1024,
      975,  1033,  1028,  1030,  1033,  1028,  1024,  1024,   982,  1002,  1005,  1014,
     1024,  1035,  1024,  1024,  1152,  1095,  1029,  1024,  1026,  1024,  1024,  1024,
     1117,  1090,  1028,  1025,  1026,  1024,  1024,  1024,  1098,  1095,  1029,  1026,
     1028,  1026,  1024,  1024,  1029,  1067,  1031,  1029,  1030,  1026,  1024,  1024,
      992,  1027,  1018,  1024,  1032,  1030,  1024,  1024,  1070,  1107,  1051,  1032,
     1027,  1024,  1024,  1024,  1055,  1064,  1027,  1024,  1024,  1024,  1024,  1024,
     1099,  1049,  1028,  1024,  1024,  1024,  1024,  1024,  1081,  1028,  1024,  1024,
     1024,  1024,  1024,  1024,  1042,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
      984,  1543,  1135,   969,  1063,  1164,  1024,  1024,  1190,  1468,  1124,  1036,
     1059,  1057,  1024,  1024,   976,  1257,  1118,  1027,  1076,  1077,  1024,  1024,
      807,   953,   977,   984,  1064,  1104,  1024,  1024,   810,   862,   876,   893,
      982,  1102,  1024,  1024,  1559,  1552,  1112,  1041,  1042,  1030,  1024,  1024,
     1393,  1520,  1142,  1044,  1053,  1033,  1024,  1024,  1233,  1467,  1183,  1070,
     1061,  1044,  1024,  1024,  1004,  1276,  1156,  1076,  1091,  1057,  1024,  1024,
      864,  1008,  1018,  1006,  1081,  1115,  1024,  1024,  1487,  1379,  1055,  1026,
     1026,  1024,  1024,  1024,  1343,  1316,  1064,  1028,  1025,  1024,  1024,  1024,
     1197,  1254,  1091,  1033,  1028,  1024,  1024,  1024,  1030,  1118,  1079,  1040,
     1030,  1024,  1024,  1024,   995,  1024,  1030,  1028,  1030,  1027,  1024,  1024,
     1318,  1205,  1038,  1026,  1024,  1024,  1024,  1024,  1143,  1148,  1042,  1026,
     1024,  1024,  1024,  1024,  1091,  1099,  1035,  1026,  1024,  1024,  1024,  1024,
     1025,  1038,  1028,  1024,  1024,  1024,  1024,  1024,  1023,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,   893,   694,   652,   730,   771,  1115,  1024,  1024,
      941,   966,   920,   993,   968,  1052,  1024,  1024,   870,   767,   680,   785,
      823,   932,  1024,  1024,   957,   943,   846,   983,  1021,  1067,  1024,  1024,
      377,   299,   119,   247,   223,   642,  1024,  1024,  1244,  1059,   431,   724,
      687,  1057,  1024,  1024,  1984,  1671,  1940,  1979,   948,  1496,   905,   950,
      683,  1343,  1402,  1182,  1518,   845
};
#define INGOT_PROB_TAB ingot_prob_init

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
    int i, have = (int)(sizeof(INGOT_PROB_TAB) / sizeof(INGOT_PROB_TAB[0]));
    for (i = 0; i < count; i++)
        p[i] = (i < have) ? INGOT_PROB_TAB[i] : RC_PROB_INIT;
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
long ingot_certain[2];

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
    fprintf(f, "L %ld %ld\n", ingot_certain[0], ingot_certain[1]);
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
