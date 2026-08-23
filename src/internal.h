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
/* internal.h - 내부 공통. 공개 인터페이스가 아니다. */
#ifndef INGOT_INTERNAL_H
#define INGOT_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ingot.h"

#define INGOT_SIG0 0x49 /* 'I' */
#define INGOT_SIG1 0x4E /* 'N' */
#define INGOT_SIG2 0x47 /* 'G' */
#define INGOT_SIG3 0x54 /* 'T' */

#define INGOT_VERSION      0
#define INGOT_HEADER_SIZE  24
/* 목차 한 칸의 크기. 8 이면 (오프셋, 길이), 4 면 길이만이다.
 * 오프셋은 앞 조각들의 길이 합이라 디코더가 이미 안다 — 인코더가 조각을
 * 마지막에 붙여 모으므로 파일 안에서 빈틈이 없기 때문이다. */
#ifndef INGOT_TOC4
#define INGOT_TOC4 1
#endif
#if INGOT_TOC4
#define INGOT_TOC_ENTRY    4
#else
#define INGOT_TOC_ENTRY    8
#endif
#define INGOT_BLOCK        8
#define INGOT_BLOCK16     16
/* 왜곡 자가 평면 밖 화소(가장자리 복제분)를 세지 않게 하는 손잡이.
 * 0 이면 지금과 같다. 인코더만의 판단이라 규격이 아니다. */

/* 경계 화소 가중을 「지금 견주는 블록의 가장자리」가 아니라 「16x16 묶음의
 * 가장자리」에만 준다. 0 이면 지금과 같다.
 * 지금은 16x16 통짜 후보가 31/256 칸만 두 배로 세고 8x8 넷 후보는 60/256 을
 * 두 배로 센다 — 같은 자리를 서로 다른 자로 재는 셈이라 나눔 쪽이 왜곡을
 * 10.1% 더 무겁게 받는다((256+60)/(256+31)). 인코더만의 판단이라 규격이 아니다. */

/* 4x4 는 8x8 과 같은 눈금을 쓴다. 그래야 같은 양자화 스텝이 같은 세기로 든다. */
#define INGOT_FWD_SHIFT4  14
#define INGOT_INV_SHIFT4  18
/* 가장 큰 블록과 가장 작은 블록. 16 -> 8 -> 4 로 나뉜다. */
/* 가장 큰 블록을 32x32 까지 넓힐지. 규격이 바뀐다.
 * AVIF 는 32·64 까지 쓰는데 우리는 16 에서 멈춰 있었다. 매끈한 자리에서는
 * 큰 변환 하나가 작은 변환 넷보다 싸다. 확률표를 양쪽 다 따로 배워 재니
 * **-2.86 / -3.90 / -3.07%** 였다 (2026-08-23). 켜면 문맥 칸이 702 에서
 * 719 로 늘므로 표를 다시 배워야 한다. */
#ifndef INGOT_BLK32
#define INGOT_BLK32 1
#endif

/* 한 단계 더 넓혀 64x64 까지 쓸지. 규격이 바뀐다.
 * 32x32 가 이긴 뒤 크기별 선택 빈도를 세니 32x32 가 통짜로 남는 비율이
 * 품질 10 에서 52%, 40 에서 80% 였다. 큰 쪽일수록 안 나누는 경향이라
 * 한 단계 더 걸어 봤다. 켜면 문맥 칸이 719 에서 736 으로 늘어 표를 다시
 * 배워야 한다.
 *
 * **재서 이겼는데도 안 켠다.** -0.48 / -0.45 / -0.83% 인데 인코딩이
 * 7.43 -> 15.95 초(2.15배), 디코딩이 0.322 -> 0.544 초(1.69배)다.
 * 16->32 는 -2.86 / -3.90 / -3.07 을 인코딩 1.7배로 샀으니 값이 있었지만,
 * 32->64 는 -0.5% 대를 2.15배로 사는 셈이다. 디코딩이 1.7배 느려지는 것은
 * 쓰는 사람에게 그대로 간다. 인코더를 빠르게 만든 뒤 다시 건다
 * (2026-08-23). */
#ifndef INGOT_BLK64
#define INGOT_BLK64 0
#endif

/* 64 점 변환의 눈금. 상수 블록의 DC 가 직교 기준의 4.00 배가 되게 맞췄다.
 * FWD_PRE64 는 순변환 1단 뒤에 미리 내리는 비트다 -- 안 내리면 2단 누산이
 * int32 한계의 394% 라 넘친다. 3 이면 49.2% 로 떨어지고, 왕복 오차는 안
 * 내린 판과 같다(무작위 잔차 20 블록, 최대 6·제곱평균제곱근 1.43). */
#define INGOT_FWD_SHIFT64 16
#define INGOT_INV_SHIFT64 20
#define INGOT_FWD_PRE64   3

/* 32 점 변환의 눈금. 손잡이를 꺼도 정의해 둔다 -- 변환의 삼항식이 이름을
 * 언제나 참조하기 때문이다. n 이 32 일 때만 쓰인다.
 * 상수 블록의 DC 가 직교 기준의 4.00 배가 되게 맞췄다(16x16 과 같은 값이라
 * 같은 양자화 스텝이 같은 세기를 뜻한다). 무작위 잔차 60 블록 실측으로
 * 왕복 오차 최대 6·제곱평균제곱근 1.61 (16x16 은 5 와 1.27), 순변환 누산
 * 최대는 int32 한계의 4.1% 다. */
#define INGOT_FWD_SHIFT32 15
#define INGOT_INV_SHIFT32 19

#if INGOT_BLK64
#define INGOT_MAX_BLOCK   64
#elif INGOT_BLK32
#define INGOT_MAX_BLOCK   32
#else
#define INGOT_MAX_BLOCK   16
#endif
#ifndef INGOT_MIN_BLOCK
#define INGOT_MIN_BLOCK 4
#endif
#define INGOT_FWD_SHIFT8  14
#define INGOT_INV_SHIFT8  18
/* 세 크기가 같은 눈금을 쓴다. 2026-08-23 까지 16x16 만 15/17 이었다.
 *
 * 상수 잔차 100 을 넣으면 DC 가 4x4 는 1600, 8x8 은 3235, 16x16 은 3200 이다.
 * 직교 기준(400/800/1600)으로 나누면 배수가 4.00 / 4.04 / 2.00 이었다. 즉
 * 같은 양자화 스텝이 16x16 에서만 두 배 거칠게 들었다. 그래서 인코더가
 * 「16 을 넷으로 나눌까」를 견줄 때 두 후보가 서로 다른 세기의 양자화를 받아
 * 그 판단이 사실상 죽어 있었다. 왕복 오차가 8x8 의 두 배(최대 4 대 2)인
 * 것도 같은 이유다.
 *
 * 14/18 로 내려 셋을 맞추니 세 지표가 함께 올랐다: 같은 조건에서
 * -5.20 / -7.68 / -5.78 이 -7.46 / -9.11 / -6.87 이 된다. 디코더 줄은
 * 안 늘고 상수 두 개만 바뀐다. 비트스트림은 바뀐다. */
#ifndef INGOT_FWD_SHIFT16
#define INGOT_FWD_SHIFT16 14
#endif
#ifndef INGOT_INV_SHIFT16
#define INGOT_INV_SHIFT16 18
#endif

/* ---- 변환 (transform.c) ---- */
extern const int16_t ingot_dct4[4][4];
extern const int16_t ingot_dct8[8][8];
extern const int16_t ingot_dct16[16][16];
extern const uint16_t ingot_zz8[64];

const uint16_t *ingot_zigzag_of(int n);

/* ADST 손잡이. 1 이면 예측 모드에 맞춰 방향마다 DCT 대신 DST-VII 을 쓴다.
 * 0 이면 지금까지와 완전히 같은 바이트가 나온다 — tx 는 늘 0 이 된다.
 * 켜면 비트스트림이 바뀐다. 종류를 적는 비트는 없다: 디코더도 모드를 안다. */

#define INGOT_TX_VADST 1    /* 세로(열) 방향에 ADST */
#define INGOT_TX_HADST 2    /* 가로(행) 방향에 ADST */


/* n 은 4, 8, 16 이다. src 는 잔차 블록(행 우선, stride 지정).
 * tx 는 INGOT_TX_* 의 조합이다. ADST 가 꺼져 있으면 무시된다. */
void ingot_fdct(const int16_t *src, int src_stride, int16_t *dst, int n, int tx);
void ingot_idct(const int16_t *src, int16_t *dst, int dst_stride, int n, int tx);

/* ---- 인트라 예측 (predict.c) ---- */

/* 방향 예측 손잡이. 1 이면 각도 모드 넷이 붙어 모드가 여덟이 된다.
 * 0 이면 지금까지와 완전히 같은 바이트가 나온다 — 모드 기호의 문법도,
 * 시작 확률표도 갈라 두었다. 켜면 비트스트림이 바뀐다. */

#define INGOT_PRED_DC     0
#define INGOT_PRED_V      1
#define INGOT_PRED_H      2
#define INGOT_PRED_PLANE  3

/* 네 번째 모드를 평면 대신 「매끄러운 보간」으로 바꿀지. 규격이 바뀌지만
 * **비트 대가가 0 이다** -- 모드 수가 그대로라 기호도 그대로다.
 *
 * 지금 평면 모드는 위·왼쪽의 기울기를 반반으로 잘라 구한 거친 값으로
 * 한 평면을 편다. 매끄러운 보간은 화소마다 위·왼쪽·오른쪽끝·아래끝을
 * 거리로 섞는다. **-0.91 / -0.31 / -0.77%** (2026-08-23).
 *
 * 이 자리가 값진 이유는 대가가 0 이기 때문이다. 모드를 **늘리면** 기호가
 * 비싸져서 예측이 더 맞아도 그 대가를 못 갚는다 -- 방향 예측을 여섯 번
 * 재서 여섯 번 다 진 것이 그 때문이다. 모드를 안 늘리고 하나를 더 나은
 * 것으로 바꾸면 그 함정이 없다. */
#ifndef INGOT_SMOOTH
#define INGOT_SMOOTH 1
#endif

/* 세로·가로 모드의 첫 줄을 경계로 보정할지. 규격이 바뀌지만 비트 대가는
 * 역시 0 이다.
 *
 * 세로 모드는 위 행을 아래로 그대로 복사한다. 그런데 왼쪽 열도 이미
 * 아는 값이고, 블록의 왼쪽 끝은 그 값에 가까울 것이다. 복사한 값에
 * 「왼쪽 이웃이 위-왼쪽 모서리보다 얼마나 밝은가」를 더해 첫 몇 열을
 * 보정한다. 가로 모드는 반대다. HEVC·AV1 이 쓰는 방식이지만 모드를
 * 늘리는 것이 아니라 있는 모드를 다듬는 것이라 대가가 없다.
 *
 * **재서 뺐다: -0.06 / +0.11 / -0.01** (2026-08-23). SSIM 이 안 오르고
 * 나머지도 소수점 아래다. 매끄러운 보간이 이겼다고(-0.91 / -0.31 / -0.77)
 * 같은 갈래가 다 이기지는 않는다. 세로·가로 모드가 골라지는 자리는
 * 애초에 그 방향으로 결이 뚜렷한 곳이라, 반대쪽 이웃을 섞을 여지가
 * 적다고 본다(추정). */
#ifndef INGOT_EDGEFIX
#define INGOT_EDGEFIX 0
#endif

/* 색차의 넷째 모드를 「휘도에서 끌어오기」로 바꿀지. 규격이 바뀌지만
 * **비트 대가가 0** 이다 -- 모드 수가 그대로이고 기울기도 안 신호한다.
 *
 * 같은 자리의 휘도는 색차보다 먼저 복원되므로 디코더도 이미 안다. 색차가
 * 휘도와 함께 밝아지고 어두워지는 자리에서는 그 관계를 쓰면 예측이 훨씬
 * 잘 맞는다. 기울기와 절편은 위 행과 왼쪽 열의 (휘도, 색차) 짝을 최소제곱
 * 으로 맞춰 뽑는다. 4:4:4 라 휘도와 색차가 같은 자리에 있어 내려받기가
 * 필요 없다. 휘도는 매끄러운 보간 그대로다.
 *
 * **재서 크게 졌다: +77.81 / +13.01 / +72.52** (2026-08-23). 비트는 5%
 * 아끼는데 화질이 무너진다 -- Big_Easy_chair 품질 20 에서 8.52 dB 를
 * 잃는다. 색이 단조로운 그림에서 이웃 휘도가 평평하면 최소제곱 기울기의
 * 분모가 작아 값이 튀고, 그 튄 기울기를 블록 전체에 곱하기 때문이라고
 * 본다(추정). 쓰려면 기울기가 못 미더울 때 물러설 길이 필요하다. */
#ifndef INGOT_CFL
#define INGOT_CFL 0
#endif
#define INGOT_PRED_COUNT  4

/* ---- 예측 모드 -> 변환 종류 ----
 * 예측이 시작된 가장자리에서 멀어지는 방향에 ADST 를 쓴다. 세로 예측이면
 * 위에서 아래로 잔차가 자라므로 세로에 ADST, 가로 예측이면 가로에 ADST 다.
 * 평면·대각(모드 4)은 두 가장자리를 다 쓰므로 양쪽에 준다. 각도 모드는 주
 * 참조가 위면 세로, 왼쪽이면 가로다.
 *
 * 배정은 손잡이로 갈아 낄 수 있게 두었다 (재서 정하려고).
 *   1  DC없음 / V세로 / H가로 / 평면양쪽 / 4양쪽 / 5,6세로 / 7가로   (VP9 대응)
 *   2  모드 4 도 세로로 (주 참조가 위라서)
 *   3  V·H 에만 준다 (평면·각도는 DCT)
 *   4  DC 를 뺀 전부 양쪽
 *   5  1 번의 세로·가로를 뒤집은 것. 이득이 아니라 **계기 점검**이다 —
 *      뒤집은 쪽이 더 좋으면 내가 방향을 반대로 붙인 것이다
 *   6  3 번의 세로·가로를 뒤집은 것. 5 번과 같은 계기인데 평면 모드를 빼서
 *      방향만 남긴다 */

static inline int ingot_tx_of_mode(int mode)
{
    (void)mode;
    return 0;
}

/* 실제로 담아 보는 모드 수. 나머지는 잔차 절대합 순위에서 걸러진다.
 * 인코더만의 선택이라 규격이 아니다.
 *
 * 2 였다가 4(전부)로 올렸다(2026-08-23). 절대합 순위는 담는 값을 모르므로
 * 자주 틀린다 — 잔차가 조금 큰 모드가 확률 모델에서 훨씬 싸게 담기는 일이
 * 흔하다. 넷을 다 담아 보니 세 지표가 함께 좋아졌다: PSNR -2.13%, SSIM
 * -1.68%, SSIMULACRA2 -1.80%. 이 값은 인코딩 시간과 맞바꾸는 손잡이다. */
/* 분할 비트의 값을 후보마다 실제로 매길지. 0 이면 상수 한 비트로 친다.
 * 인코더만의 판단이라 규격이 아니다. */
/* 조각마다 나가던 첫 0 바이트를 안 낼지. 규격이 바뀐다. */
/* 경계 가중을 그림 안 16 격자로 매길지. 0 이면 블록의 끝 열·행이다.
 * 블록 기준이면 나눔 후보가 더 많은 화소를 가중받아 자가 후보마다 다르다. */
/* 기본 조각 한 변의 로그2. 9 면 512 다.
 * 첫 커밋(08-21)에서 256 으로 정하고 한 번도 다시 안 정했다. 그때 믿던
 * 조각 대가는 나중에 21 배 틀린 것으로 밝혀졌으므로 다시 쟀고, 512 가
 * -1.02 / -1.48 / -0.79% 로 세 지표를 함께 올렸다 (2026-08-23).
 * 대가는 병렬 알갱이다. 968x1188 그림에서 조각이 20 개에서 6 개로 줄어
 * 코어를 그만큼 덜 쓴다. 규격이 아니라 인코더의 기본값이므로,
 * 병렬이 급한 쪽은 INGOT_GROUP 으로 256 을 그대로 쓸 수 있다. */
#ifndef INGOT_GROUP_DEFAULT
#define INGOT_GROUP_DEFAULT 9
#endif

/* 경계 가중을 그림 안 16 격자로 매긴다. 블록의 끝 열·행으로 매기면
 * 16x16 을 넷으로 나눌 때 8x8 마다 끝이 생겨 가중받는 화소가 늘고, 그러면
 * 통짜 후보와 나눔 후보를 서로 다른 자로 재게 된다. 후보의 모양이 자를
 * 바꾸면 그 비교는 공정하지 않다. -0.13 / -0.03 / -0.02% (2026-08-23).
 * 인코더만의 판단이라 규격이 아니다. */
#ifndef INGOT_EDGE_GRID
#define INGOT_EDGE_GRID 1
#endif

/* 조각마다 나가던 첫 0 바이트를 안 낸다.
 *
 * 레인지 코더는 시작할 때 캐시가 하나 차 있다고 보므로 첫 밀어내기에서
 * 언제나 0 한 바이트가 먼저 나갔고, 디코더는 다섯 바이트를 읽어 그중 첫
 * 바이트를 32비트 code 밖으로 밀어 버렸다. 조각마다 1 바이트를 그냥 버린
 * 셈이다. 조각이 많을수록 커진다 — 품질 63 에서 파일의 0.46% 였다.
 *
 * 첫 밀어내기에만 표시를 두고 건너뛴다. 그때는 캐시가 비어 있고 자리올림도
 * 없어 0xFF 대기 갈래를 안 깬다. -0.06 / -0.08 / -0.05% (2026-08-23). */
#ifndef INGOT_NOLEAD
#define INGOT_NOLEAD 1
#endif

/* 조각 끝에서 나가던 마무리 다섯 바이트 중 필요 없는 꼬리를 자를지.
 * 규격이 바뀐다. 마무리 직전에 low 를 2^24 배수로 올림하면 아래 세 바이트가
 * 0 이 되는데, 올린 양이 range 보다 작으므로(정규화 뒤 range >= 2^24)
 * 그 값은 여전히 인코더가 뜻한 구간 안이다. 그렇게 나온 0 꼬리를 안 낸다.
 * 디코더는 조각 끝을 넘겨 읽을 때 이미 0 으로 채우고 있었으므로 같은 값을
 * 읽는다 -- 다만 그것을 손상으로 치던 표시를 꼬리 길이만큼 봐준다.
 * -0.13 / -0.34 / -0.65% (2026-08-23). */
#ifndef INGOT_TAILTRIM
#define INGOT_TAILTRIM 1
#endif

/* 블록마다 부호 하나를 안 적고 계수 절댓값 합의 홀짝으로 알릴지.
 * 규격이 바뀐다. 부호는 파일의 15~21% 인데 이웃 부호로 문맥을 잡아도
 * 평균 0.9993 비트라 모델로는 못 줄인다(2026-08-23 실측). 그래서 모델을
 * 붙이는 대신 아예 안 적는 쪽으로 간다. 감추는 자리는 마지막 비영
 * 계수이고, 홀짝이 안 맞으면 계수 하나를 +-1 옮겨 맞춘다. */
#ifndef INGOT_SIGNHIDE
#define INGOT_SIGNHIDE 0
#endif

/* 부호를 감출 최소 계수 개수(last). 짧은 블록은 +-1 옮길 자리가 적어
 * 왜곡 대가가 아낀 한 비트보다 크다. */
#ifndef INGOT_SH_MIN
#define INGOT_SH_MIN 4
#endif

#ifndef INGOT_MODE_TRIALS
#define INGOT_MODE_TRIALS 4
#endif

/* 통째로 담는 값이 이 비트 수보다 싸면 나누는 쪽을 아예 재지 않는다.
 * 0 이면 언제나 둘 다 잰다. 인코더만의 선택이라 규격이 아니다. */
#ifndef INGOT_SPLIT_SKIP
#define INGOT_SPLIT_SKIP 24
#endif

/* 그 문턱을 블록 넓이에 맞출지. 인코더만의 판단이라 규격이 아니다.
 *
 * cost_whole 에는 화소 n*n 개의 왜곡이 들어 있는데 문턱은 상수다. 그래서
 * 같은 값이 8x8 에는 헐겁고 16x16 에는 빡빡해서, 크기마다 다른 비율로
 * 나눔 후보를 안 재고 버린다. 켜면 16x16 을 기준으로 넓이에 비례시킨다:
 * SPLIT_SKIP * n * n / 256. 「크기가 바뀌면 뜻이 바뀌는 값」의 네 번째
 * 자리다 -- 앞의 셋(변환 눈금, 경계 가중, 고주파 가중)은 전부 이겼다. */
#ifndef INGOT_SPLIT_SKIP_AREA
#define INGOT_SPLIT_SKIP_AREA 0
#endif

/* 시험에서 내린 판단을 적어 두고 실제 인코딩에서 그대로 재생할지.
 * 인코더만의 일이라 규격이 아니고, 나오는 바이트도 안 바뀐다.
 *
 * 지금은 같은 나무를 두 번 돈다 -- 값을 재려고 한 번, 이긴 쪽을 쓰려고
 * 또 한 번이다. 깊이 한 단계마다 중복이 곱해져서 잔차 인코딩 횟수가
 * 8x8 은 45, 16x16 은 365, 32x32 는 2,925, 64x64 는 23,405 회가 된다.
 *
 * 시험이 내린 판단은 실제 인코딩이 내릴 판단과 정확히 같다. 확률 모델도
 * 복원 화소도 시험 전으로 되돌린 뒤 실제 인코딩을 하기 때문이다. 그래서
 * 적어 두고 재생해도 바이트가 안 바뀌고, 골든 해시가 그것을 증명한다.
 * 재생하면 횟수가 25 / 105 / 425 / 1,705 회로 준다.
 *
 * 실측: 인코딩 8.47 -> 4.54 초(1.86배), **나오는 바이트는 그대로다.**
 * 그림 넷 x 품질 여섯, 스물네 건 전부 해시가 같다 (2026-08-23). */
#ifndef INGOT_PLAN
#define INGOT_PLAN 1
#endif

#if !INGOT_PLAN
/* 손잡이를 꺼도 형은 있어야 서명이 하나로 유지된다. */
typedef struct { int dummy; } ingot_plan;
#endif

#if INGOT_PLAN
/* 한 덩어리의 판단을 담는 자리. 64x64 까지 341 칸이면 되고 여유를 둔다.
 * 칸마다 -1 이면 「넷으로 나눔」, 0..3 이면 「통째로, 이 예측 모드」다. */
#define INGOT_PLAN_MAX 512
typedef struct {
    int16_t buf[INGOT_PLAN_MAX];
    int len;        /* 적은 칸 수 */
    int pos;        /* 재생하며 읽는 자리 */
    int replay;     /* 0 이면 적는 중, 1 이면 재생 중 */
} ingot_plan;
#endif

typedef struct {
    int n;                      /* 블록 한 변. 4 부터 INGOT_MAX_BLOCK 까지 */
    int top[INGOT_MAX_BLOCK], left[INGOT_MAX_BLOCK], topleft;
    int has_top, has_left, has_topleft;
#if INGOT_CFL
    /* 색차를 담을 때만 채운다. 같은 자리의 휘도 복원값이다. */
    const uint8_t *luma;    /* 휘도 평면. 색차가 아니면 NULL */
    int luma_stride, bx, by, pw, ph;
#endif
} ingot_neighbors;

/* recon 은 지금까지 복원된 평면이다. gx0·gy0 는 조각의 원점이라
 * 그보다 위·왼쪽은 이웃으로 쓰지 않는다. */
/* 이 블록이 매끈한 자리인지 결이 많은 자리인지 이웃만 보고 잰다.
 * 위 행과 왼쪽 열의 평균 |2차 차분| 이다. 이미 복원된 화소만 보므로
 * 디코더가 같은 값을 구한다. 돌려주는 것은 양자화 세기 배수(16 분모)로,
 * 매끈하면 16 보다 크고(거칠게) 결이 많으면 작다(곱게). */
static inline int ingot_aq_mul(const ingot_neighbors *nb)
{
#if INGOT_AQ
    int i, n = nb->n, s = 0, cnt = 0;
    if (nb->has_top)
        for (i = 1; i < n - 1; i++) {
            int d = 2 * nb->top[i] - nb->top[i - 1] - nb->top[i + 1];
            s += d < 0 ? -d : d; cnt++;
        }
    if (nb->has_left)
        for (i = 1; i < n - 1; i++) {
            int d = 2 * nb->left[i] - nb->left[i - 1] - nb->left[i + 1];
            s += d < 0 ? -d : d; cnt++;
        }
    if (cnt == 0) return 16;
    s /= cnt;
    if (s <= INGOT_AQ_LO) return 16 + INGOT_AQ_STR;
    if (s >= INGOT_AQ_HI) return 16 - INGOT_AQ_STR;
    return 16;
#else
    (void)nb;
    return 16;
#endif
}

void ingot_gather_neighbors(const uint8_t *recon, int pw, int ph,
                            int bx, int by, int gx0, int gy0, int n,
                            ingot_neighbors *nb);
void ingot_predict(const ingot_neighbors *nb, int mode, int16_t *pred);

/* ---- 색 (color.c) ---- */
void ingot_rgb_to_ycbcr(const uint8_t *rgb, int count,
                     uint8_t *y, uint8_t *cb, uint8_t *cr);
void ingot_ycbcr_to_rgb(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                     int count, uint8_t *rgb);

/* ---- 양자화 ---- */
/* 품질 번호를 양자화 스텝으로 옮기는 배수(16 분모).
 *
 * 16 이었는데 32 로 올렸다. 같은 날 16x16 변환 눈금을 다른 둘과 맞추면서
 * 계수가 두 배 커졌기 때문이다. 그대로 두면 같은 품질 번호가 훨씬 고화질을
 * 뜻하게 되어(q20 이 옛 q10 쯤) 사용자 기대도 벤치 곡선도 어긋난다.
 *
 * 32 로 맞추니 옛 눈금과 거의 정확히 겹친다 — Baruch q10 에서 2.104 bpp /
 * 30.80 dB 대 옛 판 2.105 bpp / 30.68 dB 다. 같은 비트에 화질이 0.12 dB
 * 높고, 눈금은 그대로다. */
#ifndef INGOT_QSCALE
#define INGOT_QSCALE 32
#endif

static inline int ingot_qstep(int quality)
{
    if (quality < 0) quality = 0;
    if (quality > 63) quality = 63;
    /* 낮은 비트레이트까지 닿아야 다른 포맷과 같은 구간에서 견줄 수 있다.
     * 2026-08-21: 1~127 범위로는 곡선이 안 겹쳐 BD-rate 를 절반도 못 쟀다. */
    /* 눈금 배수. 16 이 예전 값이다. 변환 눈금을 바꾸면 같은 번호가 다른
     * 화질을 뜻하게 되므로 여기서 되돌린다. 16 분모다. */
    return 1 + (quality * 2 + (quality * quality) / 16)
             * INGOT_QSCALE / 16;
}

#ifndef INGOT_QROUND
#define INGOT_QROUND 6      /* 16 분모. 8 = 그냥 반올림, 작을수록 0 쪽으로 기운다.
                             * 5 였다가 6 으로 올렸다. 인코더의 자 두 곳(머리말 비트
                             * 눈금, λ 정밀도)을 고친 뒤 다시 재니 6 이 나았다:
                             * PSNR -26.8% → -26.9%, SSIMULACRA2 -16.7% → -17.3%,
                             * WebP 대비 -9.5% → -9.8% (2026-08-22, 6장) */
#endif

static inline int16_t ingot_quantize(int coef, int step)
{
    /* 0 을 향해 자르되 반올림을 넣는다. 인코더 전용이라 규격이 아니다.
     * 반올림 자리를 절반보다 0 쪽으로 당기면(데드존) 작은 계수가 0 이 되어
     * 비트를 아낀다. 그 대가로 왜곡이 조금 는다 — 어디가 이득인지 잰다. */
    int half = (step * INGOT_QROUND) >> 4;
    int v = (coef >= 0) ? (coef + half) / step : -((-coef + half) / step);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static inline int ingot_dequantize(int level, int step)
{
    return level * step;
}

/* 계수 위치와 평면에 따라 양자화 스텝을 키운다.
 * 사람 눈은 고주파와 색차에 둔하므로 그쪽을 더 거칠게 버린다.
 * idx 는 블록 안 인덱스(v*8+u). plane 0 이 휘도, 1·2 가 색차.
 * INGOT_QW_ALPHA·INGOT_QW_CHROMA 는 실측으로 정한 규격 상수다. */
/* CHROMA(색차 배수)는 2026-08-21 실측으로 정했다
 * (tools/tune_quant.py, 표준 시험 이미지 4장). 20 이 두 지표 모두에서
 * 개선되는 유일한 값이다 (PSNR -0.58%, SSIM -1.51%). SSIM 만 보면 32 가
 * -8.95% 로 훨씬 좋지만 그 값은 PSNR 에서 +2.87% 라 지표 편향이 의심된다.
 * 보수적으로 고른다.
 *
 * ALPHA(고주파 기울기)의 근거는 아래 매크로 바로 위에 있다. 그 자리를 봐라. */
#ifndef INGOT_QW_ALPHA
/* 자리가 높을수록 양자화를 거칠게 한다. 사람 눈은 고주파 오차를 덜 본다.
 * 제곱 오차로만 재면 0 이 최선으로 나온다. 그런데 지각 지표(SSIMULACRA2)로
 * 재면 0 에서는 JPEG 에게도 진다(+5.6%). 1 은 세 지표 모두에서 JPEG 을
 * 이기는 가장 작은 값이고, 거기서 PSNR 로 WebP 도 -4.4% 앞선다.
 * 2 로 더 가면 SSIMULACRA2 는 나아지지만 PSNR·SSIM 을 더 내준다.
 * (2026-08-21, AOM 8장 실측) */
#define INGOT_QW_ALPHA 1
#endif
#ifndef INGOT_QW_CHROMA
#define INGOT_QW_CHROMA 24   /* 지각 카드. 위 ALPHA16 주석 참조 */
#endif

/* 같은 기울기를 1/16 눈금으로 적는 자리. 기본값은 INGOT_QW_ALPHA 를 그대로
 * 옮긴 것이라 지금 판과 비트까지 같다. 정수 ALPHA 는 1 과 2 사이를 못 재는데
 * 그 사이가 궁금할 때 이쪽을 쓴다 (예: -DINGOT_QW_ALPHA16=20 이면 1.25). */
#ifndef INGOT_QW_ALPHA16
/* 16 이었다가 20 으로 올렸다 (2026-08-23 밤). 지각을 사고 제곱 오차를
 * 파는 거래인데, 색차 가중 24·필터 세기 6 과 **함께** 걸면 대가가 절반이
 * 된다 -- 셋을 따로 재면 지각 -2.86 / PSNR +1.00 이지만 함께 재면
 * 지각 -2.76 / PSNR +0.48 이다. 셋이 같은 화소를 서로 다른 방향에서
 * 건드려서 손해가 안 더해진다고 본다(추정). */
#define INGOT_QW_ALPHA16 20
#endif

/* 고주파 가중의 자리를 블록 크기로 맞출지. 규격이 바뀐다.
 *
 * 지금은 u+v 를 그대로 쓴다. 그런데 같은 u 라도 뜻하는 주파수가 크기마다
 * 다르다 -- 8x8 의 u=4 와 16x16 의 u=8 이 같은 물리 주파수다. 그래서 지금
 * 판은 작은 블록일수록 고주파를 덜 거칠게 다룬다. 자리의 최댓값이 4x4 는
 * 6, 8x8 은 14, 16x16 은 30 이다.
 *
 * 그 차이는 나눔 판단에 곧장 들어간다. 인코더가 「16 을 넷으로 나눌까」를
 * 견줄 때 두 후보의 양자화 세기가 서로 다르므로 그 비교가 공정하지 않다.
 * 변환 눈금에서 똑같은 결함을 찾아 고친 적이 있다 (2026-08-23, 16x16 만
 * 배수가 2.00 이던 건). 이쪽도 같은 자리다.
 *
 * 자리를 16 눈금으로 옮긴다: (u+v)*16/n. 16x16 은 옛 판과 똑같고 나머지
 * 크기가 같은 자로 맞춰진다. **-3.02 / -2.46 / -2.01%** (2026-08-23) 로
 * 그날 가장 큰 한 수였다. */
#ifndef INGOT_QW_NORM
#define INGOT_QW_NORM 1
#endif

/* 자리마다 양자화 세기를 바꿀지. 규격이 바뀌지만 **비트 대가가 0** 이다 --
 * 세기를 이미 복원된 이웃에서 재므로 디코더가 같은 값을 구한다.
 *
 * libjxl 의 적응 양자화를 읽고 만들었다. 거기서 재는 값은 분산이 아니라
 * 이웃과의 차이(라플라시안)이고, 조절 대상은 λ 가 아니라 양자화 스텝
 * 자체이며, 방향은 **매끈한 자리를 거칠게, 결이 많은 자리를 곱게** 다.
 * 우리가 여섯 번 재서 여섯 번 진 것은 분산으로 λ 를 조절하는 것이었다 --
 * 재는 값도 조절 대상도 다르다. */
#ifndef INGOT_AQ
#define INGOT_AQ 0
#endif

/* 세기를 얼마나 흔들지. 16 분모다 (2 면 ±12.5%). */
#ifndef INGOT_AQ_STR
#define INGOT_AQ_STR 2
#endif

/* 매끈함·거칢을 가르는 문턱. 평균 |2차 차분| 이다. */
#ifndef INGOT_AQ_LO
#define INGOT_AQ_LO 4
#endif
#ifndef INGOT_AQ_HI
#define INGOT_AQ_HI 16
#endif

static inline int ingot_qstep_at(int base, int idx, int n, int plane)
{
    int u = idx % n, v = idx / n;
#if INGOT_QW_NORM
    int pos = ((u + v) * 16) / n;
#else
    int pos = u + v;
#endif
    int s = (base * (256 + INGOT_QW_ALPHA16 * pos)) >> 8;
    if (plane) s = (s * INGOT_QW_CHROMA) >> 4;
    return s < 1 ? 1 : s;
}

/* 위 함수에 블록마다의 세기 배수를 얹은 것. 배수는 16 분모다. */
static inline int ingot_qstep_aq(int base, int idx, int n, int plane, int aqm)
{
    int s = ingot_qstep_at(base, idx, n, plane);
#if INGOT_AQ
    s = (s * aqm) >> 4;
#else
    (void)aqm;
#endif
    return s < 1 ? 1 : s;
}

/* ---- 적응형 부호화 무리 ----
 * 자리마다 통계가 다르므로 무리를 나누고 무리마다 라이스 파라미터를 스스로 맞춘다.
 * 인코더와 디코더가 같은 순서로 같은 갱신을 하므로 상태가 어긋나지 않는다. */
/* 계수 무리를 블록 크기로도 가를 것인가. 0 이면 크기를 안 본다. */
#ifndef INGOT_CTX_BYSIZE
#define INGOT_CTX_BYSIZE 1
#endif

/* 계수 무리 = 크기 x 대역 4 x 이웃 INGOT_NBLEV x 평면 2, 거기에 last 6 */
/* 이웃 크기 합을 몇 단계로 나눌지. 1차원 이웃이던 때는 셋이 맞았는데,
 * 2차원으로 바꾸면서 합의 범위가 세 배로 늘었다.
 *
 * 5 였다가 7 로 올렸다 (2026-08-23). 예전에 7 을 재서 3.53%p 차이로
 * 기각했는데, 그것은 **확률표가 어긋난 채로 잰 가짜 벌점**이었다 --
 * 이 값을 바꾸면 문맥 칸 번호가 밀려 학습된 표가 엉뚱한 자리에 깔린다.
 * 표까지 새로 배워 다시 재니 **-0.09 / -0.05 / -0.05%** 로 이긴다.
 * 대가는 표가 719 -> 975 칸으로 커지는 것뿐이다(1.4 KB -> 1.9 KB). */
#ifndef INGOT_NBLEV
#define INGOT_NBLEV 7
#endif

#if INGOT_BLK64
#define INGOT_CTX_SIZES ((INGOT_CTX_BYSIZE == 2) ? 5 : (INGOT_CTX_BYSIZE == 1) ? 2 : 1)
#elif INGOT_BLK32
/* 32 를 켜면 크기 무리가 하나 는다. BYSIZE==1 은 「16 인가」였으니
 * 「16 이상인가」로 넓히고, BYSIZE==2 는 32 를 제 무리로 둔다. */
#define INGOT_CTX_SIZES ((INGOT_CTX_BYSIZE == 2) ? 4 : (INGOT_CTX_BYSIZE == 1) ? 2 : 1)
#else
#define INGOT_CTX_SIZES ((INGOT_CTX_BYSIZE == 2) ? 3 : (INGOT_CTX_BYSIZE == 1) ? 2 : 1)
#endif
#define INGOT_CTX_BASE  (INGOT_CTX_SIZES * INGOT_BANDS * INGOT_NBLEV * 2)
#if INGOT_BLK64
#define INGOT_CTX_LASTN 5
#elif INGOT_BLK32
#define INGOT_CTX_LASTN 4
#else
#define INGOT_CTX_LASTN 3
#endif
#define INGOT_CTX_COUNT (INGOT_CTX_BASE + INGOT_CTX_LASTN * 2)
/* 지그재그 자리와 평면으로 무리를 고른다.
 * 블록 크기가 달라도 같은 무리를 쓰도록 자리를 64칸 눈금으로 옮긴다. */
static inline int ingot_abs_i(int v) { return v < 0 ? -v : v; }

/* 이웃 계수의 크기 합을 INGOT_NBLEV 단계로 나눈다. 0 이면 뒤도 0 이기 쉽고,
 * 컸으면 뒤도 크기 쉽다. 이 한 가지가 확률 모델을 크게 뾰족하게 만든다.
 * 처음에는 지그재그 순서상 직전 둘을 세 단계로 나눴는데, 2026-08-22 에
 * 블록 안 자리 기준(왼쪽·위·왼쪽위 셋)으로 바꾸고 다섯 단계로 늘렸다. */
/* 자리 (u, v) 의 왼쪽·위·왼쪽위에 이미 담긴 계수의 크기를 모은다.
 * 셋 다 지그재그 순서상 반드시 앞이라 디코더도 같은 값을 안다.
 * lvl 은 자리별로 담긴 값을 그대로 둔 배열이다. */
static inline int ingot_nb2d(const int16_t *lvl, int idx, int n)
{
    int u = idx % n, v = idx / n, s = 0;
    if (u > 0)            s += lvl[idx - 1] < 0 ? -lvl[idx - 1] : lvl[idx - 1];
    if (v > 0)            s += lvl[idx - n] < 0 ? -lvl[idx - n] : lvl[idx - n];
    if (u > 0 && v > 0)   s += lvl[idx - n - 1] < 0 ? -lvl[idx - n - 1]
                                                    : lvl[idx - n - 1];
    return s;
}

static inline int ingot_ctx_level(int nb)
{
#if INGOT_NBLEV >= 7
    if (nb == 0) return 0;
    if (nb <= 1) return 1;
    if (nb <= 2) return 2;
    if (nb <= 4) return 3;
    if (nb <= 7) return 4;
    if (nb <= 12) return 5;
    return 6;
#elif INGOT_NBLEV >= 5
    if (nb == 0) return 0;
    if (nb <= 1) return 1;
    if (nb <= 3) return 2;
    if (nb <= 7) return 3;
    return 4;
#elif INGOT_NBLEV == 4
    if (nb == 0) return 0;
    if (nb <= 1) return 1;
    if (nb <= 4) return 2;
    return 3;
#else
    return (nb == 0) ? 0 : (nb <= 2) ? 1 : 2;
#endif
}

/* 계수 자리를 몇 대역으로 가를지. 「0 인가」 깃발은 품질 40 에서 파일의
 * 57% 를 쓰는데, 그 문맥을 가르는 이 경계가 손으로 고른 값이었다.
 * 4 였다가 8 로 늘렸다 (2026-08-23). 경계는 4 가 5·20, 6 이 2·5·10·20·40,
 * 8 이 1·2·5·10·20·40 이다. 저주파를 잘게 가를수록 벌지만 수익이 준다:
 * 6 이 -0.25 / -0.24 / -0.05, 8 이 그 위에서 다시 -0.09 / -0.08 / -0.13.
 * 대가는 확률표가 커지는 것뿐인데, 이 표는 **파일에 안 실린다** --
 * 디코더 안에 상수로 들어가므로 실행 파일만 커진다. */
#ifndef INGOT_BANDS
#define INGOT_BANDS 8
#endif

static inline int ingot_band_of(int kk)
{
#if INGOT_BANDS == 8
    if (kk == 0) return 0;
    if (kk <= 1) return 1;
    if (kk <= 2) return 2;
    if (kk <= 5) return 3;
    if (kk <= 10) return 4;
    if (kk <= 20) return 5;
    if (kk <= 40) return 6;
    return 7;
#elif INGOT_BANDS == 6
    if (kk == 0) return 0;
    if (kk <= 2) return 1;
    if (kk <= 5) return 2;
    if (kk <= 10) return 3;
    if (kk <= 20) return 4;
    return 5;
#elif INGOT_BANDS == 5
    if (kk == 0) return 0;
    if (kk <= 2) return 1;
    if (kk <= 5) return 2;
    if (kk <= 20) return 3;
    return 4;
#else
    return (kk == 0) ? 0 : (kk <= 5) ? 1 : (kk <= 20) ? 2 : 3;
#endif
}

static inline int ingot_ctx_index(int k, int n, int plane, int lvl)
{
    int kk = (n == 8) ? k : (k * 64) / (n * n);
    int band = ingot_band_of(kk);
#if INGOT_CTX_BYSIZE
    /* 같은 대역이라도 4x4 의 셋째 계수와 16x16 의 마흔째 계수는 분포가
     * 다르다. last 를 크기별로 가른 것이 크게 먹혔으므로 여기도 가른다. */
#if INGOT_CTX_BYSIZE == 2
    int sz = (n == 4) ? 0 : (n == 8) ? 1 : (n == 16) ? 2 : (n == 32) ? 3 : 4;
    return ((sz * INGOT_BANDS + band) * INGOT_NBLEV + lvl)
         + (plane ? (INGOT_CTX_BASE / 2) : 0);
#else
    int sz = (n >= 16) ? 1 : 0;
    return ((sz * INGOT_BANDS + band) * INGOT_NBLEV + lvl)
         + (plane ? (INGOT_CTX_BASE / 2) : 0);
#endif
#else
    return (band * INGOT_NBLEV + lvl) + (plane ? (INGOT_CTX_BASE / 2) : 0);
#endif
}

/* 블록 머리말(last) 전용 무리. */
/* last 는 블록 크기마다 분포가 아주 다르다. 4x4 는 대개 한둘이고
 * 16x16 은 수십까지 간다. 그래서 크기별로 무리를 갈라 둔다. */
static inline int ingot_ctx_last_n(int plane, int n)
{
    int sz = (n == 4) ? 0 : (n == 8) ? 1 : (n == 16) ? 2 : (n == 32) ? 3 : 4;
    return INGOT_CTX_BASE + sz * 2 + (plane ? 1 : 0);
}

/* ---- 레인지 코더 (rangecoder.c) ----
 * 골롬-라이스를 대신한다. 자리마다 "0인가"의 확률이 크게 달라서,
 * 그 확률을 그대로 쓰는 편이 한 비트씩 쓰는 것보다 낫다. */

/* 무리마다 두는 모델 수. 앞의 RC_FLAGS 개는 크기 깃발이고 그 뒤가
 * 지수 골롬 접두부다. 접두부는 자리가 깊어질수록 뒤쪽 모델을 쓰되
 * 마지막 모델을 나눠 쓴다.
 *
 * 깃발이 5 개(2026-08-23 확정)이므로 지금 배치는 이렇다.
 *   0..4 : k > 0 부터 k > 4 까지 다섯 깃발
 *   5..7 : 지수 골롬 접두부 셋
 *
 * 8 이었다가 13 으로 늘렸다 (2026-08-23). 접두부 모델이 코드에 셋으로
 * 박혀 있어서 무리당 모델을 늘려도 접두부가 못 받았는데, 그것을 풀고 재니
 * 계속 좋아졌다. 32x32 를 넣으면서 계수 자릿수가 열까지 깊어졌는데 그
 * 자리에 모델이 없었던 것이 원인이다.
 *   8 (옛값) 기준      13  -0.85 / -0.70 / -0.55  <- 골랐다
 *   10  -0.65 / -0.45 / -0.21     16  -0.86 / -0.73 / -0.57
 * 16 은 0.02%p 를 더 얻으려고 표를 327 칸(650 바이트) 더 키운다. 표는
 * 규격에 박히는 것이라 작을수록 좋으므로 13 에서 멈춘다.
 * 무리 뒤에 분할 비트와 모드 12개(앞 블록 모드 4 × 트리 3)를 더한다. */
#ifndef INGOT_PROB_PER_CTX
#define INGOT_PROB_PER_CTX 13
#endif
#define INGOT_PROB_SPLIT   (INGOT_CTX_COUNT * INGOT_PROB_PER_CTX)
#if INGOT_BLK64
#define INGOT_SPLIT_LEVELS 4              /* 64->32, 32->16, 16->8, 8->4 */
#elif INGOT_BLK32
#define INGOT_SPLIT_LEVELS 3                        /* 32->16, 16->8, 8->4 */
#else
#define INGOT_SPLIT_LEVELS 2                        /* 16->8 과 8->4 */
#endif
#define INGOT_PROB_MODE    (INGOT_PROB_SPLIT + INGOT_SPLIT_LEVELS)
/* 나눔 비트의 모델 자리. 큰 크기부터 0 번이다. */
#if INGOT_BLK64
#define INGOT_SPLIT_IDX(n) ((n) == 64 ? 0 : (n) == 32 ? 1 : (n) == 16 ? 2 : 3)
#elif INGOT_BLK32
#define INGOT_SPLIT_IDX(n) ((n) == 32 ? 0 : (n) == 16 ? 1 : 2)
#else
#define INGOT_SPLIT_IDX(n) ((n) == 16 ? 0 : 1)
#endif
/* 모드 비트는 앞 블록의 모드를 문맥으로 쓴다. 모드가 넷이므로 3 자리씩 넷.
 *
 * 방향 예측을 켜면 모드가 여덟이라 이 배치를 그대로 못 쓴다. 앞 블록 모드를
 * 문맥으로 두면 8 x 7 = 56 자리가 되어 자리마다 표본이 너무 얇아진다. 그래서
 * 「앞 블록과 같은가」한 비트로 그 상관을 먼저 먹고, 아니면 3단 트리로 여덟
 * 중 하나를 적는다. 둘 다 블록 크기(4·8·16)로만 갈린다 — 3 + 21 = 24 자리다. */
/* 쓸 수 있는 모드가 둘뿐인 자리(조각의 맨 위 줄·맨 왼쪽 줄)는 한 비트로
 * 끝난다. 그 비트의 뜻이 네 모드일 때의 첫 비트와 다르므로 자리를 따로 둔다. */
/* ---- 블록 경계 필터 (loopfilter.c) ----
 * 조각을 다 푼 뒤 그 조각 안에서만 돈다. 예측은 필터 전 화소를 보므로
 * 인코더는 이 파일을 안 쓴다. 손잡이는 재서 정한다. */
#define INGOT_LF_V 1        /* 이 4x4 칸의 왼쪽이 블록 경계다 */
#define INGOT_LF_H 2        /* 이 4x4 칸의 위쪽이 블록 경계다 */
#ifndef INGOT_LF
#define INGOT_LF 1          /* 0 이면 인코더가 깃발을 안 켠다 */
#endif
#ifndef INGOT_LF_GRID
#define INGOT_LF_GRID 8     /* 8 이면 8 배수 자리만 편다. 4 면 4x4 경계까지 */
#endif
/* 아래 둘은 재서 정했다 (2026-08-23, 표준 사진 8장).
 *   판정 / 세기 :  8/16(첫 값) -6.89 -7.50 -9.87
 *                 12/12        -6.99 -7.54 -9.91
 *                 16/8         -6.94 -7.48 -10.02   ← 골랐다
 *                 16/16        -7.10 -7.73 -9.66
 * 16/16 이 제곱 오차로는 가장 좋지만 지각 지표를 0.36%p 잃는다. 우리가 지고
 * 있는 축이 지각 쪽이라 그쪽을 산다. */
#ifndef INGOT_LF_BETA
#define INGOT_LF_BETA 16    /* beta = (step * BETA) >> 5. 평탄 판정 문턱 */
#endif
#ifndef INGOT_LF_TC
#define INGOT_LF_TC 6       /* tc = (step * TC) >> 7. 지각 카드 */
#endif

void ingot_loopfilter(uint8_t *pl, int pw, int ox, int oy, int gw, int gh,
                      const uint8_t *map, int ms, int base, int chroma);

#define INGOT_PROB_COUNT   (INGOT_PROB_MODE + 12)

#ifdef INGOT_BIT_STATS
/* 비트를 갈래별로 세는 장치. 진단용이라 평소 빌드에는 없다. */
enum {
    INGOT_BC_LAST = 0,   /* 마지막 비영 계수 자리 */
    INGOT_BC_ZERO,       /* 「0 인가」 깃발 */
    INGOT_BC_FLAG,       /* 크기 깃발 둘·셋 */
    INGOT_BC_EGP,        /* 지수 골롬 접두부 (모델 붙음) */
    INGOT_BC_EGS,        /* 지수 골롬 접미부 (반반) */
    INGOT_BC_SIGN,       /* 부호 (반반) */
    INGOT_BC_MODE,       /* 예측 모드 */
    INGOT_BC_SPLIT,      /* 블록 나눔 */
    INGOT_BC_COUNT
};
extern double ingot_bitstat[2][INGOT_BC_COUNT];   /* [휘도0/색차1][갈래] */
extern int ingot_bitcat, ingot_bitplane;
/* last-1 자리의 계수를 센다. [0]=그 자리 개수, [1]=그중 0 이 아닌 것.
 * 규격의 「확실한 깃발을 안 적는다」가 서 있는 근거라 다시 셀 수 있어야 한다
 * (tools/certain_flag.py). */
extern long ingot_certain[2];
void ingot_bitstat_dump(const char *path);
#endif


typedef struct {
    uint8_t *buf;
    size_t   cap, pos;
    uint64_t low;
    uint32_t range;
    int      cache;
    int64_t  cache_size;
    int      overflow;
    uint64_t bits;      /* 담은 값의 크기. 1/INGOT_BIT_UNIT 비트 단위 */
#if INGOT_NOLEAD
    int      lead;      /* 아직 첫 0 바이트를 안 건너뛰었으면 1 */
#endif
} ingot_rc_enc;

typedef struct {
    const uint8_t *buf;
    size_t   size, pos;
    uint32_t range, code;
    int      error;
} ingot_rc_dec;

/* 인코더가 자기 비트를 재는 자의 눈금.
 * 0 이면 지금까지의 128칸 표(1/16 비트), 1 이면 512칸 표(1/256 비트).
 * 규격이 아니라 인코더만의 자다 — 켜도 옛 파일이 그대로 읽힌다. */

/* 비트 값을 세는 눈금. 1 비트 = INGOT_BIT_UNIT 이다. */
#define INGOT_BIT_UNIT 16

uint32_t ingot_rc_price(uint16_t prob, int bit);
void ingot_prob_reset(uint16_t *p, int count);

void ingot_rc_enc_init(ingot_rc_enc *e, uint8_t *buf, size_t cap);
void ingot_rc_enc_bit(ingot_rc_enc *e, uint16_t *prob, int bit);
void ingot_rc_enc_bypass(ingot_rc_enc *e, uint32_t value, int nbits);
size_t ingot_rc_enc_finish(ingot_rc_enc *e);
void ingot_rc_put_uint(ingot_rc_enc *e, uint16_t *m, uint32_t k);
uint32_t ingot_rc_get_uint(ingot_rc_dec *d, uint16_t *m);
/* lo = 「값이 lo 이상인 것을 디코더도 이미 안다」. 그만큼 깃발을 건너뛴다. */
void ingot_rc_put_uint_from(ingot_rc_enc *e, uint16_t *m, uint32_t k, int lo);
void ingot_rc_put_int_from(ingot_rc_enc *e, uint16_t *m, int v, int lo);
uint32_t ingot_rc_get_uint_from(ingot_rc_dec *d, uint16_t *m, int lo);
int ingot_rc_get_int_from(ingot_rc_dec *d, uint16_t *m, int lo);

void ingot_rc_dec_init(ingot_rc_dec *d, const uint8_t *buf, size_t size);
int  ingot_rc_dec_bit(ingot_rc_dec *d, uint16_t *prob);
uint32_t ingot_rc_dec_bypass(ingot_rc_dec *d, int nbits);
int  ingot_rc_get_int(ingot_rc_dec *d, uint16_t *m);

/* 무리 번호로 모델 두 개의 자리를 얻는다. */
static inline int ingot_prob_of(int ctx)
{
    return ctx * INGOT_PROB_PER_CTX;
}


/* ---- 리틀엔디언 도우미 ---- */
static inline void ingot_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t ingot_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 목차와 조각 데이터를 한 줄로 훑어 32비트 값을 만든다 (FNV-1a).
 * 목적은 위조 방지가 아니라 우연한 손상 감지다. 산술 부호화는 어떤 비트열도
 * 그럴듯한 값으로 읽어 내므로, 이것이 없으면 망가진 파일이 조용히 딴 그림이
 * 된다 (2026-08-22 실측: 손상 300건 중 44건이 그림을 냈고 최악 11.55 dB). */
static inline uint32_t ingot_hash32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;      /* 0 은 "해시 없음" 으로 남겨 둔다 */
}

static inline int ingot_clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/* 색차 평면 크기. 서브샘플이면 올림해서 절반이다. */
static inline int ingot_chroma_dim(int v, int sub)
{
    return sub ? ((v + 1) >> 1) : v;
}

/* 조각 격자 계산. 두 값 모두 1 이상이다. */
static inline uint32_t ingot_groups_across(int width, int gsize)
{
    return (uint32_t)((width + gsize - 1) / gsize);
}

#endif /* INGOT_INTERNAL_H */
