# -*- coding: utf-8 -*-
"""정수 DCT 표와 지그재그 순서를 만든다.

`src/transform.c` 에 든 표는 어디선가 가져온 숫자가 아니라 이 규칙으로 나온다.

  T[u][x] = round( c(u) * cos(pi*(2x+1)*u / 2n) * base * sqrt(n) )
  c(0) = sqrt(1/n),  c(u>0) = sqrt(2/n)

base 는 0 번 행의 값이고 크기마다 다르다(4 는 128, 8 은 91, 16·32 는 64).
크기가 달라도 같은 양자화 스텝이 같은 세기를 뜻해야 하므로, base 와 시프트를
짝지어 **상수 블록의 DC 가 직교 기준의 4.00 배**가 되게 맞춘다. 2026-08-23 에
16x16 만 이 배수가 2.00 이어서 나눔 판단이 죽어 있던 것을 고쳤다.

  python tools/gen_dct.py 16      # 소스의 표를 재현해 확인한다
  python tools/gen_dct.py 32      # 32 표와 지그재그를 낸다
"""
import math
import sys

BASE = {4: 128, 8: 91, 16: 64, 32: 64}


def table(n, base):
    T = []
    for u in range(n):
        row = []
        for x in range(n):
            c = math.sqrt(1.0 / n) if u == 0 else math.sqrt(2.0 / n)
            v = c * math.cos(math.pi * (2 * x + 1) * u / (2.0 * n))
            row.append(int(round(v * base * math.sqrt(n))))
        T.append(row)
    return T


def zigzag(n):
    """왼쪽 위에서 대각선을 번갈아 훑는 순서. 꼬리의 0 을 자르려면 필요하다."""
    order = []
    for s in range(2 * n - 1):
        cells = [(s - x, x) for x in range(n) if 0 <= s - x < n]
        if s % 2:
            cells.reverse()          # 홀수 대각선은 위에서 아래로
        order += [y * n + x for (y, x) in cells]
    return order


def emit(n):
    base = BASE[n]
    T = table(n, base)
    print("const int16_t ingot_dct%d[%d][%d] = {" % (n, n, n))
    for row in T:
        print("    { " + ", ".join("%4d" % v for v in row) + " },")
    print("};")
    print()
    z = zigzag(n)
    print("static const uint16_t ingot_zz%d[%d] = {" % (n, n * n))
    for i in range(0, len(z), 16):
        print("    " + ", ".join("%3d" % v for v in z[i:i + 16]) + ",")
    print("};")

    err = 0.0
    for a in range(n):
        for b in range(n):
            s = sum(T[a][k] * T[b][k] for k in range(n))
            want = base * base * n if a == b else 0
            err = max(err, abs(s - want) / float(base * base * n))
    print()
    print("/* 직교성 최대 오차 %.5f */" % err)


if __name__ == "__main__":
    emit(int(sys.argv[1]) if len(sys.argv) > 1 else 32)
