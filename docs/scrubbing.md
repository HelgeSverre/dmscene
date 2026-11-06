# Terminal scrubbing

Touchpad scrolling uses the count and arrival time of terminal wheel reports.
The timeline follows a continuous target with a 25 ms smoothing time constant.
Slow gestures make fine adjustments; dense reports increase sensitivity. All
reported movement contributes to the target, including large bursts. Reversing
direction cancels the remaining interpolation in the old direction. Ghostty
already supplies momentum through further reports, so the player adds no extra
inertial scrolling.

Ghostty receives precise touchpad deltas internally, then emits repeated wheel
button reports to terminal applications. SGR's coordinates identify the pointer
location, not the amount scrolled. There is no original pixel delta in those
reports for `dm` to recover. See Ghostty's `scrollCallback` in
[Surface.zig](https://github.com/ghostty-org/ghostty/blob/main/src/Surface.zig).

## Removing the stalls

The previous seek path replayed all audio from sample zero twice and reopened
the output device for each update. On an M2 Max, one 30-second Showcase replay
took 0.90 seconds, while seeking its tracker clock took 0.054 ms. Capping scroll
events hid motion without removing this work.

The player now previews the tracker/scene immediately. A cancellable worker
prepares the audio state, using deep copies of synth state at roughly one-second
intervals. Checkpoints include delay/reverb buffers and use a 32 MiB cache budget
with at most 64 entries. New requests replace stale work. The audio device stays
open throughout scrubbing; its callback emits silence until the gesture settles
and the requested state is ready. A cold seek can delay sound, but it cannot
block input or rendering. Older checkpoints may be evicted during long playback.

While scrubbing, drawing targets at least 60 fps (subject to shader cost and
terminal throughput). Feedback history restarts once at the start of a gesture,
then follows the preview. Historical video frames are not reconstructed.

## Checks

`make test` checks checkpoint PCM against an independent full replay, cancellation,
mute changes, velocity response, burst retention and direction changes. Live
tests drive the real `play` command with both muted playback and a clocked test
audio device; they check rendering gaps, backwards time/beat changes, pause,
resume, and that scrolling never reopens the audio device.

In a 30-second Showcase test at 80x40 and 60 fps, a 1.5-second scrolling gesture
plus 0.6 seconds settling produced one frame before the change and 112 afterward.
The new 95th-percentile frame gap was 21.3 ms, with a 26.3 ms maximum. These are
synthetic terminal-input measurements, not a measurement of physical touchpad
or display latency.
