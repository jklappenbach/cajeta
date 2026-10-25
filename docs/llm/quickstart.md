# Quickstart

The shortest complete program: load a GGUF, submit a prompt, run it to
completion, read the text.

```cajeta
import cajeta.lang.String;
import cajeta.lang.System;
import dev.cajeta.llm.EngineOptions;
import dev.cajeta.llm.LlmEngine;
import dev.cajeta.llm.gen.SampleParams;

public final class Hello {
    public static void main(String[] args) {
        EngineOptions o #= heap EngineOptions("Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf");
        LlmEngine e #= LlmEngine.load(#o);

        SampleParams p #= heap SampleParams(0.7f, 42L);   // temperature, seed
        int32 id = e.submitText("The capital of France is", #p, 32);  // 32 new tokens at most
        e.runAll(4096);                                   // step until every request is done

        String text #= e.textOf(id);
        System.stdout.println(text);
        e.close();
    }
}
```

What happened:

1. `EngineOptions` carries the model path and the defaults (context 4096,
   four scheduler slots, adaptive prefill chunk, the dispatcher's backend
   order). [Configuration](configuration.md) lists everything it can set.
2. `LlmEngine.load` forces the backend if one was named, resolves the tune
   directory, opens the checkpoint, builds the model, binds the weights,
   makes the model device resident, runs one warm-up prefill and decode so
   the first real request is not the one that pays for the probes, and builds
   the scheduler.
3. `submitText` tokenises the prompt with the model's tokenizer (BOS follows
   the model's own metadata) and queues a request with its sampling
   parameters and a budget of new tokens. Requests queue; nothing runs yet.
4. `runAll(maxSteps)` steps the scheduler until every request has finished,
   or the step cap is hit. Each step gives every running request one bounded
   piece of work: a chunk of its prompt, or one decode token.
5. `textOf(id)` detokenises what the request generated. The request stopped on
   the model's end-of-generation set, its token budget, or a stop string.

## Streaming

A `TokenSink` receives every token inside the scheduler step that produced
it, keyed by request id, for every request of the engine:

```cajeta
public final class Printer implements TokenSink {
    LlmEngine e;
    public void onToken(int32 reqId, int32 token) {
        System.stdout.print(e.tokenizer().decodePiece(token));
    }
}

e.setSink(printer);
```

Keep the callback cheap: it runs on the engine's thread between launches.

## Several requests at once

Submit more than one before running, and the scheduler batches them:

```cajeta
int32 a = e.submitText("Write a haiku about rivers.", #heap SampleParams(0.8f, 1L), 64);
int32 b = e.submitText("Explain quicksort in two sentences.", #heap SampleParams(0.0f, 2L), 96);
e.runAll(4096);
```

Prompts are prefilled in chunks that interleave with the other request's
decode steps; two or more greedy requests decode as one batch, one weight
pass serving both. `stepOnce()` is the single-step form for a host that wants
to interleave its own work, and `cancel(id)` finishes a request where it
stands, between steps, on the thread that steps.

## A chat turn

The engine renders a conversation through the model's own chat template:

```cajeta
ChatTemplate t #= ChatTemplate.fromString(ggufChatTemplate, "model", bos, eos);
String prompt #= t.renderJson(messagesJson);   // [{"role":"user","content":"..."}]
int32 id = e.submitText(prompt, #params, 256);
```

The command line's `chat` verb and cabra's `chat` operation do exactly this.
The template text comes from the GGUF (`tokenizer.chat_template`) or the
repository's `tokenizer_config.json`; a model without one is reported.

## Running it

Add the library to the project (`cajeta add dev.cajeta.llm`; it brings
`dev.cajeta.codec`, `dev.cajeta.jinja` and `dev.cajeta.logging`), build with
the backends you want device code for (`--xpu-backend=nvptx,vulkan,cpu`), and
run. Without an accelerator the same program runs on the CPU backend. Set
`CAJETA_XPU_BACKEND=cuda` (or `hip`, `vulkan`, `cpu`), or the `device`
option, to choose one explicitly; add `--verbose` to the command-line tools,
or register a `Diag` callback in your own program, to see which route each
projection took.
