# Diagnostic location (spec)

Status: approved by Julian 2026-10-07.

## 1. Definition

### 1.1 Purpose

A diagnostic in the compiler's JSON stream (compiler-jsonl, schema 1.1)
locates itself with `file`, `line` and `column`. Today `file` stands for
several different things and the record does not say which:

- a project source file, relative to the source root (`test/Keep.cajeta`);
- a stdlib source, as a pseudo-path (`<stdlib>/cajeta/hash/Sha256.cajeta`);
- an entry inside a dependency archive (`dev.cajeta.unit-0.3.3.cja!dev/...`);
- compiler-generated code, which gets `null` or the template's file with a
  shifted line;
- a subject that is not source at all (a kernel's code object on an arch, a
  `cajeta.json` field, a build step), which gets `null` or a pseudo-path.

A reader has to pattern-match `file` to tell these apart, and a generated
diagnostic cannot say what generated it.

### 1.2 What a location is (decided with Julian 2026-10-07)

1. A location says both where to point the user and what produced the
   diagnostic. `file`, `line` and `column` stay the place to show the user:
   the nearest source text a person can open.
2. Every located diagnostic names its location's kind: `source`, `stdlib`,
   `archive`, `generated` or `artifact`.
3. For generated code, the place to show the user is the declaration that
   asked for the code (the `@Json` record, the template instantiation site,
   the class whose default constructor was generated). The generator, and the
   generator's own source line when it has one, are recorded beside it.
4. A subject that is not source is a location of kind `artifact`, naming the
   subject. It still points at the source declaration when there is one.
5. The change is additive: compiler-jsonl 1.2. `file`, `line` and `column`
   keep their meaning for every existing reader.

### 1.3 Non-goals

- Renaming `file`. `source` is already the producer stamp (schema 1.1, set by
  the build tool, never forgeable by a plugin).
- A v2 stream with a nested location object.
- Locations for `progress`, `result` or other non-diagnostic records.

## 2. Location kind

### 2.1 Requirements

A diagnostic record gains `at`, one of `source`, `stdlib`,
`archive`, `generated`, `artifact`, or `null` when the diagnostic has no
location at all. It is stated, never left for a reader to infer from `file`.

### 2.2 Use cases

- **2.2.1** When a diagnostic points into a project file, `at` is
  `source` and `file` is the path relative to the source root.
- **2.2.2** When it points into the stdlib, `at` is `stdlib` and
  `file` keeps its `<stdlib>/...` form.
- **2.2.3** When it points into a dependency archive, `at` is
  `archive`, `file` keeps its current form, and `archive` names the `.cja`.
- **2.2.4** When a diagnostic has no location, `at`, `file`,
  `line` and `column` are all null.
- **2.2.5** When an old reader ignores the new fields, it sees exactly the
  `file`, `line` and `column` it sees today.

## 3. Generated code

### 3.1 Requirements

A diagnostic inside compiler-generated code has `at: "generated"`,
`file`/`line`/`column` at the requesting declaration, `via` naming what
generated the code, and `from` holding the generator's own source
position when it has one (a template body line, a stdlib generator file).

### 3.2 Use cases

- **3.2.1** When a store error fires inside an instantiation of a stdlib
  template, the user is pointed at the line that instantiated it, and
  `from` holds the template's line.
- **3.2.2** When a synthesized JSON or CSV codec fails to compile, the user
  is pointed at the record declaration that asked for it, and `via`
  names the synthesizer.
- **3.2.3** When a generated default constructor fails a check, the user is
  pointed at the class declaration, and `via` is the default
  constructor.
- **3.2.4** When the requesting declaration cannot be found, `file`, `line`
  and `column` fall back to `from`, and `at` stays
  `generated`.

## 4. Artifacts

### 4.1 Requirements

A diagnostic whose subject is not source has `at: "artifact"` and
an `artifact` object naming it: its kind (`kernel`, `manifest`, `build-step`
at first), its name, and its target when it has one. `file`, `line` and
`column` point at the source declaration of the subject, or are null.

### 4.2 Use cases

- **4.2.1** When the kernel gate refuses a kernel on a backend, the artifact
  names the kernel and the backend and arch, and the location points at the
  kernel's declaration.
- **4.2.2** When a `cajeta.json` field is wrong, the artifact names the key
  path, and `file` is the manifest with its line when known.
- **4.2.3** When a build step fails without a source position, the artifact
  names the step and `file` is null.

## 5. Schema and readers

### 5.1 Requirements

compiler-jsonl moves to 1.2 with `at`, `archive`, `via`,
`from` and `artifact` added as optional fields, and `origin` (emitted
since v0.34.0 but absent from the schema) documented. The IDE plugin's
diagnostic parser and the lint server pass the new fields through, and the IDE
shows a generated diagnostic at its requesting declaration with the generator
named.

### 5.2 Use cases

- **5.2.1** When the IDE receives a generated diagnostic, it marks the
  requesting declaration and says which generator produced the code.
- **5.2.2** When a 1.1 reader reads a 1.2 stream, it keeps working.

## 6. Decisions

- **6.1** (Julian 2026-10-07) Field names: `at` for the location kind, `via`
  for the generator, `from` for the generator's own source position,
  `archive` for the `.cja`, `artifact` for a non-source subject. `kind` was
  not available: it already names the record type.
- **6.2** (Julian 2026-10-07) `via` is a closed list in the schema:
  `template`, `json-synthesizer`, `csv-synthesizer`, `default-constructor`,
  extended by a minor bump. A name outside the list fails a test.
- **6.3** (Julian 2026-10-07) Text diagnostics carry one trailing clause for
  generated code, for example `Model.cajeta:12:5: CAJETA_ERROR_X: ... (in code
  generated by json-synthesizer from <stdlib>/cajeta/codec/Json.cajeta:88)`.
