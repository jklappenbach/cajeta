# 04 — Running your application

## Directly

A build produces a native binary; run it like one:

```bash
$ cajeta build
$ ./build/exe/com.example.basic
hello from com.example.basic
```

## As a task

`cajeta run` is not a built-in — it's a manifest task, and the `basic`
archetype doesn't define one. Add it to `tasks` in `cajeta.json`:

```json
"run": {
    "description": "Build, then execute the binary",
    "actions": [
        { "action": "build", "flavor": "debug", "id": "art" },
        { "action": "exec",  "command": "${art.path}" }
    ]
},
```

```bash
$ cajeta run
hello from com.example.basic
```

The [tour](../../samples/tour/cajeta.json) uses exactly this shape.

## Capabilities

A Cajeta program touches only what its manifest declares. `settings.capabilities`
lists what the program may do:

```json
"capabilities": ["filesystem", "clock"]
```

Code that reaches for an undeclared capability — say, opening a socket without
`"network"` — fails rather than silently succeeding. This is the security
model: the manifest is the audit surface, and a dependency can't quietly
exceed it. The [tour's manifest](../../samples/tour/cajeta.json) documents why
each of its capabilities is present; do the same in yours.

Capability violations can also be trapped in the debugger — see
[chapter 05](05-debugging.md).

Next: [Debugging](05-debugging.md).
