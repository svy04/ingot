# -*- coding: utf-8 -*-
"""복원 필터를 배선한다 (손잡이, 기본 꺼짐).

깃발 바이트의 남는 비트 넷에 번호만 담으므로 비트 대가가 0 이다.
인코더는 자기 출력을 실제로 풀어서 열다섯 후보를 다 걸어 보고 제곱오차가
가장 작은 것을 고른다. 0 번(필터 없음)이 후보에 있으므로 손해가 날 수 없다.
"""
import io
import sys


def load(p):
    return io.open(p, encoding='utf-8', newline='').read().replace(
        chr(13) + chr(10), chr(10))


def save(p, s):
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('고침: ' + p)


def rep(s, old, new, what):
    if old not in s:
        sys.exit('닻 없음: %s ... %r' % (what, old[:70]))
    return s.replace(old, new, 1)


# ---- 손잡이와 선언 ----
p = 'src/internal.h'
s = load(p)
s = rep(s, '''/* ---- 블록 경계 필터 (loopfilter.c) ----''',
'''/* ---- 복원 필터 (restore.c) ----
 *
 * 블록 경계 필터 다음에 오는 두 번째 필터다. 계수를 실어 보내지 않고 미리
 * 정한 열다섯 개 중 고른 번호만 깃발 바이트의 남는 비트 넷에 담는다 --
 * **비트 대가가 0** 이다. 인코더가 자기 출력을 실제로 풀어서 제곱오차가
 * 가장 작은 것을 고르고, 0 번(필터 없음)이 후보에 있으므로 손해가 날 수
 * 없다. */
#ifndef INGOT_RESTORE
#define INGOT_RESTORE 0
#endif

void ingot_restore_rgb(uint8_t *rgb, int w, int h, int idx, uint8_t *tmp);
int ingot_restore_pick(const uint8_t *orig, const uint8_t *dec, int w, int h,
                       uint8_t *work, uint8_t *tmp);

/* ---- 블록 경계 필터 (loopfilter.c) ----''', '복원 선언')
save(p, s)

# ---- 디코더 ----
p = 'src/decode.c'
s = load(p)
s = rep(s, '''    if (d[5] & 0xF8u) return INGOT_ERR_RESERVED;   /* 비트 3~7 은 0 이어야 한다 */''',
'''#if INGOT_RESTORE
    if (d[5] & 0x80u) return INGOT_ERR_RESERVED;   /* 비트 7 은 0 이어야 한다 */
#else
    if (d[5] & 0xF8u) return INGOT_ERR_RESERVED;   /* 비트 3~7 은 0 이어야 한다 */
#endif''', '예약 비트 검사')
s = rep(s, '''    h->lf  = (d[5] & 0x04u) ? 1 : 0;   /* 비트 2 = 블록 경계 필터 */''',
'''    h->lf  = (d[5] & 0x04u) ? 1 : 0;   /* 비트 2 = 블록 경계 필터 */
    h->rest = (int)((d[5] >> 3) & 0x0Fu);   /* 비트 3~6 = 복원 필터 번호 */''',
        '복원 번호 읽기')
s = rep(s, '''    int quality, group_log2, gsize, sub, lf;''',
           '''    int quality, group_log2, gsize, sub, lf, rest;''', '머리말 구조체')
s = rep(s, '''    ingot_ycbcr_to_rgb(plane[0], cb, cr, h.width * h.height, rgb);''',
'''    ingot_ycbcr_to_rgb(plane[0], cb, cr, h.width * h.height, rgb);
#if INGOT_RESTORE
    if (h.rest) {
        size_t n3 = (size_t)h.width * (size_t)h.height * 3;
        uint8_t *tmp = (uint8_t *)malloc(n3);
        if (tmp) {
            ingot_restore_rgb(rgb, h.width, h.height, h.rest, tmp);
            free(tmp);
        }
    }
#endif''', '복원 적용')
save(p, s)
print('배선 완료')

# ---- 인코더 ----
p = 'src/encode.c'
s = load(p)
s = rep(s, '''    ingot_put32(buf + 20, ingot_hash32(buf + INGOT_HEADER_SIZE,
                                       data_off - INGOT_HEADER_SIZE));''',
'''    ingot_put32(buf + 20, ingot_hash32(buf + INGOT_HEADER_SIZE,
                                       data_off - INGOT_HEADER_SIZE));

#if INGOT_RESTORE
    /* 자기 출력을 실제로 풀어서 복원 필터를 고른다. 열다섯 후보를 다 걸어
     * 보고 원본과의 제곱오차가 가장 작은 것을 쓴다. 0 번(필터 없음)이
     * 후보에 있으므로 어떤 그림에서도 안 거른 판보다 나빠지지 않는다.
     * 번호는 깃발 바이트의 남는 비트 넷에 담으므로 파일이 안 커진다.
     * 해시는 머리말을 빼고 계산하므로 여기서 깃발을 고쳐도 어긋나지 않는다. */
    {
        uint8_t *drgb = NULL, *work = NULL, *tmp = NULL;
        int dw = 0, dh = 0;
        size_t n3 = (size_t)width * (size_t)height * 3;
        if (ingot_decode(buf, data_off, &drgb, &dw, &dh) == INGOT_OK &&
            dw == width && dh == height) {
            work = (uint8_t *)malloc(n3);
            tmp  = (uint8_t *)malloc(n3);
            if (work && tmp) {
                int pick = ingot_restore_pick(rgb, drgb, width, height,
                                              work, tmp);
                buf[5] = (uint8_t)(buf[5] | ((pick & 0x0F) << 3));
            }
        }
        free(work); free(tmp); free(drgb);
    }
#endif''', '복원 고르기')
save(p, s)
print('인코더 배선 완료')
