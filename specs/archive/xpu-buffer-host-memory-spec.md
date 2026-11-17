# xpu-buffer-host-memory: whole-buffer transfers check the host array; cpu buffers give their pages back

> Status: **active**, approved by Julian 2026-11-16 ("Close 8.1.4, fix in cajeta"). Plan:
> [xpu-buffer-host-memory](../../agents/archive/xpu-buffer-host-memory-plan.md). Found in cajeta-llm 15.2.21.8.1.4.

## 1. Definition

Two gaps in `cajeta.xpu.KernelBuffer` and the cpu backend's buffer memory, both found while sizing
cajeta-llm's KV cache on demand (2026-11-16).

- **1.1** `upload(T[])`, `download(T[])`, `uploadAsync` and `downloadAsync` move `length()` elements
  whatever the host array holds. A host array shorter than the buffer is read or written past its
  end. On cajeta-llm a test that downloaded a 4-slot table into a 1-slot array corrupted the heap,
  and the crash surfaced two tests later as `free(): invalid size` on cpu and a hang on amdgpu. The
  counted forms (`upload(T[], n)`, `download(T[], n)`) already clamp.
- **1.2** The cpu backend allocates buffers with `posix_memalign` and frees them with `free`. glibc's
  dynamic mmap threshold serves later blocks of up to 32 MB from the brk heap, and freed heap chunks
  stay resident. cajeta-llm's KV pool on the cpu backend grows by reallocating per-layer buffers of a
  few MB. At Gemma 3 4B, 4 x 32k, peak RSS read 3.87-3.90 GB with the default allocator and 3.64 GB
  with `MALLOC_MMAP_THRESHOLD_=131072`, against 3.68 GB for the build before paging (llm dbfef42).
  The `[heap]` mapping held 935 MB against 54 MB.
- **1.3** Non-goals: the hip and cuda device block cache (28.2.2), whose retention is deliberate and
  capped; Windows' `_aligned_malloc`.

## 2. Whole-buffer transfers

- **2.1** When a whole-buffer transfer is given a host array shorter than `length()`, it throws a
  `RecoverableException` naming both counts and moves nothing.
- **2.2** When the host array is at least `length()` long, the transfer is unchanged (callers pass
  larger scratch arrays today).
- **2.3** The counted and window forms keep their clamping.

## 3. cpu buffer pages

- **3.1** When a cpu buffer at or above a size threshold is freed, its pages go back to the OS, so
  peak RSS follows the buffers alive rather than the allocator's history.
- **3.2** When a buffer below the threshold is freed, it goes back to malloc as today.
- **3.3** When an op allocates and frees large temporaries in a loop, its time stays within 1% of
  today's; the threshold is chosen by that measurement.
