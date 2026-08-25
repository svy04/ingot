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
/* decode.c - 규격 v0 디코더.
 *
 * 이 파일은 적대적 입력을 파싱한다는 전제로 쓴다. 어떤 바이트열이 들어와도
 * 오류 코드로 돌아가고 프로세스를 죽이지 않는다 (SPEC.md 「거절해야 하는 것」).
 */
#include <stdlib.h>
#include "internal.h"

typedef struct {
    int width, height;
    int quality, group_log2, gsize, sub, lf;
    int cw, ch;
    uint32_t gx_count, gy_count, group_count;
    size_t toc_off, data_min;
} ingot_hdr;

static ingot_status parse_header(const uint8_t *d, size_t size, ingot_hdr *h)
{
    uint32_t w, ht, gc;

    if (!d || size < INGOT_HEADER_SIZE) return INGOT_ERR_TRUNCATED;
    if (d[0] != INGOT_SIG0 || d[1] != INGOT_SIG1 ||
        d[2] != INGOT_SIG2 || d[3] != INGOT_SIG3) return INGOT_ERR_SIGNATURE;
    if (d[4] != INGOT_VERSION) return INGOT_ERR_VERSION;
    if (d[5] & 0xF8u) return INGOT_ERR_RESERVED;   /* 비트 3~7 은 0 이어야 한다 */
    if (d[5] & 0x01u) return INGOT_ERR_RESERVED;   /* v0 는 색공간 0 만 */
    /* 20번 자리는 목차와 데이터의 해시다. 값 자체는 여기서 안 보고,
     * 파일 전체가 들어온 뒤 ingot_decode 가 대조한다. */

    h->sub = (d[5] & 0x02u) ? 1 : 0;
    h->lf  = (d[5] & 0x04u) ? 1 : 0;   /* 비트 2 = 블록 경계 필터 */

    h->quality = d[6];
    if (h->quality > 63) return INGOT_ERR_BITSTREAM;

    h->group_log2 = d[7];
    if (h->group_log2 < INGOT_GROUP_LOG2_MIN ||
        h->group_log2 > INGOT_GROUP_LOG2_MAX) return INGOT_ERR_BITSTREAM;
    h->gsize = 1 << h->group_log2;

    w  = ingot_get32(d + 8);
    ht = ingot_get32(d + 12);
    if (w == 0 || ht == 0 || w > INGOT_MAX_DIM || ht > INGOT_MAX_DIM)
        return INGOT_ERR_DIMENSION;
    if ((uint64_t)w * (uint64_t)ht > INGOT_MAX_PIXELS) return INGOT_ERR_DIMENSION;
    h->width = (int)w;
    h->height = (int)ht;
    h->cw = ingot_chroma_dim(h->width, h->sub);
    h->ch = ingot_chroma_dim(h->height, h->sub);

    h->gx_count = ingot_groups_across(h->width, h->gsize);
    h->gy_count = ingot_groups_across(h->height, h->gsize);
    gc = ingot_get32(d + 16);
    if (gc != h->gx_count * h->gy_count) return INGOT_ERR_GROUP_COUNT;
    h->group_count = gc;

    h->toc_off = INGOT_HEADER_SIZE;
    h->data_min = h->toc_off + (size_t)gc * INGOT_TOC_ENTRY;
    if (h->data_min > size) return INGOT_ERR_TRUNCATED;
    return INGOT_OK;
}

ingot_status ingot_probe(const uint8_t *data, size_t size, int *width, int *height)
{
    ingot_hdr h;
    ingot_status st = parse_header(data, size, &h);
    if (st != INGOT_OK) return st;
    if (width) *width = h.width;
    if (height) *height = h.height;
    return INGOT_OK;
}

/* 잔차 블록 하나를 읽어 역변환까지 한다. 규격을 벗어나면 0 이 아닌 값을 돌려준다. */
static int read_residual(ingot_rc_dec *r, int base, int n, int plane,
                         uint16_t *probs, int16_t *back, int tx, int aqm,
                         int *pempty)
{
    int16_t deq[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK];
    const uint16_t *zz = ingot_zigzag_of(n);
    int total = n * n, k;
    int16_t placed[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK];
    uint32_t last;

    last = ingot_rc_get_uint(r,
               &probs[ingot_prob_of(ingot_ctx_last_e(plane, n, *pempty))]);
    *pempty = (last == 0);
    if (r->error) return 1;
    if (last > (uint32_t)total) return 1;

    for (k = 0; k < total; k++) { deq[k] = 0; placed[k] = 0; }
#if INGOT_SIGNHIDE
    {
    int sh_on = ((int)last >= INGOT_SH_MIN), sh_sum = 0;
    int sh_idx = 0, sh_step = 0;
#endif

    for (k = 0; k < (int)last; k++) {
        int idx = zz[k];
        int step = ingot_qstep_aq(base, idx, n, plane, aqm);
        int nbsum = ingot_nb2d(placed, idx, n);
        int lvl = ingot_ctx_level(nbsum);
        /* k == last-1 이면 이 계수가 0 이 아닌 것을 여기서도 안다.
         * last 가 「마지막 비영 계수의 다음 자리」이기 때문이다.
         * 인코더가 그 깃발을 안 적었으므로 여기서도 건너뛴다. */
        int level;
        int v;
#if INGOT_SIGNHIDE
        if (sh_on && k == (int)last - 1) {
            uint32_t mag = ingot_rc_get_uint_from(
                r, &probs[ingot_prob_of(ingot_ctx_index(k, n, plane, lvl))], 1);
            if (r->error) return 1;
            if (mag > 32767) return 1;
            level = (int)mag;
            sh_idx = idx; sh_step = step;
        } else
#endif
#if INGOT_ZERO_CTX
        level = ingot_rc_get_int_fromz(
            r, &probs[ingot_prob_of(ingot_ctx_index(k, n, plane, lvl))],
            &probs[INGOT_PROB_ZERO + ingot_zero_ctx(k, n, plane, nbsum)],
            (k == (int)last - 1) ? 1 : 0);
#else
        level = ingot_rc_get_int_from(
            r, &probs[ingot_prob_of(ingot_ctx_index(k, n, plane, lvl))],
            (k == (int)last - 1) ? 1 : 0);
#endif
        if (r->error) return 1;
        if (level > 32767 || level < -32768) return 1;
        placed[idx] = (int16_t)level;
        v = ingot_dequantize(level, step);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        deq[idx] = (int16_t)v;
#if INGOT_SIGNHIDE
        sh_sum += level < 0 ? -level : level;
#endif
    }
#if INGOT_SIGNHIDE
    /* 절댓값 합이 홀수면 감춘 부호가 음수다. */
    if (sh_on && (sh_sum & 1)) {
        int v2;
        placed[sh_idx] = (int16_t)(-placed[sh_idx]);
        v2 = ingot_dequantize(placed[sh_idx], sh_step);
        if (v2 >  32767) v2 =  32767;
        if (v2 < -32768) v2 = -32768;
        deq[sh_idx] = (int16_t)v2;
    }
    }
#endif

    ingot_idct(deq, back, n, n, tx);
    return 0;
}

/* 복원 블록을 평면에 쓴다. 평면 밖으로 나가는 부분은 버린다. */
static void store_block(uint8_t *plane, int pw, int ph,
                        int bx, int by, int n, const int16_t *blk)
{
    int y, x;
    for (y = 0; y < n; y++) {
        int dy = by + y;
        if (dy < 0 || dy >= ph) continue;
        for (x = 0; x < n; x++) {
            int dx = bx + x;
            if (dx < 0 || dx >= pw) continue;
            plane[(size_t)dy * pw + dx] =
                (uint8_t)ingot_clamp_u8((int)blk[y * n + x]);
        }
    }
}

/* 조각 하나를 다 읽은 뒤 거는 필터 둘. 평면마다 같은 순서로 건다. */
static void filter_group(uint8_t *plane, int pw, int ph, int ox, int oy,
                         int gw, int gh, int base, int p,
                         uint8_t *map, int ms, int lf)
{
    if (lf) ingot_loopfilter(plane, pw, ox, oy, gw, gh, map, ms, base, p ? 1 : 0);
#if INGOT_DERINGE
    /* 경계 필터 다음에 건다. 거르기 전 사본에서 읽어야 옆 화소가 이미
     * 걸러진 값에 물들지 않는다. */
    {
        size_t n = (size_t)pw * (size_t)ph;
        uint8_t *pre = (uint8_t *)malloc(n);
        if (pre) {
            memcpy(pre, plane, n);
            ingot_deringe(plane, pre, pw, ph, ox, oy, gw, gh, base,
                          p ? 1 : 0);
            free(pre);
        }
    }
#else
    (void)ph; (void)base;
#endif
}

/* 색차 둘째 평면의 자리. 나눔과 모드는 첫째 평면과 한 벌을 나눠 쓰고
 * 계수만 따로 실려 온다. 휘도일 때는 NULL 이다. */
typedef struct {
    uint8_t *plane;
    int pempty;
} ingot_side_d;

static int read_block(ingot_rc_dec *r, uint8_t *plane, int pw, int ph,
                      int bx, int by, int gx0, int gy0, int n,
                      int base, int p, uint16_t *probs, int *pmode,
                      uint8_t *map, int ms,
                      const uint8_t *luma, int lstride, int *pempty,
                      ingot_side_d *sd, int forced)
{
    int16_t pred[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK],
            back[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK],
            out[INGOT_MAX_BLOCK * INGOT_MAX_BLOCK];
    ingot_neighbors nb;
    uint32_t mode;
    int total = n * n, k;
#if !INGOT_JOINT_CHROMA
    (void)sd;
#endif

    ingot_gather_neighbors(plane, pw, ph, bx, by, gx0, gy0, n, &nb);
#if INGOT_CFL
    if (p && luma) { nb.luma = luma; nb.luma_stride = lstride; }
#else
    (void)luma; (void)lstride;
#endif
    if (forced >= 0) {
        /* 위 마디가 모드를 실어 왔다. 이 자리에는 안 실려 있다. */
        mode = (uint32_t)forced;
        *pmode = forced;
    } else {
#if INGOT_CHROMA_MODES4
        if (p) {
            /* 색차는 넷만 쓴다. 두 비트 트리이고 문맥 자리도 따로다. */
            int mo = INGOT_PROB_MODE_C + (*pmode & 3) * 3;
            int hi = ingot_rc_dec_bit(r, &probs[mo + 0]);
            int lo = ingot_rc_dec_bit(r, &probs[mo + 1 + hi]);
            mode = (uint32_t)((hi << 1) | lo);
            *pmode = (int)mode;
        } else
#endif
        {
#if INGOT_MODES32
        /* 서른두 모드: 앞 블록의 모드를 문맥으로 다섯 비트. */
        int mo = INGOT_PROB_MODE + (*pmode & 31) * 31;
        int b4 = ingot_rc_dec_bit(r, &probs[mo + 0]);
        int b3 = ingot_rc_dec_bit(r, &probs[mo + 1 + b4]);
        int b2 = ingot_rc_dec_bit(r, &probs[mo + 3 + b4 * 2 + b3]);
        int b1 = ingot_rc_dec_bit(r, &probs[mo + 7 + b4 * 4 + b3 * 2 + b2]);
        int b0 = ingot_rc_dec_bit(r,
                     &probs[mo + 15 + b4 * 8 + b3 * 4 + b2 * 2 + b1]);
        mode = (uint32_t)((b4 << 4) | (b3 << 3) | (b2 << 2) | (b1 << 1) | b0);
        *pmode = (int)mode;
#elif INGOT_MODES16
        /* 열여섯 모드: 앞 블록의 모드를 문맥으로 네 비트. */
        int mo = INGOT_PROB_MODE + (*pmode & 15) * 15;
        int b3 = ingot_rc_dec_bit(r, &probs[mo + 0]);
        int b2 = ingot_rc_dec_bit(r, &probs[mo + 1 + b3]);
        int b1 = ingot_rc_dec_bit(r, &probs[mo + 3 + b3 * 2 + b2]);
        int b0 = ingot_rc_dec_bit(r, &probs[mo + 7 + b3 * 4 + b2 * 2 + b1]);
        mode = (uint32_t)((b3 << 3) | (b2 << 2) | (b1 << 1) | b0);
        *pmode = (int)mode;
#elif INGOT_MODES8
        /* 여덟 모드: 앞 블록의 모드를 문맥으로 세 비트. */
        int mo = INGOT_PROB_MODE + (*pmode & 7) * 7;
        int b2 = ingot_rc_dec_bit(r, &probs[mo + 0]);
        int b1 = ingot_rc_dec_bit(r, &probs[mo + 1 + b2]);
        int b0 = ingot_rc_dec_bit(r, &probs[mo + 3 + b2 * 2 + b1]);
        mode = (uint32_t)((b2 << 2) | (b1 << 1) | b0);
        *pmode = (int)mode;
#else
        /* 네 모드: 앞 블록의 모드를 문맥으로 두 비트. */
        int mo = INGOT_PROB_MODE + (*pmode & 3) * 3;
        int hi = ingot_rc_dec_bit(r, &probs[mo + 0]);
        int lo = ingot_rc_dec_bit(r, &probs[mo + 1 + hi]);
        mode = (uint32_t)((hi << 1) | lo);
        *pmode = (int)mode;
#endif
        }
    }
    if (r->error) return 1;

    ingot_predict(&nb, (int)mode, pred);

#if INGOT_IDTX
    {
        int txf = ingot_rc_dec_bit(r, &probs[INGOT_PROB_TX + ingot_tx_ctx(n)]);
        if (r->error) return 1;
        if (read_residual(r, base, n, p, probs, back,
                          ingot_tx_of_mode((int)mode) | (txf ? 4 : 0),
                          ingot_aq_mul(&nb), pempty))
            return 1;
    }
#else
    if (read_residual(r, base, n, p, probs, back,
                      ingot_tx_of_mode((int)mode), ingot_aq_mul(&nb), pempty))
        return 1;
#endif

    for (k = 0; k < total; k++)
        out[k] = (int16_t)ingot_clamp_u8((int)pred[k] + (int)back[k]);
    store_block(plane, pw, ph, bx, by, n, out);

#if INGOT_JOINT_CHROMA
    if (sd) {
        ingot_neighbors nb_b;
        ingot_gather_neighbors(sd->plane, pw, ph, bx, by, gx0, gy0, n, &nb_b);
#if INGOT_CFL
        if (luma) { nb_b.luma = luma; nb_b.luma_stride = lstride; }
#endif
        ingot_predict(&nb_b, (int)mode, pred);
        if (read_residual(r, base, n, 2, probs, back,
                          ingot_tx_of_mode((int)mode), ingot_aq_mul(&nb_b),
                          &sd->pempty))
            return 1;
        for (k = 0; k < total; k++)
            out[k] = (int16_t)ingot_clamp_u8((int)pred[k] + (int)back[k]);
        store_block(sd->plane, pw, ph, bx, by, n, out);
    }
#endif

    /* 이 블록의 왼쪽·위 경계를 4x4 칸 지도에 찍는다. 조각 원점의 경계는
     * 안 찍으므로 필터가 조각을 넘지 않는다 — 조각 독립이 그대로 남는다. */
    {
        int cx = (bx - gx0) >> 2, cy = (by - gy0) >> 2, i, cn = n >> 2;
        for (i = 0; i < cn; i++) {
            if (cx > 0) map[(size_t)(cy + i) * ms + cx] |= INGOT_LF_V;
            if (cy > 0) map[(size_t)cy * ms + cx + i]   |= INGOT_LF_H;
        }
    }
    return 0;
}

/* 크기 n 덩어리 하나를 읽는다. 가장 작은 크기가 아니면 먼저 나눔 비트를 본다. */
static int read_quad(ingot_rc_dec *r, uint8_t *plane, int pw, int ph,
                     int bx, int by, int gx0, int gy0, int n,
                     int base, int p, uint16_t *probs, int *pmode,
                     uint8_t *map, int ms,
                     const uint8_t *luma, int lstride, int *pempty,
                     ingot_side_d *sd, int forced)
{
    int i, h = n >> 1, sidx = INGOT_SPLIT_IDX(n);

    if (n <= INGOT_MIN_BLOCK)
        return read_block(r, plane, pw, ph, bx, by, gx0, gy0, n, base, p,
                          probs, pmode, map, ms, luma, lstride, pempty, sd,
                          forced);

    if (!ingot_rc_dec_bit(r, &probs[INGOT_PROB_SPLIT + sidx])) {
        if (r->error) return 1;
        return read_block(r, plane, pw, ph, bx, by, gx0, gy0, n, base, p,
                          probs, pmode, map, ms, luma, lstride, pempty, sd,
                          forced);
    }
    if (r->error) return 1;
#if INGOT_SHAREMODE
    /* 나눴다. 이미 모드를 물려받은 마디가 아니면 공유 비트를 읽는다.
     * 1 이면 그 자리에 모드가 하나 실려 있고 아래 넷이 그것을 쓴다. */
    if (forced < 0 && n >= INGOT_SHARE_MIN
        && ingot_rc_dec_bit(r, &probs[INGOT_PROB_SHARE + sidx])) {
        if (r->error) return 1;
        {
#if INGOT_MODES32
            int mo = INGOT_PROB_MODE + (*pmode & 31) * 31;
            int b4 = ingot_rc_dec_bit(r, &probs[mo + 0]);
            int b3 = ingot_rc_dec_bit(r, &probs[mo + 1 + b4]);
            int b2 = ingot_rc_dec_bit(r, &probs[mo + 3 + b4 * 2 + b3]);
            int b1 = ingot_rc_dec_bit(r, &probs[mo + 7 + b4 * 4 + b3 * 2 + b2]);
            int b0 = ingot_rc_dec_bit(r,
                         &probs[mo + 15 + b4 * 8 + b3 * 4 + b2 * 2 + b1]);
            forced = (b4 << 4) | (b3 << 3) | (b2 << 2) | (b1 << 1) | b0;
#elif INGOT_MODES16
            int mo = INGOT_PROB_MODE + (*pmode & 15) * 15;
            int b3 = ingot_rc_dec_bit(r, &probs[mo + 0]);
            int b2 = ingot_rc_dec_bit(r, &probs[mo + 1 + b3]);
            int b1 = ingot_rc_dec_bit(r, &probs[mo + 3 + b3 * 2 + b2]);
            int b0 = ingot_rc_dec_bit(r, &probs[mo + 7 + b3 * 4 + b2 * 2 + b1]);
            forced = (b3 << 3) | (b2 << 2) | (b1 << 1) | b0;
#elif INGOT_MODES8
            int mo = INGOT_PROB_MODE + (*pmode & 7) * 7;
            int b2 = ingot_rc_dec_bit(r, &probs[mo + 0]);
            int b1 = ingot_rc_dec_bit(r, &probs[mo + 1 + b2]);
            int b0 = ingot_rc_dec_bit(r, &probs[mo + 3 + b2 * 2 + b1]);
            forced = (b2 << 2) | (b1 << 1) | b0;
#else
            int mo = INGOT_PROB_MODE + (*pmode & 3) * 3;
            int hi = ingot_rc_dec_bit(r, &probs[mo + 0]);
            int lo = ingot_rc_dec_bit(r, &probs[mo + 1 + hi]);
            forced = (hi << 1) | lo;
#endif
            *pmode = forced;
        }
    }
    if (r->error) return 1;
#endif
    for (i = 0; i < 4; i++)
        if (read_quad(r, plane, pw, ph, bx + (i & 1) * h, by + (i >> 1) * h,
                      gx0, gy0, h, base, p, probs, pmode, map, ms,
                      luma, lstride, pempty, sd, forced)) return 1;
    return 0;
}

static int read_plane_group(ingot_rc_dec *r, uint8_t *plane,
                            const uint8_t *luma, int lstride, int pw, int ph,
                            int ox, int oy, int gw, int gh,
                            int base, int p, uint16_t *probs,
                            uint8_t *map, int ms, int lf, uint8_t *pre_out)
{
    int my, mx, pmode = INGOT_PRED_DC;   /* 조각·평면마다 DC 에서 시작한다 */
    int pempty = 0;                     /* 앞 블록이 비었는가 */
    memset(map, 0, (size_t)ms * ms);
    for (my = 0; my < gh; my += INGOT_MAX_BLOCK)
        for (mx = 0; mx < gw; mx += INGOT_MAX_BLOCK)
            if (read_quad(r, plane, pw, ph, ox + mx, oy + my, ox, oy,
                          INGOT_MAX_BLOCK, base, p, probs, &pmode, map, ms,
                          luma, lstride, &pempty, NULL, -1))
                return 1;
    /* 색차가 볼 휘도는 **필터를 걸기 전** 값이어야 한다. 인코더는 자기
     * 복원 버퍼를 안 거르므로 필터 전 값을 보고 기울기를 뽑는다. 디코더가
     * 필터 뒤 값을 보면 양쪽이 다른 예측을 만든다 -- 2026-08-25 에 이
     * 어긋남으로 q20 에서 7.4 dB 를 잃었다. 두 필터가 걸리기 전에 뜬다. */
    if (pre_out) {
        int yy;
        for (yy = 0; yy < gh; yy++)
            memcpy(pre_out + (size_t)(oy + yy) * pw + ox,
                   plane + (size_t)(oy + yy) * pw + ox, (size_t)gw);
    }
    filter_group(plane, pw, ph, ox, oy, gw, gh, base, p, map, ms, lf);
    return 0;
}

#if INGOT_JOINT_CHROMA
/* 색차 두 평면을 한 번의 순회로 읽는다. 나눔 트리와 예측 모드는 한 벌만
 * 실려 있고, 그 자리에서 두 평면의 계수를 잇달아 읽는다. 나눔이 같으니
 * 경계 지도도 하나면 된다. */
static int read_chroma_group(ingot_rc_dec *r, uint8_t *p1, uint8_t *p2,
                             const uint8_t *luma, int lstride, int pw, int ph,
                             int ox, int oy, int gw, int gh,
                             int base, uint16_t *probs,
                             uint8_t *map, int ms, int lf)
{
    int my, mx, pmode = INGOT_PRED_DC, pempty = 0;
    ingot_side_d sd;

    sd.plane = p2; sd.pempty = 0;
    memset(map, 0, (size_t)ms * ms);
    for (my = 0; my < gh; my += INGOT_MAX_BLOCK)
        for (mx = 0; mx < gw; mx += INGOT_MAX_BLOCK)
            if (read_quad(r, p1, pw, ph, ox + mx, oy + my, ox, oy,
                          INGOT_MAX_BLOCK, base, 1, probs, &pmode, map, ms,
                          luma, lstride, &pempty, &sd, -1))
                return 1;
    filter_group(p1, pw, ph, ox, oy, gw, gh, base, 1, map, ms, lf);
    filter_group(p2, pw, ph, ox, oy, gw, gh, base, 2, map, ms, lf);
    return 0;
}
#endif

/* 절반 크기 평면을 원 크기로 늘린다. 최근접이다. */
static void upsample(const uint8_t *src, int sw, int sh,
                     uint8_t *dst, int dw, int dh)
{
    int y, x;
    for (y = 0; y < dh; y++) {
        int sy = y >> 1;
        if (sy >= sh) sy = sh - 1;
        for (x = 0; x < dw; x++) {
            int sx = x >> 1;
            if (sx >= sw) sx = sw - 1;
            dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
        }
    }
}

ingot_status ingot_decode(const uint8_t *data, size_t size,
                          uint8_t **rgb_out, int *width_out, int *height_out)
{
    ingot_hdr h;
    uint8_t *plane[3] = { NULL, NULL, NULL };
    uint8_t *big[2] = { NULL, NULL };
    const uint8_t *cb, *cr;
    uint8_t *rgb = NULL;
#if INGOT_CFL
    uint8_t *lfpre = NULL;     /* 필터 전 휘도. 색차 예측이 본다 */
#else
    uint8_t *lfpre = NULL;
#endif
    ingot_status st;
    uint32_t gi;
    int p, qbase;
    uint8_t *lfmap = NULL;      /* 4x4 칸마다 「여기가 블록 경계다」 깃발 */
#if INGOT_TOC4
    size_t run_off = 0;         /* 첫 조각의 자리. 아래에서 data_min 으로 잡는다 */
#endif
    int lfms = 0;

    if (rgb_out) *rgb_out = NULL;
    if (!data || !rgb_out) return INGOT_ERR_ARG;

    st = parse_header(data, size, &h);
    if (st != INGOT_OK) return st;
    qbase = ingot_qstep(h.quality);

    /* 화소 버퍼를 잡기 전에 목차를 먼저 본다. 이걸 안 하면 1KB 짜리 파일이
     * 65535x1524 를 주장해 600MB 를 물리게 할 수 있다. 남의 파일을 받는
     * 곳에서는 그것이 곧 서비스 정지다 (2026-08-22 실측). */
    {
        size_t toc = INGOT_HEADER_SIZE;
        uint64_t sum = 0;
        uint32_t g;
        if ((uint64_t)h.group_count * INGOT_TOC_ENTRY > (uint64_t)(size - toc))
            return INGOT_ERR_TOC;
        for (g = 0; g < h.group_count; g++) {
            const uint8_t *e = data + toc + (size_t)g * INGOT_TOC_ENTRY;
#if INGOT_TOC4
#if INGOT_GUIDED
            uint32_t len = INGOT_TOC_LEN2(ingot_get32(e));
#else
            uint32_t len = INGOT_TOC_LEN(ingot_get32(e));
#endif
#else
            uint32_t off = ingot_get32(e), len = ingot_get32(e + 4);
            if ((uint64_t)off > (uint64_t)size) return INGOT_ERR_TOC;
            if ((uint64_t)len > (uint64_t)size - off) return INGOT_ERR_TOC;
#endif
            sum += len;
            if (sum > (uint64_t)size) return INGOT_ERR_TOC;
        }
        if (sum > (uint64_t)size) return INGOT_ERR_TOC;
    }

    /* 목차와 조각 데이터의 해시를 대조한다. 산술 부호화는 어떤 비트열도
     * 그럴듯한 값으로 읽어 내므로, 이것이 없으면 망가진 파일이 조용히 딴
     * 그림이 된다. 0 은 "해시 없음" 이라 지나간다. */
    {
        uint32_t want = ingot_get32(data + 20);
        if (want != 0 && size > INGOT_HEADER_SIZE) {
            uint32_t got = ingot_hash32(data + INGOT_HEADER_SIZE,
                                        size - INGOT_HEADER_SIZE);
            if (got != want) return INGOT_ERR_BITSTREAM;
        }
    }

    {
        size_t n = (size_t)h.width * (size_t)h.height;
        size_t cn = (size_t)h.cw * (size_t)h.ch;
        plane[0] = (uint8_t *)calloc(n, 1);
        plane[1] = (uint8_t *)calloc(cn, 1);
        plane[2] = (uint8_t *)calloc(cn, 1);
        rgb = (uint8_t *)malloc(n * 3);
        lfms = h.gsize >> 2;
        lfmap = (uint8_t *)malloc((size_t)lfms * lfms);
#if INGOT_CFL
        if (!h.sub) {
            lfpre = (uint8_t *)calloc(n, 1);
            if (!lfpre) { st = INGOT_ERR_MEMORY; goto done; }
        }
#endif
        if (!plane[0] || !plane[1] || !plane[2] || !rgb || !lfmap) {
            st = INGOT_ERR_MEMORY; goto done;
        }
    }

#if INGOT_TOC4
    run_off = h.data_min;       /* 첫 조각은 목차 바로 뒤에서 시작한다 */
#endif
    for (gi = 0; gi < h.group_count; gi++) {
        const uint8_t *entry = data + h.toc_off + (size_t)gi * INGOT_TOC_ENTRY;
#if INGOT_TOC4
        /* 오프셋은 앞 조각들의 길이 합이다. 목차에 안 적혀 있다. */
#if INGOT_GUIDED
        uint32_t len = INGOT_TOC_LEN2(ingot_get32(entry));
#else
        uint32_t len = INGOT_TOC_LEN(ingot_get32(entry));
#endif
        uint32_t off = (uint32_t)run_off;
#else
        uint32_t off = ingot_get32(entry);
        uint32_t len = ingot_get32(entry + 4);
#endif
        uint32_t gx = gi % h.gx_count, gy = gi / h.gx_count;
        int ox = (int)gx * h.gsize, oy = (int)gy * h.gsize;
        int gw = h.width - ox, gh = h.height - oy;
        int csize = h.sub ? (h.gsize >> 1) : h.gsize;
        int cox = h.sub ? (ox >> 1) : ox, coy = h.sub ? (oy >> 1) : oy;
        int cgw, cgh;
        ingot_rc_dec r;
        uint16_t probs[INGOT_PROB_COUNT];

        if (off < h.data_min || (uint64_t)off + len > (uint64_t)size) {
            st = INGOT_ERR_TOC; goto done;
        }
        if (gw > h.gsize) gw = h.gsize;
        if (gh > h.gsize) gh = h.gsize;
        cgw = h.cw - cox; if (cgw > csize) cgw = csize;
        cgh = h.ch - coy; if (cgh > csize) cgh = csize;

#if INGOT_TOC4
        run_off = (size_t)off + len;
#endif
        ingot_rc_dec_init(&r, data + off, len);
        ingot_prob_reset(probs, INGOT_PROB_COUNT, h.quality);

        if (read_plane_group(&r, plane[0], NULL, 0, h.width, h.height,
                             ox, oy, gw, gh,
                             qbase, 0, probs, lfmap, lfms, h.lf, lfpre)) {
            st = INGOT_ERR_BITSTREAM; goto done;
        }
#if INGOT_JOINT_CHROMA
        if (read_chroma_group(&r, plane[1], plane[2],
#if INGOT_CFL
                              h.sub ? NULL : (lfpre ? lfpre : plane[0]),
#else
                              h.sub ? NULL : plane[0],
#endif
                              h.width, h.cw, h.ch, cox, coy, cgw, cgh,
                              qbase, probs, lfmap, lfms, h.lf)) {
            st = INGOT_ERR_BITSTREAM; goto done;
        }
#else
        for (p = 1; p < 3; p++) {
            if (read_plane_group(&r, plane[p],
#if INGOT_CFL
                                 h.sub ? NULL : (lfpre ? lfpre : plane[0]),
#else
                                 h.sub ? NULL : plane[0],
#endif
                                 h.width,
                                 h.cw, h.ch, cox, coy, cgw, cgh,
                                 qbase, p, probs, lfmap, lfms, h.lf, NULL)) {
                st = INGOT_ERR_BITSTREAM; goto done;
            }
        }
#endif
    }

    if (h.sub) {
        size_t n = (size_t)h.width * (size_t)h.height;
        for (p = 0; p < 2; p++) {
            big[p] = (uint8_t *)malloc(n);
            if (!big[p]) { st = INGOT_ERR_MEMORY; goto done; }
            upsample(plane[p + 1], h.cw, h.ch, big[p], h.width, h.height);
        }
        cb = big[0]; cr = big[1];
    } else {
        cb = plane[1]; cr = plane[2];
    }

    ingot_ycbcr_to_rgb(plane[0], cb, cr, h.width * h.height, rgb);
#if INGOT_GUIDED
    /* 자기 유도 필터 두 장을 만들고, 조각마다 실려 온 비율로 섞는다. */
    {
        size_t n3 = (size_t)h.width * (size_t)h.height * 3;
        size_t wn = (size_t)(h.width + 1) * (size_t)(h.height + 1);
        uint8_t *g1 = (uint8_t *)malloc(n3);
        uint8_t *g2 = (uint8_t *)malloc(n3);
        int64_t *work = (int64_t *)malloc(wn * 2 * sizeof(int64_t));
        if (g1 && g2 && work) {
            uint32_t g;
            ingot_guided_pair(rgb, h.width, h.height, qbase, g1, g2, work);
            for (g = 0; g < h.group_count; g++) {
                const uint8_t *e = data + h.toc_off
                                 + (size_t)g * INGOT_TOC_ENTRY;
                uint32_t raw = ingot_get32(e);
                uint32_t gx = g % h.gx_count, gy = g / h.gx_count;
                int ox = (int)gx * h.gsize, oy = (int)gy * h.gsize;
                int gw = h.width - ox, gh = h.height - oy;
                if (gw > h.gsize) gw = h.gsize;
                if (gh > h.gsize) gh = h.gsize;
                ingot_guided_apply(rgb, g1, g2, h.width, ox, oy, gw, gh,
                                   INGOT_TOC_WA(raw), INGOT_TOC_WB(raw));
            }
        }
        free(g1); free(g2); free(work);
    }
#endif
#if INGOT_RESTORE
    /* 조각마다 고른 복원 필터를 건다. 참조는 언제나 거르기 전 사본이라
     * 조각마다 번호가 달라도 서로의 결과를 안 본다. */
    {
        size_t n3 = (size_t)h.width * (size_t)h.height * 3;
        uint8_t *tmp = (uint8_t *)malloc(n3);
        uint8_t *pre = (uint8_t *)malloc(n3);
        if (tmp && pre) {
            uint32_t g;
            memcpy(pre, rgb, n3);
            for (g = 0; g < h.group_count; g++) {
                const uint8_t *e = data + h.toc_off
                                 + (size_t)g * INGOT_TOC_ENTRY;
                int filt = INGOT_TOC_FILT(ingot_get32(e));
                uint32_t gx = g % h.gx_count, gy = g / h.gx_count;
                int ox = (int)gx * h.gsize, oy = (int)gy * h.gsize;
                int gw = h.width - ox, gh = h.height - oy;
                if (gw > h.gsize) gw = h.gsize;
                if (gh > h.gsize) gh = h.gsize;
                if (filt)
                    ingot_restore_region(rgb, pre, h.width, h.height,
                                         ox, oy, gw, gh, filt, tmp);
            }
        }
        free(tmp); free(pre);
    }
#endif

    *rgb_out = rgb;
    rgb = NULL;
    if (width_out)  *width_out  = h.width;
    if (height_out) *height_out = h.height;
    st = INGOT_OK;

done:
    for (p = 0; p < 3; p++) free(plane[p]);
    free(lfmap);
    free(lfpre);
    for (p = 0; p < 2; p++) free(big[p]);
    free(rgb);
    return st;
}

const char *ingot_strerror(ingot_status s)
{
    switch (s) {
    case INGOT_OK:              return "ok";
    case INGOT_ERR_ARG:         return "bad argument";
    case INGOT_ERR_MEMORY:      return "out of memory";
    case INGOT_ERR_SIGNATURE:   return "signature mismatch";
    case INGOT_ERR_VERSION:     return "unsupported version";
    case INGOT_ERR_RESERVED:    return "reserved bit set";
    case INGOT_ERR_DIMENSION:   return "dimension out of range";
    case INGOT_ERR_TRUNCATED:   return "truncated file";
    case INGOT_ERR_TOC:         return "table of contents out of bounds";
    case INGOT_ERR_GROUP_COUNT: return "group count mismatch";
    case INGOT_ERR_BITSTREAM:   return "malformed group data";
    }
    return "unknown error";
}
