# cajeta.buildtool.plugin

The plugin-author API for cajeta build-tool plugins. Every plugin
imports types from this package; the build tool's runtime provides
the implementations at action-dispatch time.

## Surface

| Type            | Purpose                                                     |
|-----------------|-------------------------------------------------------------|
| `ActionContext` | Passed to every action entry. workdir / project name + version / write+warn+log. |
| `ActionResult`  | Returned from every action entry. outputs + findings + error message. |
| `Finding`       | One structured finding (rule, severity, file, line, column, message). |
| `Severity`      | `ERROR` / `WARNING` / `INFO` tag for findings.              |

JSON values (action params, resolved config) use the stdlib types
from `cajeta.codec.json` directly — `JsonObject`, `JsonArray`,
`JsonValue` — with Optional-returning typed getters
(`getString(key) → Optional<String>`, `getInt`, `getLong`,
`getBoolean`, `getArray`, `getObject`, plus `getStringArray` and
`containsKey`/`keys`). One canonical JSON model in stdlib, no
parallel hierarchy here.

## Action entry shape

Every plugin action has the same entry-symbol signature:

```cajeta
package my.plugin;

import cajeta.buildtool.plugin.ActionContext;
import cajeta.buildtool.plugin.ActionResult;
import cajeta.codec.json.JsonObject;

public class MyAction {
    public static ActionResult run(ActionContext ctx, JsonObject params) {
        // 1. Read params.
        Optional<String> input = params.getString("input");
        if (input.isEmpty()) {
            return ActionResult.error("my-action: 'input' required");
        }

        // 2. Do work.
        // ...

        // 3. Publish outputs + findings.
        ActionResult r = #heap ActionResult();
        r.output("result", "...");
        r.findings(#findings);
        return #r;
    }
}
```

The plugin's `cajeta.json` declares the entry symbol path:

```jsonc
"details": {
    "plugin": {
        "id":      "my.plugin",
        "actions": ["my.plugin.do-thing"],
        "entries": {
            "my.plugin.do-thing": "my.plugin.MyAction::run"
        }
    }
}
```

## Why these types

### `ActionContext` is an interface

The runtime implements `ActionContext` per-dispatch. Two
implementations are anticipated:

- The **subprocess runtime** (Phase 7c) — plugin runs as a child
  process; `ctx.write(...)` writes one JSON-line record on stdout
  that the parent decodes.
- The **in-process runtime** (later) — plugin is JIT-loaded;
  `ctx.write(...)` is a direct call into the parent's logging
  channel.

Plugins write against the interface; the runtime picks the
implementation.

### `ActionResult` is a builder

The fluent shape lets the call sites read as one expression:

```cajeta
return r.error("coverage 73.5% < min 80%");
```

`error()` returns `this` and is idempotent — the first error wins,
so a follow-up "and also..." doesn't shadow the truthful cause.

### `Finding` is point-positioned

v1 carries `(file, line, column)` per finding — no end-positions,
no range. Most consumers (CI, IDE gutter) render point-positions
fine; adds when a real consumer needs range. Severity is narrowed
to `ERROR` / `WARNING` / `INFO` so it maps cleanly to the native
`lint` action's `--fail-on-severity=<level>` flag.

### `Severity` is an enum

Compare with `==` (`f.severity() == Severity.ERROR`); render with
`.toName()` when a format wants the string form. SARIF mapping:
ERROR→`error`, WARNING→`warning`, INFO→`note`.

## Versioning

This package ships with the build tool and is versioned in lockstep
with it. A plugin pinning `"cajeta.buildtool.plugin": "1.0.*"`
gets the surface frozen for the lifetime of the 1.x build tool
series. Breaking changes bump the major version; new accessors
land in minor versions and old plugins compile against them
unchanged.
