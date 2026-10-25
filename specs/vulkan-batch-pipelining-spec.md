# Vulkan batch pipelining — spec

Status: draft, 2026-10-24. Plan: `agents/vulkan-batch-pipelining-plan.md`.
Resolves the `vulkan-dispatch-serialization` filing (INDEX, 2026-08-24) as
far as the batched path is concerned.

## 1. Definition

### 1.1 Purpose

The Vulkan backend records launches into a batch and submits them in chunks
(`cajeta_xpu_vulkan.c`, `caj_vk_launch_batched`). This spec makes two
changes to that path. The batch hands its first chunk to the device early,
so the device runs while the host is still recording the rest of a step.
And the profiler brackets dispatches inside the batch's own command buffer,
so a profiled step is the step that runs unprofiled, and its per-kernel
spans carry no per-launch submit cost.

### 1.2 The problem it solves

Measured 2026-10-24 on proton (gfx1151, RADV, the 8B Q4_K_M, four
sequences decoding, `cajeta-llm` bench):

- A decode step issues 429 dispatches and 332 barriers. Recording them
  costs the host 2.1 ms a step, about 5 µs a dispatch (descriptor set,
  writes, elision bookkeeping, three `vkCmd*` calls).
- The chunk is 100 dispatches, so the device idles while the host records
  the first 100: about 0.5 ms at every step start. One submit a step
  (`CAJETA_XPU_VK_NOCHUNK`) costs +2.15 ms a step; a chunk of 16 gains
  +2.4% at four sequences (151.6 to 155.2 tok/s) and +2.8% at one
  (42.3 to 43.45).
- llama.cpp's Vulkan backend issues 549 ops a step on the same driver and
  its trivial ops (ADD, SET_ROWS, CPY) take 2.3 to 2.5 µs, so the driver's
  per-dispatch floor is about 2.5 µs. The "10 µs launch floor" the llm
  plan quoted came from the profiler's eager bracket.
- Under the profiler every Vulkan launch takes the eager path: its own
  command buffer, a barrier, a bottom-of-pipe timestamp, a submit and
  `vkQueueWaitIdle`. Every per-kernel number in the llm attribution
  (cajeta-llm 15.2.12.3) was measured that way, so each carries several µs
  that the unprofiled batched step does not pay, and the profiled step's
  overlap is not the real step's. llama.cpp's per-op logger stamps inside
  its single command buffer.

### 1.3 Scope

- The batch's submit cadence: when the first chunk goes to the queue, and
  the chunk size after it. A knob for both, so a measurement can sweep them.
- The profiler's Vulkan seam: timestamp brackets recorded into the batch,
  resolved after the batch's fence, at EVENT tier.
- Tests in `test/xpu` for both, and the measurement that picks the default.
- The llm re-attribution with in-stream numbers is cajeta-llm plan work
  (15.2.12.3.9) and only named here.

### 1.4 Non-goals

- Descriptor set or command buffer reuse across steps, capture and replay:
  `xpu-tile-scheduling` Unit 15.
- Multiple queues or timeline semaphores; barrier semantics or elision rules.
- The hip, CUDA and cpu backends.
- Making the host's recording cheaper than 5 µs a dispatch. It matters only
  before the first submit once the first chunk is small; a follow-up if a
  small model shows it.

### 1.5 Constraints

1. Results are bit-identical to today's: the cadence changes when work
   reaches the queue, never its order or its barriers.
2. With the profiler off nothing changes but the cadence.
3. `CAJETA_XPU_VK_SUBMIT=eager` keeps the per-launch submit-and-wait path,
   and the eager bracket with it, as the control.
4. A bracket never changes the step it measures beyond its two timestamp
   writes: no added barrier, no added submit, no added wait.
5. Timestamp results are read by availability, never with a wait
   (profiler spec 5.5.4); an unavailable bracket publishes at host tier.

## 2. The first chunk is submitted early

- **2.1** When a batch opens, its first chunk is submitted after `first`
  dispatches (default 8), and every chunk after it after `chunk`
  dispatches (default 16; from Unit 5, the chunks ramp from 8 by 2 to a
  cap of 128, see 2.6 and 2.7); measured on the 8B (the tables in the
  plan, 1.3 and 5.3.1).
- **2.2** When `CAJETA_XPU_VK_CHUNK=N` is set, both sizes are N;
  `CAJETA_XPU_VK_FIRST_CHUNK=N` sets the first alone;
  `CAJETA_XPU_VK_NOCHUNK` keeps meaning one submit at flush.
- **2.3** When a flush arrives before any chunk was submitted, the batch
  submits what it holds with the fence, as today.
- **2.4** When `CAJETA_XPU_VK_SYNC_LOG` is set, the flush line also reports
  the chunks flown and the dispatches in the first one.
- **2.5** When the command-buffer ring wraps (32 chunks), the batch waits on
  its fence and resets the descriptor pools, as today; a smaller chunk
  reaches the wrap sooner, and the wait is accounted in 4.2's measurement.
- **2.6** When `CAJETA_XPU_VK_CHUNK_RAMP=G` is set (G > 1), each chunk after
  the first is G times the one before it, up to `chunk` as the cap; the
  flush line reports the factor. A submit costs the device a
  command-processor drain and re-prime (~17 us on gfx1151), so once the
  device has work queued the chunks should be large; the first chunk stays
  small so the device starts early.
- **2.7** When no knob is set, the cadence is first 8, then ramp 2 to a cap
  of 128 (plan 5.3.1: the ramp matched a fixed 100 pipelined, held the
  unpipelined step where fixed 100 lost 1.3%, and took the 1-seq gain).
  `CAJETA_XPU_VK_CHUNK=N` still means both sizes N, so the ramp has nothing
  to grow; `CAJETA_XPU_VK_CHUNK_RAMP=1` turns the ramp off.

## 3. In-stream device timing under the profiler

- **3.1** When the profiler is on and a launch is recorded into the batch,
  two timestamp queries bracket its `vkCmdDispatch` in the batch's command
  buffer: top-of-pipe before, bottom-of-pipe after. The launch id parks as
  today.
- **3.2** When a chunk's or the flush's fence signals, the batch reads that
  chunk's query results by availability and resolves each parked launch at
  EVENT tier with device start and end converted by the profiler's Vulkan
  clock correlation; an unavailable pair resolves at host tier.
- **3.3** When a bracket's end is a bottom-of-pipe stamp in a stream, a
  later dispatch may already be running; the span is the dispatch's own
  drain, and the trace names the bracket kind (`in-stream` or `eager`) so a
  reader knows which was taken.
- **3.4** When the query pool for a chunk is exhausted, the batch submits
  the chunk and continues in the next pool slot; brackets are never dropped
  for pool capacity.
- **3.5** When the profiler is on, the submit cadence is 2.1's, so the
  profiled step overlaps host and device as the unprofiled one does; the
  host-blocked span (profiler spec 5.5.8) is emitted at the fence waits.
- **3.6** When a step is profiled in-stream, the sum of its dispatch spans
  and the gaps between them equals the wall from the first submit to the
  fence within the calibration tolerance (profiler spec 11); a run that
  fails this demotes the in-stream bracket to host tier for the run.
- **3.7** When `CAJETA_XPU_VK_SUBMIT=eager` is set under the profiler, the
  eager bracket is taken, as today.

## 4. Acceptance

- **4.1** Tests in `test/xpu`: the first chunk is submitted after `first`
  dispatches and later chunks after `chunk` (counted through the sync-log
  or a test hook); the knobs are honoured; results are bit-identical under
  every cadence; in-stream brackets resolve every launch of a batch at
  EVENT tier with monotonic, non-overlapping-start spans; the eager control
  still brackets.
- **4.2** The default cadence is the one measured best on the 8B at one and
  four sequences on proton (quiet box), from a sweep of `first` in
  {4, 8, 16, 32} and `chunk` in {16, 32, 100}; the table lives in the plan.
- **4.3** The llm 4-seq step profiled in-stream yields per-kernel spans
  comparable to llama.cpp's per-op logger; the re-attribution is
  cajeta-llm 15.2.12.3.9.

## 5. Events inside the batch (added 2026-10-24, for cajeta-llm 15.2.12.3.9.5)

The Vulkan `KernelEvent` is a sentinel today: recording it lands the whole
batch and waits, so nothing recorded after it can overlap the wait. A
pipelined decode (step N+1 recorded before step N's tokens are read) needs
an event the host can wait on while the device runs what follows it.

- **5.1** When an event is recorded on the batched path, a `VkEvent` is set
  in the batch's command buffer at that point (all prior commands complete),
  and the event remembers the batch's dispatch count at the record.
- **5.2** When the host waits on such an event, the chunk holding it is
  submitted if it is still open, and the host polls the event's status
  without flushing the batch; dispatches recorded after it keep running.
- **5.3** When the host reads a buffer after waiting on an event, the read
  flushes the batch only if a dispatch recorded after that event writes the
  buffer; a buffer whose last write precedes the awaited event is read
  directly (the batch tracks each bound buffer's last write index).
- **5.4** When an event is re-recorded, it is reset in-stream before the set,
  so a stale signal is never read.
- **5.5** When the device cannot provide events (an ICD without them), the
  sentinel behaviour remains: record lands the batch, wait returns.
- **5.6** Tests: an event recorded between two dispatch groups lets the host
  read the first group's output while the second runs (the second group's
  completion is observed only at the flush); results are bit-identical; the
  sentinel path still works under a fault switch.

