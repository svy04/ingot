#!/bin/sh
# Build. The only dependency is the C standard library.
#
# The source list is a glob on purpose. It used to be written out by hand and
# went stale three times as files were added -- once here and twice in
# tools/learn_probs.py -- each time surfacing as a link error or, worse, a
# silently different build.
set -e
CC=${CC:-gcc}
CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter"
$CC $CFLAGS src/*.c tools/cli.c -o ingot -lm
echo "built: ./ingot"
