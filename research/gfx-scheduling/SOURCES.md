# HTML-only sources — game rendering pipelines & GPU scheduling

These exist only as web pages (vendor blogs, engine docs, spec chapters). No PDF is
fetched for them; the extract below is the scheduling-relevant guidance. PDF-backed
sources live in `papers/` with `.pdf.txt` markers.

---

## FrameGraph: Extensible Rendering Architecture in Frostbite (Yuriy O'Donnell, GDC 2017)

- url: https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in
- video: https://www.youtube.com/watch?v=1Sb3s7Xie4M
- slides (pptx, no stable public PDF): https://www.slideshare.net/slideshow/framegraph-extensible-rendering-architecture-in-frostbite/72795495

The talk introduces the render-graph model that the whole industry then copied: a frame is
described as "a graph of all render passes and resources", built fresh every frame in a
setup phase where each pass declares the resources it creates, reads and writes; a compile
phase then culls passes whose outputs nobody consumes, computes resource lifetimes and
derives all barriers/transitions; an execute phase records the actual commands. O'Donnell's
stated motivation is that it "enables implementation of rendering features in a decoupled
and modular way, while still maintaining efficiency" — no pass has to know the global frame
structure, and async-compute placement and transient memory aliasing become properties the
compiler derives rather than things a human hand-codes.

---

## Render graphs and Vulkan — a deep dive (Hans-Kristian Arntzen / themaister, 2017)

- url: https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/

The most concrete published description of render-graph *mechanics*. Passes classify each
resource as write-only (`loadOp = CLEAR/DONT_CARE`), read-write (`loadOp = LOAD`) or
read-only. The graph is built by recursively walking back from the backbuffer, then the
list is reversed and deduplicated to give a valid submission order; a reordering pass then
moves passes apart to maximize overlap, ranked by (1) whether passes can be merged into one
render pass, (2) how many passes fit between a write and its dependent read, (3) whether
the reorder keeps the graph acyclic. Each pass sorts its resources into an *invalidate*
bucket (inputs needing cache invalidation + layout transition) and a *flush* bucket
(outputs needing cache flush); barriers are omitted when caches are already in the right
state. Cross-queue (graphics↔compute) dependencies must use `VkSemaphore`, not `VkEvent`,
and shared resources are declared `CONCURRENT` to avoid queue-family ownership transfers.
Transient aliasing reuses one `VkImage` for resources with disjoint pass ranges, copying the
outgoing alias's barrier state to the incoming one with layout forced to `UNDEFINED`.

---

## Render Dependency Graph in Unreal Engine (Epic)

- url: https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine

RDG is an immediate-mode API (`FRDGBuilder::AddPass()`) where every pass supplies a name, a
parameter struct whose members *are* the resource read/write declarations
(`SHADER_PARAMETER_RDG_TEXTURE`, `RDG_TEXTURE_ACCESS`, …), pass flags
(`Compute` / `Raster` / `AsyncCompute`), and a lambda that is deferred until graph
execution. Resources are transient (lifetime bounded by the graph, memory may alias with
any disjoint-lifetime resource) or external (registered/extracted, outliving the graph).
The transient allocator plans allocations across the execution timeline and produces a
"significant reduction in the GPU memory watermark". Transitions and *split* barriers are
derived by traversing the graph "to hide latency and improve overlap on the GPU". Async
compute placement is derived, not hand-placed: RDG finds the last graphics producer and the
first graphics consumer of an `AsyncCompute` pass and inserts the fences there. Passes whose
outputs are never consumed are culled.

---

## Render graph system — Unity SRP/URP

- url: https://docs.unity3d.com/Manual/urp/render-graph-introduction.html

Two-stage model: a *recording* stage where a pass declares the textures it uses, and an
*execution* stage where commands run. The graph then automatically (a) skips allocating
resources the frame does not use, (b) frees a resource after its last reader, (c) reuses
allocated memory when a later texture has identical properties, (d) culls passes whose
output the final frame does not use, and (e) *automatically synchronizes the compute and
graphics queues* when compute shaders are involved. On tile-based mobile GPUs it also merges
adjacent passes into one native render pass so intermediates stay in tile memory. Notably,
Unity exposes none of this as manual control — the declaration is the only input.

---

## Advanced API Performance: Async Compute and Overlap (NVIDIA)

- url: https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/

The stated principle is "to increase the overall unit throughput by reducing the number of
unused warp slots and to facilitate the simultaneous use of nonconflicting datapaths".
Practical rules: pick overlap pairs by *unit throughput*, not warp occupancy; look for
passes with high "SM Idle % without conflicting high throughput units"; overlap different
datapaths (FP / ALU / memory / RT core / tensor core / graphics pipe); compute-over-compute
overlap is very efficient on Ampere; converting post-processing to compute creates new
overlap opportunities; async work can be run *between* frames. Anti-rules: do not overlap
workloads sharing read/write resources; do not overlap two passes that both saturate
L1/L2/VRAM; do not overlap two RT-core workloads (same units, interference degrades);
be careful with more than two queues when hardware-accelerated GPU scheduling is off; do
not use D3D12 command-queue priorities to influence async/sync scheduling. Crucially:
resource transitions (barriers) and descriptor-heap changes trigger a *wait-for-idle*, and
"WFI forces all warps on the same queue to be fully drained" — the gap a WFI opens is
exactly the window async work should fill. Long async workloads that miss their sync point
are called out as the main failure mode.

---

## Concurrent execution / asynchronous queues (AMD GPUOpen)

- url: https://gpuopen.com/learn/concurrent-execution-asynchronous-queues/

Three queue types — copy/DMA, compute, direct/graphics. "GCN hardware contains a single
geometry frontend, so no additional performance will be gained by creating multiple direct
queues", and AMD reports they "haven't seen significant performance benefits from using more
than one compute queue in applications profiled so far". Pair by *complementary
bottleneck*: LDS/ALU-heavy compute shaders are good async candidates, depth-only rendering
pairs well with compute, and post-processing of frame N can overlap shadow rendering of
frame N+1. Explicitly: "manually specifying the tasks to run in parallel is more efficient
than trying to automate this process." Command lists must be big enough that the gain
exceeds "the cost of splitting the command list and stalling on fences". One case study
saved ~10% frame time by double-buffering copy-queue uploads to remove an inter-queue stall.

---

## Async compute all the things (Kostas Anagnostou / Interplay of Light, 2025)

- url: https://interplayoflight.wordpress.com/2025/05/27/async-compute-all-the-things/

Measured on an RTX 3080 Mobile at 1080p, async'ing GTAO, RTGI ray generation and BRDF LUT
generation:

| configuration | serial | async | saved |
|---|---|---|---|
| GTAO + raytraced shadows | 5.73 ms | 4.60 ms | 1.13 ms |
| GTAO + BRDF LUT + Hi-Z + shadowmap | 7.00 ms | 5.70 ms | 1.30 ms |
| GTAO + RTGI + BRDF LUT over shadowmap | 6.63 ms | 4.71 ms | 1.92 ms |

GTAO over a *rasterized* shadowmap costs 2.1 ms combined versus 3.22 ms over *raytraced*
shadows — the good pairing is memory/ALU-bound compute against geometry-bound rasterization.
"Overlapping tasks with high SM and cache throughput leads to somewhat reduced gains."

---

## D3D12 Work Graphs (Microsoft DirectX blog, 2024)

- url: https://devblogs.microsoft.com/directx/d3d12-work-graphs/

GPU-side work generation and scheduling. Three launch modes: *broadcasting* (classic
dispatch grid per record), *coalescing* (hardware packs multiple records into one thread
group up to a declared max, so LDS can be shared across records), *thread* (one thread per
record; threads from different launches pack into the same wave). Producers allocate
consumer records with `GetGroupNodeOutputRecords()` / `GetThreadNodeOutputRecords()`; the
app kicks the graph with `DispatchGraph()` from CPU or GPU memory. The scheduling claim:
"the system can schedule the requested work as soon as the GPU has capacity to run it" —
unlike `ExecuteIndirect`, which serializes on command-buffer processing. Apps supply a
backing store sized from `D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS`; the system buffers
producer→consumer data in cache when it fits. Constraints: acyclic except self-recursion of
depth ≤ 32; no explicit multi-producer synchronization primitives; record size is bounded
(bulk data goes through UAVs). The motivating quote (Epic): "GPU-driven rendering was
accomplished by the CPU having to guess what temporary allocations were needed by the GPU,
often over-allocating to the worst case."

---

## Vulkan timeline semaphores (Khronos blog)

- url: https://www.khronos.org/blog/vulkan-timeline-semaphores

A single primitive carrying "a monotonically increasing 64-bit integer value"; submissions
name wait values and signal values. It replaces the binary-semaphore (device↔device) plus
fence (device→host) split with one omnidirectional object, and adds three properties a
scheduler needs: **wait-before-signal** (a submission may wait on a value that has not been
signalled yet, so the whole graph can be submitted up front), **many waiters per signal**,
and **no reset between reuses**. Host side: `vkSignalSemaphore`, `vkWaitSemaphores`,
`vkGetSemaphoreCounterValue`. The one gap: WSI/presentation APIs are still binary-semaphore
only.

---

## Vulkan queue priority and VK_KHR_global_priority (Vulkan spec)

- url: https://registry.khronos.org/vulkan/specs/latest/html/chap5.html#devsandqueues-priority
- url: https://registry.khronos.org/vulkan/specs/latest/html/chap53.html#VK_KHR_global_priority

Per-device priority (`VkDeviceQueueCreateInfo::pQueuePriorities`) is "a normalized
floating-point value between 0.0 and 1.0, which is then translated to a discrete priority
level by the implementation". The spec deliberately promises almost nothing: "the
implementation makes no guarantees with regards to ordering or scheduling among queues with
the same priority"; "an implementation **may** allow a higher-priority queue to starve a
lower-priority queue on the same VkDevice until the higher-priority queue has no further
commands to execute"; and flatly, "no specific guarantees are made about higher priority
queues receiving more processing time or better quality of service than lower priority
queues."

`VK_KHR_global_priority` (core in Vulkan 1.4) raises this to system scope with a discrete
enum (LOW / MEDIUM / HIGH / REALTIME), queryable per queue family via
`VkQueueFamilyGlobalPriorityProperties::priorities` (a contiguous ascending run, so a driver
may expose only a subset). Default is MEDIUM. "The driver implementation will attempt to
skew hardware resource allocation in favor of the higher-priority task", and global priority
"shall take precedence over the per-process queue priority". Two failure modes are specified:
requesting above MEDIUM without privilege returns `VK_ERROR_NOT_PERMITTED`, and exhausted
resources return `VK_ERROR_INITIALIZATION_FAILED`. The spec's own warning: "abuse of this
feature may result in starving the rest of the system from hardware resources." All queues
created from the same queue family in one device must share one global priority level.

---

## Android Frame Pacing (Swappy) library

- url: https://developer.android.com/games/sdk/frame-pacing

The canonical statement of the frame-budget problem. A game rendering at a rate that does
not divide the display refresh gets frame times like "49 ms, 16 ms, 33 ms" even at a stable
average. Naive present-as-fast-as-possible causes *buffer stuffing*: the present queue
fills, the game loop blocks on VSYNC, and one whole frame of latency is added permanently.
Swappy fixes both with (a) presentation timestamps (`EGL_ANDROID_presentation_time`,
`VK_GOOGLE_display_timing`) so short frames are not presented early, and (b) sync fences
(`EGL_KHR_fence_sync`, `VkFence`) so long frames inject a wait instead of building
back-pressure. Auto mode picks the swap interval from *measured CPU and GPU times* and
disables pipelining for ultra-fast frames to cut latency. Multi-rate displays give a ladder
of achievable rates (60 Hz → 60/30/20; 60+90 Hz → 90/60/45/30; +120 Hz →
120/90/60/45/40/30), i.e. the scheduler's deadline is quantized, not continuous. Case study:
Mir 2's slow-session rate fell from 40% to 10%. Common budget arithmetic quoted across the
Android/Unity docs: 16.67 ms total at 60 fps, of which mobile titles target 8–10 ms CPU and
8–10 ms GPU, leaving 2–3 ms of headroom for thermal throttling and OS overhead.
