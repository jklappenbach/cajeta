---
id: ifx-recording-sinks
applies-to: [cajeta/ifx/AudioSink, cajeta/ifx/WavAudioSink, cajeta/ifx/VideoSink, cajeta/ifx/PngSequenceVideoSink]
title: ifx recording sinks (Audio/Video seams + royalty-free fallbacks)
description: How to record presented frames and mixed PCM through ifx's VideoSink/AudioSink seams and their PngSequence/Wav stdlib fallbacks, and how sinks are selected.
---

# ifx recording sinks

To record what the app **presents** (frames) or **mixes** (PCM), write through the two
seam interfaces: `VideoSink` (frames) and `AudioSink` (audio). Both follow the same
`open → write(borrowed) → close → name` lifecycle. Stdlib ships only **royalty-free
reference fallbacks** — `PngSequenceVideoSink` (`name()=="png-sequence"`) and
`WavAudioSink` (`name()=="wav"`) — that prove the contract by counting what passes the
seam. The real byte-encoding (actual PNG/WAV files, licensed codecs, capture/replay)
lives in the **external `cajeta-ifx-harness`**, not here: ifx is pure-Cajeta and stdlib
carries no encoder.

## Members and roles

- **`VideoSink`** (interface) — frame recorder seam. `boolean open(uint32 width, uint32 height, uint32 fps)`, `void writeFrame(int8[] rgba)`, `void close()`, `String name()`.
- **`AudioSink`** (interface) — audio recorder seam, the audio sibling of `VideoSink`. `boolean open(uint32 sampleRate, uint32 channels)`, `void writeSamples(float32[] frames)`, `void close()`, `String name()`.
- **`PngSequenceVideoSink`** (final class) — the stdlib `VideoSink` fallback: window frames → a PNG image sequence. Records params + counts frames; **no actual file output**.
- **`WavAudioSink`** (final class) — the stdlib `AudioSink` fallback: the mix → a WAV/PCM file. Records format + counts sample-buffers; **no actual file output**.

## Lifecycle and the call sequence (identical for both seams)

1. `heap PngSequenceVideoSink()` / `heap WavAudioSink()` — no-arg constructors; starts **closed**.
2. `open(...)` — begin recording; returns `boolean` (the fallbacks always return `true`). Calling `open()` again **resets** the frame/buffer counter and the stored params.
3. `writeFrame(rgba)` / `writeSamples(frames)` — one buffer per call, **only while open**.
4. `close()` — finish/flush; flips back to not-recording.
5. `name()` — stable format tag for selection/logging.

**Open-gating (the key gotcha):** writes before `open()` or after `close()` are **silently dropped** — no exception, no error. Only writes between `open()` and `close()` count. The fallbacks never throw; `IfxException` is for backend *selection*, not sinks.

## Ownership across the seam boundary

- **Frame / sample buffers are BORROWED.** `writeFrame(int8[] rgba)` and `writeSamples(float32[] frames)` do **not** take ownership and do **not** retain the array — the caller keeps it and may reuse or free it immediately after the call returns. (The fallbacks don't even read it; the harness copies what it needs synchronously.)
- **`name()` returns a stable tag** (`"png-sequence"` / `"wav"`) — treat as a borrowed literal; compare with `.equals(...)`, don't free.
- Pixel layout: `rgba` is tightly-packed RGBA8, exactly `width*height*4` bytes. PCM: `frames` is interleaved `float32` across `channels`.

## Selection (registry + probe + priority + CAJETA_IFX_VIDEO)

The intended model mirrors the window/input/audio domains in `cajeta/ifx/BackendRegistry`
(highest-`priority()` viable `probe()`, with a `CAJETA_IFX_VIDEO` env override). **It is
not wired yet:** `BackendRegistry` has `selectWindow/selectInput/selectAudio` but **no
`selectVideo`/`registerVideo` and no sink registry**, and `CAJETA_IFX_VIDEO` is not read
anywhere. So today you **construct a sink directly** (`heap PngSequenceVideoSink()`), or
the harness hands one to you. Don't hunt `BackendRegistry` for a sink selector — there
isn't one. See `BackendRegistry` (cajeta/ifx/BackendRegistry) for the pattern the wiring
will follow.

## Worked example

```cajeta
import cajeta.ifx.VideoSink;
import cajeta.ifx.PngSequenceVideoSink;

// Record three frames; the seam interface is the shape the external harness implements.
VideoSink sink = heap PngSequenceVideoSink();   // starts closed
sink.writeFrame(null);                           // dropped: not open
sink.open(1280, 720, 30);                         // true; counter reset
sink.writeFrame(rgbaA);                            // counted (rgbaA stays caller-owned)
sink.writeFrame(rgbaB);                            // counted
sink.close();                                       // flush
sink.writeFrame(null);                               // dropped: closed
// name() == "png-sequence"; through PngSequenceVideoSink: frameCount() == 2.
```

Audio is the mirror image: `import cajeta.ifx.WavAudioSink;`, `open(48000, 2)`,
`writeSamples(pcm)`, `close()`, `name() == "wav"`, `bufferCount()` counts buffers.

## Fallback-only observers (reference/tests)

When you hold the concrete fallback type (not the seam interface) you can inspect state:
`PngSequenceVideoSink.frameCount()/recordedWidth()/recordedHeight()/isRecording()` and
`WavAudioSink.bufferCount()/sampleRate()/channels()/isRecording()`. These are **not** on
the `VideoSink`/`AudioSink` interfaces — through the seam you only get
`open/write*/close/name`.
