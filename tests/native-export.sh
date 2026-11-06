#!/bin/sh
# Compare complete native-export files against the interpreter, including PCM.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/dmscene-native.XXXXXX")
trap 'rm -rf "$T"' 0
trap 'exit 1' HUP INT TERM
for f in demos/*.dmt; do
  name=${f##*/}; name=${name%.dmt}
  make "build/native/$name" > "$T/build.log" 2>&1 || { cat "$T/build.log"; exit 1; }
  ./avi-prototype "$f" "$T/vm.avi" --seconds 1 --fps 4 --size 64x40 --threads 4 2>"$T/vm.log"
  "build/native/$name" "$f" "$T/native.avi" --seconds 1 --fps 4 --size 64x40 --threads 4 2>"$T/native.log"
  cmp "$T/vm.avi" "$T/native.avi"
  echo "identical: $name"
done
# Exercise all showcase chapters, feedback history, and both carnival scenes.
for name in showcase carnival; do
  ./avi-prototype "demos/$name.dmt" "$T/vm.mov" --seconds 44 --fps 2 --size 64x40 --threads 4 2>"$T/vm.log"
  "build/native/$name" "demos/$name.dmt" "$T/native.mov" --seconds 44 --fps 2 --size 64x40 --threads 4 2>"$T/native.log"
  cmp "$T/vm.mov" "$T/native.mov"
  if command -v ffmpeg >/dev/null 2>&1; then ffmpeg -v error -i "$T/native.mov" -f null -; fi
  echo "identical: $name full timeline"
done
# AVI scanline padding and lossless MOV decoding must agree at odd dimensions.
if command -v ffmpeg >/dev/null 2>&1; then
  for ext in avi mov; do
    build/native/threshold demos/threshold.dmt "$T/odd.$ext" --seconds 1 --fps 4 --size 97x63 --threads 4 2>/dev/null
    ffmpeg -v error -i "$T/odd.$ext" -map 0:v:0 -pix_fmt rgb24 -f rawvideo "$T/$ext.rgb"
    ffmpeg -v error -i "$T/odd.$ext" -map 0:a:0 -c copy -f s16le "$T/$ext.pcm"
  done
  cmp "$T/avi.rgb" "$T/mov.rgb"
  cmp "$T/avi.pcm" "$T/mov.pcm"
fi
# Reject using a specialized executable with a different demo.
if build/native/showcase demos/dunes.dmt "$T/wrong.avi" 2>/dev/null; then
  echo "FAIL: mismatched bytecode accepted"; exit 1
fi
echo "ok: native/interpreter video and audio equivalence"
