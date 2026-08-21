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
#define INGOT_BLOCK16     16
/* 4x4 는 8x8 과 같은 눈금을 쓴다. 그래야 같은 양자화 스텝이 같은 세기로 든다. */
#define INGOT_FWD_SHIFT4  14
#define INGOT_INV_SHIFT4  18
/* 가장 큰 블록과 가장 작은 블록. 16 -> 8 -> 4 로 나뉜다. */
#define INGOT_MAX_BLOCK   16
#ifndef INGOT_MIN_BLOCK
#define INGOT_MIN_BLOCK 4
#endif
#define INGOT_FWD_SHIFT8  14
#define INGOT_INV_SHIFT8  18
#define INGOT_FWD_SHIFT16 15
#define INGOT_INV_SHIFT16 17

/* ---- 변환 (transform.c) ---- */
extern const int16_t ingot_dct4[4][4];
extern const int16_t ingot_dct8[8][8];
extern const int16_t ingot_dct16[16][16];
extern const uint16_t ingot_zz8[64];

const uint16_t *ingot_zigzag_of(int n);

/* n 은 8 또는 16 이다. src 는 잔차 블록(행 우선, stride 지정). */
void ingot_fdct(const int16_t *src, int src_stride, int16_t *dst, int n);
void ingot_idct(const int16_t *src, int16_t *dst, int dst_stride, int n);

/* ---- 인트라 예측 (predict.c) ---- */
#define INGOT_PRED_DC     0
#define INGOT_PRED_V      1
#define INGOT_PRED_H      2
#define INGOT_PRED_PLANE  3
#define INGOT_PRED_COUNT  4

/* 실제로 담아 보는 모드 수. 나머지는 잔차 절대합 순위에서 걸러진다.
 * 인코더만의 선택이라 규격이 아니다. */
#ifndef INGOT_MODE_TRIALS
#define INGOT_MODE_TRIALS 2
#endif

/* 통째로 담는 값이 이 비트 수보다 싸면 나누는 쪽을 아예 재지 않는다.
 * 0 이면 언제나 둘 다 잰다. 인코더만의 선택이라 규격이 아니다. */
#ifndef INGOT_SPLIT_SKIP
#define INGOT_SPLIT_SKIP 24
#endif

typedef struct {
    int n;                      /* 블록 한 변. 8 또는 16 */
    int top[16], left[16], topleft;
    int has_top, has_left, has_topleft;
} ingot_neighbors;

/* recon 은 지금까지 복원된 평면이다. gx0·gy0 는 조각의 원점이라
 * 그보다 위·왼쪽은 이웃으로 쓰지 않는다. */
void ingot_gather_neighbors(const uint8_t *recon, int pw, int ph,
                            int bx, int by, int gx0, int gy0, int n,
                            ingot_neighbors *nb);
void ingot_predict(const ingot_neighbors *nb, int mode, int16_t *pred);

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
    /* 낮은 비트레이트까지 닿아야 다른 포맷과 같은 구간에서 견줄 수 있다.
     * 2026-08-21: 1~127 범위로는 곡선이 안 겹쳐 BD-rate 를 절반도 못 쟀다. */
    return 1 + quality * 2 + (quality * quality) / 16;   /* 1 ~ 375 */
}

#ifndef INGOT_QROUND
#define INGOT_QROUND 6      /* 16 분모. 8 = 그냥 반올림, 작을수록 0 쪽으로 기운다.
                             * 5 였다가 6 으로 올렸다. 인코더의 자 두 곳(머리말 비트
                             * 눈금, λ 정밀도)을 고친 뒤 다시 재니 6 이 나았다:
                             * PSNR -26.8% → -26.9%, SSIMULACRA2 -16.7% → -17.3%,
                             * WebP 대비 -9.5% → -9.8% (2026-08-22, 6장) */
#endif

static inline int16_t ingot_quantize(int coef, int step)
{
    /* 0 을 향해 자르되 반올림을 넣는다. 인코더 전용이라 규격이 아니다.
     * 반올림 자리를 절반보다 0 쪽으로 당기면(데드존) 작은 계수가 0 이 되어
     * 비트를 아낀다. 그 대가로 왜곡이 조금 는다 — 어디가 이득인지 잰다. */
    int half = (step * INGOT_QROUND) >> 4;
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
/* 자리가 높을수록 양자화를 거칠게 한다. 사람 눈은 고주파 오차를 덜 본다.
 * 제곱 오차로만 재면 0 이 최선으로 나온다. 그런데 지각 지표(SSIMULACRA2)로
 * 재면 0 에서는 JPEG 에게도 진다(+5.6%). 1 은 세 지표 모두에서 JPEG 을
 * 이기는 가장 작은 값이고, 거기서 PSNR 로 WebP 도 -4.4% 앞선다.
 * 2 로 더 가면 SSIMULACRA2 는 나아지지만 PSNR·SSIM 을 더 내준다.
 * (2026-08-21, AOM 8장 실측) */
#define INGOT_QW_ALPHA 1
#endif
#ifndef INGOT_QW_CHROMA
#define INGOT_QW_CHROMA 20
#endif

static inline int ingot_qstep_at(int base, int idx, int n, int plane)
{
    int u = idx % n, v = idx / n;
    int s = (base * (16 + INGOT_QW_ALPHA * (u + v))) >> 4;
    if (plane) s = (s * INGOT_QW_CHROMA) >> 4;
    return s < 1 ? 1 : s;
}

/* ---- 적응형 부호화 무리 ----
 * 자리마다 통계가 다르므로 무리를 나누고 무리마다 라이스 파라미터를 스스로 맞춘다.
 * 인코더와 디코더가 같은 순서로 같은 갱신을 하므로 상태가 어긋나지 않는다. */
/* 계수 무리를 블록 크기로도 가를 것인가. 0 이면 크기를 안 본다. */
#ifndef INGOT_CTX_BYSIZE
#define INGOT_CTX_BYSIZE 1
#endif

/* 계수 무리 = 크기 x 대역 4 x 이웃 3 x 평면 2, 거기에 last 6 */
#define INGOT_CTX_BASE  ((INGOT_CTX_BYSIZE == 2) ? 72 : (INGOT_CTX_BYSIZE == 1) ? 48 : 24)
#define INGOT_CTX_COUNT (INGOT_CTX_BASE + 6)
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

/* 지그재그 자리와 평면으로 무리를 고른다.
 * 블록 크기가 달라도 같은 무리를 쓰도록 자리를 64칸 눈금으로 옮긴다. */
static inline int ingot_abs_i(int v) { return v < 0 ? -v : v; }

/* 직전 계수 둘의 크기 합을 세 단계로 나눈다. 0 이면 뒤도 0 이기 쉽고,
 * 컸으면 뒤도 크기 쉽다. 이 한 가지가 확률 모델을 크게 뾰족하게 만든다. */
static inline int ingot_ctx_level(int prev_sum)
{
    /* 다섯 단계로 나눠 봤더니 오히려 나빠졌다 (2026-08-21). 무리가 늘면
     * 무리마다 들어오는 표본이 줄어 확률이 늦게 자리를 잡는다. 셋이 맞다. */
    return (prev_sum == 0) ? 0 : (prev_sum <= 2) ? 1 : 2;
}

static inline int ingot_ctx_index(int k, int n, int plane, int lvl)
{
    int kk = (n == 8) ? k : (k * 64) / (n * n);
    int band = (kk == 0) ? 0 : (kk <= 5) ? 1 : (kk <= 20) ? 2 : 3;
#if INGOT_CTX_BYSIZE
    /* 같은 대역이라도 4x4 의 셋째 계수와 16x16 의 마흔째 계수는 분포가
     * 다르다. last 를 크기별로 가른 것이 크게 먹혔으므로 여기도 가른다. */
#if INGOT_CTX_BYSIZE == 2
    int sz = (n == 4) ? 0 : (n == 8) ? 1 : 2;
    return ((sz * 4 + band) * 3 + lvl) + (plane ? 36 : 0);
#else
    int sz = (n == 16) ? 1 : 0;
    return ((sz * 4 + band) * 3 + lvl) + (plane ? 24 : 0);
#endif
#else
    return (band * 3 + lvl) + (plane ? 12 : 0);
#endif
}

/* 블록 머리말(last) 전용 무리. */
/* last 는 블록 크기마다 분포가 아주 다르다. 4x4 는 대개 한둘이고
 * 16x16 은 수십까지 간다. 그래서 크기별로 무리를 갈라 둔다. */
static inline int ingot_ctx_last_n(int plane, int n)
{
    int sz = (n == 4) ? 0 : (n == 8) ? 1 : 2;
    return INGOT_CTX_BASE + sz * 2 + (plane ? 1 : 0);
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

/* ---- 레인지 코더 (rangecoder.c) ----
 * 골롬-라이스를 대신한다. 자리마다 "0인가"의 확률이 크게 달라서,
 * 그 확률을 그대로 쓰는 편이 한 비트씩 쓰는 것보다 낫다. */

/* 무리마다 모델 두 개(0보다 큰가 / 1보다 큰가)를 둔다.
 * 거기에 분할 1개와 모드 3개를 더한다. */
/* 무리마다 두는 모델 수.
 *   0..4 : k > 0, k > 1, k > 2, k > 3, k > 4 다섯 깃발
 *   5..7 : 지수 골롬 접두부. 자리가 깊어질수록 뒤쪽 모델을 쓴다 */
#define INGOT_PROB_PER_CTX 8
#define INGOT_PROB_SPLIT   (INGOT_CTX_COUNT * INGOT_PROB_PER_CTX)
#define INGOT_PROB_MODE    (INGOT_PROB_SPLIT + 2)   /* 16->8 과 8->4, 둘 */
#define INGOT_PROB_COUNT   (INGOT_PROB_MODE + 3)

typedef struct {
    uint8_t *buf;
    size_t   cap, pos;
    uint64_t low;
    uint32_t range;
    int      cache;
    int64_t  cache_size;
    int      overflow;
    uint32_t bits;      /* 담은 값의 크기. 1/16 비트 단위 (INGOT_BIT_UNIT) */
} ingot_rc_enc;

typedef struct {
    const uint8_t *buf;
    size_t   size, pos;
    uint32_t range, code;
    int      error;
} ingot_rc_dec;

/* 비트 값을 세는 눈금. 1 비트 = INGOT_BIT_UNIT 이다. */
#define INGOT_BIT_UNIT 16

uint32_t ingot_rc_price(uint16_t prob, int bit);
void ingot_prob_reset(uint16_t *p, int count);

void ingot_rc_enc_init(ingot_rc_enc *e, uint8_t *buf, size_t cap);
void ingot_rc_enc_bit(ingot_rc_enc *e, uint16_t *prob, int bit);
void ingot_rc_enc_bypass(ingot_rc_enc *e, uint32_t value, int nbits);
size_t ingot_rc_enc_finish(ingot_rc_enc *e);
void ingot_rc_put_uint(ingot_rc_enc *e, uint16_t *m, uint32_t k);
void ingot_rc_put_int(ingot_rc_enc *e, uint16_t *m, int v);

void ingot_rc_dec_init(ingot_rc_dec *d, const uint8_t *buf, size_t size);
int  ingot_rc_dec_bit(ingot_rc_dec *d, uint16_t *prob);
uint32_t ingot_rc_dec_bypass(ingot_rc_dec *d, int nbits);
uint32_t ingot_rc_get_uint(ingot_rc_dec *d, uint16_t *m);
int  ingot_rc_get_int(ingot_rc_dec *d, uint16_t *m);

/* 무리 번호로 모델 두 개의 자리를 얻는다. */
static inline int ingot_prob_of(int ctx)
{
    return ctx * INGOT_PROB_PER_CTX;
}

/* ---- 비트 쓰기 ---- */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;      /* 다음에 쓸 바이트 */
    uint32_t acc;      /* 상위 비트부터 채운다 */
    int      nbits;    /* acc 안의 유효 비트 수 */
    int      overflow; /* 1 이면 버퍼 부족 */
    uint32_t written_bits;  /* 지금까지 쓴 비트 수. 시험 인코딩에서 비용을 잴 때 쓴다 */
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
