# -*- coding: utf-8 -*-
"""인코더 매개변수 하나를 값마다 재서 고른다.

인코더만의 선택이라 규격이 바뀌지 않는다. 그래서 값을 골라도 옛 파일이 그대로 읽힌다.
"""
import subprocess, sys, os, re

CANDS = [87, 150, 250, 45]
SRC = ("src/transform.c src/color.c src/predict.c src/bitio.c "
       "src/rangecoder.c src/encode.c src/decode.c tools/cli.c")


NAME = os.environ.get("TUNE", "INGOT_QROUND")


def build(val):
    if os.path.exists("ingot.exe"):
        os.remove("ingot.exe")
    cmd = ("gcc -std=c99 -O2 -D%s=%d -w " % (NAME, val)) + SRC + " -o ingot -lm"
    r = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if r.returncode:
        print((r.stdout or b"").decode("utf-8", "replace")[-600:])
        sys.exit(1)


def measure(images):
    r = subprocess.run([sys.executable, "tools/bench.py", "--images", str(images), "--all"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = (r.stdout or b"").decode("utf-8", "replace")
    got = {}
    which = None
    for line in out.splitlines():
        if "PSNR 기준" in line:
            which = "psnr"
        elif "SSIM 기준" in line:
            which = "ssim"
        m = re.match(r"\s+vs (\w+)\s+([-+][\d.]+)%", line)
        if m and which:
            got[which + ":" + m.group(1)] = float(m.group(2))
    return got


def main():
    images = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    for v in CANDS:
        build(v)
        g = measure(images)
        print(NAME + " %2d | jpeg  P %+6.2f%%  S %+6.2f%%  |  webp  P %+6.2f%%  S %+6.2f%%"
              % (v, g.get("psnr:jpeg", 0), g.get("ssim:jpeg", 0),
                 g.get("psnr:webp", 0), g.get("ssim:webp", 0)))
        sys.stdout.flush()


main()
