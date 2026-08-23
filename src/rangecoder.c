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
static const uint16_t ingot_prob_init[975] = {
      512,   926,   796,   689,   613,   626,   782,  1183,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1098,  1364,  1052,   937,
      913,   919,  1012,  1078,   990,  1344,  1035,   918,   873,   901,   993,  1104,
      820,  1149,  1002,   915,   898,   895,   994,  1107,   637,   987,   923,   882,
      849,   842,   981,  1129,   573,   788,   814,   812,   800,   839,   960,  1148,
      544,   670,   702,   731,   754,   769,   913,  1156,   502,   582,   596,   611,
      630,   636,   779,  1106,  1444,  1500,  1161,  1023,   962,   954,  1031,  1107,
     1205,  1477,  1157,  1005,   947,   945,  1042,  1124,  1037,  1364,  1141,  1013,
      950,   941,  1020,  1126,   885,  1260,  1110,  1030,   948,   924,  1037,  1161,
      748,  1088,  1018,   982,   931,   918,  1044,  1174,   643,   914,   924,   897,
      881,   893,  1040,  1194,   586,   727,   774,   804,   783,   798,   966,  1196,
     1608,  1587,  1199,  1057,  1017,  1007,  1036,  1049,  1368,  1597,  1261,  1069,
     1022,   996,  1044,  1065,  1228,  1547,  1263,  1100,  1032,  1007,  1038,  1058,
     1078,  1469,  1305,  1144,  1061,  1018,  1074,  1085,   901,  1271,  1206,  1112,
     1050,  1026,  1098,  1101,   784,  1056,  1074,  1040,  1040,   999,  1106,  1127,
      764,   889,   927,   929,   948,   949,  1079,  1171,   756,   828,   564,   427,
      375,   358,   507,  1003,   925,  1198,   946,   838,   801,   801,   926,  1094,
      759,  1068,   908,   809,   793,   811,   944,  1100,   618,   894,   806,   755,
      726,   714,   868,  1124,   481,   729,   690,   663,   659,   644,   846,  1136,
      388,   567,   562,   555,   560,   574,   770,  1147,   293,   402,   395,   386,
      386,   392,   572,  1068,  1632,  1545,  1151,   951,   906,   879,   988,  1090,
     1265,  1527,  1183,   949,   890,   860,   990,  1134,  1128,  1426,  1191,   977,
      890,   858,   981,  1124,   918,  1283,  1176,  1001,   883,   829,   996,  1163,
      680,  1021,  1020,   936,   882,   858,  1027,  1188,   498,   767,   801,   796,
      788,   783,  1030,  1227,   431,   519,   557,   561,   570,   589,   822,  1222,
     1768,  1680,  1213,  1022,   941,   932,  1013,  1099,  1432,  1628,  1275,  1064,
      964,   922,  1037,  1121,  1253,  1501,  1286,  1069,   972,   931,  1037,  1144,
      986,  1369,  1255,  1094,  1005,   946,  1082,  1180,   741,  1027,  1081,  1044,
      983,   942,  1087,  1204,   620,   806,   875,   905,   901,   903,  1081,  1224,
      614,   680,   710,   743,   754,   774,   966,  1213,  1797,  1711,  1300,  1093,
     1020,  1001,  1048,  1058,  1492,  1681,  1357,  1147,  1047,   997,  1067,  1084,
     1320,  1544,  1348,  1159,  1065,  1034,  1097,  1091,  1069,  1360,  1295,  1157,
     1085,  1036,  1131,  1138,   860,  1117,  1097,  1070,  1047,  1014,  1147,  1178,
      770,   903,   936,   970,   970,   963,  1118,  1198,   763,   835,   854,   872,
      882,   897,  1031,  1145,   824,  1251,  1128,  1034,  1019,   986,  1062,  1157,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1220,  1477,  1159,  1067,  1044,  1027,  1026,  1026,  1179,  1459,  1158,  1061,
     1058,  1024,  1032,  1027,  1028,  1326,  1159,  1063,  1057,  1032,  1028,  1026,
      941,  1222,  1141,  1079,  1057,  1036,  1025,  1030,   901,  1088,  1071,  1066,
     1063,  1035,  1034,  1031,   916,  1012,  1012,  1047,  1037,  1030,  1036,  1031,
      941,   975,   993,  1001,  1009,  1006,  1023,  1038,  1336,  1341,  1116,  1047,
     1036,  1027,  1024,  1024,  1276,  1330,  1127,  1061,  1034,  1027,  1028,  1024,
     1175,  1283,  1120,  1054,  1044,  1028,  1026,  1026,  1141,  1248,  1101,  1074,
     1045,  1027,  1028,  1026,  1040,  1160,  1076,  1061,  1049,  1034,  1030,  1027,
     1005,  1103,  1042,  1031,  1038,  1028,  1036,  1026,   974,  1049,  1030,  1026,
     1018,  1015,  1030,  1030,  1235,  1301,  1105,  1039,  1025,  1024,  1024,  1024,
     1224,  1143,  1031,  1024,  1024,  1024,  1024,  1024,  1224,  1055,  1026,  1024,
     1024,  1024,  1024,  1024,  1240,  1033,  1024,  1024,  1024,  1024,  1024,  1024,
     1171,  1026,  1024,  1024,  1024,  1024,  1024,  1024,  1074,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1030,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1200,  1535,  1125,   924,   851,   818,   963,  1164,  1291,  1654,  1203,  1062,
     1021,  1011,  1037,  1049,  1093,  1470,  1154,  1051,   999,   997,  1033,  1056,
      944,  1295,  1134,  1052,   989,   989,  1045,  1070,   809,  1050,  1024,   978,
      968,   961,  1032,  1087,   747,   909,   924,   917,   926,   934,  1021,  1097,
      758,   836,   863,   863,   864,   882,   955,  1098,  1745,  1672,  1160,  1050,
     1028,  1024,  1024,  1025,  1494,  1665,  1219,  1054,  1027,  1026,  1025,  1027,
     1349,  1562,  1229,  1057,  1027,  1023,  1028,  1026,  1165,  1466,  1244,  1101,
     1035,  1023,  1025,  1029,   952,  1241,  1159,  1083,  1033,  1029,  1030,  1029,
      895,  1032,  1037,  1032,  1027,  1015,  1041,  1040,   895,   956,   954,   972,
      982,   991,  1023,  1056,  1567,  1437,  1049,  1028,  1024,  1024,  1024,  1024,
     1411,  1377,  1049,  1026,  1024,  1024,  1024,  1024,  1282,  1235,  1039,  1027,
     1026,  1024,  1024,  1024,  1128,  1143,  1047,  1028,  1026,  1024,  1024,  1024,
     1029,  1052,  1039,  1026,  1024,  1024,  1024,  1024,  1013,  1021,  1028,  1024,
     1026,  1024,  1024,  1024,  1016,  1019,  1023,  1021,  1024,  1024,  1024,  1024,
     1381,  1200,  1037,  1024,  1024,  1024,  1024,  1024,  1132,  1120,  1034,  1026,
     1024,  1024,  1024,  1024,  1063,  1063,  1034,  1024,  1024,  1024,  1024,  1024,
     1032,  1044,  1030,  1025,  1024,  1024,  1024,  1024,  1020,  1026,  1026,  1026,
     1024,  1024,  1024,  1024,  1022,  1022,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,   655,   453,   294,   527,
      362,   488,   610,  1376,   842,   959,   849,  1244,   690,  1031,   938,  1251,
      578,   372,   229,   359,   291,   332,   370,   872,   945,   847,   663,   920,
      797,   935,   973,  1132,   323,   213,   112,   195,   131,   169,   163,   652,
      763,   660,   392,   624,   489,   566,   607,  1080,   220,   193,   101,   216,
      121,   135,   133,   493,   923,   790,   333,   592,   374,   437,   451,   873,
     1640,  1687,  1613,  1894,  1939,  1242,  1635,   901,   997,   714,  1581,  1589,
     1354,  1658,   958
};
#define INGOT_PROB_TAB ingot_prob_init

/* 표는 지금 문맥 배치에 맞춰 학습된 것이다. 배치를 바꾸는 손잡이
 * (INGOT_NBLEV·INGOT_CTX_BYSIZE 등)를 돌리면 칸 번호가 밀려 표가 엉뚱한
 * 자리에 깔린다. 그 상태로 재면 그 손잡이가 가짜 벌점을 받는다 — 실측으로
 * NBLEV=7 이 3.53%p, BYSIZE=0 이 4.56%p 였고 상당 부분이 표 어긋남이었다.
 * 조용히 틀리느니 안 돌게 막는다 (2026-08-23). */
#if INGOT_PROB_COUNT != 975 && !defined(INGOT_PROB_LEARN)
#error "학습 확률표 975 칸이 지금 문맥 배치와 안 맞는다. tools/learn_probs.py 로 다시 배워 심거나, 견주는 양쪽 모두 -DINGOT_PROB_LEARN 으로 표를 걷고 재라."
#endif

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
#if INGOT_NOLEAD
    /* 첫 밀어내기에서 나가는 0 한 바이트를 안 낸다. 그때는 캐시가 비어 있고
     * 자리올림도 없으므로 0xFF 대기 갈래를 안 깬다. 조각마다 1 바이트다. */
    e->lead = 1;
#endif
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
#if INGOT_NOLEAD
            if (e->lead) e->lead = 0;      /* 첫 0 바이트는 안 낸다 */
            else
#endif
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
#if INGOT_TAILTRIM
    /* 마무리 값을 2^24 배수로 올린다. 올린 양이 2^24 미만이고 정규화 뒤
     * range 는 항상 2^24 이상이므로, 올린 값은 여전히 [low, low+range) 안이다.
     * 즉 디코더가 읽는 답이 안 바뀐다. 대신 아래 세 바이트가 0 이 된다. */
    uint64_t r = e->low & 0xFFFFFFu;
    if (r) e->low += (uint64_t)0x1000000u - r;
#endif
    for (i = 0; i < 5; i++) rc_shift_low(e);
#if INGOT_TAILTRIM
    /* 끝에 남은 0 은 안 낸다. 디코더가 조각 끝을 넘기면 0 을 채워 읽으므로
     * 같은 값이 나온다. 조각마다 서너 바이트다. */
    while (e->pos > 0 && e->buf[e->pos - 1] == 0) e->pos--;
#endif
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
#if INGOT_NOLEAD
    /* 인코더가 첫 0 바이트를 안 냈으므로 네 바이트만 읽는다. */
    for (i = 0; i < 4; i++) {
#else
    for (i = 0; i < 5; i++) {
#endif
        d->code = (d->code << 8) |
                  (d->pos < d->size ? d->buf[d->pos++] : 0);
    }
}

static uint8_t rc_next_byte(ingot_rc_dec *d)
{
    if (d->pos < d->size) return d->buf[d->pos++];
#if INGOT_TAILTRIM
    /* 조각 끝을 넘겨 읽는 것은 정상이다. 인코더가 끝의 0 을 안 냈고,
     * 디코더는 그 자리를 0 으로 채워 읽으므로 답이 같다.
     *
     * 봐주는 길이를 다섯으로 묶었다가 실패했다. 잘리는 0 이 마무리가 낸
     * 다섯 바이트만이 아니라 그 앞의 코딩 바이트까지 포함하기 때문이다.
     * 품질 63 의 Baruch 에서는 129 바이트가 잘렸고, 다섯을 넘긴 순간
     * 멀쩡한 파일이 손상으로 거절됐다. 정상 파일에서 디코더가 인코더가
     * 낸 길이를 넘겨 읽는 일은 없으므로 여기서 세는 것은 값이 없다.
     * 잘린 파일은 해시가 잡는다 (2026-08-23). */
    return 0;
#endif
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
/* 깃발을 몇 개까지 둘지. 이 수를 넘는 크기만 지수 골롬으로 넘어간다.
 *
 * 3 이었다가 5 로 올렸다 (2026-08-23). 8/22 에 3·5·8 을 재서 3 을 골랐는데,
 * 그것은 **확률표가 어긋난 채로 잰 값**이었다 -- 깃발 수를 바꾸면 모델
 * 자리가 밀려(깃발이 0..RC_FLAGS-1, 지수 골롬이 그 뒤) 배운 표가 엉뚱한
 * 자리에 깔린다. 표까지 새로 배워 다시 재니 -0.48 / -0.49 / -0.36% 다.
 * 표 크기는 안 바뀌고 자리의 뜻만 바뀐다. */
#ifndef RC_FLAGS
#define RC_FLAGS 5
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
