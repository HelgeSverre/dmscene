# Metadata in demo source

Optional header comments describe a demo without changing its bytecode, audio
or visuals. No hand-maintained manifest or README entry is required for a demo
to appear in the gallery.

```text
# @author Your name
# @model Model name, if applicable
# @description A short description for the gallery and demo page.
# @description A second paragraph with background or production notes.
# @music Dub bass, filtered chords and tape echoes.
# @visual Slowly rotating geometry with feedback trails.
# @tags ambient, feedback, texture
# @duration 48.5
# @collection Weekend experiments
# @license CC-BY-4.0
# @url https://example.com/demo
# @warning Contains flashing lights.
title NIGHT SIGNAL
song bpm=100 speed=6 rpb=4 channels=4
```

All fields are optional. Put them before the first non-comment directive.
Both `# @key value` and `// @key value` work; ordinary comments and blank lines
can be mixed in. The native compiler already ignores these comments, so old
players and the binary format need no changes. `title` remains the display
title; metadata does not duplicate BPM, channels or other executable settings.

| Field | Meaning |
|---|---|
| `author`, `model` | Human credits and optional model attribution; repeated entries are joined |
| `description` | Gallery summary and page notes; repeat for additional paragraphs |
| `music`, `visual` | Separate descriptions of the soundtrack and imagery |
| `tags` | Comma- or space-separated tags; lowercase, deduplicated, repeatable; letters, digits, `_` and `-` |
| `duration` | Intended preview length in positive decimal seconds; one value only |
| `collection` | Optional grouping/series name |
| `license` | Author-supplied license label; no default license is inferred |
| `url` | Absolute HTTP(S) link |
| `warning` | Author-supplied content/playback notice |

Unknown keys are ignored for forward compatibility. Invalid duration, tag or
URL values fail site generation with the metadata line number. Values are
plain text, escaped before insertion into HTML; they are not Markdown or HTML.
Repeated text fields other than description are comma-separated; use one URL.

The gallery reads every `.dmt` automatically. Explicit description metadata
takes precedence over leading prose comments; without it, those comments are
used as before. Custom tags automatically become filters. A valid MP4's actual
duration takes precedence for the video; otherwise `@duration` controls the
preview, followed by the source scanner's estimated first traversal length.
It does not change native playback or automatically set `dm render --seconds`.
The current browser player prepares at most 300 seconds of audio.

`make site` writes derived HTML and per-demo JSON for browser use. That JSON is
generated output, not another place to edit metadata. Update the `.dmt` and
run `make site` again. Existing model credits and editorial descriptions were
migrated from the README into demo headers; the generator no longer reads the
README for metadata.

Sample/texture paths for the website must remain within `demos/`, using relative
paths without `..`. Nested directories and duplicate basenames are supported;
only declared assets are copied into the generated site's `source/` tree.
