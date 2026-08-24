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
/* deringe.c - 결을 따라 거는 잔물결 필터.
 *
 * 블록 경계 필터는 경계만 문지른다. 블록 안쪽에 생기는 잔물결(양자화로
 * 고주파가 잘려 모서리 둘레에 생기는 물결)은 그대로 남는다.
 *
 * 이 필터는 8x8 마다 **결이 어느 쪽으로 흐르는지**를 복원값에서 재고, 그
 * 결과 나란한 방향으로만 고른다. 결을 가로지르는 방향은 안 건드리므로
 * 모서리가 안 뭉개진다. **방향을 복원값에서 재므로 신호할 비트가 0** 이다.
 *
 * 고칠 양은 두 번 묶는다:
 *   - 이웃과의 차이를 세기 S 로 자르고, 차이가 클수록 0 으로 내린다(감쇠).
 *     결이 뚜렷한 자리는 안 건드린다는 뜻이다.
 *   - 화소가 움직일 수 있는 폭을 이웃과의 최대 차이 안으로 묶는다.
 *     그래야 저역 필터의 성질이 유지된다.
 *
 * AV1 의 CDEF (Constrained Directional Enhancement Filter) 를 읽고 만들었다
 * (Valin 외, "An Overview of Core Coding Tools in the AV1 Video Codec").
 * 원문 식은 다음과 같다:
 *
 *   y(i,j) = R( x(i,j) + g( sum w_mn * f(x(m,n) - x(i,j), S, D) ) )
 *
 * f 가 세기·감쇠로 차이를 자르는 함수이고 g 가 움직임 폭을 묶는 함수다.
 * 우리 판은 방향을 여덟이 아니라 넷으로 줄이고 5탭 한 줄만 쓴다.
 */
#include "internal.h"

#if INGOT_DERINGE

/* 방향 넷의 (dy, dx). 0 도(가로), 45 도, 90 도(세로), 135 도다. */
static const int dr_dir[4][2] = { {0, 1}, {1, 1}, {1, 0}, {1, -1} };

/* 차이를 세기 안으로 자른다. 차이가 세기를 넘으면 0 으로 내려간다 --
 * 진짜 모서리는 안 건드린다는 뜻이다. damp 는 내려가는 기울기다. */
static int dr_constrain(int diff, int s, int damp)
{
    int a = diff < 0 ? -diff : diff;
    int lim;
    if (s <= 0) return 0;
    lim = a - (a >> damp);
    if (lim > s) lim = s;
    if (lim > a) lim = a;
    return diff < 0 ? -lim : lim;
}

/* 8x8 칸에서 결이 흐르는 방향을 고른다. 그 방향으로 잰 1차 차분의 절대합이
 * 가장 작은 쪽이 결과 나란한 방향이다. */
static int dr_direction(const uint8_t *p, int stride, int w, int h)
{
    int best = 0, bestv = -1, d;
    for (d = 0; d < 4; d++) {
        int dy = dr_dir[d][0], dx = dr_dir[d][1];
        int y, x, s = 0, cnt = 0;
        for (y = 0; y < h; y++) {
            int ny = y + dy;
            if (ny < 0 || ny >= h) continue;
            for (x = 0; x < w; x++) {
                int nx = x + dx, t;
                if (nx < 0 || nx >= w) continue;
                t = (int)p[(size_t)ny * stride + nx]
                  - (int)p[(size_t)y * stride + x];
                s += t < 0 ? -t : t;
                cnt++;
            }
        }
        if (cnt == 0) continue;
        s = (s * 64) / cnt;              /* 칸 수가 달라도 견줄 수 있게 */
        if (bestv < 0 || s < bestv) { bestv = s; best = d; }
    }
    return best;
}

/* 한 평면을 거른다. src 는 거르기 전 사본이고 dst 에 쓴다.
 * map 은 블록 지도로, 잔차가 담긴 자리만 거른다 -- 빈 블록에는 잔물결이
 * 없고, 건드리면 평탄면만 흐려진다. */
void ingot_deringe(uint8_t *dst, const uint8_t *src, int pw, int ph,
                   int ox, int oy, int gw, int gh, int base, int chroma)
{
    int by, bx;
    int s = (base * INGOT_DERINGE_STR) >> 6;
    int damp = INGOT_DERINGE_DAMP;
    if (chroma) s = (s * INGOT_DERINGE_CHROMA) >> 4;
    if (s <= 0) return;
    if (s > 63) s = 63;

    for (by = 0; by < gh; by += 8) {
        for (bx = 0; bx < gw; bx += 8) {
            int w = gw - bx, h = gh - by, d, y, x;
            int dy, dx;
            if (w > 8) w = 8;
            if (h > 8) h = 8;
            d = dr_direction(src + (size_t)(oy + by) * pw + (ox + bx),
                             pw, w, h);
            dy = dr_dir[d][0]; dx = dr_dir[d][1];
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    int gy = oy + by + y, gx = ox + bx + x;
                    int c = (int)src[(size_t)gy * pw + gx];
                    int sum = 0, k, mx = 0, mn = 0, v;
                    /* 결과 나란한 방향으로 두 칸씩 본다. 가까운 쪽에 2,
                     * 먼 쪽에 1 을 준다. */
                    for (k = 1; k <= 2; k++) {
                        int wgt = (k == 1) ? 2 : 1, sgn;
                        for (sgn = -1; sgn <= 1; sgn += 2) {
                            int ny = gy + sgn * k * dy;
                            int nx = gx + sgn * k * dx;
                            int t;
                            if (ny < 0 || ny >= ph || nx < 0 || nx >= pw)
                                continue;
                            t = (int)src[(size_t)ny * pw + nx] - c;
                            if (t > mx) mx = t;
                            if (t < mn) mn = t;
                            sum += wgt * dr_constrain(t, s, damp);
                        }
                    }
                    /* 움직임 폭을 이웃과의 최대 차이 안으로 묶는다. */
                    v = (sum + (sum < 0 ? -4 : 4)) >> 3;
                    if (v > mx) v = mx;
                    if (v < mn) v = mn;
                    dst[(size_t)gy * pw + gx] = (uint8_t)ingot_clamp_u8(c + v);
                }
            }
        }
    }
}

#endif  /* INGOT_DERINGE */
