/* encode.c - 규격 v0 인코더.
 *
 * 조각을 순서대로 쓰고 목차를 채운다. 조각 사이에 상태가 흐르지 않으므로
 * 나중에 이 반복문을 스레드로 나눠도 결과 비트스트림이 같다.
 *
 * 인코더는 디코더와 **같은 복원 화소**를 본다. 인트라 예측을 넣은 뒤로는
 * 그러지 않으면 다음 블록의 예측이 어긋난다.
 *
 * 16x16 묶음마다 "한 덩어리로 담을지, 8x8 넷으로 나눌지"를 고른다.
 * 고르는 잣대는 실제 비트 수와 왜곡을 함께 본 값이다.
 */
#include <stdlib.h>
#include "internal.h"

void ingot_encode_options_default(ingot_encode_options *opt)
{
    if (!opt) return;
    opt->quality = 20;
    opt->group_log2 = 8;
    opt->subsample = 0;
}

/* 평면에서 n x n 블록을 뽑는다. 평면 밖은 가장자리 화소를 복제한다. */
static void fetch_block(const uint8_t *plane, int pw, int ph,
                        int bx, int by, int n, int16_t *out)
{
    int y, x;
    for (y = 0; y < n; y++) {
        int sy = by + y;
        if (sy >= ph) sy = ph - 1;
        if (sy < 0) sy = 0;
        for (x = 0; x < n; x++) {
            int sx = bx + x;
            if (sx >= pw) sx = pw - 1;
            if (sx < 0) sx = 0;
            out[y * n + x] = (int16_t)plane[(size_t)sy * pw + sx];
        }
    }
}

/* 2x2 평균으로 절반 크기로 줄인다. 가장자리에서 짝이 없으면 있는 것만 쓴다. */
static void downsample(const uint8_t *src, int sw, int sh,
                       uint8_t *dst, int dw, int dh)
{
    int y, x;
    for (y = 0; y < dh; y++) {
        int y0 = y * 2, y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        for (x = 0; x < dw; x++) {
            int x0 = x * 2, x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            int s = (int)src[(size_t)y0 * sw + x0] + (int)src[(size_t)y0 * sw + x1]
                  + (int)src[(size_t)y1 * sw + x0] + (int)src[(size_t)y1 * sw + x1];
            dst[(size_t)y * dw + x] = (uint8_t)((s + 2) >> 2);
        }
    }
}

/* 복원 블록을 평면에 쓴다. 평면 밖은 버린다. */
static void store_recon(uint8_t *recon, int pw, int ph,
                        int bx, int by, int n, const int16_t *blk)
{
    int y, x;
    for (y = 0; y < n; y++) {
        int dy = by + y;
        if (dy < 0 || dy >= ph) continue;
        for (x = 0; x < n; x++) {
            int dx = bx + x;
            if (dx < 0 || dx >= pw) continue;
            recon[(size_t)dy * pw + dx] =
                (uint8_t)ingot_clamp_u8((int)blk[y * n + x]);
        }
    }
}

/* 양자화까지 한 계수를 담고, 같은 값으로 복원 블록을 만든다.
 * w 가 NULL 이면 비트를 세기만 하고 아무것도 쓰지 않는다(시험 인코딩). */
static void code_residual(ingot_rc_enc *w, const int16_t *resid, int base, int n,
                          int plane, uint16_t *probs,
                          const int16_t *pred, int16_t *recon, int cut)
{
    int16_t coef[256], z[256], deq[256], back[256];
    const uint16_t *zz = ingot_zigzag_of(n);
    int total = n * n, k, last = 0;

    ingot_fdct(resid, n, coef, n);

    for (k = 0; k < total; k++) {
        int idx = zz[k];
        int step = ingot_qstep_at(base, idx, n, plane);
        int level = (k < cut) ? ingot_quantize(coef[idx], step) : 0;
        z[k] = (int16_t)level;
        if (level != 0) last = k + 1;
    }

    ingot_rc_put_uint(w, &probs[ingot_prob_of(ingot_ctx_last(plane))], (uint32_t)last);
    for (k = 0; k < last; k++) {
        int p1 = (k >= 1) ? ingot_abs_i(z[k - 1]) : 0;
        int p2 = (k >= 2) ? ingot_abs_i(z[k - 2]) : 0;
        int lvl = ingot_ctx_level(p1 + p2);
        ingot_rc_put_int(w, &probs[ingot_prob_of(ingot_ctx_index(k, n, plane, lvl))], z[k]);
    }

    for (k = 0; k < total; k++) {
        int idx = zz[k];
        int step = ingot_qstep_at(base, idx, n, plane);
        int v = (k < last) ? ingot_dequantize(z[k], step) : 0;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        deq[idx] = (int16_t)v;
    }
    ingot_idct(deq, back, n, n);
    for (k = 0; k < total; k++)
        recon[k] = (int16_t)ingot_clamp_u8((int)pred[k] + (int)back[k]);
}

/* 복원과 원본의 제곱 오차. 분할을 고를 때 쓴다. */
static int64_t block_distortion(const int16_t *a, const int16_t *b, int total)
{
    int64_t s = 0;
    int i;
    for (i = 0; i < total; i++) {
        int d = (int)a[i] - (int)b[i];
        s += (int64_t)d * d;
    }
    return s;
}

/* 블록 하나를 네 모드로 시험해 가장 싼 것을 고르고 쓴다.
 * w 가 버리는 통로면 비용만 재는 셈이 된다.
 * cost 를 돌려준다 (왜곡 + lambda * 비트). */
static int64_t code_block(ingot_rc_enc *w, const uint8_t *orig, uint8_t *recon,
                          int pw, int ph, int bx, int by, int gx0, int gy0,
                          int n, int base, int plane, uint16_t *probs,
                          int64_t lambda)
{
    int16_t src[256], pred[256], best_pred[256], resid[256], out[256];
    ingot_neighbors nb;
    uint16_t trial_p[INGOT_PROB_COUNT];
    ingot_rc_enc trial;
    uint8_t scratch[1];
    int total = n * n, m, k, best = INGOT_PRED_DC;
    int64_t best_cost = -1;

    fetch_block(orig, pw, ph, bx, by, n, src);
    ingot_gather_neighbors(recon, pw, ph, bx, by, gx0, gy0, n, &nb);

    for (m = 0; m < INGOT_PRED_COUNT; m++) {
        int64_t bits, dist, cost;
        ingot_predict(&nb, m, pred);
        for (k = 0; k < total; k++)
            resid[k] = (int16_t)((int)src[k] - (int)pred[k]);

        /* 비트만 세는 시험 인코딩. 무리 상태는 사본으로 굴린다. */
        memcpy(trial_p, probs, sizeof(trial_p));
        ingot_rc_enc_init(&trial, scratch, 0);     /* 용량 0 = 세기만 한다 */
        code_residual(&trial, resid, base, n, plane, trial_p, pred, out, total);
        bits = (int64_t)trial.bits;
        dist = block_distortion(src, out, total);
        cost = dist + lambda * bits;

        if (best_cost < 0 || cost < best_cost) {
            best_cost = cost;
            best = m;
            for (k = 0; k < total; k++) best_pred[k] = pred[k];
        }
    }

    for (k = 0; k < total; k++)
        resid[k] = (int16_t)((int)src[k] - (int)best_pred[k]);

    /* 고른 모드로 쓴다. 모드 비트도 값에 넣는다. */
    best_cost += lambda * 2;
    ingot_rc_enc_bit(w, &probs[INGOT_PROB_MODE + 0], (best >> 1) & 1);
    ingot_rc_enc_bit(w, &probs[INGOT_PROB_MODE + 1 + ((best >> 1) & 1)], best & 1);
    code_residual(w, resid, base, n, plane, probs, best_pred, out, total);
    store_recon(recon, pw, ph, bx, by, n, out);
    return best_cost;
}

/* 떠 둔 16x16 복원 화소를 되돌린다. */
static void restore_patch(uint8_t *recon, int pw, int ph,
                          int mx, int my, const uint8_t *patch)
{
    int y, x;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int dy = my + y, dx = mx + x;
            if (dy >= 0 && dy < ph && dx >= 0 && dx < pw)
                recon[(size_t)dy * pw + dx] = patch[y * 16 + x];
        }
}

/* 16x16 묶음 하나. 한 덩어리와 넷으로 나눈 것을 견주어 싼 쪽을 쓴다.
 *
 * 두 후보를 모두 '버리는 통로'에 써서 비용을 재고, 이긴 쪽만 진짜 통로에 쓴다.
 * 그 사이 무리 상태와 복원 화소는 매번 되돌린다. */
static void code_macro(ingot_rc_enc *w, const uint8_t *orig, uint8_t *recon,
                       int pw, int ph, int mx, int my, int gx0, int gy0,
                       int base, int plane, uint16_t *probs, int64_t lambda)
{
    uint16_t save[INGOT_PROB_COUNT], p16[INGOT_PROB_COUNT];
    uint8_t patch[256];
    uint8_t scratch[1];
    ingot_rc_enc trial;
    int64_t cost16, cost8 = 0;
    int i, y, x;

    /* 복원 평면의 이 자리를 떠 둔다. */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int dy = my + y, dx = mx + x;
            patch[y * 16 + x] =
                (dy < ph && dx < pw && dy >= 0 && dx >= 0)
                ? recon[(size_t)dy * pw + dx] : 0;
        }
    memcpy(save, probs, sizeof(save));

    /* 후보 1: 16x16 하나 */
    memcpy(p16, probs, sizeof(p16));
    ingot_rc_enc_init(&trial, scratch, 0);
    cost16 = code_block(&trial, orig, recon, pw, ph, mx, my, gx0, gy0,
                        16, base, plane, p16, lambda) + lambda;
    restore_patch(recon, pw, ph, mx, my, patch);

    /* 후보 2: 8x8 넷. 앞 블록의 복원이 뒤 블록의 이웃이라 순서대로 굴려야 한다. */
    memcpy(probs, save, sizeof(save));
    ingot_rc_enc_init(&trial, scratch, 0);
    for (i = 0; i < 4; i++) {
        int bx = mx + (i & 1) * 8, by = my + (i >> 1) * 8;
        cost8 += code_block(&trial, orig, recon, pw, ph, bx, by, gx0, gy0,
                            8, base, plane, probs, lambda);
    }
    cost8 += lambda;
    restore_patch(recon, pw, ph, mx, my, patch);
    memcpy(probs, save, sizeof(save));

    /* 이긴 쪽을 진짜로 쓴다. */
    if (cost16 <= cost8) {
        ingot_rc_enc_bit(w, &probs[INGOT_PROB_SPLIT], 0);
        code_block(w, orig, recon, pw, ph, mx, my, gx0, gy0,
                   16, base, plane, probs, lambda);
    } else {
        ingot_rc_enc_bit(w, &probs[INGOT_PROB_SPLIT], 1);
        for (i = 0; i < 4; i++) {
            int bx = mx + (i & 1) * 8, by = my + (i >> 1) * 8;
            code_block(w, orig, recon, pw, ph, bx, by, gx0, gy0,
                       8, base, plane, probs, lambda);
        }
    }
}

static void write_plane_group(ingot_rc_enc *w, const uint8_t *orig, uint8_t *recon,
                              int pw, int ph, int ox, int oy, int gw, int gh,
                              int base, int p, uint16_t *probs, int64_t lambda)
{
    int my, mx;
    for (my = 0; my < gh; my += 16)
        for (mx = 0; mx < gw; mx += 16)
            code_macro(w, orig, recon, pw, ph, ox + mx, oy + my, ox, oy,
                       base, p, probs, lambda);
}

ingot_status ingot_encode(const uint8_t *rgb, int width, int height,
                          const ingot_encode_options *opt,
                          uint8_t **out, size_t *out_size)
{
    ingot_encode_options def;
    uint8_t *full[3] = { NULL, NULL, NULL };
    uint8_t *small[2] = { NULL, NULL };
    uint8_t *recon[3] = { NULL, NULL, NULL };
    const uint8_t *plane[3];
    uint8_t *buf = NULL;
    size_t cap, need, toc_off, data_off = 0;
    uint32_t gx_count, gy_count, group_count, gi;
    int gsize, quality, group_log2, sub, p;
    int cw, ch, qbase;
    int64_t lambda;
    ingot_status st = INGOT_OK;

    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (!rgb || !out || !out_size) return INGOT_ERR_ARG;
    if (width <= 0 || height <= 0) return INGOT_ERR_DIMENSION;
    if (width > INGOT_MAX_DIM || height > INGOT_MAX_DIM) return INGOT_ERR_DIMENSION;
    if ((uint64_t)width * (uint64_t)height > INGOT_MAX_PIXELS) return INGOT_ERR_DIMENSION;

    if (!opt) { ingot_encode_options_default(&def); opt = &def; }
    quality = opt->quality;
    if (quality < 0) quality = 0;
    if (quality > 63) quality = 63;
    group_log2 = opt->group_log2 ? opt->group_log2 : 8;
    if (group_log2 < INGOT_GROUP_LOG2_MIN) group_log2 = INGOT_GROUP_LOG2_MIN;
    if (group_log2 > INGOT_GROUP_LOG2_MAX) group_log2 = INGOT_GROUP_LOG2_MAX;
    gsize = 1 << group_log2;
    sub = opt->subsample ? 1 : 0;
    qbase = ingot_qstep(quality);

    /* 왜곡과 비트를 견주는 무게. 양자화가 거칠수록 비트가 비싸진다.
     * 계수는 관행값(0.85 * step^2)의 정수 근사다. */
    /* 비트 하나를 왜곡 얼마와 맞바꿀지 정하는 값. 100 분모다.
     * 재서 정한다 — 너무 크면 인코더가 계수를 과하게 버린다. */
#ifndef INGOT_LAMBDA
#define INGOT_LAMBDA 45
#endif
    lambda = ((int64_t)qbase * qbase * INGOT_LAMBDA) / (100 * INGOT_BIT_UNIT);
    if (lambda < 1) lambda = 1;

    gx_count = ingot_groups_across(width, gsize);
    gy_count = ingot_groups_across(height, gsize);
    group_count = gx_count * gy_count;

    cw = ingot_chroma_dim(width, sub);
    ch = ingot_chroma_dim(height, sub);

    {
        size_t n = (size_t)width * (size_t)height;
        size_t cn = (size_t)cw * (size_t)ch;
        for (p = 0; p < 3; p++) {
            full[p] = (uint8_t *)malloc(n);
            if (!full[p]) { st = INGOT_ERR_MEMORY; goto done; }
        }
        ingot_rgb_to_ycbcr(rgb, (int)n, full[0], full[1], full[2]);

        plane[0] = full[0];
        if (sub) {
            for (p = 0; p < 2; p++) {
                small[p] = (uint8_t *)malloc(cn);
                if (!small[p]) { st = INGOT_ERR_MEMORY; goto done; }
                downsample(full[p + 1], width, height, small[p], cw, ch);
                plane[p + 1] = small[p];
            }
        } else {
            plane[1] = full[1];
            plane[2] = full[2];
        }

        recon[0] = (uint8_t *)calloc(n, 1);
        recon[1] = (uint8_t *)calloc(cn, 1);
        recon[2] = (uint8_t *)calloc(cn, 1);
        if (!recon[0] || !recon[1] || !recon[2]) { st = INGOT_ERR_MEMORY; goto done; }
    }

    {
        uint32_t blocks = (uint32_t)((gsize / 8) * (gsize / 8)) * 3u;
        need = INGOT_HEADER_SIZE + (size_t)group_count * INGOT_TOC_ENTRY
             + (size_t)group_count * blocks * 24u + 1024u;
    }
    cap = need;

    for (;;) {
        int retry = 0;
        buf = (uint8_t *)malloc(cap);
        if (!buf) { st = INGOT_ERR_MEMORY; goto done; }

        buf[0] = INGOT_SIG0; buf[1] = INGOT_SIG1;
        buf[2] = INGOT_SIG2; buf[3] = INGOT_SIG3;
        buf[4] = INGOT_VERSION;
        buf[5] = (uint8_t)(sub ? 0x02 : 0x00);
        buf[6] = (uint8_t)quality;
        buf[7] = (uint8_t)group_log2;
        ingot_put32(buf + 8,  (uint32_t)width);
        ingot_put32(buf + 12, (uint32_t)height);
        ingot_put32(buf + 16, group_count);
        ingot_put32(buf + 20, 0);

        toc_off  = INGOT_HEADER_SIZE;
        data_off = toc_off + (size_t)group_count * INGOT_TOC_ENTRY;

        for (gi = 0; gi < group_count; gi++) {
            uint32_t gx = gi % gx_count, gy = gi / gx_count;
            int ox = (int)gx * gsize, oy = (int)gy * gsize;
            int gw = width - ox, gh = height - oy;
            int csize = sub ? (gsize >> 1) : gsize;
            int cox = sub ? (ox >> 1) : ox, coy = sub ? (oy >> 1) : oy;
            int cgw, cgh;
            size_t len;
            ingot_rc_enc w;
            uint16_t probs[INGOT_PROB_COUNT];

            if (gw > gsize) gw = gsize;
            if (gh > gsize) gh = gsize;
            cgw = cw - cox; if (cgw > csize) cgw = csize;
            cgh = ch - coy; if (cgh > csize) cgh = csize;

            if (data_off >= cap) { retry = 1; break; }
            ingot_rc_enc_init(&w, buf + data_off, cap - data_off);
            ingot_prob_reset(probs, INGOT_PROB_COUNT);

            write_plane_group(&w, plane[0], recon[0], width, height,
                              ox, oy, gw, gh, qbase, 0, probs, lambda);
            for (p = 1; p < 3; p++)
                write_plane_group(&w, plane[p], recon[p], cw, ch,
                                  cox, coy, cgw, cgh, qbase, p, probs, lambda);

            len = ingot_rc_enc_finish(&w);
            if (w.overflow) { retry = 1; break; }

            ingot_put32(buf + toc_off + (size_t)gi * INGOT_TOC_ENTRY,     (uint32_t)data_off);
            ingot_put32(buf + toc_off + (size_t)gi * INGOT_TOC_ENTRY + 4, (uint32_t)len);
            data_off += len;
        }

        if (!retry) break;
        free(buf);
        buf = NULL;
        {
            size_t n = (size_t)width * (size_t)height;
            size_t cn = (size_t)cw * (size_t)ch;
            memset(recon[0], 0, n);
            memset(recon[1], 0, cn);
            memset(recon[2], 0, cn);
        }
        if (cap > (size_t)1 << 31) { st = INGOT_ERR_MEMORY; goto done; }
        cap *= 2;
    }

    *out = buf;
    *out_size = data_off;
    buf = NULL;

done:
    for (p = 0; p < 3; p++) { free(full[p]); free(recon[p]); }
    for (p = 0; p < 2; p++) free(small[p]);
    free(buf);
    return st;
}
