# Cajeta plugin extension API — v1

This is the contract a plugin honors to extend the build tool with new
actions and structured findings. Plugins are subprocesses spawned by the
task runner; they communicate over stdin/stdout in a JSON-line protocol.
Authoritative C++ host: `src/cajeta/buildtool/PluginRuntime.{h,cpp}` and
`PluginAction.{h,cpp}`. First-party plugins live under
`build-tools/plugins/<plugin>/src/main/cajeta/`.

## Identity

A plugin is a directory shipped as a normal cajeta package
(`name`, `version`, etc.), with a `details.plugin` sub-block:

```json
{
  "details": {
    "name": "cajeta.coverage",
    "version": "1.0.0",
    "plugin": {
      "id": "cajeta.coverage",
      "binary": "build/exe/cajeta-coverage",
      "actions": ["cajeta.coverage.instrument", "cajeta.coverage.collect",
                  "cajeta.coverage.report"],
      "capabilities": ["filesystem"]
    }
  }
}
```

- `id` is the namespaced plugin identifier (always begins with the package's
  group; first-party plugins use the `cajeta.*` reserved namespace).
- `binary` names the executable inside the package; resolved relative to
  the plugin's install root.
- `actions` enumerates the action names the plugin can dispatch.
- `capabilities` is the union the plugin declares it needs.

## Resolution + capability gate

When the host loads the manifest:

1. The plugin's declared capabilities are checked against
   `settings.plugins-allowed-capabilities`. Anything not in the allowlist
   fails plugin load with a citation naming the plugin + offending
   capability. First-party plugins get a wider default allowlist; user
   plugins default to `["filesystem"]`.
2. Resolved plugins are recorded in the lockfile under the top-level
   `plugins` array (sorted set of capabilities included — so PRs surface
   capability additions).

## Lifecycle

For each plugin action invocation:

1. Host spawns `<plugin-binary>` with stdin/stdout pipes.
2. Host writes one `invoke` envelope on stdin and closes stdin only after
   the plugin emits its `result` envelope.
3. Plugin reads the envelope, runs, may stream zero or more `finding`
   envelopes back, then emits exactly one `result` envelope.
4. Host reaps the child. Non-zero exit code that did not produce a `result`
   envelope is a hard error.

## Wire format

Each line on the wire is a JSON object terminated by `\n`. Fields are
documented below; the host tolerates extra fields for forward compatibility
but warns when the `kind` is unknown.

### Host → plugin: `invoke`

```json
{
  "kind": "invoke",
  "action": "cajeta.coverage.instrument",
  "params": {"input": "src/main/cajeta/Foo.cajeta", "grain": "line"},
  "context": {
    "project-root": "/abs/path",
    "manifest-checksum": "sha256:...",
    "properties": {"profile": "test", "flavor": "release", "...": "..."}
  }
}
```

### Plugin → host: `finding` (zero or more)

A structured diagnostic the host should surface to the user. Equivalent to
a SARIF result entry but with a flatter shape.

```json
{
  "kind": "finding",
  "rule": "cajeta.lint.security.banned-import",
  "severity": "warning",
  "location": {
    "file": "src/main/cajeta/Foo.cajeta",
    "line": 42,
    "column": 1
  },
  "message": "import org.danger.* is banned by policy"
}
```

`severity` is one of `info`, `warning`, `error`. Default is `info`.
Findings stream live; the host appends them to `ActionResult.findings`.

### Plugin → host: `result` (exactly one)

```json
{
  "kind": "result",
  "outputs": {
    "instrumented-source": "build/coverage/Foo.cajeta",
    "marker-count": "37"
  },
  "exit-code": 0
}
```

All values in `outputs` are strings (mirrors the action-catalog rule —
threading via `${id.field}` is text). The host populates
`ActionResult.outputs` from this map.

A plugin signaling failure emits `{"kind":"result","exit-code":1,"error":"..."}`;
the host surfaces `error` as the task-level message.

## Capability checks at runtime

The capability allowlist is enforced at action invocation: an action whose
declared capabilities aren't in the granted set never reaches the plugin
binary. Plugins are also expected to honor the granted set themselves
(e.g. don't open sockets when `network` wasn't granted) — the host runs
the subprocess inside the same sandbox policy as native actions, so a
plugin that tries to exceed its allowance hits the bwrap boundary first.

## Versioning

This document is `extension-api-v1`. A wire-incompatible change is a major
bump; additive changes (new envelope kinds, new fields tolerated by
forward-compat readers) are minor bumps. The protocol version that a
plugin targets is implied by its package's `cajeta-lang-version` field —
older plugins keep running under the toolchain's N±k compatibility window.

## Reference plugins

- `cajeta.coverage` — instrument / collect / report. Source under
  `build-tools/plugins/code-coverage/src/main/cajeta/`.
- `cajeta.lint.security` — banned-imports + secret-pattern scan. Source
  under `build-tools/plugins/security-lint/src/main/cajeta/`.

Both ship as part of the v1 first-party plugin set (see the open decision
"First-party plugin list" in plan/build-tool-plan.md).

## Reference host implementation

- `src/cajeta/buildtool/PluginRuntime.{h,cpp}` — subprocess + JSON-line
  protocol parser.
- `src/cajeta/buildtool/PluginAction.{h,cpp}` — adapter wrapping a plugin
  action as a normal `Action`.
- `src/cajeta/buildtool/Plugin.{h,cpp}` — plugin manifest model, capability
  gate, lockfile slot.
- Tests: `PluginTests.cpp`, `PluginRuntimeTests.cpp`,
  `Phase7AcceptanceTests.cpp`.
