# distributed-training

Research corpus for **training a model that does not fit on one device** —
collectives, sharded parameter/optimizer state, pipeline and tensor parallelism,
activation-memory tradeoffs, and automatic sharding.

Consumers, all in `cajeta-six/specs/`:

| Spec | What this corpus supports |
|---|---|
| `research-platform-roadmap` §6.1 → `distributed-training-spec` | collectives, DDP, ZeRO/FSDP, pipeline + tensor parallelism |
| `research-platform-roadmap` §6.2 → `activation-memory-spec` | rematerialization, offload, gradient accumulation |
| `research-platform-roadmap` §6.5 → `auto-sharding-spec` | sharding annotation, propagation, and plan search |

## Scope boundary

This is **training-side** distribution. Two neighbours, deliberately separate:

- **Inference and serving** — KV-cache residency, continuous batching,
  prefill/decode disaggregation, speculative decoding — is `research/llm-serving/`,
  consumed by `llm-kernel-scheduling-spec`. Megatron and Switch/MoE live there
  because they were read for their kernel and parallelism structure; the
  training-systems reading of Megatron is `narayanan-2021-megatron-3d-parallelism`
  here.
- **Distributed classical ML** — sharding *data* across nodes and merging
  sufficient statistics — is `cajeta-ml-dist-spec`. Different problem: it can and
  does promise a model bit-identical to the single-node fit. Nothing here can
  make that promise across topologies (see `narayanan-2018-pipedream`).

## Papers

PDFs are gitignored; each has a `<name>.pdf.txt` sidecar carrying title,
description, and source URL. Refetch from the URL.

### Collectives and data parallelism
- `patarasuk-2009-bandwidth-optimal-allreduce` — the ring all-reduce, proved optimal
- `sergeev-2018-horovod` — ring all-reduce applied to DL training
- `li-2014-parameter-server` — the async parameter-server alternative
- `li-2020-pytorch-ddp` — gradient bucketing and comm/compute overlap
- `goyal-2017-large-minibatch-sgd` — LR scaling and the accounting that keeps N-way data-parallel correct

### Sharded state
- `rajbhandari-2019-zero` — ZeRO stages 1–3
- `zhao-2023-pytorch-fsdp` — ZeRO-3 as a production API
- `ren-2021-zero-offload` — optimizer state and step to host
- `rajbhandari-2021-zero-infinity` — the device/host/NVMe tier ladder

### Pipeline and tensor parallelism
- `huang-2018-gpipe` — synchronous micro-batch pipelining, exact gradients
- `narayanan-2018-pipedream` — async 1F1B with weight stashing, stale weights
- `narayanan-2021-megatron-3d-parallelism` — composing tensor × pipeline × data

### Activation memory
- `chen-2016-sublinear-memory` — gradient checkpointing

### Automatic sharding
- `xu-2021-gspmd` — annotation + propagation + collective insertion
- `zheng-2022-alpa` — searching the parallelization plan

## Known gaps

Not yet collected, and worth adding if the corresponding spec is written:

- **Elasticity and fault tolerance** at training scale (torchelastic, Bamboo,
  Oobleck). `research-platform-roadmap` §6.1 names failure/elasticity as
  research; nothing here covers it.
- **Deterministic/reproducible collectives.** §6.4's reproducible-reduction
  requirement has no supporting paper here.
- **Communication compression** (gradient quantization, PowerSGD, 1-bit Adam).
- **Federated learning** (FedAvg and successors) — a different problem
  (privacy, non-IID edge clients), not currently slated by any spec.
