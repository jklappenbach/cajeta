# kv-paging: tiered KV pages with the sequence as the unit of residency

> Status: **draft**, opened 2026-11-16 at Julian's request ("a paging system that supports a variety of
> different strategies"; the refinement to units agreed the same evening). Extends cajeta-llm spec §6
> (cache, scheduler, store), which was written before KV lived on the device. Absorbs cajeta-llm plan
> 15.2.21.8.1.5.

## 1. Definition

- **1.1** cajeta-llm's device KV is paged (plan 15.2.21.8.1): fixed blocks of 64 positions in one pool per
  window kind, drawn as sequences grow, read through block tables. Paging so far buys allocation
  granularity (memory follows tokens in use, not each sequence's maximum context), rings as recycled
  blocks, and the indirection that lets a block move. It does not yet let the KV of the admitted
  sequences exceed device memory: a pool that cannot grow refuses by name and the request fails.
- **1.2** This spec makes device memory one tier of several. A sequence's pages leave the device when
  something else needs the room, and come back before the sequence next runs, or they are dropped and
  recomputed, whichever costs less on the machine at hand.
- **1.3** The unit of residency is a sequence's page set, not a page. Every decode step of a sequence reads
  all of its pages (a windowed layer, its ring), so a live sequence has no cold pages; evicting one page
  of it faults the page back on the next step. Pages that no live sequence holds (cached prefixes of
  finished requests) are evicted individually, coldest first.
- **1.4** A GPU kernel cannot take a page fault. Residency is settled by the scheduler before a step
  launches: it knows which sequences the step runs.
- **1.5** Constraints: a quantity a choice depends on (transfer bandwidth per tier, prefill rate) is measured
  per machine, never tabled per device (xpu-kernel-adaptor §1.4). A restored sequence's output is the
  output it would have produced unevicted, to logits KL, not only greedy tokens.
- **1.6** Non-goals: KV across processes or machines; quantized KV (q8_0 KV is a separate open item);
  multi-GPU placement; speculative prefetch past the next step.

## 2. Units

- **2.1** When a sequence is admitted, its unit is the set of blocks its tables reference, one table per
  window kind; a windowed kind contributes only its ring's blocks.
- **2.2** When a step is assembled, every sequence in it is wholly resident before launch; a partly
  resident unit never runs.
- **2.3** When a unit is evicted, all of its blocks leave together, and a block another resident unit
  references (a shared prefix, by refcount, §6.14) stays.
- **2.4** When a unit is restored, its blocks land in any free blocks of their kinds and only its table
  entries change (block ids are not stable across eviction, §6.17).
- **2.5** When a sequence completes, its full prefix blocks may stay in device memory as cached pages
  outside any unit (§6); they are evicted one at a time, coldest first, from the tail of a prefix chain,
  since a block is useless without its predecessors (§6.26).

## 3. Tiers

- **3.1** When a unit leaves the device, it goes to the cheapest tier that can take it and from which a
  restore costs less than recomputing it (§4): pinned host memory, then the block store on disk
  (§6.26-6.31), else it is dropped and recomputed on resume (§6.32).
- **3.2** When device memory is system memory (Strix Halo's GTT or carve-out, the cpu backend), there is no
  host-memory tier: moving a unit there frees nothing. Its tiers are the store and recompute.
- **3.3** When a tier is full, its own coldest units are demoted to the next tier or dropped, before a new
  unit is refused.
- **3.4** When a unit is written to a tier, it is written in the device's format (f16 today), so a
  restore is a copy, not a conversion. The store's key carries the format (§6.27).
- **3.5** When a transfer can overlap other work, it runs on a copy stream while other sequences decode.

## 4. Restore or recompute

- **4.1** When a unit is evicted, the choice between keeping it in a tier and dropping it compares the
  measured restore cost of its bytes from that tier against the measured cost of prefilling its tokens.
- **4.2** When a machine has not measured a rate yet, it measures it once (bandwidth per tier, prefill rate
  by length) and keeps it with the calibration set; until then the choice is the conservative one
  (keep, if a tier has room).
- **4.3** When a sequence is short, recompute wins and the unit is dropped (on proton a 4k-token Gemma 3
  4B unit is about 200 MB; at 25 GB/s PCIe that is about 8 ms each way against about 1.4 s to
  re-prefill, so the crossing is far below typical lengths; the measurement decides).

## 5. Scheduling

- **5.1** When a step is formed, the scheduler restores every unit the step needs before launch; when device
  memory is short, it first evicts a victim unit by the stated policy (§6.22: youngest running).
- **5.2** When a sequence waits for admission, admission counts the device pool's free blocks (and what
  eviction can free), not the host cache's.
- **5.3** When device memory cannot hold even the step's own units, the request is refused by name, as today.
- **5.4** When eviction and restore run, their counts, bytes, tiers and choices are observable (§6.25).

## 6. The prefix cache in device memory

- **6.1** When a request completes, its full prefix blocks stay in device memory as cached pages keyed by
  the chained prefix hash (llm §6.26-6.27), outside any unit.
- **6.2** When a request arrives, the longest cached prefix in device memory is adopted before the host tier
  and the store are asked (llm §6.18); only the uncached suffix is prefilled.
- **6.3** When device memory is needed, cached pages are evicted before any live unit, one at a time,
  coldest first, from the tail of a chain; an evicted cached page may go down a tier like a unit's blocks.
- **6.4** When an adopted cached block would be written, it is copied first (llm §6.15).

## 7. Decisions (Julian, 2026-11-16)

- **7.1** Host tier budget: the smaller of half of `MemAvailable` at load and an optional fixed size. When
  it is full, 3.3 applies.
- **7.2** Victim: the youngest running sequence (llm §6.22). Cost decides where an evicted unit goes, not
  which unit is evicted.
- **7.3** Rings travel with their unit to every tier, so a windowed model's sequence can be swapped rather
  than recomputed; the store's no-rings restriction (llm §6, v1) is lifted.
- **7.4** Cached prefix pages in device memory are in this spec (§6).
- **7.5** The host PagedKvCache (f32) stays the cpu-attention path only; it is not a tier.

## 8. Open questions

None at this draft. The plan will order the work: the device-route admission and preemption fix (llm
15.2.21.8.1.5) first, then units and recompute, then the host tier, the store tier and the prefix cache.
