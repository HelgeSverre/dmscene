#!/bin/sh
# Exercise the actual CLI protocol, not private serialization functions.
set -eu
T=$(mktemp -d "${TMPDIR:-/tmp}/dmscene-inspect.XXXXXX")
trap 'rm -rf "$T"' 0
trap 'exit 1' HUP INT TERM
DM="$(pwd)/dm"
fail() { echo "FAIL: $1" >&2; exit 1; }
cd "$T"

cat > demo.dmt <<'EOF'
title Café "quoted" \ scene
song channels=1
pattern 0 rows=1 {
 .
}
pixel {
 rgb(0,0,0)
}
EOF

"$DM" --version > actual
printf 'dmscene v4 (DMS format 4)\n' > expected
cmp actual expected || fail "version output"
"$DM" check demo.dmt --json > actual 2> errors
printf '{"schema_version":1,"ok":true,"file":"demo.dmt","error":null}\n' > expected
cmp actual expected || fail "check JSON success"
[ ! -s errors ] || fail "JSON success contaminated stderr"
"$DM" check demo.dmt > actual 2> errors
[ ! -s actual ] && [ -s errors ] || fail "legacy check output changed"

"$DM" stats demo.dmt --json > actual 2> errors
cat > expected <<'EOF'
{"schema_version":1,"ok":true,"file":"demo.dmt","title":"Café \"quoted\" \\ scene","module_version":4,"song":{"bpm":125,"speed":6,"rows_per_beat":4,"channels":1,"orders":1,"loop_order":0},"resources":{"code_bytes":7,"instruments":0,"patterns":1,"cells":1,"uniforms":0,"pixel_programs":1,"palettes":0,"texts":0,"samples":0,"sample_bytes":0,"textures":0,"texture_bytes":0,"envelopes":0,"event_names":0,"events":0},"programs":[{"kind":"pixel","index":0,"name":"main","offset":0,"bytes":7,"locals":0,"state":0,"delays":0,"reverbs":0}]}
EOF
cmp actual expected || fail "stats JSON and UTF-8/quote escaping"
[ ! -s errors ] || fail "stats JSON contaminated stderr"
"$DM" compile demo.dmt -o demo.dms >/dev/null 2> errors
"$DM" stats demo.dms --json > actual
sed 's/"file":"demo.dmt"/"file":"demo.dms"/' expected > binary-expected
cmp actual binary-expected || fail "source/module statistics differ"
"$DM" stats demo.dms > actual
grep -q '^pixel' actual || fail "human program table"

printf 'nonsense\n' > bad.dmt
set +e
"$DM" check bad.dmt --json > actual 2> errors
status=$?
set -e
[ "$status" = 1 ] || fail "invalid source exit status"
printf '{"schema_version":1,"ok":false,"file":"bad.dmt","error":{"kind":"load","message":"bad.dmt:1: unknown directive '\''nonsense'\''"}}\n' > expected
cmp actual expected || fail "compiler error JSON"
[ ! -s errors ] || fail "JSON error contaminated stderr"

set +e
"$DM" check --json > actual 2> errors
status=$?
set -e
[ "$status" = 2 ] || fail "usage exit status"
printf '{"schema_version":1,"ok":false,"file":null,"error":{"kind":"usage","message":"missing input file"}}\n' > expected
cmp actual expected || fail "usage error JSON"

# Invalid bytes may occur in source titles too; macOS filesystems do not permit
# all malformed UTF-8 filenames, so exercise that encoding path through a title.
printf 'title bad\377\n' > invalid-utf8.dmt
sed '1d' demo.dmt >> invalid-utf8.dmt
"$DM" stats invalid-utf8.dmt --json > actual
grep -Fq '"title":"bad\u00ff"' actual || fail "invalid UTF-8 escaping"
tab_name=$(printf 'tab\tfile.dmt')
cp demo.dmt "$tab_name"
"$DM" check "$tab_name" --json > actual
printf '{"schema_version":1,"ok":true,"file":"tab\\u0009file.dmt","error":null}\n' > expected
cmp actual expected || fail "control-character filename escaping"
cp demo.dmt ./--help
"$DM" stats --json -- --help > actual
grep -q '"ok":true,"file":"--help"' actual || fail "end-of-options handling"
echo 'ok: CLI version, stats, source/module parity, JSON success/errors and escaping'
