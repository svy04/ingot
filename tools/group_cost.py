# -*- coding: utf-8 -*-
"""조각을 잘게 나눌 때 무엇을 얼마나 잃는지 잰다.

이 포맷은 스레드 수를 비트스트림에 싣지 않는 대신, 조각마다 확률표를
처음부터 다시 배운다. 조각이 작으면 배울 표본이 모자란다. 그 대가가
얼마인지가 이 파일이 답하는 것이다.

v0 에서 쟀던 1.4% 는 골롬-라이스 시절 값이다. 산술 부호화가 들어간 뒤로는
확률표 학습이 대가의 본체가 됐으므로 다시 재야 한다.
"""
import math, os, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "ingot.exe" if os.name == "nt" else "ingot")
QUALITIES = [1, 4, 10, 20, 36, 63]
GROUPS = [6, 7, 8, 9, 10]        # 2^n. 8 이 기본값이다


def sh(args):
    return subprocess.run(args, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)


def ppm_pixels(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        vals = []
        while len(vals) < 3:
            line = f.readline()
            if line.startswith(b"#"):
                continue
            vals += [int(v) for v in line.split()]
    return vals[0] * vals[1]


def psnr_of(ref, test):
    r = sh(["ffmpeg", "-v", "info", "-i", test, "-i", ref,
            "-lavfi", "psnr", "-f", "null", "-"])
    out = (r.stdout or b"").decode("utf-8", "replace")
    import re
    m = re.search(r"average:([0-9.]+)", out)
    return float(m.group(1)) if m else None


def _pchip_slopes(x, y):
    n = len(x)
    h = [x[i + 1] - x[i] for i in range(n - 1)]
    d = [(y[i + 1] - y[i]) / h[i] for i in range(n - 1)]
    m = [0.0] * n
    m[0], m[-1] = d[0], d[-1]
    for i in range(1, n - 1):
        if d[i - 1] * d[i] <= 0:
            m[i] = 0.0
        else:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])
    return m


def _pchip_eval(x, y, m, t):
    n = len(x)
    lo = 0
    for i in range(n - 1):
        if t >= x[i]:
            lo = i
    h = x[lo + 1] - x[lo]
    s = (t - x[lo]) / h
    s2, s3 = s * s, s * s * s
    return ((2 * s3 - 3 * s2 + 1) * y[lo] + (s3 - 2 * s2 + s) * h * m[lo] +
            (-2 * s3 + 3 * s2) * y[lo + 1] + (s3 - s2) * h * m[lo + 1])


def bd_rate(ref, test, steps=400):
    """ref 대비 test 가 같은 화질에서 비트를 몇 퍼센트 더 쓰는가."""
    ref = sorted((q, math.log(b)) for b, q in ref)
    test = sorted((q, math.log(b)) for b, q in test)
    lo = max(ref[0][0], test[0][0])
    hi = min(ref[-1][0], test[-1][0])
    if hi <= lo:
        return None
    rx = [p[0] for p in ref]; ry = [p[1] for p in ref]
    tx = [p[0] for p in test]; ty = [p[1] for p in test]
    rm, tm = _pchip_slopes(rx, ry), _pchip_slopes(tx, ty)
    acc = 0.0
    for i in range(steps):
        t = lo + (hi - lo) * (i + 0.5) / steps
        acc += _pchip_eval(tx, ty, tm, t) - _pchip_eval(rx, ry, rm, t)
    val = (math.exp(acc / steps) - 1.0) * 100.0
    return val if -99.0 < val < 5000.0 else None


def main():
    data = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "testdata")
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    imgs = [os.path.join(data, f)
            for f in sorted(os.listdir(data)) if f.endswith(".ppm")][:count]
    tmp = tempfile.mkdtemp()
    curves = {}

    for g in GROUPS:
        for src in imgs:
            n = ppm_pixels(src)
            for q in QUALITIES:
                enc = os.path.join(tmp, "g.igt")
                dec = os.path.join(tmp, "g.ppm")
                if sh([BIN, "enc", src, enc, str(q), str(g), "0"]).returncode:
                    continue
                if sh([BIN, "dec", enc, dec]).returncode:
                    continue
                p = psnr_of(src, dec)
                if p is None:
                    continue
                bpp = 8.0 * os.path.getsize(enc) / n
                curves.setdefault((g, os.path.basename(src)), []).append((bpp, p))
        print("조각 2^%d 측정 끝" % g)
        sys.stdout.flush()

    base = 10      # 가장 큰 조각을 기준으로
    print("\n조각을 잘게 나눌 때의 대가 (조각 2^%d 기준, 양수 = 비트를 더 쓴다)" % base)
    print("이미지 %d장, 품질 %d단계, PSNR 기준 BD-rate\n" % (len(imgs), len(QUALITIES)))
    for g in GROUPS:
        ds = []
        for src in imgs:
            k0, k1 = (base, os.path.basename(src)), (g, os.path.basename(src))
            if k0 not in curves or k1 not in curves:
                continue
            d = bd_rate(curves[k0], curves[k1])
            if d is not None:
                ds.append(d)
        if ds:
            ds.sort()
            mid = ds[len(ds) // 2]
            print("  조각 %5d  평균 %+7.2f%%   중앙 %+7.2f%%   최악 %+7.2f%%  (n=%d)"
                  % (1 << g, sum(ds) / len(ds), mid, ds[-1], len(ds)))


main()
