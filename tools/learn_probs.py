# -*- coding: utf-8 -*-
"""시작 확률표를 다시 학습해 소스에 심는다.

규격에 박힌 확률표(`src/rangecoder.c` 의 `ingot_prob_init`)는 어디선가 뚝
떨어진 숫자가 아니라 이 스크립트가 만든 것이다. 같은 이미지·같은 품질로
돌리면 같은 표가 나온다. 표를 검증하거나 다른 소재에 맞춰 다시 만들려면
이것을 쓴다.

절차는 넷이다.

  1. 확률 수집 장치를 켜고 빌드한다 (`-DINGOT_PROB_DUMP`)
  2. 시험 이미지들을 여러 품질로 담으면서, 조각 하나가 끝난 시점의 확률을 모은다
  3. 평균을 내고, 누적 확률표 부분은 단조와 최소 간격을 지키게 다듬어 심는다
  4. 심은 표를 시작값으로 두고 2~3 을 한 번 더 돈다

넷째가 필요한 이유: 첫 바퀴는 반반에서 출발해 모은 값이라 "반반에서 시작했을 때
어디로 가는가"를 담는다. 그 표를 깔고 다시 돌면 "좋은 값에서 시작했을 때 어디로
가는가"가 되어 실제 쓰임에 가깝다. 두 바퀴에서 대체로 수렴한다.

바퀴 수를 고정해 두는 것이 요점이다. 그래야 같은 절차가 같은 표를 낸다.

주의: 표를 바꾸면 비트스트림이 바뀐다. 옛 파일이 안 읽힌다.
바꾼 뒤에는 `python test/run_tests.py --update` 로 고정 결과를 갱신하고,
`SPEC.md` 의 문법 변경 이력에 적는다.

사용:
  python tools/learn_probs.py                     # testdata 8장, 품질 4단계
  python tools/learn_probs.py --data ourdata --images 12 --qualities 4,12,24,48
  python tools/learn_probs.py --dry               # 표만 뽑고 심지는 않는다
"""
import argparse
import io
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = ("src/transform.c src/adst.c src/restore.c src/color.c src/predict.c src/loopfilter.c "
       "src/rangecoder.c src/encode.c src/decode.c tools/cli.c")
BIN = os.path.join(ROOT, "ingot.exe" if os.name == "nt" else "ingot")
DUMP = os.path.join(ROOT, "probs.txt")
CDF_TOTAL = 1 << 15


def sh(cmd):
    return subprocess.run(cmd, shell=True, cwd=ROOT,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def read_const(name, default):
    """internal.h 에서 상수 하나를 읽는다."""
    p = os.path.join(ROOT, "src/internal.h")
    for line in io.open(p, encoding="utf-8"):
        m = re.match(r"#define\s+%s\s+(\d+)" % re.escape(name), line.strip())
        if m:
            return int(m.group(1))
    return default


EXTRA_FLAGS = [""]      # --flags 로 받은 빌드 손잡이. 표마다 문법이 다르다


def build_with_dump(flat):
    """flat 이면 표를 무시하고 반반에서 시작한다 (첫 바퀴)."""
    if os.path.exists(BIN):
        os.remove(BIN)
    extra = " -DINGOT_PROB_LEARN" if flat else ""
    extra += " " + EXTRA_FLAGS[0]
    r = sh("gcc -std=c99 -O2 -DINGOT_PROB_DUMP" + extra + " -w " + SRC + " -o ingot -lm")
    if r.returncode or not os.path.exists(BIN):
        print((r.stdout or b"").decode("utf-8", "replace")[-800:])
        sys.exit("수집용 빌드 실패")
    print("수집용 빌드 완료")
    _FLAT[0] = flat


_FLAT = [True]


def wait_runnable():
    """이 기계는 갓 만든 실행 파일을 한동안 막는다. 풀릴 때까지 기다리고,
    그래도 안 되면 지웠다 다시 만든다."""
    for attempt in range(6):
        try:
            subprocess.run([BIN], cwd=ROOT, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            return
        except OSError:
            time.sleep(2.0)
            if attempt == 2:
                build_with_dump(_FLAT[0])
    sys.exit("실행 파일이 끝내 안 돌아갑니다: " + BIN)


def run_enc(path, q, env):
    for attempt in range(6):
        try:
            subprocess.run([BIN, "enc", path, "_learn.igt", str(q), "0", "0"],
                           cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            return
        except OSError:
            time.sleep(2.0)
    sys.exit("실행 파일을 못 돌렸습니다: " + BIN)


def collect(data, count, qualities):
    if os.path.exists(DUMP):
        os.remove(DUMP)
    d = os.path.join(ROOT, data)
    imgs = [f for f in sorted(os.listdir(d)) if f.endswith(".ppm")][:count]
    if not imgs:
        sys.exit("PPM 이 없습니다: " + d)
    wait_runnable()
    env = dict(os.environ)
    env["INGOT_DUMP_PATH"] = "probs.txt"
    for f in imgs:
        for q in qualities:
            run_enc(os.path.join(data, f), q, env)
    for junk in ("_learn.igt",):
        p = os.path.join(ROOT, junk)
        if os.path.exists(p):
            os.remove(p)
    print("수집 완료: 이미지 %d장 × 품질 %d단계" % (len(imgs), len(qualities)))


def average():
    acc = {}
    for line in io.open(DUMP, encoding="utf-8"):
        i, v = line.split()
        acc.setdefault(int(i), []).append(float(v))
    n = len(acc)
    return n, [int(round(sum(acc[i]) / len(acc[i]))) for i in range(n)]


def fix_up(vals, syms):
    """앞쪽은 이진 확률, 뒤쪽은 누적 확률표다. 성질이 달라 따로 다듬는다."""
    n = len(vals)
    base = None
    for i in range(n - syms, 0, -syms):
        if abs(vals[i + syms - 1] - CDF_TOTAL) < 8:
            base = i
        else:
            break
    if base is not None:
        for i in range(base, n, syms):
            seg = vals[i:i + syms]
            if len(seg) < syms:
                break
            seg[syms - 1] = CDF_TOTAL
            for j in range(syms - 1):
                lo = seg[j - 1] if j > 0 else 0
                cap = CDF_TOTAL - (syms - 1 - j)
                if seg[j] <= lo:
                    seg[j] = lo + 1
                if seg[j] > cap:
                    seg[j] = cap
            vals[i:i + syms] = seg
        head_end = base
    else:
        head_end = n
    # 이진 확률이 극단으로 가면 새 이미지에서 되레 손해다. 안전선 안으로 당긴다.
    for i in range(head_end):
        vals[i] = max(64, min(1984, vals[i]))
    return vals, base


def emit(vals, base, name="ingot_prob_init"):
    n = len(vals)
    rows = []
    for i in range(0, n, 12):
        rows.append("    " + " ".join("{:5d},".format(v) for v in vals[i:i + 12]))
    table = "\n".join(rows).rstrip(",")
    where = ("앞 %d 칸은 이진 확률, 뒤는 누적 확률표다." % base) if base else ""
    return """/* ---- 시작 확률표 ----
 *
 * 조각마다 확률이 반반에서 출발하면, 조각 하나가 끝날 때쯤에야 자리를 잡고
 * 다음 조각에서 다시 반반으로 돌아간다. 조각을 잘게 나눌수록 이 낭비가 커진다.
 * 그래서 시험 이미지를 여러 품질로 담아 보고, 조각이 끝난 시점의 확률을 모아
 * 평균한 값을 시작값으로 깐다. %s
 *
 * 이 표는 규격의 일부다. 디코더도 같은 값에서 출발해야 한다.
 * 다시 만들려면 `python tools/learn_probs.py` 를 쓴다.
 */
static const uint16_t %s[%d] = {
%s
};
""" % (where, name, n, table)


def install(code, name="ingot_prob_init"):
    p = os.path.join(ROOT, "src/rangecoder.c")
    s = io.open(p, encoding="utf-8").read()
    a = s.index("static const uint16_t %s[" % name)
    b = s.index("};", a) + 3
    c = s.rfind("/* ---- 시작 확률표", 0, a)
    if c < 0:
        c = a
    s = s[:c] + code + s[b:]
    # 표를 지키는 가드의 칸 수도 같이 고친다. 안 고치면 표는 새 배치인데
    # 가드는 옛 수를 들고 있어서, 배치를 넓히는 손잡이가 컴파일에서 막힌다
    # (2026-08-23 에 32x32 를 재려다 걸렸다).
    m = re.search(r"static const uint16_t %s\[(\d+)\]" % re.escape(name), s)
    if m:
        cnt = m.group(1)
        s = re.sub(r"#if INGOT_PROB_COUNT != \d+", 
                   "#if INGOT_PROB_COUNT != " + cnt, s)
        s = re.sub(r"학습 확률표 \d+ 칸이", "학습 확률표 " + cnt + " 칸이", s)
    io.open(p, "w", encoding="utf-8").write(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="testdata")
    ap.add_argument("--images", type=int, default=8)
    ap.add_argument("--qualities", default="4,12,24,48")
    ap.add_argument("--rounds", type=int, default=2,
                    help="학습 바퀴 수. 기본 2 (규격의 표를 만든 값)")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--flags", default="",
                    help='빌드 손잡이. 예: "-DINGOT_DIRPRED=1"')
    ap.add_argument("--table", default="base", choices=("base", "dir"),
                    help="심을 표. dir 이면 ingot_prob_init_dir 을 갈아 끼운다")
    a = ap.parse_args()

    EXTRA_FLAGS[0] = a.flags
    name = "ingot_prob_init_dir" if a.table == "dir" else "ingot_prob_init"
    syms = read_const("INGOT_CDF_SYMS", 0)
    qs = [int(x) for x in a.qualities.split(",")]

    code = None
    for rnd in range(a.rounds):
        print("--- 바퀴 %d/%d ---" % (rnd + 1, a.rounds))
        build_with_dump(flat=(rnd == 0))
        collect(a.data, a.images, qs)
        n, vals = average()
        vals, base = fix_up(vals, syms) if syms else (
            [max(64, min(1984, v)) for v in vals], None)
        print("무리 %d 칸%s" % (n, (", 누적 확률표 시작 %d" % base) if base else ""))
        code = emit(vals, base, name)
        if rnd < a.rounds - 1:
            install(code, name)   # 다음 바퀴가 이 표를 시작값으로 쓴다

    if a.dry:
        print(code[:400] + " ...")
        print("(--dry 라 마지막 표는 심지 않았습니다)")
        return
    install(code, name)
    if os.path.exists(DUMP):
        os.remove(DUMP)
    print("src/rangecoder.c 에 심었습니다.")
    print("다음: sh build.sh && python test/run_tests.py --update")
    print("      그리고 SPEC.md 의 문법 변경 이력에 적으십시오.")


main()
