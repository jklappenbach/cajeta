---
id: ifx-audio-playback
applies-to: [cajeta/ifx/AudioBackend, cajeta/ifx/AudioStream, cajeta/ifx/NullAudioBackend]
title: Audio playback — AudioBackend, AudioStream, NullAudioBackend
description: How to open an audio output stream and push interleaved PCM through cajeta.ifx's portable audio backend, plus the silent null floor.
---

# Audio playback

To play audio: get an `AudioBackend` (from the registry, not by `heap`), call
`openOutput(rate, channels)` to get an `AudioStream` handle, push interleaved
`float32[]` PCM with `submit(stream, frames)` as fast as you generate it, then
`close(stream)` when done. The same three calls are the whole output API across
every OS; the concrete device (WASAPI / CoreAudio / PipeWire / AAudio / harness /
null) is chosen by the backend registry. Audio is fully orthogonal to graphics —
nothing here touches windows or surfaces.

## Members and roles
- **`AudioBackend`** (interface, extends `Backend`) — the access point. Owns the
  three verbs that drive sound: `openOutput`, `submit`, `close`. You do **not**
  instantiate a real one; you receive it from `BackendRegistry.selectAudio()`.
- **`AudioStream`** (`final class`) — an **opaque value handle** to one open
  output. Carries an `int64 backendHandle` plus the negotiated `sampleRate`/
  `channels`, exposed via `streamSampleRate()` / `streamChannels()`. It is data,
  not a controller: it has **no `close()` / no `submit()` of its own** — you pass
  it back into the backend that made it.
- **`NullAudioBackend`** (`final class implements AudioBackend`) — the silent
  floor, auto-registered at `priority() -1000`, `name() "null"`. `openOutput`
  returns **`null`**, `submit`/`close` are no-ops. It lets audio code run with no
  device present.

## Collaboration / call sequence
`AudioBackend` is the factory, owner, and sink for its own `AudioStream`s:

1. `AudioBackend ab = BackendRegistry.instance().selectAudio();` — best viable
   backend, the null floor at worst. Audio has **no loud-error case**: selection
   never throws and never returns null (unlike `selectWindow`, which can throw
   `IfxException`). The backend is **borrowed** — owned by the registry, do not
   free it.
2. `AudioStream s = ab.openOutput(48000, 2);` — `s` is a plain value handle. On
   the **null floor this returns `null`**, so a portable app must null-check
   before submitting.
3. `ab.submit(s, frames)` repeatedly — `frames` is interleaved PCM
   (`[L0,R0,L1,R1,...]`), length a multiple of `channels`. The array is
   **borrowed**: `submit` reads it and does not take ownership (no `#`), so you
   may reuse/refill the same buffer for the next call.
4. `ab.close(s)` — release the device. Always close on the **same backend** that
   opened the stream; the `AudioStream` value itself drops with normal scope.

## Ownership / lifecycle (no `#` anywhere here)
None of these signatures transfer ownership. `frames` stays the caller's;
`AudioStream` is a copied-by-value handle the backend interprets. The lifecycle
lives on the **backend**, not the handle: there is no `AudioStream.close()`,
no drop-on-scope device teardown — forgetting `ab.close(s)` leaks the OS stream.

## When to use which
- Real-time output you push yourself → **`AudioBackend` + `AudioStream`** (this
  skill).
- Just need code to run headless / in tests with no device → the **null floor**
  is already auto-registered; `selectAudio()` falls to it silently.
- **Recording** the mix to a file is **not** this seam — that is `AudioSink`
  (`open`/`writeSamples`/`close`) with the `WavAudioSink` fallback. This API
  plays out; it does not capture or encode.
- Mic **capture** is gated by `Backend.requestPermission(Permission.Microphone)`
  (returns a `PermissionState`, never throws); the null floor returns
  `NotRequired`.

## Worked example
A fake/real output and the silent floor share one interface, so portable code
guards on the `null` stream:

```cajeta
import cajeta.ifx.AudioBackend;
import cajeta.ifx.AudioStream;
import cajeta.ifx.BackendRegistry;

public final class Tone {
    public static void play(float32[] frames) {
        AudioBackend ab = BackendRegistry.instance().selectAudio();  // borrowed; never null
        AudioStream  s  = ab.openOutput(48000, 2);                   // null on the silent floor
        if (s == null) { return; }                                   // headless: nothing to do
        ab.submit(s, frames);                                        // frames borrowed, reusable
        ab.close(s);                                                 // close on the SAME backend
    }
}
```

Constructing an `AudioStream` directly is only for backend authors/tests:
`heap AudioStream(0, 48000, 2)` then `s.streamSampleRate()` reports `48000`.

See the `cajeta.ifx` library/registry skills for backend selection,
`CAJETA_IFX_AUDIO` override, `probe()/priority()` and the `Feature`/`Permission`
contracts; see the `AudioSink`/`WavAudioSink` skill for the recording seam.
