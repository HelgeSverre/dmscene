#!/bin/sh
# Focused qtrle MOV round-trip: odd width, static/delta rows, variable PCM chunks.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/dmscene-qtrle.XXXXXX")
trap 'rm -rf "$T"' EXIT HUP INT TERM
cc -O2 -std=c99 -Wall -Wextra -Werror -Iinclude tests/qtrle-equivalence.c src/dm_mov.c -o "$T/qtrle-equivalence"
"$T/qtrle-equivalence" "$T/test.mov" "$T/expect.rgb" "$T/expect.pcm"
ffmpeg -v error -i "$T/test.mov" -map 0:v:0 -f rawvideo -pix_fmt rgb24 "$T/got.rgb"
ffmpeg -v error -i "$T/test.mov" -map 0:a:0 -f s16le -acodec pcm_s16le "$T/got.pcm"
cmp "$T/expect.rgb" "$T/got.rgb"
cmp "$T/expect.pcm" "$T/got.pcm"
[ "$(ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 "$T/test.mov" | grep -c K)" = 2 ]
echo "ok: qtrle RGB/PCM equivalence, variable chunks, periodic keyframes"
