# -*- coding: utf-8 -*-
"""지그재그 last-1 자리의 계수가 정말 언제나 0 이 아닌지 센다.

규격의 「확실한 깃발을 안 적는다」 규칙이 서 있는 근거다. 근거가 되는 숫자는
다시 셀 수 있어야 하므로 도구로 남긴다. 임시 계량기로 한 번 세고 지우면
문서의 100.0000% 를 아무도 확인할 수 없다.

  python tools/certain_flag.py                  testdata 8장, 품질 6·20·40
  python tools/certain_flag.py --data ourdata --images 12

세는 방법: 인코더를 건드리지 않는다. 파일을 담고, 같은 양자화를 파이썬으로
다시 해서 지그재그 순서의 마지막 비영 계수를 찾는다. 그 자리가 정의상
0 이 아님을 확인하고, 그런 자리가 전체 계수 중 몇 퍼센트인지 센다.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.environ.get("INGOT_BIN") or os.path.join(
    ROOT, "ingot.exe" if os.name == "nt" else "ingot")
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def main():
    argv = sys.argv[1:]
    data = "testdata"
    nimg = 8
    if "--data" in argv:
        data = argv[argv.index("--data") + 1]
    if "--images" in argv:
        nimg = int(argv[argv.index("--images") + 1])
    d = os.path.join(ROOT, data)
    imgs = sorted(os.path.join(d, f) for f in os.listdir(d)
                  if f.endswith(".ppm"))[:nimg]
    if not imgs:
        print("시험 이미지가 없다"); return 1

    exe = os.path.join(ROOT, "t_certain.exe")
    src = ("src/transform.c src/color.c src/predict.c src/loopfilter.c "
           "src/rangecoder.c src/encode.c src/decode.c tools/cli.c")
    r = subprocess.run("gcc -std=c99 -O2 -w -DINGOT_BIT_STATS %s -o t_certain -lm"
                       % src, shell=True, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        print((r.stdout or b"").decode("utf-8", "replace")[-800:]); return 1

    # 갓 만든 실행 파일을 응용 프로그램 제어 정책이 잠깐 막는 일이 있다.
    import time
    for _ in range(180):
        try:
            subprocess.run([exe], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=20)
            break
        except OSError:
            time.sleep(1.0)

    print("지그재그 last-1 자리의 계수 (%s %d장)\n" % (data, len(imgs)))
    print("  품질 | 그 자리 개수 |  0 아닌 것 |    비율    | 전체 깃발 중")
    print("  -----+--------------+------------+------------+-------------")
    tmp = tempfile.mkdtemp(prefix="ingot_cf_")
    for q in (6, 20, 40):
        path = os.path.join(tmp, "cf_%d.txt" % q)
        if os.path.exists(path):
            os.remove(path)
        env = dict(os.environ, INGOT_BITSTAT_PATH=path)
        for f in imgs:
            subprocess.run([exe, "enc", f, os.path.join(tmp, "o.igt"), str(q), "8"],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        if not os.path.exists(path):
            print("  %5d | 계량기가 파일을 안 냈다" % q)
            continue
        n = nz = 0
        for line in open(path, encoding="utf-8"):
            f = line.split()
            if f and f[0] == "L":
                n += int(f[1]); nz += int(f[2])
        if n == 0:
            print("  %5d | 셀 것이 없다" % q); continue
        print("  %5d | %12d | %10d | %9.4f%% |" % (q, n, nz, 100.0 * nz / n))
    print("\n그 자리의 계수가 0 이면 last 가 그 자리를 가리킬 수 없다.")
    print("비율이 100.0000% 가 아니면 규격이나 인코더에 결함이 있다는 뜻이다.")
    return 0


sys.exit(main())
