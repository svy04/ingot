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
BIN = os.path.join(ROOT, "ingot.exe" if os.name == "nt" else "ingot")
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
        cases.append("%dx%d q%d g%d %s %d" %
                     (w, h, q, g, digest(dst), os.path.getsize(dst)))

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


def test_roundtrip(tmp):
    """크기가 8이나 조각 크기의 배수가 아닐 때도 크기가 보존되는가."""
    ok = 0
    for (w, h) in ((1, 1), (7, 3), (8, 8), (65, 64), (129, 130), (300, 71)):
        src = os.path.join(tmp, "r.ppm")
        make_synthetic(src, w, h, seed=w + h)
        enc = os.path.join(tmp, "r.igt")
        dec = os.path.join(tmp, "r_out.ppm")
        if sh([BIN, "enc", src, enc, "10", "6"]).returncode != 0:
            fails.append("왕복 인코딩 실패 %dx%d" % (w, h)); continue
        if sh([BIN, "dec", enc, dec]).returncode != 0:
            fails.append("왕복 디코딩 실패 %dx%d" % (w, h)); continue
        with open(dec, "rb") as f:
            head = f.readline() + f.readline()
        if b"%d %d" % (w, h) not in head:
            fails.append("왕복 크기 어긋남 %dx%d" % (w, h)); continue
        ok += 1
    print("  왕복 %d/6 통과" % ok)


def test_corrupt(tmp):
    """망가진 입력에 디코더가 죽지 않아야 한다. 크래시 = 실패."""
    src = os.path.join(tmp, "c.ppm")
    make_synthetic(src, 120, 90, seed=3)
    good = os.path.join(tmp, "c.igt")
    sh([BIN, "enc", src, good, "20", "6"])
    data = bytearray(open(good, "rb").read())

    rnd = random.Random(20260821)
    crashes = 0
    trials = 300
    out = os.path.join(tmp, "c_out.ppm")
    bad = os.path.join(tmp, "bad.igt")

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
        r = sh([BIN, "dec", bad, out], timeout=30)
        # 종료 코드 0 또는 1 은 정상 (성공 또는 거절). 그 밖은 크래시다.
        if r.returncode not in (0, 1):
            crashes += 1
            if crashes <= 3:
                fails.append("손상 입력에서 비정상 종료 (코드 %d, 모드 %d)"
                             % (r.returncode, mode))
    print("  손상 입력 %d건 중 비정상 종료 %d건" % (trials, crashes))


def main():
    update = "--update" in sys.argv
    if not os.path.exists(BIN):
        print("실행 파일이 없다. build.sh 를 먼저 돌려라."); return 1
    tmp = tempfile.mkdtemp(prefix="ingot_test_")
    print("ingot 시험")
    test_determinism(tmp)
    test_roundtrip(tmp)
    test_golden(tmp, update)
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
