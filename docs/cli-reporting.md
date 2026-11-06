# Module inspection and JSON diagnostics

```sh
./dm --version
./dm stats demos/threshold.dmt
./dm stats build/modules/threshold.dms --json
./dm check demos/threshold.dmt --json
```

`--version` (also `-V`) reports the engine's current v4 generation and DMS
format version. It does not report a Git revision or imply a published binary
release. `stats` and `check` accept either source `.dmt` or compiled `.dms`.
Both compile/load and verify before returning success. Neither renders audio
or video or writes a module file. `stats` without `--json` prints a readable
summary and program table to stdout. Ordinary `check` keeps its existing
human summary on stderr.

With `--json`, stdout contains exactly one JSON object followed by a newline,
without a banner or human diagnostics. Option order is flexible; use `--` to
end options for `stats`, or for `check --json`, before a filename starting
with `-`. `--help` still displays human help.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | Module verified and report written |
| 1 | Source/module could not be read, compiled or verified, or output failed |
| 2 | Invalid command arguments |

## Check result

```json
{"schema_version":1,"ok":true,"file":"demo.dmt","error":null}
```

On failure:

```json
{"schema_version":1,"ok":false,"file":"bad.dmt","error":{"kind":"load","message":"bad.dmt:1: unknown directive 'nonsense'"}}
```

`error.kind` is `load` for read/compile/verification failures, or `usage` for
invalid arguments. `file` is null when no filename was supplied. The compiler's
existing diagnostic, including its source line when available, is preserved
in `message`. Separate line/column fields and multiple diagnostics are future
work; consumers should not depend on parsing the message wording.

## Stats result

The top-level fields are `schema_version`, `ok`, `file`, `title`,
`module_version`, `song`, `resources`, and `programs`.

| Field | Contents |
|---|---|
| `song` | Initial `bpm`, `speed` (ticks/row), `rows_per_beat`, `channels`, `orders` (count), `loop_order` (zero-based index) |
| `resources` | `code_bytes`, `instruments`, `patterns`, `cells`, `uniforms`, `pixel_programs`, `palettes`, `texts`, `samples`, `sample_bytes`, `textures`, `texture_bytes`, `envelopes`, `event_names`, `events` |
| Each `programs` entry | `kind`, `index`, `name`, `offset`, `bytes`, `locals`, `state`, `delays`, `reverbs` |

Program kinds are `inst`, `chan`, `master`, `frame`, `pixel`, and `post`.
Instrument indices start at 1; channel and pixel indices start at 0. Singleton
programs use index 0. A channel effect has an empty `name` because DMS stores
its channel index rather than a name. Pixel entries form the scene index/name
map used by `select()`.

Bytecode offsets and sizes are bytes. `locals` and `state` are float slots;
`delays` and `reverbs` are allocated effect counts. Sample bytes describe the
embedded mono PCM; texture bytes describe embedded RGB8 pixels. These are
payload sizes, not total file size or runtime heap estimates. Instrument count
follows the module's instrument slot count, including gaps in sparse binaries.

`uniforms` is a count: the v4 binary does not retain uniform names. Tempo
effects can change playback timing, so initial song fields do not imply a
duration estimate. The report measures storage/resources, not rendering cost;
`dm bench` and authoring lint remain separate work.

Strings escape quotes and control bytes and preserve valid UTF-8. Invalid
UTF-8 bytes are represented as `\u00XX`, giving valid JSON with a readable byte
value rather than failing the entire report. Ignore unknown fields so future
additive schema changes do not break consumers.
