# The engine API

The public surface of `dev.cajeta.llm`. Everything the command line and cabra
do, they do through this; there is no privileged path.

## 1. `LlmEngine`

| member | meaning |
|---|---|
| `static #LlmEngine load(#EngineOptions)` | opens the checkpoint, builds and binds the model, makes it device resident, warms up, builds the scheduler; throws `LlmException` on an invalid option, an unsupported architecture or format, or a missing `config` for a safetensors model |
| `int32 submitText(String prompt, #SampleParams p, int32 maxNew)` | tokenises with the model's tokenizer (BOS per the model's metadata) and queues a request; returns its id; throws when the model carries no tokenizer |
| `int32 submitTokens(int32[] ids, int32 len, int32 maxNew, #SampleParams p)` | queues raw token ids, adding nothing |
| `void runAll(int32 maxSteps)` | steps the scheduler until every request is done or the cap is reached |
| `boolean stepOnce()` | one scheduler step; true while work remains |
| `Request request(int32 id)` | the request, borrowed |
| `#String textOf(int32 id)` | the generated tokens detokenised; needs the tokenizer |
| `boolean cancel(int32 id)` | finishes a request where it stands with `FINISH_CANCEL`; false when already done or unknown; call between steps on the stepping thread |
| `void setSink(TokenSink)` | the streaming callback, one for the whole engine |
| `int32[] eogTokens()`, `boolean isEog(int32 t)` | the end-of-generation set: the GGUF's eos, eot and eom ids unioned with the tokenizer's |
| `Scheduler scheduler()`, `CausalLM model()`, `Tokenizer tokenizer()` | the parts, borrowed |
| `void close()` | closes the block store |
| `static #String resolveTuneDir(String given)` | the tune directory the engine would use ([Configuration §2](configuration.md)) |

A request's `stopTokens` default to the end-of-generation set when the
caller leaves them null.

## 2. `EngineOptions`

Constructed with the model path; every other field has a default.
[Configuration §1](configuration.md) is the full table. The ones most
programs touch: `ctx`, `maxSeqs`, `device`, `tuneDir`, `kvStore`, and for a
safetensors model `config`.

## 3. `SampleParams`

One instance per request; each sequence in a batch samples with its own.

| field | default | meaning |
|---|---|---|
| `temperature` (constructor) | | 0 is greedy and deterministic |
| `seed` (constructor) | | each request has its own random stream |
| `topK` | 0 (off) | |
| `topP` | 0 (off; at or above 1 also off) | |
| `repeatPenalty` | 1.0 (off) | |
| `stopTokens` | the model's end-of-generation set | token ids that end the request |
| `stops` | none | strings matched against a rolling detokenised window; needs the tokenizer |

## 4. `Request`

| member | meaning |
|---|---|
| `generatedCount()`, `generatedAt(i)` | the generated token ids so far |
| `isDone()` | finished |
| `finishReason()` | `FINISH_EOS` (an end-of-generation token), `FINISH_BUDGET` (`maxNew` reached), `FINISH_STOP` (a stop string), `FINISH_CANCEL`, or `FINISH_NONE` while running |

A request is WAITING until admitted, RUNNING while it holds a scheduler
slot, PREEMPTED when its K/V blocks were taken for an older request (it
resumes ahead of new admissions), and DONE.

## 5. `TokenSink`

```cajeta
public interface TokenSink { void onToken(int32 reqId, int32 token); }
```

Called inside the scheduler step that produced the token, before the next
step begins, for every request of the engine. One sink serves the batch;
dispatch on `reqId`. Keep it cheap.

## 6. `Tokenizer`

| member | meaning |
|---|---|
| `encodePrompt(text)` | ids with BOS per the model's policy |
| `encode(text)`, `encodeBytes(bytes)` | ids without the prompt policy |
| `decode(ids, n)`, `decodePiece(id)` | bytes |
| `vocabSize()`, `tokenOf(id)`, `idOf(piece)`, `eogIds()`, `bosToken()`, `addsBos()`, `setBosPolicy(…)` | lookup and policy |

One pipeline (normaliser, pre-tokeniser, BPE, decoder) serves the Llama and
Qwen families, from the GGUF's embedded vocabulary or a Hugging Face
`tokenizer.json`; SentencePiece `tokenizer.model` files are read directly.

## 7. `ChatTemplate`

`ChatTemplate.fromString(template, name, bosToken, eosToken)` interprets the
model's own template through the `dev.cajeta.jinja` engine; `render` and
`renderJson` produce the prompt text from a message list. The template text
is read from the GGUF (`GgufFile.chatTemplate()`) or `tokenizer_config.json`.
The engine does not expose the GGUF's template itself; the command line and
cabra open the file for it, which is an API gap worth knowing.

## 8. Diagnostics

The engine records routing and scheduling decisions as `DiagRecord`s
(category, session, name, six typed values) and hands each to the callback
registered with `Diag.register(DiagCallback)`; `Diag.register(null)` turns
recording off, and with nothing registered nothing is built. The callback runs
on the engine's thread mid-generation, the record is valid only during the
call, and a callback that throws is swallowed. The engine formats nothing,
buffers nothing and chooses no logging backend; the command line and cabra
bridge records onto `dev.cajeta.logging`.

Categories a consumer sees: `batch-route` and `route` (which kernel served
a projection), `tile-choice`, `prefill-mode`, `batch-refused`,
`decode-batch` and `decode-batch-phase`, `sched-step`, `sched-admit`,
`sched-phase`, `kv-ceiling`, `moe-rows`, `expert-cache`, `widen-budget`,
`moe-budget`, `tune-dir`, `kernel-avail`, `launch-failure`, `load-phase`.

## 9. Exceptions

`LlmException` is thrown for a user-facing fault: an invalid option, an
unsupported architecture or format (the message lists the supported ones), a
model with no tokenizer or no chat template where one is needed, a readiness
promise a route did not keep. A device launch refused by the runtime surfaces
as `XpuLaunchException` with the refusal's reason.
