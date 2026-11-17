#!/bin/sh
# Builds and runs rtp_video_stats_test.c against the real RtpVideoQueue.c.
# Needs only a C compiler and the enet/nanors submodules (git submodule update --init).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/rtp-video-stats-test
mkdir -p "$OUT"
CC=${CC:-cc}
FLAGS="-std=c11 -g -DHAS_SOCKLEN_T -Wall -Wextra -Wno-unused-parameter
  -I$ROOT/src -I$ROOT/enet/include -I$ROOT/nanors -I$ROOT/nanors/deps -I$ROOT/nanors/deps/obl"
# shellcheck disable=SC2086
$CC $FLAGS -o "$OUT/test" \
  "$ROOT/tools/rtp_video_stats_test.c" "$ROOT/src/RtpVideoQueue.c" \
  "$ROOT/nanors/rs.c" "$ROOT/nanors/deps/obl/oblas_common.c" "$ROOT/nanors/deps/obl/oblas_lite.c"
"$OUT/test"
