/* ingot — a lossy still-image codec.
 * Copyright (C) 2026 svy04
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* color.c - RGB 와 YCbCr 사이의 정수 변환.
 *
 * BT.601 계수를 16비트 고정소수점으로 옮긴 값이다. 이 상수도 규격의 일부다.
 * 되돌릴 수 없는 변환이다 — 색변환 자체에서 손실이 난다. v0 가 보장하는 것은
 * 인코더와 디코더가 같은 정수 결과를 낸다는 것뿐이다 (SPEC.md 「색변환」 절).
 */
#include "internal.h"

#define INGOT_CS 16
#define INGOT_CR (1 << (INGOT_CS - 1))

void ingot_rgb_to_ycbcr(const uint8_t *rgb, int count,
                     uint8_t *y, uint8_t *cb, uint8_t *cr)
{
    int i;
    for (i = 0; i < count; i++) {
        int r = rgb[i * 3 + 0];
        int g = rgb[i * 3 + 1];
        int b = rgb[i * 3 + 2];

        int yy = ( 19595 * r + 38470 * g +  7471 * b + INGOT_CR) >> INGOT_CS;
        int cbv = ((-11056 * r - 21712 * g + 32768 * b + INGOT_CR) >> INGOT_CS) + 128;
        int crv = (( 32768 * r - 27440 * g -  5328 * b + INGOT_CR) >> INGOT_CS) + 128;

        y[i]  = (uint8_t)ingot_clamp_u8(yy);
        cb[i] = (uint8_t)ingot_clamp_u8(cbv);
        cr[i] = (uint8_t)ingot_clamp_u8(crv);
    }
}

void ingot_ycbcr_to_rgb(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                     int count, uint8_t *rgb)
{
    int i;
    for (i = 0; i < count; i++) {
        int yy = y[i];
        int u = (int)cb[i] - 128;
        int v = (int)cr[i] - 128;

        int r = yy + ((  91881 * v + INGOT_CR) >> INGOT_CS);
        int g = yy - ((  22554 * u + 46802 * v + INGOT_CR) >> INGOT_CS);
        int b = yy + (( 116130 * u + INGOT_CR) >> INGOT_CS);

        rgb[i * 3 + 0] = (uint8_t)ingot_clamp_u8(r);
        rgb[i * 3 + 1] = (uint8_t)ingot_clamp_u8(g);
        rgb[i * 3 + 2] = (uint8_t)ingot_clamp_u8(b);
    }
}
