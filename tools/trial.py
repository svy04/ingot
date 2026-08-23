# -*- coding: utf-8 -*-
"""인코더 쪽 변경 하나를 기준선과 견준다.

바깥 코덱을 안 부르므로 tune_round.py 보다 훨씬 빠르다. 인코더만의 판단이나
작은 규격 변경을 여러 번 훑을 때 쓴다. 마지막에 이겼다 싶으면 bench --all 로
바깥 코덱까지 다시 재서 굳힌다.

  python tools/trial.py "-DINGOT_MODE_TRIALS=4"          testdata 8장
  python tools/trial.py "-DX=1" --data ourdata --images 12
"""
import subprocess, sys, os, re, json, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = "src/transform.c src/color.c src/predict.c src/loopfilter.c " \
      "src/rangecoder.c src/encode.c src/decode.c tools/cli.c"


NAME = os.environ.get("TRIAL_NAME", "trial")


def build(flags):
    exe = os.path.join(ROOT, NAME + ".exe")
    if os.path.exists(exe):
        os.remove(exe)
    cmd = "gcc -std=c99 -O2 -w %s %s -o %s -lm" % (flags, SRC, NAME)
    r = subprocess.run(cmd, shell=True, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        print((r.stdout or b"").decode("utf-8", "replace")[-1500:])
        sys.exit(1)


def wait_runnable(exe, tries=180):
    """갓 만든 실행 파일을 응용 프로그램 제어 정책이 잠깐 막는 일이 있다.
    이 컴퓨터에서 실제로 겪은 현상이라 될 때까지 잠깐씩 기다린다."""
    import time
    for _ in range(tries):
        try:
            subprocess.run([exe], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=20)
            return True
        except OSError:
            time.sleep(1.0)
    return False


def run(args):
    env = dict(os.environ)
    env["INGOT_BIN"] = os.path.join(ROOT, NAME + ".exe")
    r = subprocess.run([sys.executable, "tools/bench.py"] + args, cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return (r.stdout or b"").decode("utf-8", "replace")


def main():
    argv = sys.argv[1:]
    flags = argv[0] if argv and not argv[0].startswith("--") else ""
    data = "testdata"
    images = "8"
    if "--data" in argv:
        data = argv[argv.index("--data") + 1]
    if "--images" in argv:
        images = argv[argv.index("--images") + 1]
    # 기준선은 **지금 담긴 판**이 기본이다. 그래야 「변경 없음」이 +0.00 으로
    # 찍히고, 실험이 이득인지 손해인지 부호만 보면 된다. 예전에는 아침 판을
    # 기본으로 두어 아무것도 안 고쳐도 -7% 가 찍혔고, 읽는 사람이 매번
    # 머릿속으로 빼야 했다 (2026-08-23 에 실제로 오독이 났다).
    # 옛 판과 견주려면 TRIAL_BASE 로 지목한다.
    base = os.environ.get("TRIAL_BASE") or (
        "base_final" if data == "testdata" else "base_mode_our")

    if not os.path.exists(os.path.join(HERE, base + ".json")):
        print("기준선 없음: tools/%s.json — bench.py --save 로 먼저 만든다" % base)
        return 1

    build(flags)
    if not wait_runnable(os.path.join(ROOT, NAME + ".exe")):
        print("실행 파일이 정책에 계속 막힌다: %s" % NAME)
        return 1
    out = run(["--images", images, "--data", data, "--vs", base])
    got = {}
    for line in out.splitlines():
        m = re.search(r"(PSNR|SSIM|S2) 기준 평균 BD-rate ([-+][\d.]+)%", line)
        if m:
            got[m.group(1)] = float(m.group(2))
    if not got:
        print(out[-2000:])
        return 1
    # 못 잰 값을 0 으로 찍으면 「변화 없음」과 구별이 안 된다. 지각 지표는
    # 파이썬 모듈이 없으면 조용히 빠지므로 이 자리가 실제로 위험하다.
    def cell(k):
        return "%+6.2f" % got[k] if k in got else " 못잼 "
    print("%-42s | %s  P %s  S %s  S2 %s   (음수 = 기준선보다 낫다)"
          % (flags or "(변경 없음)", data, cell("PSNR"), cell("SSIM"), cell("S2")))
    return 0


sys.exit(main())
