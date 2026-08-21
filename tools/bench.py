# -*- coding: utf-8 -*-
"""ingot 성능 측정 하네스.

압축-판정기준-검증본이 정한 것을 지킨다.
  - 고정 시험 세트를 쓴다. 나중에 바꾸면 이력을 못 읽는다
  - 지표를 하나만 쓰지 않는다 (PSNR, SSIM)
  - 인코딩·디코딩 시간을 함께 잰다
  - BD-rate 로 곡선끼리 견준다 (PSNR 기준. 3차 다항식은 PSNR 에서 오차 1% 미만)

사용:
  python tools/bench.py                 우리 코덱만 (개선 전후 비교용)
  python tools/bench.py --save base     결과를 base.json 으로 저장
  python tools/bench.py --vs base       저장본과 견주어 BD-rate 를 낸다
  python tools/bench.py --all           기존 포맷까지 함께 (느리다)
  python tools/bench.py --images 8      쓸 이미지 수
"""
import json, math, os, re, subprocess, sys, tempfile, time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
try:
    import numpy as np
except ImportError:
    np = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "ingot.exe" if os.name == "nt" else "ingot")
DATA = os.path.join(ROOT, "testdata")
QUALITIES = [1, 4, 10, 20, 36, 63]


def sh(args, timeout=600):
    t0 = time.time()
    r = subprocess.run(args, capture_output=True, text=True,
                       errors="replace", timeout=timeout)
    return r, time.time() - t0


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


def quality_of(ref, test):
    out = {}
    for filt, key, pat in (("psnr", "psnr", r"average:([0-9.]+)"),
                           ("ssim", "ssim", r"All:([0-9.]+)")):
        r, _ = sh(["ffmpeg", "-v", "info", "-i", test, "-i", ref,
                   "-lavfi", filt, "-f", "null", "-"])
        m = re.search(pat, r.stdout + r.stderr)
        out[key] = float(m.group(1)) if m else None
    return out


def measure_ours(images, tmp, sub=0):
    rows = []
    for src in images:
        n = ppm_pixels(src)
        for q in QUALITIES:
            enc = os.path.join(tmp, "b.igt")
            dec = os.path.join(tmp, "b.ppm")
            r, et = sh([BIN, "enc", src, enc, str(q), "8", str(sub)])
            if r.returncode != 0:
                continue
            r2, dt = sh([BIN, "dec", enc, dec])
            if r2.returncode != 0:
                continue
            m = quality_of(src, dec)
            rows.append({
                "image": os.path.basename(src), "codec": "ingot", "setting": q,
                "bytes": os.path.getsize(enc), "pixels": n,
                "bpp": 8.0 * os.path.getsize(enc) / n,
                "psnr": m["psnr"], "ssim": m["ssim"],
                "enc_s": round(et, 3), "dec_s": round(dt, 3),
            })
    return rows


EXTERNAL = [
    ("jpeg", ".jpg", [2, 5, 9, 14, 20, 28],
     lambda q: ["-q:v", str(q), "-pix_fmt", "yuvj444p"]),
    ("webp", ".webp", [96, 90, 82, 72, 60, 45],
     lambda q: ["-c:v", "libwebp", "-quality", str(q)]),
    ("avif", ".avif", [18, 26, 34, 42, 50, 58],
     lambda q: ["-c:v", "libaom-av1", "-still-picture", "1", "-crf", str(q),
                "-cpu-used", "6", "-pix_fmt", "yuv444p"]),
    ("jxl", ".jxl", [0.7, 1.3, 2.2, 3.4, 5.0, 7.5],
     lambda q: ["-c:v", "libjxl", "-distance", str(q)]),
]


def measure_external(images, tmp):
    rows = []
    for src in images:
        n = ppm_pixels(src)
        for name, ext, qs, mk in EXTERNAL:
            for q in qs:
                enc = os.path.join(tmp, "x" + ext)
                dec = os.path.join(tmp, "x.ppm")
                if os.path.exists(enc):
                    os.remove(enc)
                r, et = sh(["ffmpeg", "-v", "error", "-y", "-i", src] + mk(q) + [enc])
                if r.returncode != 0 or not os.path.exists(enc):
                    continue
                r2, dt = sh(["ffmpeg", "-v", "error", "-y", "-i", enc, dec])
                if r2.returncode != 0:
                    continue
                m = quality_of(src, dec)
                rows.append({
                    "image": os.path.basename(src), "codec": name, "setting": q,
                    "bytes": os.path.getsize(enc), "pixels": n,
                    "bpp": 8.0 * os.path.getsize(enc) / n,
                    "psnr": m["psnr"], "ssim": m["ssim"],
                    "enc_s": round(et, 3), "dec_s": round(dt, 3),
                })
    return rows


def _pchip_slopes(x, y):
    """단조성을 보존하는 기울기 (Fritsch-Carlson)."""
    n = len(x)
    h = [x[i + 1] - x[i] for i in range(n - 1)]
    dl = [(y[i + 1] - y[i]) / h[i] if h[i] else 0.0 for i in range(n - 1)]
    d = [0.0] * n
    for i in range(1, n - 1):
        if dl[i - 1] * dl[i] > 0:
            w1 = 2.0 * h[i] + h[i - 1]
            w2 = h[i] + 2.0 * h[i - 1]
            d[i] = (w1 + w2) / (w1 / dl[i - 1] + w2 / dl[i])
    for a, b, e in ((0, 1, 0), (n - 1, n - 2, n - 2)):
        if n < 3:
            d[a] = dl[0]
            continue
        i = 0 if a == 0 else n - 2
        h0, h1 = h[i], h[i - 1] if a else h[1]
        d0, d1 = dl[i], dl[i - 1] if a else dl[1]
        v = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1) if (h0 + h1) else d0
        if v * d0 <= 0:
            v = 0.0
        elif d0 * d1 < 0 and abs(v) > abs(3.0 * d0):
            v = 3.0 * d0
        d[a] = v
    return d


def _pchip_eval(x, y, d, t):
    """구간을 찾아 3차 에르미트로 값을 낸다."""
    n = len(x)
    if t <= x[0]:
        return y[0]
    if t >= x[-1]:
        return y[-1]
    lo, hi = 0, n - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if x[mid] <= t:
            lo = mid
        else:
            hi = mid
    h = x[lo + 1] - x[lo]
    if h == 0:
        return y[lo]
    s = (t - x[lo]) / h
    s2, s3 = s * s, s * s * s
    return ((2 * s3 - 3 * s2 + 1) * y[lo] + (s3 - 2 * s2 + s) * h * d[lo] +
            (-2 * s3 + 3 * s2) * y[lo + 1] + (s3 - s2) * h * d[lo + 1])


def bd_rate(ref, test, steps=400):
    """Bjontegaard delta rate. 음수면 test 가 ref 보다 적은 비트를 쓴다.

    ref, test 는 [(bpp, 화질), ...]. 겹치는 화질 구간에서만 잰다.
    단조 보존 보간을 쓰고, 결과가 터무니없이 크면 None 을 돌려준다.
    """
    if len(ref) < 4 or len(test) < 4:
        return None

    def prep(pts):
        m = {}
        for b, q in pts:
            if b > 0 and q is not None:
                m[q] = math.log(b)          # 같은 화질이 겹치면 뒤엣것을 쓴다
        xs = sorted(m)
        if len(xs) < 4:
            return None, None, None
        ys = [m[x] for x in xs]
        return xs, ys, _pchip_slopes(xs, ys)

    rx, ry, rd = prep(ref)
    tx, ty, td = prep(test)
    if rx is None or tx is None:
        return None

    lo = max(rx[0], tx[0])
    hi = min(rx[-1], tx[-1])
    if hi - lo < 0.5:
        return None

    step = (hi - lo) / steps
    ir = it = 0.0
    prev_r = _pchip_eval(rx, ry, rd, lo)
    prev_t = _pchip_eval(tx, ty, td, lo)
    for i in range(1, steps + 1):
        q = lo + step * i
        cur_r = _pchip_eval(rx, ry, rd, q)
        cur_t = _pchip_eval(tx, ty, td, q)
        ir += (prev_r + cur_r) * 0.5 * step
        it += (prev_t + cur_t) * 0.5 * step
        prev_r, prev_t = cur_r, cur_t

    val = (math.exp((it - ir) / (hi - lo)) - 1.0) * 100.0
    if not (-99.0 < val < 500.0):
        return None                          # 곡선이 어긋났다. 숫자를 믿지 않는다
    return val


def ssim_db(v):
    """AOM 시험 조건이 쓰는 변환. 1 에 가까운 SSIM 을 dB 로 펴서 곡선을 맞춘다."""
    if v is None or v >= 1.0:
        return None
    return -10.0 * math.log10(max(1e-9, 1.0 - v))


def curves(rows, codec, metric="psnr"):
    """이미지별 (bpp, 화질) 곡선. metric 은 psnr 또는 ssim."""
    out = {}
    for r in rows:
        if r["codec"] != codec:
            continue
        q = r["psnr"] if metric == "psnr" else ssim_db(r["ssim"])
        if q is None:
            continue
        out.setdefault(r["image"], []).append((r["bpp"], q))
    return out


def summarize(rows, label):
    codecs = sorted(set(r["codec"] for r in rows))
    print("\n{:<8} {:>7} {:>9} {:>8} {:>8} {:>8} {:>8}".format(
        "코덱", "설정", "bpp", "PSNR", "SSIM", "인코딩", "디코딩"))
    print("-" * 62)
    for c in codecs:
        sel = [r for r in rows if r["codec"] == c]
        settings = sorted(set(r["setting"] for r in sel), key=float)
        for s in settings:
            g = [r for r in sel if r["setting"] == s and r["psnr"]]
            if not g:
                continue
            print("{:<8} {:>7} {:>9.4f} {:>8.2f} {:>8.4f} {:>8.3f} {:>8.3f}".format(
                c, s,
                sum(r["bpp"] for r in g) / len(g),
                sum(r["psnr"] for r in g) / len(g),
                sum(r["ssim"] for r in g) / len(g),
                sum(r["enc_s"] for r in g) / len(g),
                sum(r["dec_s"] for r in g) / len(g)))


def main():
    args = sys.argv[1:]
    nimg = 8
    if "--images" in args:
        nimg = int(args[args.index("--images") + 1])
    data_dir = DATA
    if "--data" in args:
        data_dir = os.path.join(ROOT, args[args.index("--data") + 1])
    images = sorted(os.path.join(data_dir, f) for f in os.listdir(data_dir)
                    if f.endswith(".ppm"))[:nimg]
    if not images:
        print("시험 이미지가 없다"); return 1
    print("이미지 %d장, 품질 %d단계%s"
          % (len(images), len(QUALITIES),
             ", 색차 4:2:0" if "--sub" in sys.argv else ", 색차 4:4:4"))

    tmp = tempfile.mkdtemp(prefix="ingot_bench_")
    sub = 1 if "--sub" in args else 0
    rows = measure_ours(images, tmp, sub)
    if "--all" in args:
        rows += measure_external(images, tmp)

    summarize(rows, "현재")

    if "--save" in args:
        name = args[args.index("--save") + 1]
        with open(os.path.join(HERE, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(rows, f, ensure_ascii=False)
        print("\n저장: tools/%s.json" % name)

    if "--vs" in args:
        name = args[args.index("--vs") + 1]
        path = os.path.join(HERE, name + ".json")
        if not os.path.exists(path):
            print("\n비교 대상 없음: %s" % path); return 0
        old = json.load(open(path, encoding="utf-8"))
        for metric in ("psnr", "ssim"):
            oc = curves(old, "ingot", metric)
            nc = curves(rows, "ingot", metric)
            deltas, skipped = [], 0
            for img in sorted(set(oc) & set(nc)):
                d = bd_rate(oc[img], nc[img])
                if d is None:
                    skipped += 1
                else:
                    deltas.append(d)
            if deltas:
                note = ", 못 잰 것 %d" % skipped if skipped else ""
                print("\n  %s 기준 평균 BD-rate %+.2f%%   (음수 = 비트를 덜 쓴다, n=%d%s)"
                      % (metric.upper(), sum(deltas) / len(deltas), len(deltas), note))

    if "--all" in args:
        for metric in ("psnr", "ssim"):
            base = curves(rows, "ingot", metric)
            print("\n기존 포맷 대비, %s 기준 (양수 = 우리가 더 많은 비트를 쓴다)"
                  % metric.upper())
            for c in ("jpeg", "webp", "avif", "jxl"):
                other = curves(rows, c, metric)
                ds = [bd_rate(other[i], base[i])
                      for i in sorted(set(base) & set(other))]
                ds = [d for d in ds if d is not None]
                if ds:
                    print("  vs %-6s %+8.1f%%  (n=%d)"
                          % (c, sum(ds) / len(ds), len(ds)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
