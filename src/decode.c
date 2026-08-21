/* decode.c - 규격 v0 디코더.
 *
 * 이 파일은 적대적 입력을 파싱한다는 전제로 쓴다. 어떤 바이트열이 들어와도
 * 오류 코드로 돌아가고 프로세스를 죽이지 않는다 (SPEC.md 「거절해야 하는 것」).
 */
#include <stdlib.h>
#include "internal.h"

typedef struct {
    int width, height;
    int quality, group_log2, gsize, sub;
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
    if (d[5] & 0xFCu) return INGOT_ERR_RESERVED;   /* 비트 2~7 은 0 이어야 한다 */
    if (d[5] & 0x01u) return INGOT_ERR_RESERVED;   /* v0 는 색공간 0 만 */
    if (ingot_get32(d + 20) != 0) return INGOT_ERR_RESERVED;

    h->sub = (d[5] & 0x02u) ? 1 : 0;

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
static int read_residual(ingot_br *r, int base, int plane,
                         ingot_ctx *ctx, int16_t *back)
{
    int16_t deq[64];
    uint32_t last;
    int k;

    last = ingot_br_get_rice_u(r, &ctx[ingot_ctx_last(plane)]);
    if (r->error) return 1;
    if (last > 64) return 1;

    for (k = 0; k < 64; k++) deq[ingot_zigzag[k]] = 0;

    for (k = 0; k < (int)last; k++) {
        int idx = ingot_zigzag[k];
        int step = ingot_qstep_at(base, idx, plane);
        int level = ingot_br_get_rice(r, &ctx[ingot_ctx_index(k, plane)]);
        int v;
        if (r->error) return 1;
        if (level > 32767 || level < -32768) return 1;
        v = ingot_dequantize(level, step);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        deq[idx] = (int16_t)v;
    }

    ingot_idct8x8(deq, back, 8);
    return 0;
}

/* 복원 블록을 평면에 쓴다. 평면 밖으로 나가는 부분은 버린다. */
static void store_block(uint8_t *plane, int pw, int ph,
                        int bx, int by, const int16_t *blk)
{
    int y, x;
    for (y = 0; y < 8; y++) {
        int dy = by + y;
        if (dy < 0 || dy >= ph) continue;
        for (x = 0; x < 8; x++) {
            int dx = bx + x;
            if (dx < 0 || dx >= pw) continue;
            plane[(size_t)dy * pw + dx] =
                (uint8_t)ingot_clamp_u8((int)blk[y * 8 + x]);
        }
    }
}

static int read_plane_group(ingot_br *r, uint8_t *plane, int pw, int ph,
                            int ox, int oy, int gw, int gh,
                            int base, int p, ingot_ctx *ctx)
{
    int by, bx, k;
    for (by = 0; by < gh; by += 8) {
        for (bx = 0; bx < gw; bx += 8) {
            int16_t pred[64], back[64], out[64];
            ingot_neighbors nb;
            uint32_t mode;

            mode = ingot_br_get(r, 2);
            if (r->error) return 1;

            ingot_gather_neighbors(plane, pw, ph, ox + bx, oy + by, ox, oy, &nb);
            ingot_predict(&nb, (int)mode, pred);

            if (read_residual(r, base, p, ctx, back)) return 1;

            for (k = 0; k < 64; k++)
                out[k] = (int16_t)ingot_clamp_u8((int)pred[k] + (int)back[k]);
            store_block(plane, pw, ph, ox + bx, oy + by, out);
        }
    }
    return 0;
}

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
    ingot_status st;
    uint32_t gi;
    int p;

    if (rgb_out) *rgb_out = NULL;
    if (!data || !rgb_out) return INGOT_ERR_ARG;

    st = parse_header(data, size, &h);
    if (st != INGOT_OK) return st;

    {
        size_t n = (size_t)h.width * (size_t)h.height;
        size_t cn = (size_t)h.cw * (size_t)h.ch;
        plane[0] = (uint8_t *)calloc(n, 1);
        plane[1] = (uint8_t *)calloc(cn, 1);
        plane[2] = (uint8_t *)calloc(cn, 1);
        rgb = (uint8_t *)malloc(n * 3);
        if (!plane[0] || !plane[1] || !plane[2] || !rgb) {
            st = INGOT_ERR_MEMORY; goto done;
        }
    }

    for (gi = 0; gi < h.group_count; gi++) {
        const uint8_t *entry = data + h.toc_off + (size_t)gi * INGOT_TOC_ENTRY;
        uint32_t off = ingot_get32(entry);
        uint32_t len = ingot_get32(entry + 4);
        uint32_t gx = gi % h.gx_count, gy = gi / h.gx_count;
        int ox = (int)gx * h.gsize, oy = (int)gy * h.gsize;
        int gw = h.width - ox, gh = h.height - oy;
        int csize = h.sub ? (h.gsize >> 1) : h.gsize;
        int cox = h.sub ? (ox >> 1) : ox, coy = h.sub ? (oy >> 1) : oy;
        int cgw, cgh;
        ingot_br r;
        ingot_ctx ctx[INGOT_CTX_COUNT];

        if (off < h.data_min || (uint64_t)off + len > (uint64_t)size) {
            st = INGOT_ERR_TOC; goto done;
        }
        if (gw > h.gsize) gw = h.gsize;
        if (gh > h.gsize) gh = h.gsize;
        cgw = h.cw - cox; if (cgw > csize) cgw = csize;
        cgh = h.ch - coy; if (cgh > csize) cgh = csize;

        ingot_br_init(&r, data + off, len);
        ingot_ctx_reset(ctx);

        if (read_plane_group(&r, plane[0], h.width, h.height, ox, oy, gw, gh,
                             ingot_qstep(h.quality), 0, ctx)) {
            st = INGOT_ERR_BITSTREAM; goto done;
        }
        for (p = 1; p < 3; p++) {
            if (read_plane_group(&r, plane[p], h.cw, h.ch, cox, coy, cgw, cgh,
                                 ingot_qstep(h.quality), p, ctx)) {
                st = INGOT_ERR_BITSTREAM; goto done;
            }
        }
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

    *rgb_out = rgb;
    rgb = NULL;
    if (width_out)  *width_out  = h.width;
    if (height_out) *height_out = h.height;
    st = INGOT_OK;

done:
    for (p = 0; p < 3; p++) free(plane[p]);
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
