# -*- coding: utf-8 -*-
"""손잡이 하나를 값마다 기준선과 견준다. trial.py 를 되풀이해 부른다.

**주의**: trial.py 가 `-w` 로 빌드하므로 없는 이름을 넘겨도 아무 말 없이
같은 바이너리가 나온다. 값마다 결과가 똑같으면 손잡이가 실제로 있는지
`grep` 으로 먼저 확인한다.

  python tools/sweep.py INGOT_LF_TC 8 16 24
  python tools/sweep.py INGOT_MODE_TRIALS 2 3 4 --data ourdata --images 12
"""
import subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

argv = sys.argv[1:]
extra = []
for k in ("--data", "--images"):
    if k in argv:
        i = argv.index(k)
        extra += [k, argv[i + 1]]
        del argv[i:i + 2]

name = argv[0]
vals = argv[1:]
print("%s 훑기 — 기준선 대비 (음수 = 낫다)" % name)
for v in vals:
    r = subprocess.run([sys.executable, "tools/trial.py", "-D%s=%s" % (name, v)] + extra,
                       cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print((r.stdout or b"").decode("utf-8", "replace").rstrip())
    sys.stdout.flush()
