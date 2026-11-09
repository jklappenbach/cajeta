# Next-Concept Modeling — research findings

Evidence for the spec and plan of a `dev.cajeta.ncm` module and its serving in
cabra. Opened **2026-11-08** at Julian's request ("deep research on ncm models
and creating a ncm library application module like LLM and then add support
for it to Cabra"), with a second question: whether SPELA, the single-forward-pass
local-loss training algorithm, can be of use. Sources were searched on
2026-11-08; anything marked *unverified* was read only through a summarizing
fetch or not checked.

## 1. Julian's decisions, 2026-11-08

1. **The architecture** is arXiv 2610.14162 (§2), not Meta's Large Concept
   Models or ConceptLM.
2. **Purpose:** both a research port and a serving product.
3. **Training:** yes, we will train new NCM models.
4. **Streaming:** open. One chunk per span and replacing coarse-to-fine
   previews are both candidates, to be explored.
5. **Text longer than one span:** chaining spans is untested in the paper;
   we will test it.
6. **Weight format:** whichever is most efficient, to be measured.
7. **SPELA:** to be worked out how it can be brought to bear (§6).
8. **Backend:** CPU first to get things working, then kernels.

## 2. What NCM is

"Next-Concept Modeling: Fast, Parallel Coarse-to-Fine Text Generation", by
Hermann Kumbong and Ishan S. Khare (equal contribution), with Mlauzi, Woen,
Ding, Buchanan, Arora (Together AI), Mirhoseini and Ré (Stanford). arXiv
[2610.14162](https://arxiv.org/abs/2610.14162), 9 October 2026, no venue yet.
[Project page](https://scalingintelligence.stanford.edu/pubs/ncm/),
[Hazy Research blog](https://hazyresearch.stanford.edu/blog/2026-10-12-ncm),
code at [HazyResearch/ncm](https://github.com/HazyResearch/ncm) (MIT, PyTorch),
weights at [hazyresearch/ncm-checkpoints](https://huggingface.co/hazyresearch/ncm-checkpoints/tree/main)
(about 8 GB of PyTorch `.pt` pickles).

A multiscale residual VQ-VAE turns a fixed-length text span into 8 to 12 levels
of discrete codes, coarse to fine. A transformer generates one whole level per
forward pass. It is VAR's next-scale prediction
([2404.02905](https://arxiv.org/abs/2404.02905)) applied to text, and it is
positioned against diffusion language models rather than token LLMs. A
"concept" is a code at some level of the hierarchy.

Released models range from 25M to 725M parameters: LM1B small, medium and
large, OpenWebText, TinyStories, XSum, WMT14 De-En and three Sudoku sizes. None
is a chat model.

**Not to be confused with:** Meta's Large Concept Models
([2412.08821](https://arxiv.org/abs/2412.08821), sentence embeddings in SONAR
space); ConceptLM ([2602.08984](https://arxiv.org/abs/2602.08984)) and
NCP-ArchPreview ([2609.10715](https://arxiv.org/abs/2609.10715)), which add a
concept objective to a model that still generates token by token; DLCM
([2512.24617](https://arxiv.org/html/2512.24617)); CALM
([2510.27688](https://arxiv.org/abs/2510.27688)).

## 3. The model, as the reference code builds it

**Tokenizer and VQ-VAE.** Text passes through an ordinary subword tokenizer
first (GPT-2, BERT or a task BPE). LM1B uses `bert-base-uncased` WordPiece,
30,528 entries (`config/eval/lm1b-large.yaml`); the other tasks' tokenizers are
*unverified*. The LM1B VQ-VAE has a 6-layer encoder at width 384 and a 4-layer
decoder over 128 positions, with 12 levels of lengths 1, 8, 16, 24, 32, 40, 52,
64, 76, 92, 108 and 128, each with its own 8,192-entry codebook. The quantizer
(`models/quant.py`) area-interpolates the running signal down to each level's
length and linearly interpolates the quantized level back up, blended in by a
per-level `Conv1d`. Output length is fixed per model: 128 tokens for LM1B,
1,024 for OpenWebText, 48 for XSum, 64 for WMT14.

**Generator.** A GPT-2-style transformer: LayerNorm with bias, GELU, RoPE,
QK-norm, a level encoding, a kernel-3 `Conv1d` after upsampling, and per-level
output heads tied to the codebooks (`models/next_concept.py`). LM1B-large is
36 layers, 20 heads, width 1,280 (725M). Attention is block-diagonal, not
causal: a level attends to itself and the prompt.

**Decoding loop** (`generate`): cache the prompt's K and V once; produce level
0 from a learned query; for each later level, add the codebook entries into
the running reconstruction, shrink it to the next level's length, run one
unmasked pass over prompt plus level, and sample all its positions at once;
then run the VQ-VAE decoder and detokenize. Passes equal the level count (8 on
XSum, 11 on WMT14, 12 on Sudoku). The KV cache holds only the prompt.

**Results** (generating 1,024 sequences at batch 16 on one H100): LM1B, better
generative perplexity than DUO with 11x fewer passes, 21x faster; OpenWebText
competitive and 10x faster; XSum ROUGE-L 28.0 at 8 passes against AURORA-LM's
26.6 at 128, 31x faster; WMT14 BLEU 22.6 at 11 passes, against 25.2 for a 99M
token model at 64 passes (25.2 for NCM only with MBR at 132 passes); Sudoku
99.5 / 96.5 / 86.1%. No wall-clock comparison against a token model with a KV
cache was found (*unverified*: the appendix tables were read through a
summary).

## 4. What a cajeta engine needs

**Reusable from `dev.cajeta.llm`:** `Tensor`, the xpu launch path, `Linear` and
the quantized tile families (level passes are prefill-shaped, their strongest
case), `Embedding`, `ActivationPool`, `KernelCap`, `io/SafetensorsFile` after a
one-time offline conversion of the pickles (cajeta never unpickles), and
`gen/Sampler`, `sched/Scheduler` and the prompt half of `PagedKvCache`.

**New:** a non-causal attention route (`AttnKernel` is causal by construction);
LayerNorm with bias and GELU as device ops (the engine has RMSNorm and
SwiGLU/GeGLU only); a 1-D convolution and 1-D interpolation (dev.cajeta.ml has
`Conv2d` only); the quantizer's per-level accumulate; the VQ-VAE decoder; a
WordPiece tokenizer (`Tokenizer` covers ByteLevel BPE and SentencePiece);
per-level heads, RoPE tables and level encodings. Training (decision 3) adds
the VQ-VAE encoder, codebook updates by moving average, and the generator's
per-level cross-entropy, on dev.cajeta.ml's training stack.

**Not needed for inference:** the growing decode KV cache and the paged,
speculative and pipelined decode machinery.

## 5. Serving in cabra

cabra's protocol (§3.1.2, §4) streams `{"chunk": text}` spans appended in order
and ends on an end-of-generation token, a stop string or `max_tokens`. NCM
breaks each assumption: tokens exist only after the last level and the decoder;
length is fixed by the model; stop strings apply only to a finished span. The
first chunk arrives when the last does, unless previews are streamed, which
would need a message kind that replaces rather than appends (decision 4).
Batching goes level by level with identical shapes across requests, new
requests can join at level 0, and memory per request is fixed (prompt KV plus
one reconstruction buffer). Usage should count levels and passes. Chaining
spans for longer text is decision 5's experiment.

## 6. SPELA

**What it has shown** ([2402.09769](https://arxiv.org/abs/2402.09769), TMLR):
image classification only. MLPs on MNIST-class and CIFAR datasets trail
backprop by 4 to 12 points; two-layer CNNs by 7 to 8; a classifier head trained
on a frozen ResNet50 comes within 0.4 to 2.3 points (CIFAR-10 98.20% against
99.12%). At most 101 classes, and no transformer, attention or language task.
The cajeta port is `cajeta-ml/.../train/SpelaTrainer.cajeta`, one tape per
layer.

**NCM's objective:** the generator is per-level, per-position cross-entropy
over the next level's codes given the reconstruction so far; the VQ-VAE is
reconstruction plus a commitment loss and moving-average codebook updates.

**Where it could bear (decision 7):**

| Part | Fit | Why |
|---|---|---|
| Per-level heads, or an adapter on a frozen generator | Best | Matches the frozen-backbone head result; unproven at 8,192 classes (SPELA's 5x head trick would make it 40,960 wide). |
| Generator body from scratch | Poor | 36 layers, attention and 8,192-way outputs are far past anything shown. Research. |
| VQ-VAE | Poorest | A reconstruction objective; SPELA has no generative result. |

NCM suits SPELA better than a token LLM does: it is not left to right, and each
level is an independent classification problem. The cheapest real experiment
is adapting the heads of the frozen 97M LM1B generator, compared against
backprop on the heads alone.

**Related forward-only and local-loss work:** Forward-Forward
([2212.13345](https://arxiv.org/abs/2212.13345)) and PEPITA
([2201.11665](https://arxiv.org/abs/2201.11665), *id unverified*) on small MLP
and image tasks; Contrastive Forward-Forward on ViT
([2502.00571](https://arxiv.org/abs/2502.00571)); Mono-Forward
([2501.09238](https://arxiv.org/abs/2501.09238)); Greedy Local Learning for
language-model pretraining ([2610.04867](https://arxiv.org/abs/2610.04867)),
125M and 400M models in separately trained blocks, the gap to normal training
narrowing with scale; LoPT ([2605.04913](https://arxiv.org/abs/2605.04913));
and MeZO ([2305.17333](https://arxiv.org/abs/2305.17333)), zeroth-order
forward-only fine-tuning of OPT up to 66B within about a point of full
fine-tuning. Nothing was found applying SPELA, Forward-Forward or PEPITA to a
language model or a VQ-VAE.

## 7. Toward the spec

Milestone 1 (decision 8, CPU first): a bit-exact port of LM1B-small, converted
offline from the `.pt` files, running the 12 levels and the VQ-VAE decoder on
the CPU and matching the PyTorch reference's codes per level and final tokens
at temperature 0, with generative perplexity reported against the paper.
Milestone 2: the same model on device kernels. Then XSum conditional
generation (prompt plus 48 positions, the paper's headline speed result),
cabra serving once the streaming question (decision 4) has an answer from
exploration, the weight-format measurement (decision 6), span chaining
(decision 5), and the SPELA head-adaptation experiment (decision 7). Training
new models (decision 3) needs its own spec section on dev.cajeta.ml.
