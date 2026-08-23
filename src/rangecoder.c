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
      509,   912,   787,   679,   868,  1242,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1091,  1337,  1039,   947,
     1050,  1145,  1024,  1024,   974,  1322,  1029,   918,  1048,  1170,  1024,  1024,
      815,  1146,  1004,   917,  1060,  1169,  1024,  1024,   653,   982,   918,   890,
     1040,  1204,  1024,  1024,   568,   795,   808,   806,  1030,  1210,  1024,  1024,
      513,   659,   683,   720,   958,  1213,  1024,  1024,   495,   575,   592,   598,
      764,  1144,  1024,  1024,  1450,  1510,  1162,  1024,  1112,  1160,  1024,  1024,
     1199,  1477,  1161,  1015,  1127,  1196,  1024,  1024,  1033,  1366,  1137,  1024,
     1124,  1193,  1024,  1024,   885,  1260,  1114,  1035,  1153,  1238,  1024,  1024,
      742,  1084,  1021,   975,  1139,  1257,  1024,  1024,   635,   926,   934,   913,
     1122,  1273,  1024,  1024,   571,   721,   769,   810,   981,  1256,  1024,  1024,
     1599,  1587,  1198,  1050,  1102,  1090,  1024,  1024,  1362,  1597,  1266,  1083,
     1115,  1111,  1024,  1024,  1225,  1552,  1265,  1102,  1131,  1110,  1024,  1024,
     1082,  1479,  1312,  1148,  1191,  1149,  1024,  1024,   892,  1276,  1210,  1117,
     1200,  1188,  1024,  1024,   788,  1046,  1069,  1048,  1193,  1210,  1024,  1024,
      760,   888,   924,   926,  1110,  1227,  1024,  1024,   765,   817,   566,   439,
      605,  1055,  1024,  1024,   918,  1199,   946,   849,   991,  1160,  1024,  1024,
      771,  1076,   907,   820,   999,  1177,  1024,  1024,   627,   911,   812,   773,
      979,  1195,  1024,  1024,   479,   723,   693,   658,   924,  1226,  1024,  1024,
      387,   567,   565,   549,   824,  1227,  1024,  1024,   294,   397,   395,   382,
      588,  1107,  1024,  1024,  1641,  1546,  1146,   954,  1062,  1157,  1024,  1024,
     1274,  1526,  1190,   956,  1096,  1207,  1024,  1024,  1134,  1424,  1186,   976,
     1093,  1194,  1024,  1024,   923,  1286,  1176,  1003,  1134,  1264,  1024,  1024,
      674,  1022,  1019,   933,  1191,  1317,  1024,  1024,   498,   766,   799,   796,
     1115,  1360,  1024,  1024,   430,   529,   552,   564,   845,  1287,  1024,  1024,
     1769,  1682,  1213,  1024,  1098,  1154,  1024,  1024,  1441,  1632,  1274,  1074,
     1125,  1195,  1024,  1024,  1255,  1503,  1288,  1073,  1161,  1206,  1024,  1024,
      990,  1367,  1257,  1099,  1226,  1265,  1024,  1024,   733,  1031,  1082,  1050,
     1223,  1289,  1024,  1024,   623,   806,   875,   905,  1165,  1306,  1024,  1024,
      612,   678,   705,   739,   946,  1259,  1024,  1024,  1800,  1712,  1298,  1092,
     1125,  1104,  1024,  1024,  1490,  1681,  1355,  1144,  1173,  1150,  1024,  1024,
     1326,  1546,  1350,  1155,  1210,  1174,  1024,  1024,  1066,  1366,  1298,  1160,
     1246,  1238,  1024,  1024,   860,  1126,  1096,  1069,  1224,  1271,  1024,  1024,
      767,   902,   933,   967,  1145,  1270,  1024,  1024,   759,   834,   854,   871,
     1020,  1192,  1024,  1024,   829,  1241,  1130,  1046,  1161,  1204,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1209,  1456,  1154,  1066,  1066,  1033,  1024,  1024,  1170,  1435,  1149,  1069,
     1084,  1040,  1024,  1024,  1037,  1313,  1146,  1075,  1083,  1035,  1024,  1024,
      946,  1207,  1139,  1082,  1092,  1042,  1024,  1024,   897,  1084,  1067,  1064,
     1103,  1049,  1024,  1024,   910,  1009,  1015,  1048,  1072,  1057,  1024,  1024,
      928,   967,   985,   989,  1024,  1058,  1024,  1024,  1351,  1347,  1111,  1039,
     1040,  1026,  1024,  1024,  1282,  1325,  1132,  1056,  1048,  1029,  1024,  1024,
     1183,  1298,  1119,  1056,  1046,  1030,  1024,  1024,  1139,  1251,  1108,  1075,
     1058,  1030,  1024,  1024,  1032,  1171,  1079,  1069,  1072,  1033,  1024,  1024,
     1001,  1101,  1045,  1042,  1065,  1038,  1024,  1024,   967,  1040,  1027,  1024,
     1039,  1042,  1024,  1024,  1233,  1296,  1105,  1040,  1026,  1024,  1024,  1024,
     1217,  1155,  1035,  1024,  1024,  1024,  1024,  1024,  1218,  1065,  1026,  1024,
     1024,  1024,  1024,  1024,  1230,  1039,  1024,  1024,  1024,  1024,  1024,  1024,
     1165,  1025,  1024,  1024,  1024,  1024,  1024,  1024,  1073,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1028,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1204,  1540,  1124,   937,  1077,  1245,  1024,  1024,  1296,  1659,  1202,  1071,
     1118,  1091,  1024,  1024,  1089,  1462,  1159,  1057,  1091,  1095,  1024,  1024,
      958,  1300,  1140,  1051,  1122,  1128,  1024,  1024,   810,  1048,  1024,   981,
     1105,  1148,  1024,  1024,   736,   907,   922,   917,  1049,  1154,  1024,  1024,
      746,   838,   866,   857,   952,  1125,  1024,  1024,  1746,  1671,  1163,  1053,
     1032,  1026,  1024,  1024,  1491,  1663,  1227,  1056,  1040,  1030,  1024,  1024,
     1353,  1563,  1231,  1056,  1039,  1030,  1024,  1024,  1169,  1472,  1248,  1098,
     1054,  1036,  1024,  1024,   957,  1232,  1157,  1087,  1082,  1041,  1024,  1024,
      891,  1030,  1041,  1030,  1079,  1070,  1024,  1024,   889,   947,   944,   969,
     1043,  1079,  1024,  1024,  1573,  1441,  1049,  1028,  1024,  1024,  1024,  1024,
     1415,  1385,  1048,  1027,  1024,  1024,  1024,  1024,  1279,  1244,  1042,  1026,
     1026,  1024,  1024,  1024,  1119,  1156,  1049,  1029,  1026,  1024,  1024,  1024,
     1024,  1057,  1041,  1028,  1026,  1024,  1024,  1024,  1008,  1020,  1028,  1023,
     1028,  1024,  1024,  1024,  1014,  1018,  1022,  1022,  1026,  1024,  1024,  1024,
     1380,  1210,  1040,  1024,  1024,  1024,  1024,  1024,  1143,  1127,  1037,  1026,
     1024,  1024,  1024,  1024,  1071,  1068,  1036,  1024,  1024,  1024,  1024,  1024,
     1033,  1048,  1030,  1026,  1026,  1024,  1024,  1024,  1020,  1026,  1026,  1026,
     1024,  1024,  1024,  1024,  1022,  1022,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,   672,   448,   293,   533,
      626,  1332,  1024,  1024,   855,   947,   844,  1255,  1032,  1106,  1024,  1024,
      566,   371,   224,   352,   413,   880,  1024,  1024,   952,   837,   671,   913,
     1037,  1198,  1024,  1024,   324,   218,   108,   196,   225,   647,  1024,  1024,
      762,   669,   388,   629,   691,  1074,  1024,  1024,   213,   200,   101,   218,
      179,   493,  1024,  1024,   926,   785,   330,   589,   559,   892,  1024,  1024,
     1643,  1688,  1626,  1889,  1939,  1219,  1631,   900,   990,   739,  1571,  1577,
     1343,  1654,   948
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
