# cajeta-jinja — spec

Authored 2026-08-14. Extracted from `cajeta-llama` Unit 13, where a
chat-template interpreter was being built privately inside an inference
engine — the wrong home for it.

## 1. Definition

### 1.1 Purpose

A Jinja2-compatible template engine for cajeta: lexer, parser,
interpreter, and the filter/test library, with a value model that is
JSON-shaped. It renders text from a template plus a context.

### 1.2 Where it belongs, and why not the alternatives

**Its own library, `dev.cajeta.jinja`** (repo `cajeta-jinja`).

- **Not `dev.cajeta.codec`.** That library's identity is wire formats —
  protobuf, avro, parquet, thrift, orc: encode a value, decode a value.
  Jinja is not a codec. It is a small programming language with a
  grammar, an AST, an evaluator, scopes, and a builtin library. Filing
  it under `codec` would make that library mean "miscellaneous parsing"
  and would drag a language runtime into the dependency of anything
  that only wanted to read protobuf.
- **Not the standard library.** The stdlib should carry what the
  language and its core libraries need. A template engine is a
  legitimate application-level dependency, and its compatibility target
  moves with upstream Jinja and with template authors — churn that
  should not force stdlib releases.
- **Not `cajeta-llama`.** The engine is one consumer. The obvious second
  is `cajeta-http`: server-side HTML rendering is Jinja's home turf, and
  it is the consumer that makes autoescaping load-bearing (§4).

Independent versioning is the deciding argument: template compatibility
is a moving target driven by other people's artifacts, and it should be
able to move at its own pace.

### 1.3 Scope

The Jinja2 language as actually used by two consumer classes: chat
templates shipped by model vendors, and server-side HTML rendering.
That is a large subset, not the whole of Jinja — see non-goals.

### 1.4 Non-goals

- Sandboxing as a security boundary. `transformers` uses Jinja's
  `ImmutableSandboxedEnvironment`; this engine has no arbitrary-attribute
  access to sandbox in the first place, and it will not claim to be safe
  against hostile templates.
- Python expression semantics beyond the documented subset — no
  arbitrary method dispatch onto host objects, no imports.
- Template compilation to cajeta source. Interpretation is the model;
  revisit only against a measurement.
- Async rendering, `{% trans %}`/i18n extensions.

### 1.5 Constraints observed while prototyping

- **ANTLR4 has no cajeta target**, and there is no cajeta-side ANTLR
  runtime. The compiler front end is ANTLR-generated **C++**; every
  parser written *in* cajeta is hand-written recursive descent (stdlib
  `JsonReader`, `dev.cajeta.codec`'s `ProtobufCursor`,
  `dev.cajeta.docs`'s Html/Markdown readers). A `.g4` grammar is
  therefore the normative CONTRACT, and the parser implements it by
  hand. Writing an ANTLR cajeta target is a separate project of its own
  (a target is a runtime plus StringTemplate codegen), not a unit here.
- Whitespace semantics must be configurable and exact: `transformers`
  renders with `trim_blocks=True, lstrip_blocks=True`, while Jinja's own
  default for both is `False`. A default-mismatched engine produces
  byte-wrong output with no error.

## 2. Language surface

Use cases are stated against Jinja2 semantics; "matches Jinja" means
byte-identical output for the same template and context.

- **2.1** When a template contains `{{ expr }}`, the expression is
  evaluated and its `str()` form emitted.
- **2.2** When a template uses `{% if %}` / `{% elif %}` / `{% else %}` /
  `{% endif %}`, branches render per Jinja truthiness.
- **2.3** When a template uses `{% for x in seq %}` with `{% else %}`,
  iteration and the empty-sequence arm match Jinja, including tuple
  unpacking (`for k, v in mapping.items()`).
- **2.4** When a loop body reads `loop`, the attributes `index`,
  `index0`, `revindex`, `revindex0`, `first`, `last`, `length`,
  `previtem`, `nextitem`, and `cycle()` are available. (`previtem` and
  `nextitem` are not exotic: shipped chat templates branch on them.)
- **2.5** When a template uses `{% set %}`, both the inline form and the
  block form (`{% set x %}…{% endset %}`) bind.
- **2.6** When a template declares `{% macro %}` / `{% call %}`, macros
  render with positional, keyword, and defaulted parameters.
- **2.7** When a template uses `{% with %}`, a nested scope is created.
- **2.8** When a template uses `{% filter %}`, the block's output passes
  through the named filter.
- **2.9** When a template uses `{% raw %}`, its contents are emitted
  verbatim.
- **2.10** When a template uses `namespace()`, attribute assignment on
  the namespace persists across loop iterations — the standard
  workaround for Jinja's loop scoping.
- **2.11** When a template uses `{% break %}` / `{% continue %}`, they
  behave as the `loopcontrols` extension (which `transformers` enables).
- **2.12** When a template uses `{% extends %}`, `{% block %}`, or
  `{% include %}`, inheritance and inclusion resolve through a
  caller-supplied loader.
- **2.13** When whitespace control is spelled (`{%-`, `-%}`, `{{-`,
  `-}}`), adjacent whitespace including newlines is stripped, and this
  overrides the block-trimming settings.
- **2.14** When `trim_blocks` is enabled, the first newline after a
  block tag is removed; when `lstrip_blocks` is enabled, horizontal
  whitespace from line start to a block tag is removed. Both are
  configurable and both default to Jinja's defaults (off), with the
  consumer choosing.
- **2.15** When an expression uses `~`, `+`, `-`, `*`, `/`, `//`, `%`,
  comparisons, `and`/`or`/`not`, `in`/`not in`, or the inline
  conditional, results match Jinja.
- **2.16** When an expression indexes, slices, calls a method, or reads
  an attribute, results match Jinja, including negative indices.
- **2.17** When a filter is applied, the builtin set matches Jinja for:
  `abs`, `attr`, `batch`, `capitalize`, `default`/`d`, `dictsort`,
  `escape`/`e`, `first`, `float`, `format`, `groupby`, `indent`, `int`,
  `items`, `join`, `last`, `length`/`count`, `list`, `lower`, `map`,
  `max`, `min`, `reject`, `rejectattr`, `replace`, `reverse`, `round`,
  `safe`, `select`, `selectattr`, `sort`, `string`, `striptags`, `sum`,
  `title`, `tojson`, `trim`, `unique`, `upper`, `wordcount`.
- **2.18** When a test is applied (`is …`), the builtin set matches
  Jinja for: `defined`, `undefined`, `none`, `boolean`, `false`, `true`,
  `integer`, `float`, `number`, `string`, `mapping`, `sequence`,
  `iterable`, `callable`, `sameas`, `in`, `eq`/`ne`/`lt`/`gt`/`le`/`ge`,
  `divisibleby`, `odd`, `even`, `upper`, `lower`.
- **2.19** When a template calls `range()`, `dict()`, `lipsum()`,
  `cycler()`, or `joiner()`, they behave as Jinja's globals.
- **2.20** When a template calls `raise_exception(msg)`, rendering fails
  with an error carrying the message — chat templates rely on it to
  reject malformed conversations.
- **2.21** When a template calls `strftime_now(fmt)`, the clock is
  injectable so rendering is deterministic under test.
- **2.22** When a template uses a construct outside the supported set,
  the error names the construct and the line, rather than rendering
  something subtly wrong.
- **2.23** When `undefined` is read, the configured undefined behaviour
  applies (default: render empty, `is defined` false; strict mode:
  error) — and `undefined` is distinct from `none`.

## 3. Value model

- **3.1** When a context is supplied, it is `JsonValue`-shaped —
  `dev.cajeta.codec.json`'s value type, which is what both consumers
  already hold.
- **3.2** When a value is stored in a scope, the engine owns it; storing
  a borrow of a caller temporary is a defect
  (`stdlib-ownership-convention`).
- **3.3** When `tojson` serializes, output matches Python's
  `json.dumps` separators as Jinja produces them, including `\uXXXX`
  escaping of control bytes.

## 4. Autoescaping

- **4.1** When autoescaping is enabled, `{{ }}` output is HTML-escaped
  and values marked `safe` are not.
- **4.2** When autoescaping is disabled (the chat-template case), no
  escaping occurs and `safe` is an identity filter.
- **4.3** Autoescaping is a per-environment setting chosen by the
  consumer; it is never inferred.

## 5. Surface

- **5.1** An `Environment` carries the settings (whitespace, undefined
  behaviour, autoescape, loader, clock) and compiles templates.
- **5.2** A compiled `Template` renders repeatedly against different
  contexts without reparsing.
- **5.3** A parse error reports line and column; a render error reports
  the template line.

## 6. Correctness gates

- **6.1** When the fixture corpus renders, output matches Jinja2 byte
  for byte. The reference is generated by running Jinja2 itself, not by
  hand-writing expectations.
- **6.2** The corpus includes the four `cajeta-llama` target families'
  chat templates and a tool-call template, since those are real shipped
  artifacts that exercise filters, tests, macros, and whitespace
  control together.
- **6.3** The corpus includes HTML-rendering cases with autoescaping on,
  including inheritance and includes.
- **6.4** An unsupported construct fails loudly, and the test asserts on
  the message naming construct and line.

## 7. Open questions

- **7.1** Does `{% extends %}` land in v1 or wait for the first real
  consumer? Recommendation: specify it now (2.12), implement when
  `cajeta-http` asks, so the AST does not need reshaping later.
- **7.2** Should the parser be generated from `ChatTemplate.g4`'s
  successor by a future ANTLR cajeta target? Recommendation: keep the
  `.g4` as the normative grammar and the hand-written parser as the
  implementation; revisit only if a cajeta target is ever built for
  other reasons.
