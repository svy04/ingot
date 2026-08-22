#!/bin/sh
# 확정판을 바깥 코덱과 전면 대조한다. 세 지표 · 두 소재 · 속도.
# 결과는 화면과 파일 양쪽에 남긴다.
set -e
cd "$(dirname "$0")/.."
gcc -std=c99 -O2 -Wall -Wextra -pedantic src/*.c tools/cli.c -o ingot -lm
echo "== 표준 사진 8장 =="
python tools/bench.py --images 8 --all | tee /tmp/final_std.txt
echo "== 우리 소재 12장 =="
python tools/bench.py --images 12 --data ourdata --all | tee /tmp/final_our.txt
echo "== 속도 =="
python tools/speed.py | tee /tmp/final_speed.txt
