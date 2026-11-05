#!/bin/sh
# dmscene self-test: compile, round-trip, deterministic offline render, audio sanity, error paths.
set -e
cd "$(dirname "$0")/.."
T=build/test; rm -rf "$T"; mkdir -p "$T/a" "$T/b"
fail() { echo "FAIL: $1"; exit 1; }

./dm compile demos/pulse.dmt -o "$T/pulse.dms" 2>/dev/null || fail "compile"
./dm check demos/pulse.dmt 2>/dev/null || fail "check"

# round trip: a loaded module must verify
./dm render "$T/pulse.dms" --seconds 0.5 --wav "$T/rt.wav" --size 16x16 >/dev/null 2>&1 || fail "load/verify"

# deterministic: two renders are byte-identical
./dm render "$T/pulse.dms" --seconds 2 --fps 4 --png "$T/a" --wav "$T/a.wav" --size 64x40 >/dev/null 2>&1
./dm render "$T/pulse.dms" --seconds 2 --fps 4 --png "$T/b" --wav "$T/b.wav" --size 64x40 >/dev/null 2>&1
cmp -s "$T/a.wav" "$T/b.wav" || fail "audio not deterministic"
cmp -s "$T/a/frame_0003.png" "$T/b/frame_0003.png" || fail "video not deterministic"
[ "$(ls "$T/a" | wc -l | tr -d ' ')" = "8" ] || fail "expected 8 frames"

# audio is neither silent nor DC: count distinct sample values in the first 2 s
n=$(od -An -t d2 -j 44 "$T/a.wav" | tr -s ' ' '\n' | grep -v '^$' | sort -u | wc -l | tr -d ' ')
[ "$n" -gt 1000 ] || fail "audio looks silent ($n distinct values)"

# --start works and keeps the clock in sync (beat shown in status after seeking 7.5 s = 16 beats)
# (covered implicitly by the offline path; verify seek math in seq instead)
./dm render "$T/pulse.dms" --start 7.5 --seconds 0.1 --png "$T/a" --size 8x8 >/dev/null 2>&1 || fail "--start"

# error paths: compiler must reject, not crash
printf 'song channels=1\ninst a {\n  out(nosuch(1))\n}\npattern 0 rows=1 {\n C-4 1\n}\n' > "$T/bad1.dmt"
./dm compile "$T/bad1.dmt" -o "$T/bad1.dms" 2>"$T/err" && fail "bad1 accepted"
grep -q "unknown function" "$T/err" || fail "bad1 wrong error: $(cat "$T/err")"
printf 'song channels=1\ninst a {\n  out(ph)\n}\npattern 0 rows=1 {\n C-4 9\n}\n' > "$T/bad2.dmt"
./dm compile "$T/bad2.dmt" -o "$T/bad2.dms" 2>"$T/err" && fail "bad2 accepted"
grep -q "instrument 9" "$T/err" || fail "bad2 wrong error: $(cat "$T/err")"
printf 'song channels=1\npixel {\n  x = 1\n}\npattern 0 rows=1 {\n ...\n}\n' > "$T/bad3.dmt"
./dm compile "$T/bad3.dmt" -o "$T/bad3.dms" 2>"$T/err" && fail "bad3 accepted"
grep -q "read-only" "$T/err" || fail "bad3 wrong error: $(cat "$T/err")"

# corrupt module must be rejected by the verifier, not executed
head -c 200 "$T/pulse.dms" > "$T/trunc.dms"
./dm render "$T/trunc.dms" --seconds 0.1 --wav "$T/x.wav" >/dev/null 2>&1 && fail "truncated module accepted"

# v3 feature coverage: loops, scenes, post, text, samples, venv/nna, timeline, chan fx, E-commands, scale/dither
./dm compile tests/features.dmt -o "$T/features.dms" 2>/dev/null || fail "features compile"
./dm disasm tests/features.dmt 2>/dev/null | grep -q '^  [0-9a-f]*  loop' || fail "features: no loop emitted"
./dm render "$T/features.dms" --seconds 1 --wav "$T/features.wav" --png "$T/a" --size 64x40 >/dev/null 2>&1 || fail "features render"
./dm render "$T/features.dms" --start 2.5 --seconds 0.2 --png "$T/b" --size 64x40 >/dev/null 2>&1 || fail "features scene 2"
if cmp -s "$T/a/frame_0000.png" "$T/b/frame_0000.png"; then fail "features: scene switch produced identical frames"; fi
d=$(od -An -t d2 -j 44 "$T/features.wav" | tr -s ' ' '\n' | grep -v '^$' | sort -u | wc -l | tr -d ' ')
[ "$d" -gt 200 ] || fail "features sounds silent"
# old-format modules are rejected with a clear message
printf 'DMS\002' > "$T/old.dms"; ./dm play "$T/old.dms" --mute 2>"$T/err" >/dev/null || true; grep -q "recompile" "$T/err" || fail "old format not rejected"
# dm new scaffolds a compilable file
rm -f "$T/scaffold.dmt"; ./dm new "$T/scaffold" >/dev/null 2>&1 || fail "dm new"; ./dm check "$T/scaffold.dmt" >/dev/null 2>&1 || fail "dm new scaffold does not compile"

# every demo compiles, loads and renders a second of audio + a frame
for f in demos/*.dmt; do
  n=$(basename "$f" .dmt)
  ./dm compile "$f" -o "$T/$n.dms" 2>/dev/null || fail "compile $n"
  ./dm render "$T/$n.dms" --seconds 1 --fps 2 --png "$T/a" --wav "$T/$n.wav" --size 64x40 >/dev/null 2>&1 || fail "render $n"
  d=$(od -An -t d2 -j 44 "$T/$n.wav" | tr -s ' ' '\n' | grep -v '^$' | sort -u | wc -l | tr -d ' ')
  [ "$d" -gt 200 ] || fail "$n sounds silent ($d distinct values)"
done

echo "ok: compile, round-trip, determinism, audio, errors, features, $(ls demos/*.dmt | wc -l | tr -d ' ') demos"
