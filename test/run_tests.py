# -*- coding: utf-8 -*-
"""ingot 시험 장치.

조사(압축-구현절차-검증본 9절)가 첫 석 달에 만들라고 한 것 중 둘을 여기서 한다.
  4번 골든 파일 — 고정 입력이 고정 바이트를 낸다
  6번 손상 입력 — 망가진 파일이 프로세스를 죽이지 않는다

성공하면 종료 코드 0, 하나라도 실패하면 1.
"""
import hashlib, os, random, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# 실행 파일 이름은 INGOT_BIN 으로 갈아 끼울 수 있다. 고친 판을 본 빌드와
# 나란히 시험하려면 서로 다른 파일이어야 한다.
BIN = os.environ.get("INGOT_BIN") or os.path.join(
    ROOT, "ingot.exe" if os.name == "nt" else "ingot")
GOLDEN = os.path.join(HERE, "golden.txt")
TESTDATA = os.path.join(ROOT, "testdata")

fails = []


def sh(args, timeout=300):
    return subprocess.run(args, capture_output=True, text=True,
                          errors="replace", timeout=timeout)


def make_synthetic(path, w, h, seed):
    """의존성 없이 시험용 PPM 을 만든다. 값이 고정이라 골든에 쓸 수 있다."""
    rnd = random.Random(seed)
    rows = [b"P6\n%d %d\n255\n" % (w, h)]
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row.append((x * 3 + y) & 0xFF)
            row.append((x ^ y) & 0xFF)
            row.append((rnd.randrange(256) // 32) * 32)
        rows.append(bytes(row))
    with open(path, "wb") as f:
        f.write(b"".join(rows))


def make_banded(path, w, h, seed):
    """조각마다 「잡음 띠 + 평평한 면」이 오는 그림.

    조각 끝의 0 꼬리를 자르는 규격에서, 잘리는 0 이 몇 바이트나 되는지는
    조각의 끝이 얼마나 조용한가에 달렸다. 무늬가 고른 그림은 조각마다
    두세 바이트밖에 안 잘려서, 봐주는 길이를 다섯으로 묶어 둔 결함이 안
    걸렸다. 실사진(968x1188)에서는 조각 하나가 스물 몇 바이트까지 잘려
    품질 40 이상이 전부 안 열렸는데도 시험은 통과했다 (2026-08-23).

    그래서 그 조건을 일부러 만든다: 조각 높이(512)마다 위쪽 160 줄은
    잡음이고 나머지는 단색이다. 잡음이 조각의 값을 채우고, 단색이 조각
    끝의 0 꼬리를 길게 만든다."""
    rnd = random.Random(seed)
    rows = [b"P6\n%d %d\n255\n" % (w, h)]
    flat = bytes((128, 128, 128)) * w
    for y in range(h):
        if (y % 512) < 160:
            row = bytearray()
            for _ in range(w):
                row += bytes((rnd.randrange(256), rnd.randrange(256),
                              rnd.randrange(256)))
            rows.append(bytes(row))
        else:
            rows.append(flat)
    with open(path, "wb") as f:
        f.write(b"".join(rows))


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:16]


def test_golden(tmp, update):
    """같은 입력 + 같은 설정 -> 같은 바이트. 규격이 바뀌면 여기서 깨진다."""
    cases = []
    for (w, h, q, g) in ((64, 64, 20, 6), (129, 77, 5, 6), (200, 150, 40, 7)):
        src = os.path.join(tmp, "g_%dx%d.ppm" % (w, h))
        make_synthetic(src, w, h, seed=w * 1000 + h)
        dst = os.path.join(tmp, "g.igt")
        r = sh([BIN, "enc", src, dst, str(q), str(g)])
        if r.returncode != 0:
            fails.append("골든 인코딩 실패 %dx%d: %s" % (w, h, r.stderr.strip()))
            continue
        # 인코딩 바이트만 잠그면 디코더를 통째로 망가뜨려도 안 잡힌다.
        # 디코딩 결과의 화소까지 함께 잠근다.
        out = os.path.join(tmp, "g_out.ppm")
        r2 = sh([BIN, "dec", dst, out])
        if r2.returncode != 0:
            fails.append("골든 디코딩 실패 %dx%d: %s" % (w, h, r2.stderr.strip()))
            continue
        cases.append("%dx%d q%d g%d %s %d dec:%s" %
                     (w, h, q, g, digest(dst), os.path.getsize(dst), digest(out)))

    if update or not os.path.exists(GOLDEN):
        with open(GOLDEN, "w", encoding="utf-8") as f:
            f.write("# ingot 골든 파일. 규격이 바뀌면 이 값이 바뀐다.\n")
            f.write("# 바뀌었는데 의도한 게 아니면 그것이 회귀다.\n")
            f.write("\n".join(cases) + "\n")
        print("  골든 파일을 새로 썼다 (%d건)" % len(cases))
        return

    want = [l.strip() for l in open(GOLDEN, encoding="utf-8")
            if l.strip() and not l.startswith("#")]
    if want != cases:
        fails.append("골든 불일치:\n    기대 %s\n    실제 %s" % (want, cases))
    else:
        print("  골든 %d건 일치" % len(cases))


def test_determinism(tmp):
    """같은 입력을 두 번 인코딩하면 바이트가 같아야 한다."""
    src = os.path.join(tmp, "d.ppm")
    make_synthetic(src, 173, 91, seed=7)
    a, b = os.path.join(tmp, "a.igt"), os.path.join(tmp, "b.igt")
    sh([BIN, "enc", src, a, "12"])
    sh([BIN, "enc", src, b, "12"])
    if not os.path.exists(a) or digest(a) != digest(b):
        fails.append("결정성 깨짐")
    else:
        print("  결정성 통과")


def read_ppm(path):
    """P6 를 (w, h, 화소바이트) 로 읽는다."""
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        vals = []
        while len(vals) < 3:
            line = f.readline()
            if line.startswith(b"#"):
                continue
            vals += [int(v) for v in line.split()]
        return vals[0], vals[1], f.read()


def psnr_of(a, b):
    """두 화소 덩어리의 PSNR. 같으면 무한대 대신 999 를 준다."""
    import math
    n = min(len(a), len(b))
    if n == 0:
        return 0.0
    se = 0
    for i in range(n):
        d = a[i] - b[i]
        se += d * d
    if se == 0:
        return 999.0
    return 10.0 * math.log10(255.0 * 255.0 * n / se)


def test_roundtrip(tmp):
    """크기가 배수가 아닐 때도 크기와 **화소**가 보존되는가.

    크기만 보면 디코더가 잔차를 통째로 버려도 통과한다. 그래서 화소를 읽어
    PSNR 을 재고, 품질 설정이 보장하는 선 아래로 떨어지면 실패로 센다."""
    ok = 0
    cases = [(1, 1, 10, 6, 0), (7, 3, 10, 6, 0), (8, 8, 10, 6, 0),
             (65, 64, 10, 6, 0), (129, 130, 10, 6, 0), (300, 71, 10, 6, 0),
             (120, 90, 0, 6, 0),      # 품질 최상
             (120, 90, 63, 6, 0),     # 품질 최하
             (300, 200, 10, 9, 0),    # 조각 큼
             (300, 200, 10, 10, 0),   # 조각 최대
             (120, 90, 10, 6, 1)]     # 색차 절반
    # 조각 끝이 조용한 큰 그림. 위 목록의 고른 무늬로는 0 꼬리가
    # 두세 바이트라 안 걸린다. make_banded 의 주석에 근거를 적었다.
    banded = [(1024, 1536, 40, 9, 0), (1024, 1536, 55, 9, 0),
              (1024, 1536, 63, 9, 0)]
    for (w, h, q, g, sub) in cases + banded:
        tag = "%dx%d q%d g%d sub%d" % (w, h, q, g, sub)
        src = os.path.join(tmp, "r.ppm")
        if (w, h, q, g, sub) in banded:
            make_banded(src, w, h, seed=w + h + q)
        else:
            make_synthetic(src, w, h, seed=w + h + q)
        enc = os.path.join(tmp, "r.igt")
        dec = os.path.join(tmp, "r_out.ppm")
        if sh([BIN, "enc", src, enc, str(q), str(g), str(sub)]).returncode != 0:
            fails.append("왕복 인코딩 실패 " + tag); continue
        if sh([BIN, "dec", enc, dec]).returncode != 0:
            fails.append("왕복 디코딩 실패 " + tag); continue
        ow, oh, opix = read_ppm(dec)
        if (ow, oh) != (w, h):
            fails.append("왕복 크기 어긋남 " + tag); continue
        _, _, spix = read_ppm(src)
        if len(opix) != len(spix):
            fails.append("왕복 화소 수 어긋남 " + tag); continue
        # 이 선은 "화질이 좋은가"를 재는 자가 아니다. 시험 이미지마다 값이
        # 다르므로 그런 자는 자기 채점이 된다. 여기서 잡으려는 것은 디코더가
        # 통째로 망가진 경우다. 정밀한 잠금은 골든 해시가 맡는다.
        #
        # 8.0 이었는데 그 값이 고장 구간 안에 앉아 있었다 (2026-08-23 실측:
        # 일부러 망가뜨린 디코더 아홉 판이 6.67~12.51 dB, 정상 출력이
        # 15.06~51.97 dB). 색차를 통째로 버리는 디코더가 12.27 dB 로
        # 통과하고 있었다. 14.0 은 두 구간 사이에 있는 값이다.
        #
        # 올린 뒤 확인했다: 색차를 안 읽는 디코더를 만들어 돌리니 열한 사례
        # 전부에서 걸리고, 정상 판은 열한 사례 전부 통과한다. 이 값을 내리려면
        # 같은 확인을 다시 하고 내려라.
        floor = 14.0
        p = psnr_of(spix, opix)
        if p < floor:
            fails.append("왕복 화질 미달 %s: PSNR %.2f < %.1f" % (tag, p, floor))
            continue
        ok += 1
    print("  왕복 %d/%d 통과 (화소까지 확인)" % (ok, len(cases) + len(banded)))


def put32(b, off, v):
    b[off] = (v >> 24) & 0xFF
    b[off + 1] = (v >> 16) & 0xFF
    b[off + 2] = (v >> 8) & 0xFF
    b[off + 3] = v & 0xFF


def rehash(b):
    """머리말을 고친 뒤 해시를 다시 맞춘다. 그래야 우리가 겨눈 규칙에서
    걸리지, 해시에서 먼저 걸리지 않는다."""
    h = 2166136261
    for i in range(24, len(b)):
        h ^= b[i]
        h = (h * 16777619) & 0xFFFFFFFF
    put32(b, 20, h if h else 1)


def test_reject(tmp):
    """거절 규칙마다 그 규칙만 어긋난 파일을 만들어 실제로 거절되는지 본다.

    이것이 없으면 규칙을 통째로 지워도 시험이 초록이다. 실제로 2026-08-22 에
    열 가지 중 여섯이 한 번도 안 돌고 있었다."""
    src = os.path.join(tmp, "j.ppm")
    make_synthetic(src, 100, 80, seed=11)
    good = os.path.join(tmp, "j.igt")
    if sh([BIN, "enc", src, good, "20", "6"]).returncode != 0:
        fails.append("거절 시험용 인코딩 실패")
        return
    base = bytearray(open(good, "rb").read())
    out = os.path.join(tmp, "j_out.ppm")
    bad = os.path.join(tmp, "j_bad.igt")

    def attempt(name, mutate, want):
        b = bytearray(base)
        mutate(b)
        with open(bad, "wb") as f:
            f.write(b)
        r = sh([BIN, "dec", bad, out], timeout=30)
        msg = (r.stdout + r.stderr).lower()
        if r.returncode == 0:
            fails.append("거절 안 함: %s" % name)
            return 0
        if want and want not in msg:
            fails.append("거절 이유가 다름: %s (기대 '%s', 실제 '%s')"
                         % (name, want, msg.strip()[:60]))
            return 0
        return 1

    ok = 0
    ok += attempt("1 시그니처", lambda b: b.__setitem__(0, 0x58), "signature")
    ok += attempt("2 버전", lambda b: b.__setitem__(4, 9), "version")

    def flip_reserved(b):
        b[5] |= 0x40           # flags 의 예약 비트
        rehash(b)
    ok += attempt("3 예약 비트", flip_reserved, "reserved")

    def zero_width(b):
        put32(b, 8, 0)
        rehash(b)
    ok += attempt("5 폭 0", zero_width, "dimension")

    def huge_width(b):
        put32(b, 8, 70000)
        rehash(b)
    ok += attempt("5 폭 상한", huge_width, "dimension")

    def bad_group_log2(b):
        b[7] = 3               # 6 미만
        rehash(b)
    ok += attempt("6 조각 크기", bad_group_log2, "group")

    def wrong_group_count(b):
        put32(b, 16, 999)
        rehash(b)
    ok += attempt("7 조각 수", wrong_group_count, "")

    ok += attempt("8 잘린 파일", lambda b: b.__delitem__(slice(len(b) // 2, None)),
                  "")

    def toc_outside(b):
        put32(b, 24, 0x7FFFFFFF)   # 첫 조각이 파일 밖
        rehash(b)
    ok += attempt("8 목차가 밖", toc_outside, "")

    def corrupt_group(b):
        b[-5] ^= 0xFF              # 조각 데이터만 건드린다
    ok += attempt("해시 대조", corrupt_group, "")

    print("  거절 규칙 %d/10 통과" % ok)


def test_corrupt(tmp):
    """망가진 입력에 디코더가 죽지 않아야 한다. 크래시 = 실패."""
    src = os.path.join(tmp, "c.ppm")
    make_synthetic(src, 120, 90, seed=3)
    good = os.path.join(tmp, "c.igt")
    sh([BIN, "enc", src, good, "20", "6"])
    data = bytearray(open(good, "rb").read())

    rnd = random.Random(20260821)
    crashes = 0
    silent = 0
    trials = 300
    out = os.path.join(tmp, "c_out.ppm")
    bad = os.path.join(tmp, "bad.igt")

    # 정상 파일을 푼 결과. 손상본이 이것과 다른 그림을 내면 잡는다.
    ref = os.path.join(tmp, "c_ref.ppm")
    sh([BIN, "dec", good, ref])
    good_digest = digest(ref)

    for i in range(trials):
        d = bytearray(data)
        mode = i % 3
        if mode == 0:                                  # 바이트 뒤집기
            for _ in range(rnd.randrange(1, 8)):
                d[rnd.randrange(len(d))] = rnd.randrange(256)
        elif mode == 1:                                # 자르기
            d = d[:rnd.randrange(1, len(d))]
        else:                                          # 무작위 쓰레기
            d = bytearray(rnd.randrange(256) for _ in range(rnd.randrange(1, 400)))
        with open(bad, "wb") as f:
            f.write(d)
        if os.path.exists(out):
            os.remove(out)
        r = sh([BIN, "dec", bad, out], timeout=30)
        if r.returncode not in (0, 1):
            crashes += 1
            if crashes <= 3:
                fails.append("손상 입력에서 비정상 종료 (코드 %d, 모드 %d)"
                             % (r.returncode, mode))
            continue
        # 여기가 요점이다. 거절(코드 1)은 정상이지만, 받아들였다면(코드 0)
        # 그 결과가 정상 파일의 결과와 같아야 한다. 다르면 망가진 파일을
        # 조용히 딴 그림으로 내놓은 것이고, 그것은 거절보다 나쁘다.
        if r.returncode == 0 and os.path.exists(out):
            if digest(out) != good_digest:
                silent += 1
                if silent <= 3:
                    fails.append("손상 입력을 조용히 받아들여 다른 그림을 냈다 "
                                 "(모드 %d, 사례 %d)" % (mode, i))
    print("  손상 입력 %d건: 비정상 종료 %d건, 조용한 오답 %d건"
          % (trials, crashes, silent))


def main():
    update = "--update" in sys.argv
    if not os.path.exists(BIN):
        print("실행 파일이 없다. build.sh 를 먼저 돌려라."); return 1
    tmp = tempfile.mkdtemp(prefix="ingot_test_")
    print("ingot 시험")
    test_determinism(tmp)
    test_roundtrip(tmp)
    test_golden(tmp, update)
    test_reject(tmp)
    test_corrupt(tmp)
    print()
    if fails:
        print("실패 %d건:" % len(fails))
        for f in fails:
            print("  -", f)
        return 1
    print("전부 통과")
    return 0


if __name__ == "__main__":
    sys.exit(main())
