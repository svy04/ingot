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
/* ingot.h — public C interface.
 *
 * ingot is a lossy still-image codec. Three calls: encode, probe, decode.
 * The library allocates the output buffer; you free() it.
 *
 * Guarantees the format itself makes (see SPEC.md for the normative rules):
 *   - Integer arithmetic only. Zero floating-point operations in the library,
 *     so the same input always produces the same bytes on any machine.
 *   - No dependencies beyond the C99 standard library.
 *   - Groups are independent, and the number of them is not derived from the
 *     thread count. An encoder may use any number of threads and the bytes
 *     come out identical.
 *   - The decoder returns an error code for malformed input. It does not
 *     abort, and it does not read past the buffer you hand it.
 *
 * Source comments are Korean; this header is English because it is what a
 * caller reads.
 */
#ifndef INGOT_H
#define INGOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. Only 0 means success.
 * Pass any of these to ingot_strerror() for a short English message. */
typedef enum {
    INGOT_OK              =  0,
    INGOT_ERR_ARG         = -1,  /* a pointer was NULL, or a size was 0 */
    INGOT_ERR_MEMORY      = -2,  /* allocation failed */
    INGOT_ERR_SIGNATURE   = -3,  /* not an ingot file */
    INGOT_ERR_VERSION     = -4,  /* format version this build cannot read */
    INGOT_ERR_RESERVED    = -5,  /* a reserved bit was set (reject, not skip) */
    INGOT_ERR_DIMENSION   = -6,  /* width/height is 0, or past the limits below */
    INGOT_ERR_TRUNCATED   = -7,  /* file ends before the header or table does */
    INGOT_ERR_TOC         = -8,  /* an entry in the group table points outside */
    INGOT_ERR_GROUP_COUNT = -9,  /* group count disagrees with width/height */
    INGOT_ERR_BITSTREAM   = -10  /* group data is malformed, or its hash fails */
} ingot_status;

/* Limits written into the format. A decoder rejects any header past them. */
#define INGOT_MAX_DIM       65535
#define INGOT_MAX_PIXELS    100000000u   /* 100 million */
#define INGOT_GROUP_LOG2_MIN 6
#define INGOT_GROUP_LOG2_MAX 11

/* Encoder settings. Call ingot_encode_options_default() first, then adjust. */
typedef struct {
    int quality;      /* 0..63. Higher is coarser and smaller. */
    int group_log2;   /* 6..10, log2 of the group edge. 0 selects the default 8.
                       * Smaller groups parallelize further but cost bits:
                       * against one group per image, 256 costs 2.4% and
                       * 64 costs 22.3% (8 photos, 6 quality steps). */
    int subsample;    /* 0 = 4:4:4 (default), 1 = 4:2:0.
                       * Never use 1 on screenshots or text — it costs 8.5 dB
                       * there. It also measures as a loss on photographs. */
} ingot_encode_options;

/* Fills opt with the defaults. Safe to call with any non-NULL pointer. */
void ingot_encode_options_default(ingot_encode_options *opt);

/* Encodes 8-bit RGB into an ingot file.
 *
 *   rgb       width*height*3 bytes, row-major, no padding between rows
 *   opt       may be NULL, which means the defaults
 *   out       receives a malloc'd buffer that you free()
 *   out_size  receives its length
 *
 * On failure *out is NULL and *out_size is 0. */
ingot_status ingot_encode(const uint8_t *rgb, int width, int height,
                          const ingot_encode_options *opt,
                          uint8_t **out, size_t *out_size);

/* Reads only the header, so you can size a buffer or reject a file cheaply.
 * Does not allocate. Either output pointer may be NULL if you don't need it. */
ingot_status ingot_probe(const uint8_t *data, size_t size,
                         int *width, int *height);

/* Decodes a whole file into 8-bit RGB.
 *
 *   rgb    receives a malloc'd width*height*3 buffer that you free()
 *
 * On failure *rgb is NULL. Malformed input always returns an error rather
 * than crashing or producing a different picture — the format carries a hash
 * of the group data for exactly that reason. */
ingot_status ingot_decode(const uint8_t *data, size_t size,
                          uint8_t **rgb, int *width, int *height);

/* A short English description of a status code. Never returns NULL,
 * including for values that are not valid ingot_status. */
const char *ingot_strerror(ingot_status s);

#ifdef __cplusplus
}
#endif
#endif /* INGOT_H */
