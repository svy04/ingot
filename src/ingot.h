/* codec.h - 공개 C 인터페이스 (규격 v0)
 *
 * 설계 원칙 (SPEC.md 참조):
 *   - 정수 연산만. 부동소수점 0개.
 *   - 외부 의존 0. 표준 C99만.
 *   - 조각(group)은 서로 독립. 스레드 수가 비트스트림에 실리지 않는다.
 *
 * 이름이 정해지면 INGOT_ 접두사와 시그니처를 함께 바꾼다.
 */
#ifndef INGOT_H
#define INGOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 반환 코드. 0만 성공이다. 디코더는 어떤 입력에도 abort 하지 않는다. */
typedef enum {
    INGOT_OK              =  0,
    INGOT_ERR_ARG         = -1,  /* 인자가 NULL 이거나 크기가 0 */
    INGOT_ERR_MEMORY      = -2,
    INGOT_ERR_SIGNATURE   = -3,  /* 시그니처 불일치 */
    INGOT_ERR_VERSION     = -4,
    INGOT_ERR_RESERVED    = -5,  /* 예약 비트가 1 */
    INGOT_ERR_DIMENSION   = -6,  /* 크기가 범위 밖 */
    INGOT_ERR_TRUNCATED   = -7,  /* 파일이 짧다 */
    INGOT_ERR_TOC         = -8,  /* 목차가 파일 밖을 가리킨다 */
    INGOT_ERR_GROUP_COUNT = -9,  /* 조각 수가 계산값과 다르다 */
    INGOT_ERR_BITSTREAM   = -10  /* 조각 안의 값이 규격을 벗어난다 */
} ingot_status;

/* 규격 한계. 디코더는 이 값을 넘는 헤더를 거절한다. */
#define INGOT_MAX_DIM       65535
#define INGOT_MAX_PIXELS    100000000u   /* 1억 화소 */
#define INGOT_GROUP_LOG2_MIN 6
#define INGOT_GROUP_LOG2_MAX 10

/* 인코딩 설정 */
typedef struct {
    int quality;      /* 0~63. 클수록 거칠고 작다 */
    int group_log2;   /* 6~10. 조각 한 변의 로그2. 0을 주면 기본값 8 */
    int subsample;    /* 0 = 4:4:4, 1 = 4:2:0. 화면·문자에는 0 을 쓴다 */
} ingot_encode_options;

void ingot_encode_options_default(ingot_encode_options *opt);

/* RGB 8비트 세 채널을 인코딩한다.
 *   rgb    : width*height*3 바이트, 행 우선
 *   out    : 호출자가 free() 한다. 성공했을 때만 채워진다.
 * 실패하면 *out 은 NULL 이고 *out_size 는 0 이다. */
ingot_status ingot_encode(const uint8_t *rgb, int width, int height,
                    const ingot_encode_options *opt,
                    uint8_t **out, size_t *out_size);

/* 헤더만 읽는다. 손상 입력을 미리 거를 때 쓴다. */
ingot_status ingot_probe(const uint8_t *data, size_t size,
                   int *width, int *height);

/* 디코딩한다. *rgb 는 호출자가 free() 한다. */
ingot_status ingot_decode(const uint8_t *data, size_t size,
                    uint8_t **rgb, int *width, int *height);

/* 오류 코드를 사람이 읽는 문자열로. 항상 NULL 이 아니다. */
const char *ingot_strerror(ingot_status s);

#ifdef __cplusplus
}
#endif
#endif /* INGOT_H */
