# ncm-inference — Next-Concept Modeling inference in cajeta

Plan: [ncm-inference-plan](../agents/ncm-inference-plan.md).
Evidence: [ncm-research-findings](ncm-research-findings.md).
Approved by Julian 2026-11-21. Decided with him the same day: this spec is inference only; training, cabra serving and
SPELA get their own specs once this engine exists. The library lives in a new repo,
`cajeta-ncm`, published as `dev.cajeta.ncm`. A port matches the reference when every
level's codes and the final tokens are equal at temperature 0 and the logits agree
within a stated tolerance. Released weights enter through an offline conversion to
safetensors.

## 1. Definition

`dev.cajeta.ncm` runs Next-Concept Models (arXiv 2610.14162): a multiscale residual
VQ-VAE turns a fixed-length text span into 8 to 12 levels of discrete codes, coarse to
fine, and a transformer generates one whole level per forward pass. Generating a span
takes one pass per level and one VQ-VAE decode, where a token model takes one pass per
token.

The library loads the released HazyResearch checkpoints, generates spans from them on
the CPU and on every xpu backend, and matches the PyTorch reference code for code. It
is shaped like `dev.cajeta.llm` and reuses its tensors, linear layers and tile kernels,
sampler and safetensors reader, adding what NCM needs and a token model does not. It is
both a faithful research port, so the paper's results can be checked in cajeta, and the
engine that training and cabra serving will build on.

Out of scope here: training (the VQ-VAE encoder's use in training, codebook updates,
the generator's loss), serving in cabra and its streaming question, chaining spans for
text longer than one span, choosing a weight format by measurement, and SPELA. Each
needs this engine first, and the findings record why.

## 2. Features

### 2.1 A released model loads from converted weights

A Python script in `cajeta-ncm/tools/` converts a released checkpoint's `.pt` files and
its YAML config into a directory of safetensors plus a JSON config. cajeta never
unpickles. The library reads that directory and builds the tokenizer, VQ-VAE, quantizer
and generator from it, refusing by name anything it does not support.

Use cases:
1. Convert LM1B-small once, then `NcmModel.load(dir)` builds it on the CPU.
2. A config naming an unsupported option (a tokenizer kind, a layer variant) is refused
   with the option's name, not loaded wrong.
3. A converted directory records the checkpoint it came from and the script version, so
   a parity failure can be traced to its source.

### 2.2 Text becomes tokens and back

Text passes through a subword tokenizer before the VQ-VAE and comes back through it
after the decoder. LM1B uses `bert-base-uncased` WordPiece (30,528 entries), which
`dev.cajeta.llm`'s Tokenizer does not cover. The other released models' tokenizers are
confirmed when they are ported.

Use cases:
1. WordPiece encodes and decodes the LM1B reference texts exactly as HuggingFace's
   `bert-base-uncased` does, including lower-casing, accent stripping and `##` pieces.
2. A conditional model's prompt is tokenized the same way as its training data.

### 2.3 One level is one generator pass

The generator is a GPT-2-style transformer: LayerNorm with bias, GELU, rotary positions,
QK-norm, a learned level encoding, a kernel-3 `Conv1d` after upsampling, and per-level
output heads tied to the codebooks. Attention is block-diagonal, not causal: a level's
positions attend to each other and to the prompt. The engine's attention today is causal
by construction, so this is a new route.

Use cases:
1. One pass over a level's positions gives that level's logits for every position at
   once.
2. The prompt's keys and values are computed once and reused by every level's pass.
3. A level never sees a later level, and the prompt never sees a level.

### 2.4 The quantizer carries the reconstruction between levels

Between passes, the chosen codes' codebook entries are added into a running
reconstruction, which is shrunk to the next level's length by area interpolation; a
level's quantized signal is grown back by linear interpolation and blended in through a
per-level `Conv1d`. These are new 1-D operators (`dev.cajeta.ml` has 2-D convolution
only).

Use cases:
1. Given the reference's codes for levels 0 to k, the reconstruction after level k
   matches the reference's within tolerance.
2. Each level has its own codebook (8,192 entries for LM1B) and its own length (1, 8,
   16, ... 128 for LM1B), all read from the config.

### 2.5 The VQ-VAE decoder turns the last reconstruction into tokens

After the last level, the VQ-VAE decoder (4 layers for LM1B) maps the reconstruction to
token logits over the fixed output length, and the tokenizer decodes the tokens to text.

Use cases:
1. Given the reference's final reconstruction, the decoder's tokens equal the
   reference's.
2. The output length is the model's own (128 for LM1B, 48 for XSum), never a request
   parameter.

### 2.6 A span is generated level by level

`generate` runs the loop: cache the prompt, produce level 0 from a learned query, then
for each further level accumulate, shrink, pass and sample, then decode. Sampling per
level takes a temperature and top-k or top-p, through `dev.cajeta.llm`'s sampler, with
a seed. A caller can observe each level as it is produced, which is the hook later
preview streaming needs, without the library deciding how to stream.

Use cases:
1. `generate()` with no prompt samples an unconditional LM1B span.
2. At temperature 0 the same model and prompt give the same span on every run and every
   backend.
3. A per-level callback receives the level index and its codes as each pass finishes.
4. Generating a batch of spans runs each level's pass for the whole batch at once, since
   every request has the same shapes at the same level.

### 2.7 The port is checked against the reference, code for code

A reference harness runs the PyTorch code (HazyResearch/ncm, pinned by commit) on fixed
seeds and prompts and writes each level's codes, logits, reconstructions and the final
tokens. The cajeta tests compare against those files.

Use cases:
1. At temperature 0, every level's codes and the final tokens are equal to the
   reference's for LM1B-small, on a fixed set of seeds and prompts.
2. Logits, reconstructions and decoder outputs are within a stated tolerance (fp32 on
   the CPU; a looser bound for the device kernels, stated per backend).
3. A mismatch names the level and the first differing position.
4. Generative perplexity on the paper's protocol is reported beside the paper's figure.

### 2.8 The CPU first, then device kernels

Milestone 1 runs everything on the CPU. Milestone 2 runs the generator and the VQ-VAE
decoder on the xpu backends (CUDA, HIP, Vulkan), reusing `dev.cajeta.llm`'s linear
layers and tile kernels: a level pass is prefill-shaped, which is their strongest case.
New device ops are the non-causal attention, LayerNorm with bias, GELU, the 1-D
convolution and interpolation, and the quantizer's accumulate.

Use cases:
1. Every backend passes 2.7's code-for-code check at temperature 0.
2. The time to generate a span is reported per backend beside the PyTorch reference on
   the same machine (the 4090; gfx1151 on proton).
3. A backend that cannot run an op refuses by name, as the xpu kernels already do.

### 2.9 Conditional generation: XSum

The paper's headline speed result is XSum summarization: a prompt (the article) and 48
output positions in 8 passes. Once LM1B passes, the XSum model is ported and checked the
same way, which exercises the prompt path and a different level schedule.

Use cases:
1. Given an article, the XSum model's summary equals the reference's at temperature 0.
2. ROUGE-L on the paper's evaluation set is reported beside the paper's 28.0.

## 3. Open questions (to settle in the plan)

1. Which tolerance per backend (2.7.2): to be measured on the CPU port first.
2. Whether the reference harness lives in `cajeta-ncm/tools/` or a separate checkout:
   it needs PyTorch, which cajeta's CI does not install.
3. The other released models' tokenizers (OpenWebText, TinyStories, WMT14): confirmed
   per model when each is ported, not before.
