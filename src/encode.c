/* encode.c - 규격 v0 인코더.
 *
 * 조각을 순서대로 쓰고 목차를 채운다. 조각 사이에 상태가 흐르지 않으므로
 * 나중에 이 반복문을 스레드로 나눠도 결과 비트스트림이 같다.
 *
 * 조각 안은 평면 순서다 — Y 전부, Cb 전부, Cr 전부. 색차를 줄이면
 * 평면마다 블록 수가 달라지기 때문에 이 순서가 필요하다.
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

/* 평면에서 8x8 블록을 뽑는다. 이미지 밖은 가장자리 화소를 복제한다. */
static void fetch_block(const uint8_t *plane, int pw, int ph,
                        int bx, int by, int16_t *out)
{
    int y, x;
    for (y = 0; y < 8; y++) {
        int sy = by + y;
        if (sy >= ph) sy = ph - 1;
        if (sy < 0) sy = 0;
        for (x = 0; x < 8; x++) {
            int sx = bx + x;
            if (sx >= pw) sx = pw - 1;
            if (sx < 0) sx = 0;
            out[y * 8 + x] = (int16_t)((int)plane[(size_t)sy * pw + sx] - 128);
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

/* 블록 하나를 비트로 쓴다. SPEC.md 「블록 하나를 담는 법」 */
static void write_block(ingot_bw *w, const int16_t *coef, int base, int plane,
                        int *prev_dc, ingot_ctx *ctx)
{
    int16_t z[64];
    int k, last = 0, dc;

    for (k = 0; k < 64; k++) {
        int idx = ingot_zigzag[k];
        int level = ingot_quantize(coef[idx], ingot_qstep_at(base, idx, plane));
        z[k] = (int16_t)level;
        if (level != 0) last = k + 1;
    }
    dc = z[0];
    z[0] = (int16_t)(dc - *prev_dc);
    *prev_dc = dc;
    if (z[0] != 0 && last == 0) last = 1;

    ingot_bw_put(w, (uint32_t)last, 7);
    for (k = 0; k < last; k++)
        ingot_bw_put_rice(w, z[k], &ctx[ingot_ctx_index(k, plane)]);
}

/* 조각 하나의 한 평면을 쓴다. */
static void write_plane_group(ingot_bw *w, const uint8_t *plane, int pw, int ph,
                              int ox, int oy, int gw, int gh,
                              int base, int p, ingot_ctx *ctx)
{
    int prev_dc = 0, by, bx;
    for (by = 0; by < gh; by += 8) {
        for (bx = 0; bx < gw; bx += 8) {
            int16_t blk[64], coef[64];
            fetch_block(plane, pw, ph, ox + bx, oy + by, blk);
            ingot_fdct8x8(blk, 8, coef);
            write_block(w, coef, base, p, &prev_dc, ctx);
        }
    }
}

ingot_status ingot_encode(const uint8_t *rgb, int width, int height,
                          const ingot_encode_options *opt,
                          uint8_t **out, size_t *out_size)
{
    ingot_encode_options def;
    uint8_t *full[3] = { NULL, NULL, NULL };
    uint8_t *small[2] = { NULL, NULL };
    const uint8_t *plane[3];
    uint8_t *buf = NULL;
    size_t cap, need, toc_off, data_off = 0;
    uint32_t gx_count, gy_count, group_count, gi;
    int gsize, quality, group_log2, sub, p;
    int cw, ch;
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

    gx_count = ingot_groups_across(width, gsize);
    gy_count = ingot_groups_across(height, gsize);
    group_count = gx_count * gy_count;

    cw = ingot_chroma_dim(width, sub);
    ch = ingot_chroma_dim(height, sub);

    {
        size_t n = (size_t)width * (size_t)height;
        for (p = 0; p < 3; p++) {
            full[p] = (uint8_t *)malloc(n);
            if (!full[p]) { st = INGOT_ERR_MEMORY; goto done; }
        }
        ingot_rgb_to_ycbcr(rgb, (int)n, full[0], full[1], full[2]);

        plane[0] = full[0];
        if (sub) {
            size_t cn = (size_t)cw * (size_t)ch;
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
            ingot_bw w;
            ingot_ctx ctx[INGOT_CTX_COUNT];

            if (gw > gsize) gw = gsize;
            if (gh > gsize) gh = gsize;
            cgw = cw - cox; if (cgw > csize) cgw = csize;
            cgh = ch - coy; if (cgh > csize) cgh = csize;

            if (data_off >= cap) { retry = 1; break; }
            ingot_bw_init(&w, buf + data_off, cap - data_off);
            ingot_ctx_reset(ctx);

            write_plane_group(&w, plane[0], width, height, ox, oy, gw, gh,
                              ingot_qstep(quality), 0, ctx);
            for (p = 1; p < 3; p++)
                write_plane_group(&w, plane[p], cw, ch, cox, coy, cgw, cgh,
                                  ingot_qstep(quality), p, ctx);

            len = ingot_bw_finish(&w);
            if (w.overflow) { retry = 1; break; }

            ingot_put32(buf + toc_off + (size_t)gi * INGOT_TOC_ENTRY,     (uint32_t)data_off);
            ingot_put32(buf + toc_off + (size_t)gi * INGOT_TOC_ENTRY + 4, (uint32_t)len);
            data_off += len;
        }

        if (!retry) break;
        free(buf);
        buf = NULL;
        if (cap > (size_t)1 << 31) { st = INGOT_ERR_MEMORY; goto done; }
        cap *= 2;
    }

    *out = buf;
    *out_size = data_off;
    buf = NULL;

done:
    for (p = 0; p < 3; p++) free(full[p]);
    for (p = 0; p < 2; p++) free(small[p]);
    free(buf);
    return st;
}
