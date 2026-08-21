/* internal.h - 내부 공통. 공개 인터페이스가 아니다. */
#ifndef INGOT_INTERNAL_H
#define INGOT_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ingot.h"

#define INGOT_SIG0 0x49 /* 'I' */
#define INGOT_SIG1 0x4E /* 'N' */
#define INGOT_SIG2 0x47 /* 'G' */
#define INGOT_SIG3 0x54 /* 'T' */

#define INGOT_VERSION      0
#define INGOT_HEADER_SIZE  24
#define INGOT_TOC_ENTRY    8
#define INGOT_BLOCK        8
#define INGOT_FWD_SHIFT   14
#define INGOT_INV_SHIFT   18

/* ---- 변환 (transform.c) ---- */
extern const int16_t ingot_dct8[8][8];
extern const uint8_t ingot_zigzag[64];

/* src 는 -128..127 로 옮긴 8x8 블록(행 우선, stride 지정). dst 는 계수 64개. */
void ingot_fdct8x8(const int16_t *src, int src_stride, int16_t *dst);
void ingot_idct8x8(const int16_t *src, int16_t *dst, int dst_stride);

/* ---- 색 (color.c) ---- */
void ingot_rgb_to_ycbcr(const uint8_t *rgb, int count,
                     uint8_t *y, uint8_t *cb, uint8_t *cr);
void ingot_ycbcr_to_rgb(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                     int count, uint8_t *rgb);

/* ---- 양자화 ---- */
static inline int ingot_qstep(int quality)
{
    if (quality < 0) quality = 0;
    if (quality > 63) quality = 63;
    return 1 + quality * 2;      /* 1 ~ 127 */
}

static inline int16_t ingot_quantize(int coef, int step)
{
    /* 0 을 향해 자르되 반올림을 넣는다. 인코더 전용이라 규격이 아니다. */
    int half = step >> 1;
    int v = (coef >= 0) ? (coef + half) / step : -((-coef + half) / step);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static inline int ingot_dequantize(int level, int step)
{
    return level * step;
}

/* 계수 위치와 평면에 따라 양자화 스텝을 키운다.
 * 사람 눈은 고주파와 색차에 둔하므로 그쪽을 더 거칠게 버린다.
 * idx 는 블록 안 인덱스(v*8+u). plane 0 이 휘도, 1·2 가 색차.
 * INGOT_QW_ALPHA·INGOT_QW_CHROMA 는 실측으로 정한 규격 상수다. */
/* 2026-08-21 실측으로 정했다 (tools/tune_quant.py, 표준 시험 이미지 4장).
 * ALPHA(고주파 기울기): 0. 어떤 값도 이득이 없었다 — PSNR 기준으로도 SSIM 기준으로도
 *   가중을 넣을수록 나빠졌다. 아직 엔트로피 부호화가 고정 확률이라 고주파를 거칠게 해도
 *   비트가 그만큼 안 줄기 때문으로 본다(추정). 적응 부호화를 넣은 뒤 다시 훑는다.
 * CHROMA(색차 배수): 20. 두 지표 모두에서 개선되는 유일한 값이다
 *   (PSNR -0.58%, SSIM -1.51%). SSIM 만 보면 32 가 -8.95% 로 훨씬 좋지만
 *   그 값은 PSNR 에서 +2.87% 라 지표 편향이 의심된다. 보수적으로 고른다. */
#ifndef INGOT_QW_ALPHA
#define INGOT_QW_ALPHA 0
#endif
#ifndef INGOT_QW_CHROMA
#define INGOT_QW_CHROMA 20
#endif

static inline int ingot_qstep_at(int base, int idx, int plane)
{
    int u = idx & 7, v = idx >> 3;
    int s = (base * (16 + INGOT_QW_ALPHA * (u + v))) >> 4;
    if (plane) s = (s * INGOT_QW_CHROMA) >> 4;
    return s < 1 ? 1 : s;
}

/* ---- 적응형 부호화 무리 ----
 * 자리마다 통계가 다르므로 무리를 나누고 무리마다 라이스 파라미터를 스스로 맞춘다.
 * 인코더와 디코더가 같은 순서로 같은 갱신을 하므로 상태가 어긋나지 않는다. */
#define INGOT_CTX_COUNT 8
#define INGOT_RICE_MAX  20
#define INGOT_ESCAPE_Q  24

typedef struct {
    uint32_t sum;
    uint32_t cnt;
} ingot_ctx;

static inline void ingot_ctx_reset(ingot_ctx *c)
{
    int i;
    for (i = 0; i < INGOT_CTX_COUNT; i++) { c[i].sum = 4; c[i].cnt = 1; }
}

/* 지그재그 자리와 평면으로 무리를 고른다. */
static inline int ingot_ctx_index(int k, int plane)
{
    int band = (k == 0) ? 0 : (k <= 5) ? 1 : (k <= 20) ? 2 : 3;
    return band + (plane ? 4 : 0);
}

static inline int ingot_rice_param(const ingot_ctx *c)
{
    int m = 0;
    while (m < INGOT_RICE_MAX && ((uint64_t)c->cnt << m) < (uint64_t)c->sum)
        m++;
    return m;
}

static inline void ingot_ctx_update(ingot_ctx *c, uint32_t k)
{
    c->sum += k;
    c->cnt += 1;
    if (c->cnt >= 64) { c->sum >>= 1; c->cnt >>= 1; }
}

/* ---- 비트 쓰기 ---- */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;      /* 다음에 쓸 바이트 */
    uint32_t acc;      /* 상위 비트부터 채운다 */
    int      nbits;    /* acc 안의 유효 비트 수 */
    int      overflow; /* 1 이면 버퍼 부족 */
} ingot_bw;

void ingot_bw_init(ingot_bw *w, uint8_t *buf, size_t cap);
void ingot_bw_put(ingot_bw *w, uint32_t value, int nbits);
void ingot_bw_put_ue(ingot_bw *w, uint32_t k);        /* 지수 골롬 (부호 없음) */
void ingot_bw_put_se(ingot_bw *w, int v);             /* 지수 골롬 (부호 있음) */
void ingot_bw_put_rice(ingot_bw *w, int v, ingot_ctx *c);  /* 적응형 골롬-라이스 */
void ingot_bw_put_rice_u(ingot_bw *w, uint32_t k, ingot_ctx *c); /* 접지 않는 판 */
size_t ingot_bw_finish(ingot_bw *w);                  /* 0비트로 채우고 길이 반환 */

/* ---- 비트 읽기 ---- */
typedef struct {
    const uint8_t *buf;
    size_t   size;
    size_t   pos;
    uint32_t acc;
    int      nbits;
    int      error;    /* 1 이면 입력 끝을 넘어 읽으려 했다 */
} ingot_br;

void ingot_br_init(ingot_br *r, const uint8_t *buf, size_t size);
uint32_t ingot_br_get(ingot_br *r, int nbits);
uint32_t ingot_br_get_ue(ingot_br *r);
int      ingot_br_get_se(ingot_br *r);
int      ingot_br_get_rice(ingot_br *r, ingot_ctx *c);
uint32_t ingot_br_get_rice_u(ingot_br *r, ingot_ctx *c);
int      ingot_br_bit_pub(ingot_br *r);

/* ---- 리틀엔디언 도우미 ---- */
static inline void ingot_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t ingot_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int ingot_clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/* 색차 평면 크기. 서브샘플이면 올림해서 절반이다. */
static inline int ingot_chroma_dim(int v, int sub)
{
    return sub ? ((v + 1) >> 1) : v;
}

/* 조각 격자 계산. 두 값 모두 1 이상이다. */
static inline uint32_t ingot_groups_across(int width, int gsize)
{
    return (uint32_t)((width + gsize - 1) / gsize);
}

#endif /* INGOT_INTERNAL_H */
