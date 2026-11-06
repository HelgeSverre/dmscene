#!/bin/sh
# Default playback acceleration: cold-cache concurrency and compiler-free fallback.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/dmscene-native-launch.XXXXXX")
trap 'rm -rf "$T"' 0
trap 'exit 1' HUP INT TERM
fail() { echo "FAIL: $1" >&2; exit 1; }
rm -f build/native-player/hakkuh
./dm play demos/hakkuh.dmt --avi "$T/a.avi" --seconds .25 --fps 4 --size 32x20 --threads 1 > "$T/a.log" 2>&1 &
a=$!
./dm play demos/hakkuh.dmt --avi "$T/b.avi" --seconds .25 --fps 4 --size 32x20 --threads 1 > "$T/b.log" 2>&1 &
b=$!
wait "$a" || { cat "$T/a.log"; fail 'first native launch'; }
wait "$b" || { cat "$T/b.log"; fail 'second native launch'; }
cmp "$T/a.avi" "$T/b.avi" || fail 'concurrent native outputs differ'
grep -q 'preparing native shaders' "$T/a.log" || fail 'default omitted native build'
if grep -q 'using interpreter' "$T/a.log" "$T/b.log"; then fail 'native build silently failed'; fi
mkdir "$T/no-make"
printf 'q' | PATH="$T/no-make" ./dm play demos/hakkuh.dmt --mute > /dev/null 2> "$T/fallback.log" || fail 'automatic fallback failed'
grep -q 'using interpreter' "$T/fallback.log" || fail 'missing compiler did not report fallback'
if printf 'q' | PATH="$T/no-make" ./dm play demos/hakkuh.dmt --native --mute > /dev/null 2>&1; then fail 'required native build ignored failure'; fi
printf 'q' | PATH="$T/no-make" ./dm play demos/hakkuh.dmt --interpreted --mute > /dev/null 2> "$T/interpreted.log" || fail 'interpreted override failed'
if grep -q 'preparing native shaders' "$T/interpreted.log"; then fail 'interpreted override compiled shaders'; fi
echo 'ok: default native launch, shared build lock, build failure fallback and VM override'
