# -*- coding: utf-8 -*-
"""인코딩·디코딩 시간을 같은 자로 잰다.

한계를 먼저 적는다. 각 포맷은 같은 품질점이 아니라 bench 곡선의 가운데
설정을 쓴다. 그러니 이 표는 "같은 화질에서의 속도"가 아니라 "비슷한
비트레이트 언저리에서의 속도"다. 크기를 함께 내는 것은 그 때문이다.

기존 포맷은 bench 와 같게 ffmpeg 를 통해 부른다. 우리 것은 우리 CLI 를
직접 부른다. 양쪽 다 프로세스 시작 시간이 들어 있다.

기계 한 대, 이미지 여섯 장이다. 일반화하지 않는다.
"""
import os, subprocess, sys, tempfile, time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "ingot.exe" if os.name == "nt" else "ingot")
REPEAT = 3

# bench.py 품질 곡선의 가운데 설정
EXTERNAL = [
    ("jpeg", ".jpg", ["-q:v", "9"]),
    ("webp", ".webp", ["-c:v", "libwebp", "-quality", "72"]),
    ("avif", ".avif", ["-c:v", "libaom-av1", "-crf", "42", "-still-picture", "1"]),
    ("jxl", ".jxl", ["-c:v", "libjxl", "-distance", "3.4"]),
]


def timed(args):
    ts = []
    for _ in range(REPEAT):
        t = time.perf_counter()
        r = subprocess.run(args, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            return None
        ts.append(time.perf_counter() - t)
    return min(ts)      # 가장 빠른 회차. 다른 프로세스의 방해를 덜 받는다


def main():
    data = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "testdata")
    src = [os.path.join(data, f)
           for f in sorted(os.listdir(data)) if f.endswith(".ppm")][:6]
    if not src:
        print("PPM 파일이 없습니다:", data)
        return

    tmp = tempfile.mkdtemp()
    acc = {}

    for p in src:
        enc = os.path.join(tmp, "o.igt")
        dec = os.path.join(tmp, "o.ppm")
        te = timed([BIN, "enc", p, enc, "20", "8", "0"])
        td = timed([BIN, "dec", enc, dec])
        if te is not None:
            acc.setdefault("ingot", []).append((te, td, os.path.getsize(enc)))

        for name, ext, mk in EXTERNAL:
            out = os.path.join(tmp, "x" + ext)
            if os.path.exists(out):
                os.remove(out)
            te = timed(["ffmpeg", "-v", "error", "-y", "-i", p] + mk + [out])
            if te is None or not os.path.exists(out):
                continue
            td = timed(["ffmpeg", "-v", "error", "-y", "-i", out,
                        os.path.join(tmp, "x.ppm")])
            acc.setdefault(name, []).append((te, td, os.path.getsize(out)))

    print("\n인코딩·디코딩 시간 (초, 세 번 중 가장 빠른 회차)")
    print("bench 곡선의 가운데 설정. 같은 화질점이 아니므로 크기와 함께 본다.\n")
    for name in ("ingot", "jpeg", "webp", "jxl", "avif"):
        rows = acc.get(name)
        if not rows:
            continue
        ne = sum(r[0] for r in rows) / len(rows)
        nd = sum(r[1] for r in rows if r[1]) / max(1, len([r for r in rows if r[1]]))
        ns = sum(r[2] for r in rows) / len(rows)
        print("  %-6s 인코딩 %7.3f s   디코딩 %7.3f s   크기 %8.0f B  (n=%d)"
              % (name, ne, nd, ns, len(rows)))


main()
