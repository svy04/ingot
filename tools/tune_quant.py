# -*- coding: utf-8 -*-
"""양자화 가중치 두 개(고주파 기울기, 색차 배수)를 실측으로 정한다.

과적합을 피하려고 훑기는 표준 시험 세트 앞쪽 이미지로만 하고,
고른 값은 나머지 이미지와 우리 소재로 따로 확인한다.

BD-rate 기준이라 "같은 화질에서 비트가 얼마나 주는가"를 본다.

주의: 이 환경은 새 이름의 실행 파일을 정책으로 막는다. 그래서 실행 파일 이름을
ingot 하나로 고정하고 조합마다 덮어쓴다.
"""
import os, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench import bd_rate, quality_of, ppm_pixels, sh, ssim_db

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "testdata")
BIN = os.path.join(ROOT, "ingot.exe" if os.name == "nt" else "ingot")
QS = [4, 12, 24, 40]
METRIC = "ssim" if "--ssim" in sys.argv else "psnr"


def build(alpha, chroma):
    cmd = ("gcc -std=c99 -O2 -w -DINGOT_QW_ALPHA=%d -DINGOT_QW_CHROMA=%d "
           "src/transform.c src/color.c src/bitio.c src/encode.c "
           "src/decode.c tools/cli.c -o ingot -lm" % (alpha, chroma))
    r = subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print("   빌드 실패:", (r.stderr or "")[:200])
    return r.returncode == 0


def curve(images, tmp):
    out = {}
    for src in images:
        n = ppm_pixels(src)
        pts = []
        for q in QS:
            enc = os.path.join(tmp, "t.igt")
            dec = os.path.join(tmp, "t.ppm")
            if sh([BIN, "enc", src, enc, str(q)])[0].returncode != 0:
                continue
            if sh([BIN, "dec", enc, dec])[0].returncode != 0:
                continue
            m = quality_of(src, dec)
            q = m["psnr"] if METRIC == "psnr" else ssim_db(m["ssim"])
            if q:
                pts.append((8.0 * os.path.getsize(enc) / n, q))
        if len(pts) >= 4:
            out[os.path.basename(src)] = pts
    return out


def score(base, cand):
    ds = [bd_rate(base[i], cand[i]) for i in sorted(set(base) & set(cand))]
    ds = [d for d in ds if d is not None]
    return sum(ds) / len(ds) if ds else None


def main():
    images = sorted(os.path.join(DATA, f) for f in os.listdir(DATA)
                    if f.endswith(".ppm"))[:4]
    tmp = tempfile.mkdtemp(prefix="ingot_tune_")
    print("훑기 이미지 %d장, 품질 %s, 지표 %s" % (len(images), QS, METRIC.upper()))

    if not build(0, 16):
        print("기준 빌드 실패"); return 1
    base = curve(images, tmp)
    if not base:
        print("기준 곡선 실패"); return 1
    print("기준(가중 없음) 준비 완료\n")

    best_alpha, best_score = 0, 0.0
    print("고주파 기울기 훑기 (색차 배수 16 고정)")
    for a in (0, 2, 3, 4, 5, 6, 8, 10):
        if not build(a, 16):
            continue
        s = score(base, curve(images, tmp))
        if s is None:
            continue
        mark = ""
        if s < best_score:
            best_alpha, best_score, mark = a, s, "  <-"
        print("  alpha %2d : BD-rate %+7.2f%%%s" % (a, s, mark))

    print("\n색차 배수 훑기 (기울기 %d 고정)" % best_alpha)
    best_chroma, best2 = 16, best_score
    for c in (16, 20, 24, 28, 32, 40, 48):
        if not build(best_alpha, c):
            continue
        s = score(base, curve(images, tmp))
        if s is None:
            continue
        mark = ""
        if s < best2:
            best_chroma, best2, mark = c, s, "  <-"
        print("  chroma %2d : BD-rate %+7.2f%%%s" % (c, s, mark))

    print("\n고른 값: alpha=%d, chroma=%d, 가중 없음 대비 BD-rate %+.2f%%"
          % (best_alpha, best_chroma, best2))
    print("주의: 훑기에 쓴 4장에 맞춘 값이다. 나머지 이미지로 따로 확인해야 한다.")
    build(best_alpha, best_chroma)
    return 0


if __name__ == "__main__":
    sys.exit(main())
