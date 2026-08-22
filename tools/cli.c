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
/* cli.c - 명령줄 도구. 라이브러리를 쓰는 예제이기도 하다.
 *
 * 입출력은 PPM(P6, 8비트) 만 다룬다. 의존성을 0으로 두기 위해서다.
 * 다른 포맷은 ffmpeg 으로 PPM 으로 바꿔서 넣는다.
 *
 * 화면에 나가는 글은 영어다. 코드 주석은 한국어로 남긴다 —
 * 왜 그렇게 했는지를 담은 기록이라 옮기면 뉘앙스가 죽는다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/ingot.h"

static int read_ppm(const char *path, uint8_t **rgb, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    int c, vals[3], got = 0;
    size_t n;

    *rgb = NULL;
    if (!f) { fprintf(stderr, "cannot open: %s\n", path); return 1; }
    if (fgetc(f) != 'P' || fgetc(f) != '6') {
        fprintf(stderr, "not a P6 PPM: %s\n", path); fclose(f); return 1;
    }
    while (got < 3) {
        c = fgetc(f);
        if (c == EOF) { fclose(f); return 1; }
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[got]) != 1) { fclose(f); return 1; }
        got++;
    }
    fgetc(f);   /* 헤더 뒤 공백 한 글자 */
    if (vals[0] <= 0 || vals[1] <= 0 || vals[2] != 255) {
        fprintf(stderr, "only 8-bit PPM is supported\n"); fclose(f); return 1;
    }
    *w = vals[0];
    *h = vals[1];
    n = (size_t)*w * (size_t)*h * 3;
    *rgb = (uint8_t *)malloc(n);
    if (!*rgb) { fclose(f); return 1; }
    if (fread(*rgb, 1, n, f) != n) {
        fprintf(stderr, "pixel data is short\n");
        free(*rgb); *rgb = NULL; fclose(f); return 1;
    }
    fclose(f);
    return 0;
}

static int write_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    size_t n = (size_t)w * (size_t)h * 3;
    if (!f) { fprintf(stderr, "cannot write: %s\n", path); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    if (fwrite(rgb, 1, n, f) != n) { fclose(f); return 1; }
    fclose(f);
    return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long len;
    *data = NULL; *size = 0;
    if (!f) { fprintf(stderr, "cannot open: %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return 1; }
    *data = (uint8_t *)malloc((size_t)len);
    if (!*data) { fclose(f); return 1; }
    if (fread(*data, 1, (size_t)len, f) != (size_t)len) {
        free(*data); *data = NULL; fclose(f); return 1;
    }
    *size = (size_t)len;
    fclose(f);
    return 0;
}

/* 두 이미지의 화소 차이. 라이브러리가 아니라 도구라서 여기서만 실수 연산을 쓴다. */
static void diff_stats(const uint8_t *a, const uint8_t *b, size_t n,
                       int *max_diff, unsigned long long *sq_sum)
{
    size_t i;
    *max_diff = 0;
    *sq_sum = 0;
    for (i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) d = -d;
        if (d > *max_diff) *max_diff = d;
        *sq_sum += (unsigned long long)d * (unsigned long long)d;
    }
}

static void usage(void)
{
    printf("usage:\n");
    printf("  ingot enc <in.ppm> <out.igt> [quality 0-63] [group_log2 6-10] [subsample 0|1]\n");
    printf("  ingot dec <in.igt> <out.ppm>\n");
    printf("  ingot rt  <in.ppm> [quality] [group_log2] [subsample]   round-trip check\n");
    printf("  ingot info <in.igt>\n");
    printf("\n");
    printf("quality: 0 keeps the most detail, 63 the least.\n");
    printf("subsample: 0 = 4:4:4 (default), 1 = 4:2:0. Do not use 1 on screenshots or text.\n");
}

#ifdef INGOT_PROB_DUMP
void ingot_prob_dump(const char *path);
static const char *g_dump_path;
static void dump_at_exit(void) { if (g_dump_path) ingot_prob_dump(g_dump_path); }
#endif

#ifdef INGOT_BIT_STATS
void ingot_bitstat_dump(const char *path);
static const char *g_bs_path;
static void bs_at_exit(void) { if (g_bs_path) ingot_bitstat_dump(g_bs_path); }
#endif

int main(int argc, char **argv)
{
#ifdef INGOT_PROB_DUMP
    g_dump_path = getenv("INGOT_DUMP_PATH");
    if (g_dump_path) atexit(dump_at_exit);
#endif
#ifdef INGOT_BIT_STATS
    g_bs_path = getenv("INGOT_BITSTAT_PATH");
    if (g_bs_path) atexit(bs_at_exit);
#endif
    if (argc < 2) { usage(); return 1; }

    if (!strcmp(argv[1], "enc") && argc >= 4) {
        uint8_t *rgb = NULL, *out = NULL;
        int w, h;
        size_t osize;
        ingot_encode_options opt;
        ingot_status st;

        if (read_ppm(argv[2], &rgb, &w, &h)) return 1;
        ingot_encode_options_default(&opt);
        if (argc >= 5) opt.quality = atoi(argv[4]);
        if (argc >= 6) opt.group_log2 = atoi(argv[5]);
        if (argc >= 7) opt.subsample = atoi(argv[6]);

        st = ingot_encode(rgb, w, h, &opt, &out, &osize);
        free(rgb);
        if (st != INGOT_OK) {
            fprintf(stderr, "encode failed: %s\n", ingot_strerror(st)); return 1;
        }
        {
            FILE *f = fopen(argv[3], "wb");
            if (!f) { free(out); fprintf(stderr, "cannot write\n"); return 1; }
            fwrite(out, 1, osize, f);
            fclose(f);
        }
        printf("%dx%d  q%d  -> %lu bytes (source %lu, %.2f%%)\n",
               w, h, opt.quality, (unsigned long)osize,
               (unsigned long)((size_t)w * h * 3),
               100.0 * (double)osize / (double)((size_t)w * h * 3));
        free(out);
        return 0;
    }

    if (!strcmp(argv[1], "dec") && argc >= 4) {
        uint8_t *data = NULL, *rgb = NULL;
        size_t size;
        int w, h;
        ingot_status st;

        if (read_file(argv[2], &data, &size)) return 1;
        st = ingot_decode(data, size, &rgb, &w, &h);
        free(data);
        if (st != INGOT_OK) {
            fprintf(stderr, "decode failed: %s\n", ingot_strerror(st)); return 1;
        }
        if (write_ppm(argv[3], rgb, w, h)) { free(rgb); return 1; }
        printf("decoded %dx%d\n", w, h);
        free(rgb);
        return 0;
    }

    if (!strcmp(argv[1], "info") && argc >= 3) {
        uint8_t *data = NULL;
        size_t size;
        int w, h;
        ingot_status st;
        if (read_file(argv[2], &data, &size)) return 1;
        st = ingot_probe(data, size, &w, &h);
        if (st != INGOT_OK) {
            fprintf(stderr, "%s\n", ingot_strerror(st)); free(data); return 1;
        }
        printf("%dx%d  q%d  group %d  groups %u  chroma %s  file %lu bytes\n",
               w, h, data[6], 1 << data[7],
               (unsigned)(data[16] | (data[17] << 8) | (data[18] << 16) |
                          ((unsigned)data[19] << 24)),
               (data[5] & 0x02) ? "4:2:0" : "4:4:4",
               (unsigned long)size);
        free(data);
        return 0;
    }

    if (!strcmp(argv[1], "rt") && argc >= 3) {
        uint8_t *rgb = NULL, *enc = NULL, *dec = NULL;
        int w, h, w2, h2, max_diff;
        size_t esize, n;
        unsigned long long sq;
        ingot_encode_options opt;
        ingot_status st;

        if (read_ppm(argv[2], &rgb, &w, &h)) return 1;
        ingot_encode_options_default(&opt);
        if (argc >= 4) opt.quality = atoi(argv[3]);
        if (argc >= 5) opt.group_log2 = atoi(argv[4]);
        if (argc >= 6) opt.subsample = atoi(argv[5]);

        st = ingot_encode(rgb, w, h, &opt, &enc, &esize);
        if (st != INGOT_OK) {
            fprintf(stderr, "encode failed: %s\n", ingot_strerror(st));
            free(rgb); return 1;
        }
        st = ingot_decode(enc, esize, &dec, &w2, &h2);
        if (st != INGOT_OK) {
            fprintf(stderr, "decode failed: %s\n", ingot_strerror(st));
            free(rgb); free(enc); return 1;
        }
        if (w != w2 || h != h2) {
            fprintf(stderr, "size changed\n");
            free(rgb); free(enc); free(dec); return 1;
        }

        n = (size_t)w * (size_t)h * 3;
        diff_stats(rgb, dec, n, &max_diff, &sq);
        {
            double mse = (double)sq / (double)n;
            double psnr = (mse > 0.0) ? 10.0 * log10(255.0 * 255.0 / mse) : 999.0;
            printf("%dx%d q%-2d group %-4d %s | %8lu B (%6.2f%%) | maxdiff %3d | PSNR %6.2f dB\n",
                   w, h, opt.quality, 1 << opt.group_log2,
                   opt.subsample ? "4:2:0" : "4:4:4",
                   (unsigned long)esize, 100.0 * (double)esize / (double)n,
                   max_diff, psnr);
        }
        free(rgb); free(enc); free(dec);
        return 0;
    }

    usage();
    return 1;
}
