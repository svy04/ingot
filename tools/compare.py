# -*- coding: utf-8 -*-
"""우리 코덱과 기존 포맷을 같은 자로 비교한다.

규칙 (압축-판정기준-검증본 「고정해야 할 변수」):
  - 같은 원본, 같은 기계, 같은 색공간(4:4:4 유지)
  - 지표를 하나만 쓰지 않는다. PSNR 과 SSIM 을 함께 낸다
  - 각 포맷의 품질을 훑어 곡선을 만들고, 같은 품질점에서 크기를 견준다
  - 인코딩 시간을 함께 잰다

한계: 이 표는 우리 기계 한 대, 이미지 몇 장이다. 일반화하지 않는다.
"""
import io, os, re, subprocess, sys, time, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VX = os.path.join(ROOT, "vx.exe" if os.name == "nt" else "vx")


def run(cmd, timeout=600):
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True,
                       errors="replace", timeout=timeout)
    return r, time.time() - t0


def metrics(ref_ppm, test_ppm):
    """PSNR 과 SSIM 을 ffmpeg 필터로 잰다."""
    out = {}
    for filt, key, pat in (("psnr", "psnr", r"average:([0-9.]+)"),
                           ("ssim", "ssim", r"All:([0-9.]+)")):
        r, _ = run(["ffmpeg", "-v", "info", "-i", test_ppm, "-i", ref_ppm,
                    "-lavfi", filt, "-f", "null", "-"])
        m = re.search(pat, r.stdout + r.stderr)
        out[key] = float(m.group(1)) if m else None
    return out


def encode_vx(src, dst, quality):
    r, t = run([VX, "enc", src, dst, str(quality)])
    if r.returncode != 0:
        return None, None
    return os.path.getsize(dst), t


def decode_vx(src, dst):
    r, t = run([VX, "dec", src, dst])
    return (t if r.returncode == 0 else None)


def encode_ff(src, dst, args):
    r, t = run(["ffmpeg", "-v", "error", "-y", "-i", src] + args + [dst])
    if r.returncode != 0 or not os.path.exists(dst):
        return None, None
    return os.path.getsize(dst), t


def decode_ff(src, dst):
    r, t = run(["ffmpeg", "-v", "error", "-y", "-i", src, dst])
    return (t if r.returncode == 0 else None)


CANDIDATES = [
    # (이름, 확장자, 품질 목록, 인자 만드는 함수)
    ("jpeg(4:4:4)", ".jpg", [2, 5, 8, 12, 18, 25],
     lambda q: ["-q:v", str(q), "-pix_fmt", "yuvj444p"]),
    ("webp",        ".webp", [95, 90, 80, 70, 55, 40],
     lambda q: ["-c:v", "libwebp", "-quality", str(q), "-lossless", "0"]),
    ("avif",        ".avif", [20, 28, 36, 44, 52, 60],
     lambda q: ["-c:v", "libaom-av1", "-still-picture", "1",
                "-crf", str(q), "-cpu-used", "6", "-pix_fmt", "yuv444p"]),
    ("jxl",         ".jxl", [0.5, 1.0, 2.0, 3.0, 5.0, 8.0],
     lambda q: ["-c:v", "libjxl", "-distance", str(q)]),
]

VX_QUALITIES = [0, 4, 10, 20, 30, 45, 63]


def main():
    imgs = sys.argv[1:]
    if not imgs:
        d = os.path.join(ROOT, "testdata")
        imgs = [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".ppm")]
    tmp = tempfile.mkdtemp(prefix="vxcmp_")

    for src in imgs:
        name = os.path.basename(src)
        raw = os.path.getsize(src)
        print("=" * 96)
        print("{}   원본 PPM {:.2f} MB".format(name, raw / 1048576))
        print("{:<14} {:>7} {:>10} {:>8} {:>8} {:>8} {:>8}".format(
            "포맷", "설정", "바이트", "비율%", "PSNR", "SSIM", "인코딩s"))
        print("-" * 96)

        for q in VX_QUALITIES:
            dst = os.path.join(tmp, "t.vx")
            sz, et = encode_vx(src, dst, q)
            if sz is None:
                print("{:<14} {:>7} {:>10}".format("ours", q, "실패")); continue
            rec = os.path.join(tmp, "t_vx.ppm")
            decode_vx(dst, rec)
            m = metrics(src, rec)
            print("{:<14} {:>7} {:>10} {:>7.2f}% {:>8} {:>8} {:>8.2f}".format(
                "ours(v0)", q, sz, 100.0 * sz / raw,
                "%.2f" % m["psnr"] if m["psnr"] else "-",
                "%.4f" % m["ssim"] if m["ssim"] else "-", et))

        for label, ext, qs, mk in CANDIDATES:
            for q in qs:
                dst = os.path.join(tmp, "t" + ext)
                sz, et = encode_ff(src, dst, mk(q))
                if sz is None:
                    continue
                rec = os.path.join(tmp, "t_ff.ppm")
                if decode_ff(dst, rec) is None:
                    continue
                m = metrics(src, rec)
                print("{:<14} {:>7} {:>10} {:>7.2f}% {:>8} {:>8} {:>8.2f}".format(
                    label, q, sz, 100.0 * sz / raw,
                    "%.2f" % m["psnr"] if m["psnr"] else "-",
                    "%.4f" % m["ssim"] if m["ssim"] else "-", et))
        print()


if __name__ == "__main__":
    main()
