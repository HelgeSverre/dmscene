# ONE VM / NO SLEEP: music research and rewrite

Research date: 2026-11-06. [Research brief](music-brief.md).

Version note: this report records the earlier 150 BPM composition experiment
and the subsequent synth fix. The current FULL SEND revision, requested after
that experiment, runs at 180 BPM (42.632 seconds) with a faster rave score,
recursive zoom scenes and kinetic typography. Current timings and preview
are in the [README](../../README.md#one-vm--no-sleep).

## Decision

Rewrite the original score as lean 150 BPM chip/electro: one original two-bar
call/response, a bass that answers the kick, fewer simultaneous bright parts,
a chord-led breakdown, and a single late full-texture payoff. Retain the
51.2-second runtime and four-bar visual chapter boundaries.

The findings below come from three independent research tracks. Forum advice
is practitioner opinion; agreement does not make it a universal musical rule.
Pouët credits and production metadata are stronger evidence than comments about
quality. We read the production pages and comments; we did **not** audition the
reference soundtracks, and do not present those comments as our own listening observations.

## Composition and arrangement report

**Confidence: medium for musical recommendations; high that the linked discussions
contain these practitioner positions.**

The useful pattern is repetition with small changes: establish a short motif,
then alter its ending, register, placement, or answering part. The KVR
[melody discussion](https://www.kvraudio.com/forum/viewtopic.php?t=425619)
emphasizes recognizable expectations, repetition and variation; the
[production discussion](https://www.kvraudio.com/forum/viewtopic.php?start=15&t=142123)
includes the practical idea of alternating a short phrase with accompaniment
and a response. These are useful constraints for this rewrite, rather than a
formula for a successful tune.

Gearspace's [arrangement techniques discussion](https://gearspace.com/threads/arrangement-techniques.523073/)
argues for developing a groove and its variations before layering the rest of
the track. Its [section repetition discussion](https://gearspace.com/threads/more-natural-section-copy-and-paste.688448/)
suggests changes in harmony, texture, fills and dropouts when a section returns.
The [UK garage production tips thread](https://gearspace.com/threads/a-few-tips-on-how-to-make-uk-garage-by-jeremy-sylvester.787359/)
provides an adjacent example of nesting short phrases within longer arrangement
boundaries. We apply the phrase principle, not its genre-specific recipe.

**Application:** a four-note call with a descending answer occupies two bars;
its last statement changes toward the dominant turnaround. The arp supplies
small replies instead of running continuously against the lead. The complete
texture is held back until bars 25–28.

**Limitations:** writers disagree on how much theory or repetition is desirable.
No source can establish that the resulting original melody sounds good to this
project's listener. Four-bar visual cuts also impose unusually short musical sections.

## Groove and production report

**Confidence: medium for musical recommendations; high for the documented range
of production choices.**

Elektronauts' [kick/bass mixing discussion](https://www.elektronauts.com/t/best-ways-to-good-mixing-kick-and-bass/146913)
and [working-together thread](https://www.elektronauts.com/t/getting-kick-and-bass-working-together/7547)
focus on deciding which source occupies the deepest range. The
[clash discussion](https://www.elektronauts.com/t/avoiding-bass-kick-clash/182628)
and [ducking thread](https://www.elektronauts.com/t/kick-and-bass-reverb-ducking/146991)
include arranging the hits and note lengths so kick and bass do not compete,
with ducking as an additional option rather than a mandatory answer.

ChipMusic's [production tips](https://chipmusic.org/forums/topic/317/what-trickstips-do-you-you-when-producing-your-chiptune/)
and [mixing discussion](https://chipmusic.org/forums/topic/8555/mixing/)
put useful emphasis on sound choice and tracking-stage balance, including
controlling unwanted low end in other parts. The
[heavy kick discussion](https://chipmusic.org/forums/topic/605/how-do-i-make-a-heavy-kick-sound-in-famitracker/)
is a constraint-based reference for distinct percussion roles.

**Application:** a shorter kick, independently phased triangle sub/pulse bass,
mostly offbeat bass attacks, quiet pickup hats and ghost snares. Pad, arp, lead
and effects have filtered low end. The old broad bass pump and long wet layers
are reduced; the score creates the space. Main propulsion uses a syncopated
kick cell; four-on-the-floor is reserved for the payoff.

**Limitations:** producers disagree about which voice should own the sub and
how prominent pumping should be. Tracker rows/ticks provide coarse timing;
we use note placement, velocity and occasional note delay rather than claim
DAW-like microtiming. Global swing could disturb the fixed audiovisual chapter
times, so it is not introduced here.

## Pouët inspiration report

**Confidence: high for page metadata; medium for lessons inferred from community
reactions. These are references to study, not sources of score or artwork.**

| Reference | Page evidence | Useful technique for this project |
| --- | --- | --- |
| [Everyway](https://www.pouet.net/prod.php?which=67790) | Amiga 64k credits include djh0ffman and Blueberry; comments praise music/visual fit and sync | Give every chapter entrance a musical event, and a clear role for its instruments |
| [obsidian](https://www.pouet.net/prod.php?which=61621) | Comments praise minimalism and the effect of sync on a central changing object | A new musical role can make an existing visual idea feel significant; density alone is not development |
| [Temper](https://www.pouet.net/prod.php?which=96647) | Comments praise the synth, restrained aesthetic, techno track and flow | Commit to a small sonic palette whose parts each do a distinct job |
| [[Type]](https://www.pouet.net/prod.php?which=66963) | Reactions include praise for style/sync and criticism of repetition | A repeated motif needs changes in orchestration and meaningful returns |
| [RedShift](https://www.pouet.net/prod.php?which=61884), [Struct](https://www.pouet.net/prod.php?which=56929) | Some comments question music/scene fit or the sense of climax | Align visual cuts with the arrangement's energy and reserve a real payoff |

**Originality:** no soundtrack, note transcription, instrument patch, pattern,
visual asset or scene sequence was extracted. The new notes, rhythms, envelopes,
instrument code and arrangement are authored for dmscene. The existing visual
scenes and chapter timing remain this project's work.

## Cross-track synthesis

| Decision question | Composition forums | Production forums | Pouët pages/comments | Adopted decision |
| --- | --- | --- | --- | --- |
| Memorable motif | Repeat a short phrase with selective variation | Keep the main voice distinguishable | Comments reward coherent musical identity | One original two-bar call/response; alter phrase endings |
| Groove and low end | Develop the rhythm before adding layers | Arrange short kick/bass events to share space | Restrained synth identities can work | Short independent sub/pulse bass, syncopated kick, deliberate rests |
| Breakdown and payoff | Withhold parts, vary repeated sections | Empty rows and contrasting density matter | Repetition and absent climax draw criticism | Chord-led breakdown; percussion rebuild; one full payoff |
| Tracker character | Use a small set of meaningful variants | Balance at source; distinguish sound roles | Own synth/palette can provide identity | Pulse/FM hook, bell replies, warm chord voice, dry percussion |
| Music/visual fit | Phrase boundaries support transitions | Fills and dropouts communicate change | Exact sync alone is not necessarily coherent flow | Musical state changes at each existing four-bar scene cut |
| Verification | Subjective listening remains necessary | Check overlap, envelopes and levels | Comments cannot validate our output | Gate regression, rendered audio dynamics/headroom and media checks; user audition for taste |

The convergence is around identity, space and purposeful contrast. The main
divergence is how much repetition or pumping is desirable; we choose clear
phrasing and limited ducking for this short theme. None of the sources gives
an objective quality guarantee.

## Implemented original arrangement

| Bars / time | Musical action |
| --- | --- |
| 1–4 / 0:00.0 | Warm chords and two-note fragments introduce the hook; sparse half-time kick enters |
| 5–8 / 0:06.4 | Full call/response, short bass answers, restrained backbeat |
| 9–12 / 0:12.8 | Syncopated kick/bass groove, offbeat hats and a phrase-ending fill |
| 13–16 / 0:19.2 | Stronger rhythm and a shifted hook accent; two-beat dropout before the break |
| 17–20 / 0:25.6 | No kick or bass; warm harmony and bell fragments expose the musical shape |
| 21–24 / 0:32.0 | Percussion and bass rebuild while the lead is withheld; noise ramp, then one beat of space |
| 25–28 / 0:38.4 | Four-on-the-floor payoff, complete hook and quiet interstitial bell answers |
| 29–32 / 0:44.8 | Subtract layers, descend into a short tonic tag, explicit note-offs and room for the loop |

The rewrite also corrects an instrument-design weakness: multiplying an already
wrapped fundamental phase by 0.5 does not produce a correctly running sub
oscillator. Fractional-frequency oscillator layers now maintain independent
phase in the score's instrument programs. This is a local code finding, not a
claim derived from forum consensus.

## Engine finding after the musical rewrite

Reading the sequencer, compiler input mapping, VM envelope/oscillator code and
synth voice allocator exposed a reproducible playback bug. Silent channels
allocated inactive slots and retained references to slots later used by other
channels. Trigger serials are per channel, but the synth compared them without
also tracking slot ownership. A silent or released channel could therefore
reset, retune or release a sounding voice. A 100 ms held-note probe ended with
only 16.7 ms of voice age; adding silent channels changed the actual PCM after
the first tick. This is a playback defect, not a compositional judgment.

The fix tracks the last handled note-on independently for every channel,
allocates only on a new trigger, and rejects stale references before modifying
a voice. The composition is unchanged for the before/after render.

The earlier gate-only regression and aggregate level checks did not cover
oscillator lifetime or audio continuity. Passing them did not establish that
the intended notes were actually being played correctly.

## Validation after the voice fix

- Five focused regression checks fail against the original synth and pass with
  the fix: identical PCM with silent neighbours, held normal notes, held NNA
  notes, NNA retrigger tails, and released-slot reuse.
- `make test`: all 22 demos pass, including deterministic audio, load/render,
  error and feature checks. The showcase's score-level gate regression also passes.
- The same score rendered by the fixed engine is 51.2 seconds, stereo, 44.1 kHz;
  PCM peak **-4.7 dBFS**. Chapter RMS (dBFS):
  **-24.0, -20.9, -19.3, -19.4, -35.4, -21.0, -18.8, -20.8**. These are headroom/dynamics measurements, not a quality score.
- MP4/GIF are regenerated with the fixed synth so channel meters reflect the
  actual corrected sound. The single `dm` CLI remains the entry point.
- The corrected render still needs the user's listening judgment for melody,
  timbre and taste. No subjective quality claim follows from the regression tests.
