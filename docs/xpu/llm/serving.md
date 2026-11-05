# Running and serving

There are three ways to run a model. You can use the library's command line
for one prompt or one chat, embed the library in your own program, or run
cabra, the serving harness that keeps a model loaded and serves
conversations over stdio or WebSocket.

## 1. The command line

The library ships a command line as one caller of its batching API:

```
cajeta-llm run  --model <path> --prompt <text> [flags]
cajeta-llm chat --model <path> [--system <text>] [flags]
```

`run` continues the prompt as plain text: no chat template is applied, so an
instruct model may answer and then keep writing until the `-n` budget. It
streams the tokens to standard output, then prints the token count and why
generation stopped (`eos`, `stop`, `budget` or `cancel`). `chat` is the
command for an instruct model: it holds a conversation on the terminal
(`/exit` to quit), re-rendering the whole history through the model's own
template each turn, and a turn ends at the model's end-of-turn token. The flags are the engine options and the sampling parameters of
[Configuration](configuration.md). `--verbose` sends the engine's route and
scheduling records to the log at DEBUG.

The `dev.cajeta.jinja` engine interprets a model's chat template from the
model itself, the GGUF `tokenizer.chat_template` or the
`tokenizer_config.json` of a Hugging Face repository. No template table
ships beside it. A model with no template is reported, and the engine never
guesses one.

## 2. Embedded

`dev.cajeta.llm` is a library with a function-call API and no wire protocol,
no listener and no HTTP dependency, so a program embeds it by adding the
dependency and calling it. [The engine API](engine.md) is the surface.
[Quickstart](quickstart.md) is the shortest complete program.

## 3. cabra

cabra (`dev.cajeta.cabra`) is the resident-model serving harness. It loads a
model once, receives prompts, streams responses and stops each on the
model's end-of-generation set. A 72B takes about 35 seconds to load, and
with cabra you pay that once. In the reference ecosystem's terms,
`dev.cajeta.llm` is the llama.cpp role, cabra is the llama-server role, and
Olla is the distribution half of the ollama role. cabra consumes only the
engine's public API, which makes it the engine's first real embedder.

### 3.1 Three modes

```
cabra serve --model <path> [--ctx 512] [--max-seqs 1] [--device <backend>] [--verbose]
cabra host  --model <path> [--port 8850] [--token T] [--ctx 512] [--max-seqs 4]
            [--max-conns 16] [--idle-expiry 600000] [--kv-store DIR] [--web DIR]
            [--device <backend>] [--verbose]
cabra serve --connect [127.0.0.1:]PORT [--token T] [--session N] [--verbose]
```

- **Embedded** (`serve --model`): the model lives in cabra's process, which
  speaks JSONL on standard input and output. Standard output is
  protocol-pure, and logs go to standard error. EOF or a `shutdown`
  operation drains and exits. A parent process drives several conversations
  over this one pipe.
- **Host** (`host --model`): cabra loads the model and serves WebSocket
  clients on loopback, each connection one conversation, all multiplexed onto
  the engine's continuous batching. `--kv-store` makes reconnects warm across
  sessions. `--web DIR` serves the browser client from the same port, and
  `--token` is the shared secret every connector presents.
- **Client** (`serve --connect`): cabra connects to a running host and drives
  one conversation, and `--session` resumes one.

Several client processes against a host share its one loaded model. Any
other arrangement costs a load per process.

### 3.2 The protocol

The protocol takes one JSON object per line in and returns id-tagged objects
per line out, with the same operation set over stdio and WebSocket:

| request | response |
|---|---|
| `{"op":"generate","id":1,"prompt":"...", ...}` | stream chunks `{"id":1,"chunk":"text"}`, then `{"id":1,"done":true,"finish":"eos","usage":{"prompt_tokens":N,"gen_tokens":M,"prefill_ms":X,"decode_ms":Y}}` |
| `{"op":"chat","id":2,"messages":[{"role":"user","content":"..."}], ...}` | the same, rendered through the model's chat template |
| `{"op":"health","id":3}` | `{"id":3,"state":"loading"\|"ready","model":"<path>"}` |
| `{"op":"cancel","id":N}` | request N ends where it is, `finish: "cancel"` |
| `{"op":"open"}`, `{"op":"close"}` | a session (one conversation) on a host connection; `auth` presents the token first |
| `{"op":"shutdown"}` | drain and exit |

Requests carry sampling parameters (`n`, `temp`, `top_k`, `top_p`,
`repeat_penalty`, `seed`, `stop`) and `diag` to receive that turn's
diagnostics. Absent fields take the model's defaults. `chat` refuses a model
that ships no template. Every completion reports a finish reason, which is
`eos`, `budget` (max tokens), `stop` (a caller's stop string) or `cancel`. A
malformed line answers with `"id": null` and never kills the server.
End-of-generation tokens are never present in a chunk.

### 3.3 The web client

The host itself serves a browser client from `--web web/dist` on the same
port. The client offers a token prompt, several conversations each on its
own session over one socket, streaming with the answer rendered as sanitised
markdown, stop, reconnect and resume across a host restart, and an opt-in
diagnostics panel that shows the turn's route, prefill-mode and
expert-cache records. The conversations are held in the browser, so a
reload loses nothing. The client is loopback only, so bring your own tunnel.

## 4. Getting the pieces

The library and cabra are published on Olla (`https://olla.cajeta.dev`) as
`dev.cajeta.llm` and `dev.cajeta.cabra`. A project adds the library with
`cajeta add dev.cajeta.llm`, and cabra installs as an application with
`cajeta install dev.cajeta.cabra`. The engine depends on `dev.cajeta.codec`
(the SentencePiece reader), `dev.cajeta.jinja` (chat templates) and
`dev.cajeta.logging`. cabra adds `dev.cajeta.http`. Device code for the
backends the publisher's toolchain could assemble ships inside the archive.
Vulkan and the CPU backend are always present, NVPTX is present where CUDA's
assembler was, and AMDGPU where ROCm was.

Models are files you supply. You can use a GGUF from any llama.cpp-compatible
source (Hugging Face, an Ollama blob store, your own conversion), or a
Hugging Face safetensors repository with its `config.json`. A safetensors
load carries no tokenizer today, so text submission and detokenisation need
a GGUF. The library's converter
(`tools/convert/hf-to-gguf.sh <dir> <out.gguf> f16|tq1_0|tq2_0`) produces
one from a Hugging Face directory.
