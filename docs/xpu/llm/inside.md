# Inside the engine

This page describes how a token is produced. It covers the model's forward
path, the scheduler that assembles each step from the running requests, the
K/V cache and its store, the pipelined and batched decodes, and what lives on
the device. This is the engine around the kernels. The kernels themselves
are in [Language models on the device](../kernels-llm.md).

## 1. `CausalLM`: the forward path

The model is built from the configuration (layers, heads, the
mixture-of-experts fields) and bound to the checkpoint by tensor name, with no
rename table. It is tape-free and decode-first.

Decode runs one token through one sequence with zero heap allocations. The
activations are pooled, the K/V cache is written in place, and every buffer
is sized at load. This matters beyond tidiness. Every allocation in a
multithreaded Cajeta process takes the live-set lock, and a decode step that
allocates nothing never touches it.

Prefill runs a chunk of prompt tokens through one arena sized to the chunk,
carved into borrowed slices. Nothing survives the call, so the peak
activation memory is one layer's regardless of depth. A chunk is a batch of
rows, and the projections take the batched routes described in the kernel
pages, the tensor-core tiles and the widen GEMMs.

Both are the same step, `stepSeq(sequence, tokens, offset, count)`, and
decode is the one-token chunk. That is the identity the scheduler is built
on.

Each layer runs the attention projections (fused q|k|v where the load-time
A/B said it pays), rotary embedding and the K/V write fused with the norm
(`qkPrep`), the flash attention over the sequence's K/V (split across the
context and reduced, with the q8_K pack of the result fused into the
reduce), the output projection, and the feed-forward (gate and up fused with
the activation, then down). A mixture-of-experts layer runs the router, the
top-k, and the expert banks' indexed mat-vecs or GEMMs in place of the
feed-forward. The fused tail keeps the final norm, the output head and the
argmax on the chain's stream, so a greedy step reads back one integer.

A loaded model is device resident. The weights live on the device in their
packed form, with the K/V pool, the activation scratch and the tiles'
buffers. Under the `int8` or `auto` prefill-weight policy the widened int8
copies of the formats that pay for them are resident too. The
mixture-of-experts expert banks are resident up to the residency budget,
which `auto` freezes at the first bank's bind from the free device memory
less a reserve for the K/V pool and scratch. Banks past the budget stream
through a windowed path. Decode always reads the packed weights.

## 2. The scheduler: continuous batching

`Scheduler` assembles each iteration from the requests it holds. The policies:

- **Admission.** A request is admitted only when the free K/V blocks cover
  its whole uncached suffix plus one block per unwindowed layer of decode
  headroom. Admitting work that would immediately preempt would only churn.
  Completions free blocks, and freed blocks admit waiting requests in the
  same step.
- **Chunks.** A prefilling request advances by a chunk of at most `chunk`
  tokens, a decoding request by one. The chunk policy learns a size (32 to
  512) when nothing else is decoding and takes the largest that fits the
  pause budget (`chunkBudgetMs`, 150 by default) when something is, so a
  long prompt never stalls the sequences already generating.
- **Preemption.** The victim is the youngest running request. Older work is
  never sacrificed for newer, which bounds every request's wait.
- **Resume.** Preempted requests re-admit first-in first-out by preemption
  time, ahead of new admissions, so no request starves. Without a store,
  resume recomputes the prefix through the same chunked path. With the block
  store it adopts the stored blocks.

A step runs in this order. It admits, then batches the prompt chunks of
greedy requests across requests (one prefill over all their rows). If
exactly one greedy request is decoding, it runs it through the pipeline
(§4), and otherwise it batches the decoding rows (§5). Then it records any
launch failures and emits the `sched-phase` record (admission, K/V reserve,
model and total nanoseconds). The command line's `run` is one caller of this
loop with one request. cabra's host multiplexes every connection's
conversation onto it.

## 3. The K/V cache and the block store

The host-side cache is paged in fixed blocks of `blockTokens` tokens, shared
by reference count with copy-on-write, so sequences that fork from a common
prefix share its blocks. Windowed layers use rings. When the pool is full,
`append` refuses and the scheduler decides who yields.

On the device, `KvPool` holds one K and one V buffer per layer, slot `s` at
element `s × plane`, so a batched attention kernel handed the whole buffer
reaches every sequence's cache in one launch. Slots are reserved at
admission, and the buffers grow to the slots a batch needs, with each
staying under 2 GiB. Growth is counted and reported, because on WSL2 a
2 GiB mapping costs about 30 ms to create and as much to destroy. Pool
growth is therefore moved off the clock.

The block store (`kvStore`) is a file-backed prefix cache. Blocks evicted
from the pool are written to it, keyed by a chained hash of the prefix. Each
extent carries a length and an XXH3 checksum that is verified on every read,
so a torn write is a miss and is never served. Two B+ trees index the store,
one from primary key to extent and one from recency stamp to key for
coldest-first eviction. The store serves any later request sharing the
prefix and survives a process restart. The operating system's page cache is
its RAM tier.

## 4. The pipelined decode

A single greedy request decodes one step ahead. Step N+1 is queued before
step N's argmax is read, the embedding of the next token is taken on the
device from the previous step's result, and two events mark the boundary.
The CPU never waits for the device between tokens, and the launch sequence
of a step, replayed through the deferred stream as a captured graph, is
issued in about a millisecond. This is how single-sequence decode reaches
llama.cpp's rate. The kernels were already at the bus, and the gaps between
them were the host.

Pipelining needs the fused tail, a device argmax, a device-routed output
head and a device embedding decoder for the embedding's format. Where any is
missing the step runs plainly. The scheduler drops back to plain steps the
moment a second request is admitted, and the batched decode takes over.

## 5. The batched decode

Several decoding sequences share one pass over the weights. The batched step
embeds every sequence's token on the device, runs each projection as one Cols
launch over the batch's activations, attends every sequence with one launch
through the K/V pool, and for mixture-of-experts layers runs one indexed
mat-vec launch per bank over every row's selected experts. Admission is
latched once per model. The conditions are a device-resident model, the
fused tail, device attention, a head dimension at most 128, widths that are
multiples of 256, a Cols kernel for every projection's format, and the
zero-sync path for expert layers. A refusal is recorded with its reason,
after which sequences decode singly. The engine's `decode-batch-phase`
record breaks a batched step into host embedding, upload, issue and device
tail.

Since 2026-10-24 the batched greedy decode also runs one step ahead on HIP.
Rows are embedded on the device from the previous step's argmax with an
event per step, which is the single-sequence pipeline's design applied to
the batch.

## 6. At load

`LlmEngine.load` runs these steps in order. It forces the backend if asked,
resolves the tune directory and hands it to every kernel store, validates
the options, and opens the checkpoint. It reads the configuration (from the
GGUF metadata, or `config.json`), the tokenizer and the end-of-generation
set, then builds the model and binds it. It reports the K/V ceiling at
`maxSeqs`, prewarms the tile families at the shapes the engine will meet,
and sets the residency budget. It runs one throwaway chunk-sized prefill and
one decode step so the probes have run and the first request pays for none
of them. Finally it builds the scheduler with the chunk budget and attaches
the tokenizer and, if configured, the block store.
