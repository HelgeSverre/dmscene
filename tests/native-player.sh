#!/bin/sh
# The native player is the normal CLI with safe shader specialization/fallback.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/dmscene-native-player.XXXXXX")
trap 'rm -rf "$T"' 0
trap 'exit 1' HUP INT TERM
make build/native-player/hakkuh build/tests/native-bind > "$T/build.log" 2>&1 || { cat "$T/build.log"; exit 1; }
./build/tests/native-bind build/modules/hakkuh.dms
for name in hakkuh plasma; do
  ./dm render "demos/$name.dmt" --avi "$T/vm.avi" --seconds .5 --fps 4 --size 64x40 --threads 2 >/dev/null 2>&1
  ./build/native-player/hakkuh render "demos/$name.dmt" --avi "$T/native.avi" --seconds .5 --fps 4 --size 64x40 --threads 2 >/dev/null 2>&1
  cmp "$T/vm.avi" "$T/native.avi"
done
./build/tests/scrub-live ./build/native-player/hakkuh kitty
printf 'q' | ./build/native-player/hakkuh play demos/hakkuh.dmt --mute --graphics kitty >/dev/null
printf 'q' | ./build/native-player/hakkuh watch demos/hakkuh.dmt --mute --graphics text >/dev/null
echo 'ok: native player RGB/PCM equivalence, different-demo fallback and terminal transport'
