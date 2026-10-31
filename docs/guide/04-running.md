# 04 — Running your application

## Directly

A build produces a native binary, and you run it like one:

```bash
$ cajeta build
$ ./build/exe/com.example.basic
hello from com.example.basic
```

## As a task

`cajeta run` is a manifest task. The compiler has no built-in command of that
name, and the `basic` archetype does not define the task, so add it to `tasks`
in `cajeta.json`:

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

`settings.capabilities` declares what the program touches. The vocabulary is
`filesystem`, `process`, `network`, and `env`:

```json
"capabilities": ["filesystem", "network", "process"]
```

Today the list is the audit surface, and a reviewer reads it instead of the
whole dependency tree. Build-tool plugins are held to it. A plugin runs
against the allowlist intersection of what it asks for and what the manifest
grants. Runtime enforcement for the program's own native code, such as an
undeclared socket open failing, is designed but not yet wired. Declare
honestly anyway. The [tour's manifest](../../samples/tour/cajeta.json)
documents why each of its capabilities is present.

Next: [Debugging](05-debugging.md).
