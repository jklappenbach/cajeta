# parse-error-locality — a syntax error damages its statement, not the file

Status: active — approved 2026-09-15 (unit 3, position mapping, in scope per §6.1)
Related: `ide-symbol-index-spec.md` (PSI, xref shards, W4b/W4c recovery),
`xref-lint-emission-gap-spec.md` (6.3.1 blocked on editing resilience),
`compiler-lint-mode-spec.md`, `json-diagnostics-spec.md`.

## 1. Definition

### 1.1 Purpose
While a buffer is mid-edit and does not parse, everything outside the broken
construct keeps working in the IDE: Ctrl-click, usages, hierarchy, structure.
On the compiler side, a syntax error is reported at the token where the parse
failed, not against the whole file.

### 1.2 The problem — measured 2026-09-15
Julian's report: "we lose the ability to navigate when I'm editing code and
even a semi-colon is missing, let alone a closing quote or param."

A 15-line class with a field and two methods, one break per buffer, parsed by
the plugin's generated parser with its anchor-sync `CajetaErrorStrategy`:

| buffer | break                                  | tokens consumed | nodes in the tree |
|--------|----------------------------------------|-----------------|-------------------|
| valid  | none                                   | 74 / 74         | 1 class, 2 methods, 1 field, 2 locals, 9 identifiers |
| Broken | `;` missing after `int32 b = 2`        | 0 / 64          | none |
| Paren  | `int32 b = (2;`                        | 0 / 64          | none |
| Quote  | `String s = "abc;`                     | 0 / 62          | none |
| Brace  | `}` missing after `first()`            | 0 / 46          | none |
| Two    | two classes, `;` missing in the first  | 0 / 33          | none |

Every broken buffer produces one error, in rule `compilationUnit`, with the
prediction start at 1:0. Not even `package demo;` survives. The compiler
reports the same event as one JSON diagnostic at the failing token whose
message quotes the file from its first line:

```
"message":"no viable alternative at input 'package demo;\n\npublic final class Broken {\n ... return'","line":13,"column":9
```

The semantic control: an unknown type and an undefined variable in the same
method keep all five declarations, because the file still parses. This is a
syntax-only failure.

### 1.3 Root cause — the top-level decision needs the whole file
`antlr4/CajetaParser.g4`:

```
compilationUnit
    : packageDeclaration? importDeclaration* typeDeclaration* EOF
    | packageDeclaration? importDeclaration* scriptMember+ EOF
    ;
scriptMember
    : typeDeclaration
    | modifier* methodDeclaration
    | blockStatement
    ;
```

Both alternatives share the prefix, and `scriptMember` includes
`typeDeclaration`, so a file made of type declarations matches both. ANTLR's
adaptive prediction can only separate them at EOF. Measured with
`LL_EXACT_AMBIG_DETECTION` on the valid buffer:

```
ambiguity rule=compilationUnit alts={1, 2} span=1:0→16:0 (75 of 75 tokens)
```

Two consequences follow:

- 1.3.1 Every parse of every file runs prediction over the entire token
  stream before consuming a single token. This is paid by the compiler on
  every unit (stdlib prime included) and by the IDE on every keystroke.
- 1.3.2 A syntax error anywhere kills both alternatives inside that
  prediction. The error is raised in `compilationUnit`, before any child rule
  was entered, so there is no tree to recover into. Recovery (`recover()`
  consuming to an anchor) runs after the top rule has already failed; the
  plugin's anchor set cannot help, and the compiler's LL retry with
  `DefaultErrorStrategy` lands in the same place.

There is a second layer. Left-factoring `compilationUnit` alone moves the
same ambiguity to `scriptMember`, because `blockStatement` reaches
`localTypeDeclaration` and so also matches a class:

```
ambiguity rule=scriptMember alts={1, 3} span=3:0→15:0 (68 of 73 tokens)
```

Under that variant a break in a method keeps all 9 identifiers but loses the
class node (the class body reparses as an aggregate initializer). Both layers
must go.

### 1.4 What is not the problem
- 1.4.1 **The xref index.** A broken buffer's lint emits a version-only xref
  stream (0 records). `CajetaXrefShards.ingestStream` returns before
  replacing anything (`CajetaXrefShards.kt:110`), so the on-disk shard from
  the last good lint survives the edit. Nothing about index retention needs
  to change.
- 1.4.2 **The compiler's abort-before-visit gate.** `parseSource` throws
  `SyntaxErrorException` when the parse reported any error, because the
  semantic visitor is not safe on recovery trees. That stays.
- 1.4.3 **The lexer.** The `ERRCHAR` catch-all (2026-09-15) keeps the token
  stream contiguous for an unterminated string; without it the IDE lexer
  threw. It does nothing for the parser.

### 1.5 Constraints
- 1.5.1 **One grammar.** `antlr4/CajetaParser.g4` is the source; the plugin
  copies it at build time (`ide-plugins/idea/build.gradle.kts:100`). The
  compiler and the IDE parse identically by construction, and the fix is a
  grammar change, not a plugin-side strategy.
- 1.5.2 **The language does not change.** Every file that parses today
  parses to the same tree under the same rule names, script units included.
  The one structural difference is the `scriptMember` wrapper every file-scope
  member now carries; parity is measured with that wrapper elided.
  Script-unit detection gives the same answer for every unit.
- 1.5.3 **No target-specific actions.** The grammar generates C++ and Java
  from one file; nothing in it may be language-specific.
- 1.5.4 **Wrong > missing.** A Ctrl-click that lands on the wrong element is
  worse than one that does nothing. The declaration-side validation in
  `CajetaXrefReference` (the named element at the recorded position must
  carry the expected name) stays.
- 1.5.5 **No error alternatives in rules.** Once each decision is local,
  ANTLR's default recovery plus the plugin's anchor set localises every
  measured break (§2). Hand-written error productions are not needed and
  would be maintenance without a measured payoff.

### 1.6 Non-goals
- 1.6.1 Emitting xref records for the parts of a broken file that parsed. The
  retained shard covers the edit; the visitor stays off recovery trees.
- 1.6.2 Incremental reparse of the buffer. IntelliJ reparses on edit; the
  requirement is that the reparse yields a usable tree.
- 1.6.3 A general error-recovery framework. Anchors and telemetry from
  ide-symbol-index W4b stay as they are.

## 2. The top-level decision is local

### 2.1 Requirement
`compilationUnit` has one alternative, and a class at file scope is a
`typeDeclaration` and nothing else:

```
compilationUnit
    : packageDeclaration? importDeclaration* scriptMember* EOF
    ;
scriptMember
    : typeDeclaration
    | modifier* methodDeclaration
    | localVariableDeclaration ';'
    | statement
    ;
```

`localTypeDeclaration` stays reachable inside blocks; it is no longer reachable
from file scope, where it duplicated `typeDeclaration`. A unit is a script unit
when any member is not a `typeDeclaration` (a bare `;` is a `typeDeclaration`).
That decision moves from "which alternative matched" to a property of the
member list, computed by the compiler.

### 2.2 Use cases — measured on the §1.2 buffers with the §2.1 grammar
- 2.2.1 When the only syntax error is a missing `;` inside a method, the
  parse reports one error, `missing ';' at 'return'` at the `return` token,
  and every other construct parses to its normal rule. Measured: 1 class, 2
  methods, 1 field, 2 locals, 9 of 9 identifiers, 1 error node.
- 2.2.2 When the break is an unclosed `(` or an unterminated string literal,
  the outcome is the same as 2.2.1: one error at the break, everything else
  intact.
- 2.2.3 When a method's closing `}` is missing, the following method is
  absorbed into the open body. The class, the field, the surviving method and
  all 9 identifiers still parse; three errors are reported, the first at the
  point the parser noticed. Nothing above the break is touched.
- 2.2.4 When a file holds two classes and the first is broken, the second
  parses whole. Measured: 2 classes, 2 methods, 9 of 9 identifiers, 1 error.
- 2.2.5 When a file is valid, its tree is identical to today's: the same rule
  for every construct, the same script-unit answer (an ordinary unit is not a
  script; loose statements are; `break;` alone parses and is rejected
  semantically; the empty unit is legal).
- 2.2.6 When a valid file is parsed under exact-ambiguity detection, no
  decision in `compilationUnit` or `scriptMember` is reported ambiguous.
  Measured: none.
- 2.2.7 When the compiler reports a syntax error, the message names the
  failing token and its expectation. No syntax diagnostic quotes the file
  from its first token.
- 2.2.8 When any prefix of a valid file is parsed (the typing simulator), the
  parser never throws, and the complete file parses clean.

## 3. The compiler reads the member list

### 3.1 Requirement
The compiler stops reading `compilationUnit` alternatives. Type declarations
are enumerated through one helper over the member list; script-ness is
"any member that is not a type declaration".

Consumers today: `CajetaLlvmVisitor.h:427` (two loops), `ScriptUnitSynthesis.cpp:51`
(`isScriptUnit`), `ScriptUnitSynthesis.cpp:139`, `TemplateInstantiator.cpp:400`,
`:522`, `:875`, `Compiler.cpp:703`.

### 3.2 Use cases
- 3.2.1 When a unit holds only type declarations and stray `;`, it is not a
  script unit and its types are registered exactly as today.
- 3.2.2 When a unit holds a statement, a local variable or a top-level method,
  it is a script unit and synthesis proceeds as today.
- 3.2.3 When a template is instantiated from an archive or a source unit, the
  same type declarations are found as today.
- 3.2.4 When a unit has a syntax error, no xref records are emitted for it
  and lint reports the error (§1.4.2 unchanged).
- 3.2.5 When the stdlib, `samples/tour` and the test fixtures are compiled,
  their xref exports are byte-identical to the exports before the change.

## 4. The IDE keeps navigating during an edit

### 4.1 Requirement
With §2 in place the PSI keeps its identifier nodes outside the break, and the
shard from the last good lint is still on disk. Resolution matches a use by
exact `(file, line, col)` (`CajetaXrefReference.kt:53`), so it keeps working
for every use whose position did not move. An edit that inserts or removes a
line shifts every use below it; those positions must follow the document
until the next successful lint replaces the shard.

### 4.2 Use cases
- 4.2.1 When a method in the open buffer is broken, Ctrl-click on an
  identifier in another method resolves to the same target as before the
  edit.
- 4.2.2 When the break is above the use (a line was inserted or deleted while
  the buffer does not parse), the use still resolves; its recorded position is
  mapped through the document's edits.
- 4.2.3 When the buffer parses again and the lint succeeds, the new shard
  replaces the mapped positions, and resolution is exact again.
- 4.2.4 When a mapped position no longer holds an element with the recorded
  name, resolution reports nothing (§1.5.4).
- 4.2.5 When the buffer is broken, usages, hierarchy and structure keep
  showing the declarations outside the break; they key on rule indices and
  need no change beyond the grammar.
- 4.2.6 When an unterminated string is typed, the editor highlighter does not
  throw (`ERRCHAR`, already landed) and the file's other identifiers stay
  clickable.

## 5. Acceptance
- 5.1 The §1.2 table, re-measured after the change, shows every broken buffer
  keeping every declaration outside the break, with errors localised as in
  §2.2.
- 5.2 Corpus parity: every `.cajeta` file under the stdlib, `samples/` and
  `test/` parses to the same rule-name sequence (the `scriptMember` wrapper
  elided) and the same syntax-error count under the new grammar as under the
  old one.
- 5.3 `samples/tour` xref export byte-identical before and after.
- 5.4 Compiler parse time on the largest stdlib unit does not regress. The
  whole-file prediction pass is gone, so a reduction is expected; report the
  measurement either way.
- 5.5 Live in the IDE: with a `;` removed inside a method of
  `ServerTests.cajeta`, Ctrl-click on a symbol in another method still lands.
- 5.6 `xref-lint-emission-gap` 6.3.1 (the unsaved-buffer local, 7.3.2)
  passes live and that plan closes.

## 6. Open questions
- 6.1 **Is §4 (position mapping through edits) in scope now?** Without it,
  the retained index serves every use above the edit and every use on an
  unchanged line, and fails for uses below an inserted or deleted line until
  the next clean lint. Recommendation: in scope, as its own unit. It is
  plugin-only, independent of the grammar, and it is what makes "outside the
  line being edited" true for the whole file rather than the top half.
- 6.2 **Should the compiler keep the SLL-first two-stage parse?** It is
  unaffected by this change and stays. Noted because §5.4 will show whether
  the SLL stage was paying the whole-file pass too.
