#!/bin/sh
# Public native-container CLI smoke: flags compose with the normal renderer.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/dmscene-native-cli.XXXXXX")
trap 'rm -rf "$T"' 0
trap 'exit 1' HUP INT TERM
fail() { echo "FAIL: $1" >&2; exit 1; }

mkdir "$T/png"
./dm render demos/pulse.dmt --avi "$T/pulse.avi" --png "$T/png" --wav "$T/pulse.wav" \
  --start 0.1 --seconds 0.5 --fps 4 --size 97x63 --render-scale 2 --threads 1 --solo 0 >/dev/null 2>&1 || fail "AVI CLI export"
[ -s "$T/pulse.avi" ] && [ -s "$T/pulse.wav" ] && [ -s "$T/png/frame_0000.png" ] || fail "AVI siblings"
./dm render demos/pulse.dmt --mov "$T/pulse.mov" --start 0.1 --seconds 0.5 --fps 4 --size 97x63 --render-scale 2 --threads 1 --solo 0 >/dev/null 2>&1 || fail "MOV CLI export"
[ -s "$T/pulse.mov" ] || fail "MOV missing"
./dm render demos/pulse.dmt --avi "$T/bad.avi" --mov "$T/bad.mov" --seconds 0.5 >/dev/null 2>&1 && fail "multiple containers accepted"
./dm render demos/pulse.dmt --avi "$T/same" --wav "$T/same" --seconds 0.5 >/dev/null 2>&1 && fail "aliased native outputs accepted"
./dm render demos/pulse.dmt --wav "$T/tiny.wav" --seconds 0.01 >/dev/null 2>&1 || fail "sub-frame WAV export"
mkdir "$T/empty-path"
PATH="$T/empty-path" ./dm render demos/pulse.dmt --mov "$T/no-ffmpeg.mov" --seconds 0.5 --fps 4 --size 97x63 >/dev/null 2>&1 || fail "native export depends on ffmpeg"
printf 'keep' > "$T/huge.avi"
./dm render demos/pulse.dmt --avi "$T/huge.avi" --seconds 10000 --size 960x600 >/dev/null 2>&1 && fail "oversize AVI accepted"
[ "$(cat "$T/huge.avi")" = keep ] || fail "oversize AVI replaced output"

if command -v ffprobe >/dev/null 2>&1 && command -v ffmpeg >/dev/null 2>&1; then
  ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$T/pulse.avi" | grep -qx '97,63' || fail "AVI odd dimensions"
  ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$T/pulse.mov" | grep -qx '97,63' || fail "MOV odd dimensions"
  ffmpeg -v error -i "$T/pulse.avi" -map 0:a:0 -c copy -f s16le "$T/avi.pcm"
  dd if="$T/pulse.wav" of="$T/wav.pcm" bs=44 skip=1 status=none
  cmp "$T/avi.pcm" "$T/wav.pcm" || fail "AVI PCM differs from WAV"
  ffmpeg -v error -i "$T/pulse.avi" -map 0:v:0 -pix_fmt rgb24 -f rawvideo "$T/avi.rgb"
  ffmpeg -v error -i "$T/pulse.mov" -map 0:v:0 -pix_fmt rgb24 -f rawvideo "$T/mov.rgb"
  cmp "$T/avi.rgb" "$T/mov.rgb" || fail "native RGB differs"
fi
echo "ok: native AVI/MOV CLI"
