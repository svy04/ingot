/* transform.c - 정수 8x8 DCT.
 *
 * 표는 DCT-II 를 256배 해서 반올림한 값이다. 규격의 일부이므로
 * 여기 숫자를 바꾸면 비트스트림이 바뀐다 (SPEC.md 「변환」 절).
 *
 * 왕복 오차(양자화 없음): 최대 2, 제곱평균제곱근 0.70
 * — 무작위 블록 300개 실측 2026-08-21.
 */
#include "internal.h"

const int16_t ingot_dct8[8][8] = {
    {  91,   91,   91,   91,   91,   91,   91,   91 },
    { 126,  106,   71,   25,  -25,  -71, -106, -126 },
    { 118,   49,  -49, -118, -118,  -49,   49,  118 },
    { 106,  -25, -126,  -71,   71,  126,   25, -106 },
    {  91,  -91,  -91,   91,   91,  -91,  -91,   91 },
    {  71, -126,   25,  106, -106,  -25,  126,  -71 },
    {  49, -118,  118,  -49,  -49,  118, -118,   49 },
    {  25,  -71,  106, -126,  126, -106,   71,  -25 }
};

/* 저주파부터 훑는 순서. 꼬리의 0을 잘라내려면 이 순서가 필요하다. */
const uint8_t ingot_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

void ingot_fdct8x8(const int16_t *src, int src_stride, int16_t *dst)
{
    int32_t tmp[64];
    int u, v, x, y;

    /* 행 방향 */
    for (y = 0; y < 8; y++) {
        const int16_t *row = src + (size_t)y * src_stride;
        for (u = 0; u < 8; u++) {
            int32_t s = 0;
            for (x = 0; x < 8; x++)
                s += (int32_t)ingot_dct8[u][x] * row[x];
            tmp[y * 8 + u] = s;
        }
    }
    /* 열 방향 + 정규화 */
    for (v = 0; v < 8; v++) {
        for (u = 0; u < 8; u++) {
            int32_t s = 0;
            for (y = 0; y < 8; y++)
                s += (int32_t)ingot_dct8[v][y] * tmp[y * 8 + u];
            s = (s + (1 << (INGOT_FWD_SHIFT - 1))) >> INGOT_FWD_SHIFT;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            dst[v * 8 + u] = (int16_t)s;
        }
    }
}

void ingot_idct8x8(const int16_t *src, int16_t *dst, int dst_stride)
{
    int32_t tmp[64];
    int u, v, x, y;

    /* 세로 방향 역변환 */
    for (u = 0; u < 8; u++) {
        for (y = 0; y < 8; y++) {
            int32_t s = 0;
            for (v = 0; v < 8; v++)
                s += (int32_t)ingot_dct8[v][y] * src[v * 8 + u];
            tmp[y * 8 + u] = s;
        }
    }
    /* 가로 방향 역변환 + 정규화 */
    for (y = 0; y < 8; y++) {
        int16_t *row = dst + (size_t)y * dst_stride;
        for (x = 0; x < 8; x++) {
            int32_t s = 0;
            for (u = 0; u < 8; u++)
                s += (int32_t)ingot_dct8[u][x] * tmp[y * 8 + u];
            s = (s + (1 << (INGOT_INV_SHIFT - 1))) >> INGOT_INV_SHIFT;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            row[x] = (int16_t)s;
        }
    }
}
