# `System` — process-level intrinsics

`System` is cajeta's namespace for process-level I/O, environment access, and tunable string properties. It isn't a regular class; the compiler recognizes `System.<namespace>.<method>(...)` shapes and lowers them directly to runtime calls (no field/method resolution, no vtable lookup). The recognized namespaces are:

| Namespace        | Surface                                                              |
| ---------------- | -------------------------------------------------------------------- |
| `System.stdout`  | Standard output stream (fd 1)                                         |
| `System.stderr`  | Standard error stream  (fd 2)                                         |
| `System.stdin`   | Standard input stream  (fd 0)                                         |
| `System.env`     | OS environment-variable access (libc `getenv` / `setenv`)             |
| `System.property` | Process-scoped string properties (Java `-Dkey=value` style)          |

Any other receiver — most commonly `System.out` from Java muscle memory — throws `CAJETA_ERROR_UNKNOWN_SYSTEM_STREAM` with a "did you mean `stdout`?" hint when `--diag-hints` is on.

---

## `System.stdout` / `System.stderr` — output streams

Both streams support `print` and `println` with two forms:

### Single-argument form

```cajeta
System.stdout.println("hello");                   // string → "hello\n"
System.stdout.println(42);                        // int    → "42\n"
System.stdout.println(3.14);                      // float  → "3.14\n"
System.stdout.println(true);                      // bool   → "true\n"

System.stdout.print("hello");                     // no trailing newline
System.stderr.println("oops");                    // fd 2
```

Primitive arguments are auto-stringified through `__cajeta_*_to_str` runtime helpers (`i64`, `f64`, `bool`). String concatenation via `+` works as you'd expect; the resulting concatenated buffer becomes the `println` argument.

```cajeta
int32 n = 7;
System.stdout.println("n = " + n);                // "n = 7\n"
```

### Multi-argument format form (`{}` substitution)

`print(fmt, x, y, ...)` / `println(fmt, x, y, ...)` walks the format string and replaces each `{}` placeholder with the next argument, stringified the same way the single-arg form does (`__cajeta_i64_to_str` / `__cajeta_f64_to_str` / `__cajeta_bool_to_str`, or unwrap for class `String`).

```cajeta
System.stdout.println("x={}, y={}", 7, 13);              // "x=7, y=13\n"
System.stdout.println("hello {}", "world");              // "hello world\n"
System.stdout.println("bool {}, float {}", true, 3.14);  // "bool true, float 3.14\n"

System.stdout.print("count = {}", 42);                   // "count = 42" (no newline)
System.stderr.println("err {} = {}", "answer", 42);      // fd 2
```

The format string is a regular `String`. `{}` is the only substitution token; no positional indices, no format specifiers, no escapes (write literal `{}` only when you mean a substitution). Extra `{}` markers beyond the supplied args render as `null`; extra args beyond the markers are ignored.

A few non-format variants exist for completeness:

- `System.stdout.printf(fmt, String[] args)` — the explicit `String[]` form. Useful when args are accumulated dynamically; the multi-arg `println(fmt, x, y, ...)` is preferred for static call sites.
- Calling `printf` with a non-`String[]` second argument throws `CAJETA_ERROR_PRINTF_BAD_ARGS` with the actual type and the expected shape spelled out.

---

## `System.env` — OS environment variables

Thin wrappers over libc `getenv(3)` / `setenv(3)`.

```cajeta
String home = System.env.get("HOME");             // /home/julian (or null)
if (home != null) {
    System.stdout.println("home = {}", home);
}

System.env.set("CAJETA_DEBUG", "1");
String mode = System.env.get("CAJETA_DEBUG");     // "1"
```

`get(String name)` returns a class `String` containing a freshly-allocated copy of the env value, or `null` if the variable isn't set. The copy is necessary because the libc `getenv` pointer is invalidated by subsequent `setenv` / `putenv` calls.

`set(String name, String value)` calls `setenv(name, value, 1)`. Setting `value` to `null` unsets the variable. The change is visible to the current process and any subprocess it spawns afterward; parents and previously-spawned subprocesses see the value they were given at spawn time.

---

## `System.property` — process-scoped string properties

A process-global key→value store of strings, analogous to Java's `System.getProperty` / `setProperty`. Properties are populated at program startup from CLI args of the form `-Dkey=value` (see below), and freely settable from cajeta code at runtime.

```cajeta
System.property.set("app.mode", "debug");
String mode = System.property.get("app.mode");    // "debug"

String ver = System.property.get("app.version");  // populated via -Dapp.version=2.0
```

Properties differ from environment variables in three ways:

- **Scope.** Properties are visible only inside the current process; they aren't inherited by spawned subprocesses (env is). Use env when you need parent→child propagation, properties when you want process-local tunables.
- **Source.** Properties are typically set at the CLI (`-Dkey=value`); env typically comes from the parent process. Either side can still set the other at runtime.
- **Naming.** Properties conventionally use dotted lowercase (`app.version`, `cajeta.test.mode`); env conventionally uses uppercase with underscores (`CAJETA_DEBUG`). Neither is enforced.

### `-Dkey=value` at program startup

A binary compiled with `--emit=exe` / `--emit=obj` is launched the same way Java launchers accept `-D` flags:

```
$ ./build/myapp -Dapp.version=2.0 -Dmode=release -Dverbose
```

The C-main shim emitted by the compiler walks `argv` at startup, splits each `-Dkey=value` (or bare `-Dkey`) token at the first `=`, and installs the result into the property map via `__cajeta_property_install`. Bare `-Dflag` installs an empty-string value (testable with `if (System.property.get("flag") != null)`).

Tokens that don't start with `-D` are ignored by the shim — they're meant for the user's entry-point string args once `static int32 main(String[] args)` lands.

### Missing keys

`get(String name)` returns `null` for keys that haven't been set. Callers can branch on `null` directly:

```cajeta
String level = System.property.get("log.level");
if (level == null) {
    level = "info";                               // default
}
```

---

## Implementation notes

- The compiler's `MethodCallExpression` intrinsic dispatcher (`src/cajeta/asn/expression/MethodCallExpression.cpp`) recognizes the `System.<namespace>` shape and lowers each method to a direct runtime call. No `System` class exists in the stdlib — the namespace is purely a syntactic affordance.
- Runtime helpers live in `runtime/native/cajeta_runtime.c`: `__cajeta_env_get` / `__cajeta_env_set` / `__cajeta_property_get` / `__cajeta_property_set` / `__cajeta_property_install` (the last splits `key=value` and installs).
- The property map is a singly-linked list of (`key`, `value`) entries guarded by a `pthread_mutex_t`. Each `set` walks the list to find an existing entry and overwrites the value, or appends a new entry at the head. Sub-O(N) lookup is a future tuning step; today's expected property count is tens, not thousands.
- For JIT-mode tests, the runtime is linked twice (the test binary's native object plus the JIT-loaded bitcode). Each copy has its own static property map, so cross-source visibility doesn't hold in the JIT path. Binary builds (the user-facing case) link the runtime once and see the property map as truly process-global. Tests in `test/expression/SystemEnvPropertyTests.cpp` cover both single-runtime-copy paths (runtime-direct C calls, and cajeta-side `set→get` roundtrips) explicitly.

---

## Diagnostics

| Error ID                              | Condition                                                                 |
| ------------------------------------- | ------------------------------------------------------------------------- |
| `CAJETA_ERROR_UNKNOWN_SYSTEM_STREAM`  | `System.<name>.method(...)` where `<name>` isn't a recognized namespace. Includes a "did you mean..." hint when `--diag-hints` is on. |
| `CAJETA_ERROR_PRINTF_BAD_ARGS`        | `printf` called with a non-`String[]` second argument. Names the actual type and suggests the correct shape or `+` concatenation. |

Both errors surface cleanly through the CLI's top-level try/catch in `src/main.cpp`, landing as `cajeta: <ERROR_ID>: <message>` lines on stderr with exit status 1.

---

## Cross-references

- [`Lang.md`](../Lang.md) — overall stdlib `cajeta.lang` package surface.
- [`Process.md`](../Process.md) — subprocess I/O streams (`ProcessBuilder` + `Process`); unrelated to `System.stdout` but uses the same `Stdio` enum.
- [`io/`](../io/) — file I/O streams; complements stdout/stderr/stdin for non-process-local files.
- [`Annotations.md`](../Annotations.md) — `@SuppressLint` etc.; orthogonal to System but the cajeta-wide annotation surface.
