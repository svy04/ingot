#!/bin/sh
# Build. The only dependency is the C standard library.
set -e
CC=${CC:-gcc}
CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter"
$CC $CFLAGS src/transform.c src/color.c src/bitio.c src/encode.c src/decode.c tools/cli.c -o ingot -lm
echo "built: ./ingot"
