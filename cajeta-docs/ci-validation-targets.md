# Cajeta XPU — CI / Validation Targets

The hardware-target matrix for validating the XPU backends, and how the
dev/CI loop splits **GPU-free codegen** from **on-device validation**.
Companion to [`CajetaXPU.md`](CajetaXPU.md) (the spec) and
[`CajetaXPU-Variance.md`](CajetaXPU-Variance.md) (the cross-backend
discipline). Implementation status lives in
[`../cajeta-xpu.md`](../cajeta-xpu.md).

> **Pricing snapshot: May 2026.** Cloud GPU rates and availability move
> weekly; the dollar figures below are a point-in-time reference, not a
> contract. Re-check before relying on them. Architecture strings (`sm_*`,
> `gfx*`) are stable, but **confirm the exact target on the instance**
> (`nvidia-smi --query-gpu=compute_cap` / `rocminfo | grep gfx`) rather
> than trusting this table blind — the build is the evaluation.

---

## 1. The two-tier split — rent only for the launch step

The XPU pipeline cleaves cleanly into a part that needs no accelerator and
a part that does. This is the whole reason hardware access is cheap.

| Tier | What | Needs a GPU? | Where it runs |
|------|------|--------------|---------------|
| **Tier 0 — codegen** | AST → device LLVM IR → NVPTX/AMDGPU → `ptxas`/`llvm-mc` assembly → PTX/ISA text checks | **No** | any CPU box / ordinary CI; mirrors the existing `XpuNvptxEmitTests` / `XpuNvptxSharedEmitTests` (GPU-free) |
| **Tier 1 — on-device** | load the cubin/hsaco, launch, verify numeric results on the real arch | **Yes** | the target silicon (owned or burst-rented) |

**Implication:** ~95% of backend work is Tier 0 and runs in normal CI with
no GPU. A GPU is required only for Tier 1, which is a minutes-long device
suite per artifact — exactly the shape that suits **per-second / spot**
rental. Burst up, run the suite, tear down, pay cents.

---

## 2. Target architecture matrix

| Arch | Vendor | Example part | Coverage | How |
|------|--------|--------------|----------|-----|
| `sm_89` (Ada) | NVIDIA | RTX 4090 | **local** | owned (primary dev box) |
| `sm_120` (Blackwell) | NVIDIA | RTX 5090 | **burst-rent** | latest consumer; not owned |
| `gfx1151` (RDNA 3.5) | AMD | Strix Halo APU | **local** | owned (primary AMD box) |
| `gfx1201` (RDNA 4) | AMD | RX 9070 XT / Radeon AI PRO R9700 | **burst-rent** | latest consumer; not owned |
| `gfx1100` (RDNA 3, discrete) | AMD | RX 7900 XTX | **burst-rent (optional)** | discrete RDNA 3 reference vs. the APU |

Owned hardware covers one arch per vendor for free, continuous Tier-1
validation. The two latest-consumer arches (`sm_120`, `gfx1201`) are the
gap the cloud fills. Datacenter arches (`sm_100`/`sm_103` Blackwell,
`gfx942` CDNA3) are out of scope for the consumer-first phase but slot into
the same burst-rent model when needed.

---

## 3. Cloud access — how and how much

### NVIDIA Blackwell — RTX 5090 (`sm_120`)

Widely available (11+ providers); several bill sub-hourly, which is what
makes Tier-1 bursts cost pennies.

| Provider | $/hr | Notes |
|----------|------|-------|
| Salad | ~$0.27 | cheapest on-demand |
| Novita | ~$0.36 spot / ~$0.72 | |
| SwissGPU | ~$0.63 | |
| RunPod | ~$0.69–0.99 | **per-millisecond billing**; containerized (CUDA driver exposed — fine for cubin launch) |
| Vast.ai | ~$0.77–2.34 | marketplace; rate varies with supply |

A full device-test run is well under a dollar.

### AMD RDNA 4 — `gfx1201`

The consumer RX 9070 XT is not yet widely cloud-listed, but its **identical
ISA target** ships as a workstation card that is:

- **Radeon AI PRO R9700** (RDNA 4, `gfx1201`, 32 GB) — **HostKey, ~€0.471/hr
  (~$0.51)**, as a **dedicated server**. Dedicated bare-metal gives full
  ROCm/HIP driver control, which is more reliable for
  `hipModuleLoad` / `hipModuleLaunchKernel` than containerized marketplace
  instances. Same `gfx1201` as the RX 9070 XT, so it validates the exact
  codegen target.

### AMD RDNA 3 (optional discrete reference) — `gfx1100`

- **RX 7900 XTX** — ~$0.12–0.66/hr (Vast.ai, TensorDock, Thunder Compute,
  HostKey). Useful only if a discrete RDNA 3 reference distinct from the
  Strix Halo APU is wanted; the APU already covers `gfx1151` for free.

---

## 4. Practical workflow

1. **Day-to-day:** Tier-0 only — codegen, assembly, PTX/ISA text assertions.
   No GPU, runs in the normal build + `run_tests` loop.
2. **Per artifact / per milestone:** when a backend emits a new arch's
   cubin/hsaco, burst-rent that arch, run the Tier-1 device suite, tear
   down. NVIDIA via a per-second cloud (RunPod / Salad / Novita); AMD RDNA 4
   via the HostKey dedicated box for clean ROCm.
3. **Container vs. dedicated:** NVIDIA consumer containers expose the CUDA
   driver and launch cubins fine. For AMD, prefer a **dedicated** instance —
   HIP/ROCm driver access is more dependable on bare metal than in a
   marketplace container.
4. **Cost posture:** seconds-billed means an idle weekend costs $0; a
   validation run costs cents. Renting per-arch beats buying every card —
   the arch matrix only grows, and you pay only for the week's target.

---

## 5. Maintenance

- Update the pricing snapshot date and figures when re-checked; treat §3 as
  volatile.
- Append rows to §2 as new arches enter scope (datacenter parts, RDNA 5,
  next Blackwell refresh, …). Keep the "owned vs. burst-rent" column honest.
- Always reconcile the assumed `sm_*` / `gfx*` against the live instance
  before trusting a Tier-1 result.

---

## Sources (May 2026)

- [getdeploying — RTX 5090 providers & pricing](https://getdeploying.com/gpus/nvidia-rtx-5090)
- [RunPod — RTX 5090](https://www.runpod.io/gpu-models/rtx-5090)
- [Vast.ai — pricing](https://vast.ai/pricing)
- [HostKey — AMD Radeon servers (RX 7900 XTX, AI PRO R9700 / RDNA 4)](https://hostkey.com/gpu-dedicated-servers/radeon/)
- [cloud-gpus.com — Renting AMD Cloud GPUs in 2026](https://cloud-gpus.com/amd-gpus/)
- [Northflank — cheapest cloud GPU providers 2026](https://northflank.com/blog/cheapest-cloud-gpu-providers)
