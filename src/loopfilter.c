/* loopfilter.c - 블록 경계 필터. 규격의 일부이며 디코더가 반드시 돌린다.
 *
 * 조각의 세 평면을 다 푼 뒤, 그 조각 사각형 안에서만 돈다. 조각 경계의 edge 는
 * 건드리지 않으므로 조각은 여전히 독립이다.
 *
 * 예측은 필터 전 화소를 본다 (HEVC·AV1 의 인트라 예측과 같다). 그래서 인코더의
 * 복원 버퍼는 안 걸러도 디코더와 어긋나지 않는다 — 인코더는 이 파일을 안 쓴다.
 */
#include "internal.h"
#ifdef INGOT_LF_CHECK
#include <stdio.h>
#include <stdlib.h>
/* 조각 사각형을 벗어나면 즉시 죽는다. 조각 독립을 시험으로 확인하려고 붙인다. */
static void lf_check(int x0, int x1, int y0, int y1, int gw, int gh)
{
    if (x0 < 0 || y0 < 0 || x1 >= gw || y1 >= gh) abort();

}
#endif

static int lf_clip3(int lo, int hi, int v)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 경계 하나의 네 줄을 편다. ds 는 경계를 가로지르는 걸음, dl 은 따라가는 걸음이다.
 * e 는 경계 오른쪽(아래쪽) 첫 화소 q0 를 가리킨다. */
static void lf_segment(uint8_t *e, int ds, int dl, int beta, int tc, int chroma)
{
    int l;

    if (chroma) {
        for (l = 0; l < 4; l++) {
            uint8_t *r = e + l * dl;
            int p1 = r[-2 * ds], p0 = r[-ds], q0 = r[0], q1 = r[ds];
            int d = lf_clip3(-tc, tc, ((((q0 - p0) << 2) + p1 - q1 + 4) >> 3));
            r[-ds] = (uint8_t)ingot_clamp_u8(p0 + d);
            r[0]   = (uint8_t)ingot_clamp_u8(q0 - d);
        }
        return;
    }

    {
        uint8_t *a = e, *b = e + 3 * dl;
        int dp0 = ingot_abs_i(a[-3 * ds] - 2 * a[-2 * ds] + a[-ds]);
        int dq0 = ingot_abs_i(a[2 * ds]  - 2 * a[ds]      + a[0]);
        int dp3 = ingot_abs_i(b[-3 * ds] - 2 * b[-2 * ds] + b[-ds]);
        int dq3 = ingot_abs_i(b[2 * ds]  - 2 * b[ds]      + b[0]);
        int side = (beta + (beta >> 1)) >> 3;
        int ep, eq, t2 = tc >> 1;

        /* 경계 양쪽이 이미 무늬면 두고 간다. 이 판정이 없으면 세부까지 뭉갠다. */
        if (dp0 + dq0 + dp3 + dq3 >= beta) return;
        ep = (dp0 + dp3) < side;
        eq = (dq0 + dq3) < side;

        for (l = 0; l < 4; l++) {
            uint8_t *r = e + l * dl;
            int p2 = r[-3 * ds], p1 = r[-2 * ds], p0 = r[-ds];
            int q0 = r[0], q1 = r[ds], q2 = r[2 * ds];
            int d = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
            if (ingot_abs_i(d) >= tc * 10) continue;   /* 진짜 모서리다 */
            d = lf_clip3(-tc, tc, d);
            r[-ds] = (uint8_t)ingot_clamp_u8(p0 + d);
            r[0]   = (uint8_t)ingot_clamp_u8(q0 - d);
            if (ep) r[-2 * ds] = (uint8_t)ingot_clamp_u8(p1 +
                lf_clip3(-t2, t2, (((p2 + p0 + 1) >> 1) - p1 + d) >> 1));
            if (eq) r[ds] = (uint8_t)ingot_clamp_u8(q1 +
                lf_clip3(-t2, t2, (((q2 + q0 + 1) >> 1) - q1 - d) >> 1));
        }
    }
}

/* 조각 하나의 한 평면. map 은 4x4 칸마다 경계 깃발이다. */
void ingot_loopfilter(uint8_t *pl, int pw, int ox, int oy, int gw, int gh,
                      const uint8_t *map, int ms, int base, int chroma)
{
    int x, y, g = INGOT_LF_GRID;
    int s = chroma ? ((base * INGOT_QW_CHROMA) >> 4) : base;
    int beta = (s * INGOT_LF_BETA) >> 5, tc = (s * INGOT_LF_TC) >> 7;

    if (beta > 64) beta = 64;
    if (tc > 24) tc = 24;
    if (tc < 1 || beta < 1) return;             /* 고품질이면 아예 안 돈다 */

    for (y = 0; y + 4 <= gh; y += 4)
        for (x = g; x + 3 <= gw - 1; x += g)
            if (map[(y >> 2) * ms + (x >> 2)] & INGOT_LF_V) {
#ifdef INGOT_LF_CHECK
                lf_check(x - 3, x + 2, y, y + 3, gw, gh);
#endif
                lf_segment(pl + (size_t)(oy + y) * pw + ox + x, 1, pw,
                           beta, tc, chroma); }

    for (y = g; y + 3 <= gh - 1; y += g)
        for (x = 0; x + 4 <= gw; x += 4)
            if (map[(y >> 2) * ms + (x >> 2)] & INGOT_LF_H) {
#ifdef INGOT_LF_CHECK
                lf_check(x, x + 3, y - 3, y + 2, gw, gh);
#endif
                lf_segment(pl + (size_t)(oy + y) * pw + ox + x, pw, 1,
                           beta, tc, chroma); }
}
