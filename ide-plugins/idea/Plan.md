# Cajeta IntelliJ IDEA Plugin — Implementation Plan

> ## Status: v0.1 COMPLETE (shipped)
>
> All 11 implementation steps below are implemented and wired (verified
> against `src/main/kotlin/dev/cajeta/idea/**` and
> `resources/META-INF/plugin.xml`). Two intentional caveats carry forward:
> - **Linting** ships in **degraded mode** (regex over `cajetac` stderr) —
>   it upgrades to structured diagnostics when the compiler grows
>   `cajeta dap`/`cajeta lsp` (see Phase 2 + `docs/Debugging.md`).
> - **Markdown comments**: caret-following render/fold + a global Settings
>   toggle shipped; the per-comment **gutter-icon toggle** and a
>   menu **toggle-action** are deferred (cosmetic).
>
> **Phase 2 — Debugging: landed through CP7-6** (breakpoints incl.
> conditional/exception, launch-in-debug, stack/variables view+edit,
> threads/fibers, ownership/allocation/lifetime visualization, drop
> breakpoints), built on the `cajeta dap` Debug Adapter Protocol server per
> `docs/Debugging.md`. One open tail: **CP6f-2d** hard carrier-quiesce. See the
> [Active worklist](#active-worklist-session-2026-06-24) for this session's scope.

## Active worklist (session 2026-06-24)

Phase 2 has, in fact, landed through **CP7-6** on `main` (verified against the
FR-1 checkpoint table + git log); this status block and the README lag the code.
This session reconciles that and clears the remaining tail. Tracked on the
implement skill's focus stack (`agents/idea-focus.md`).

- [x] **W1 — Verify build + tests green.** `./gradlew test` → 87 tests, 86
  passed, 0 failed, 1 skipped (build compiles clean). Root-caused + fixed one
  flake: `DapClientIntegrationTest`'s `configurationDone` future capped at 10s,
  but the server JIT-compiles + runs the program synchronously from that request
  (first cold JIT of the 277 MB binary > 10s on a loaded box) — raised to 30s
  with a comment. The production path (`CajetaDebugSession`) already had headroom.
- [x] **W2 — Reconcile docs to landed reality.** Status header now says "Phase 2
  landed through CP7-6"; Part C marks CP6a–e, CP6f-1/2a/2b/2c/3 and CP7-1a…CP7-6
  DONE, with CP6f-2d flagged as the lone OPEN tail; branch note corrected
  (`idea-ide` → merged to `main`); README layout diagram gains `debugger/`,
  `harness/`, `wizard/`. (Debugging-tier *acceptance checkboxes* left unticked —
  ticking them needs a real `runIde` walkthrough, not a doc edit.)
- [ ] **W3 — Finish loose threads.** Decomposed:
  - [ ] **W3a — CP6f-2d hard carrier-quiesce.** C++ JIT-debug runtime: guarantee
    the entry thread and live fibers are fully quiesced for the
    entry-thread-vs-live-fibers edge before enumeration. TDD against
    `cajeta_debug_test`. Highest risk (concurrency).
  - [x] **W3b — "Toggle Markdown Rendering" menu action.** Added
    `ToggleMarkdownRenderingAction` (a `ToggleAction` whose checked state mirrors
    `renderMarkdownInComments`) under the Cajeta tools group; flipping it applies
    to all open editors via new `MarkdownFoldEditorListener.applyRenderingState()`
    / `uninstall()` (teardown extracted from `refresh()`). Decision core
    `MarkdownRenderingToggle` (plain JVM) unit-tested (3 tests, green).
  - [x] **W3c — Per-comment gutter-icon toggle — CUT (not built).** Dropped by
    developer decision 2026-06-24: per-comment markdown gutter toggles are
    confusing and unwanted. The global Settings checkbox + the W3b menu action +
    existing click-to-expand cover the real needs. Removed from scope (not a
    deferral).
- [ ] **W4 — v0.2 candidates.** `MarkdownEngine` extension point, error-recovery
  telemetry, typing-simulator test harness. (`TODO(codegen-keywords)` is
  already implemented via the `generateTokenCategories` Gradle task — confirm
  + strike from the v0.2 list.)

End-to-end plan to go from an empty directory to a Cajeta language
plugin installed and running in IntelliJ IDEA. Scope of this document
is the **syntax tier** (lexer, parser, highlighting, structure view,
brace matching), **compiler-driven linting** (real-time squigglies
from `cajetac --lint --json`, including wildcard / capture-conversion
warnings — see [LintRules.md](../../docs/LintRules.md) and
[CaptureConversion.md](../../docs/CaptureConversion.md)), and
**Obsidian-style markdown rendering in comments** (rendered markdown
on every comment line *except* the one the caret is on, which reverts
to raw source for editing), plus the build, run, and install loop.
Semantic-resolution features (go-to-def, find-usages, rename,
type-aware completion) are listed in
[Out of scope for v0.1](#out-of-scope-for-v01) and deferred to a
follow-up plan.

The plugin reuses `antlr4/CajetaParser.g4` and `antlr4/CajetaLexer.g4`
via the `antlr4-intellij-adaptor` rather than re-expressing the grammar
in JetBrains' Grammar-Kit BNF. This means grammar changes in the
compiler propagate to the IDE without a parallel maintenance burden.

---

## Table of contents

1. [Goals](#goals)
2. [Non-goals](#non-goals)
3. [Prerequisites](#prerequisites)
4. [Architecture at a glance](#architecture-at-a-glance)
5. [Project layout](#project-layout)
6. [Implementation sequence](#implementation-sequence)
   1. [Step 1 — Bootstrap from the official template](#step-1--bootstrap-from-the-official-template)
   2. [Step 2 — Add ANTLR4 dependencies and code generation](#step-2--add-antlr4-dependencies-and-code-generation)
   3. [Step 3 — Register the file type and language](#step-3--register-the-file-type-and-language)
   4. [Step 4 — Wire the lexer and parser into PSI](#step-4--wire-the-lexer-and-parser-into-psi)
   5. [Step 5 — Syntax highlighting](#step-5--syntax-highlighting)
   6. [Step 6 — Brace matching, commenter, structure view](#step-6--brace-matching-commenter-structure-view)
   7. [Step 7 — Compiler-driven linting via ExternalAnnotator](#step-7--compiler-driven-linting-via-externalannotator)
   8. [Step 8 — Markdown rendering in comments (Obsidian-style)](#step-8--markdown-rendering-in-comments-obsidian-style)
   9. [Step 9 — Run in a sandbox IDE](#step-9--run-in-a-sandbox-ide)
   10. [Step 10 — Package as a distributable .zip](#step-10--package-as-a-distributable-zip)
   11. [Step 11 — Install into a real IntelliJ](#step-11--install-into-a-real-intellij)
7. [Verification checklist](#verification-checklist)
8. [Phase 2 — Debugging](#phase-2--debugging)
9. [Out of scope for v0.1](#out-of-scope-for-v01)
10. [Resolved design decisions](#resolved-design-decisions)
11. [Future work — v0.2 candidates](#future-work--v02-candidates)

---

## Goals

- A pluggable, installable `.zip` that registers `.cajeta` as a
  first-class file type in IntelliJ IDEA 2024.2+ (Community and
  Ultimate).
- Lexer-based syntax highlighting driven by the existing ANTLR4
  lexer — no second grammar.
- Parse tree visible in IntelliJ's PSI Viewer, with structure-view
  entries for packages, classes, methods, and fields.
- Brace matching, line/block commenter, file icon.
- **Real-time linting.** Syntax errors (from the ANTLR error
  listener) and Cajeta lint warnings (from `cajetac --lint --json`)
  appear as squigglies in the editor and entries in the Problems
  tool window, debounced on edit, within ~300 ms of typing pause.
  Includes wildcard / capture-conversion misuse — e.g. the
  `Box<? extends Animal>` read-back patterns from
  [CaptureConversion.md](../../docs/CaptureConversion.md)
  and rule IDs like `uncaught-throws` from
  [LintRules.md](../../docs/LintRules.md).
- **Markdown-rendered comments (Obsidian live-preview model).**
  Comments in Cajeta source are interpreted as markdown and
  rendered in place (headings, emphasis, lists, links, fenced code
  blocks). When the caret is on a line inside a comment, *that
  comment* reverts to raw source for editing; moving the caret
  away re-renders. Toggle via gutter icon, action, or per-comment.
- `./gradlew runIde` brings up a sandbox IntelliJ with the plugin
  loaded, suitable for daily dev iteration.

## Non-goals

- No type-aware completion, go-to-definition, find-usages, rename, or
  refactoring. These belong to the semantic tier and require either
  reimplementing Cajeta's type system in Kotlin or fronting the
  compiler with an LSP server. Tracked separately.
- No debugger integration.
- No build-tool integration (`cajeta build` from the gutter). Deferred.
- No formatter beyond what comes free from brace matching.
- No support for IntelliJ versions older than 2024.2 — the Platform
  SDK has churned enough that supporting older builds is its own
  project.
- **Linting is read-only display in v0.1** — no quick-fixes, no
  one-click suppression-annotation insertion. Surfacing the warning
  text and ID is enough; the user adds `@SuppressLint("…")` by hand.
- **Markdown rendering is display-only in v0.1** — no WYSIWYG
  editing, no clickable links in rendered mode, no embedded image
  loading from remote URLs (local file paths only). Render to HTML
  via the IntelliJ-bundled `org.jetbrains:markdown` library; don't
  ship a second markdown parser.

## Prerequisites

Install once on the dev machine:

```sh
sudo apt install openjdk-21-jdk
```

Gradle is wrappered (`./gradlew`); no system install needed. IntelliJ
IDEA Community 2024.2+ is what you install the built plugin into. The
sandbox IDE that `runIde` launches is downloaded by Gradle on first
run; you do **not** need to pre-install anything for development.

Verify:

```sh
java -version    # expect 21.x
```

## Architecture at a glance

```
┌─────────────────────────┐
│  antlr4/CajetaLexer.g4  │  (single source of truth)
│  antlr4/CajetaParser.g4 │
└───────────┬─────────────┘
            │  generated by ANTLR Gradle plugin
            ▼
┌─────────────────────────┐
│  Generated Java         │
│  CajetaLexer.java       │
│  CajetaParser.java      │
└───────────┬─────────────┘
            │  wrapped by antlr4-intellij-adaptor
            ▼
┌─────────────────────────┐
│  PSI tree               │  ← IntelliJ consumes this for
│  CajetaParserDefinition │     highlighting, structure view,
│  CajetaTokenTypes       │     navigation, etc.
│  CajetaElementTypes     │
└─────────────────────────┘
```

The adaptor converts ANTLR `Token` → IntelliJ `IElementType` and
ANTLR `ParseTree` → IntelliJ `ASTNode`. We do not hand-write a
second parser.

## Project layout

```
ide-plugins/idea/
├── Plan.md                          (this file)
├── build.gradle.kts
├── settings.gradle.kts
├── gradle.properties
├── gradle/
│   └── wrapper/
├── gradlew
├── gradlew.bat
└── src/main/
    ├── kotlin/dev/cajeta/idea/
    │   ├── CajetaLanguage.kt
    │   ├── CajetaFileType.kt
    │   ├── CajetaIcons.kt
    │   ├── parser/
    │   │   ├── CajetaParserDefinition.kt
    │   │   ├── CajetaTokenTypes.kt
    │   │   ├── CajetaElementTypes.kt
    │   │   └── CajetaErrorStrategy.kt
    │   ├── highlighting/
    │   │   ├── CajetaSyntaxHighlighter.kt
    │   │   └── CajetaSyntaxHighlighterFactory.kt
    │   ├── editor/
    │   │   ├── CajetaBraceMatcher.kt
    │   │   ├── CajetaCommenter.kt
    │   │   └── CajetaStructureViewFactory.kt
    │   ├── lint/
    │   │   ├── CajetaLintAnnotator.kt
    │   │   ├── CajetacRunner.kt
    │   │   └── Diagnostic.kt
    │   └── markdown/
    │       ├── CajetaCommentFoldingBuilder.kt
    │       ├── CajetaCommentRenderer.kt
    │       ├── CommentCaretListener.kt
    │       ├── MarkdownEngine.kt           (interface)
    │       ├── MarkdownEngineRegistry.kt   (app-level service)
    │       └── engines/
    │           └── JetBrainsMarkdownEngine.kt
    └── resources/
        ├── META-INF/
        │   ├── plugin.xml
        │   └── pluginIcon.svg
        └── icons/
            └── cajetaFile.svg
```

Cajeta source the plugin consumes lives in `../../antlr4/` — the
Gradle build copies the two `.g4` files into the plugin's
`build/generated` and runs ANTLR over them.

## Implementation sequence

### Step 1 — Bootstrap from the official template

JetBrains maintains
[`intellij-platform-plugin-template`](https://github.com/JetBrains/intellij-platform-plugin-template).
Use it as the starting point, not a vanilla Gradle project — it ships
opinionated defaults for the IntelliJ Platform Gradle Plugin 2.x,
qodana, changelog automation, and CI.

```sh
cd ide-plugins/idea
# clone the template into a temp dir, then copy in just the files we need
git clone --depth 1 https://github.com/JetBrains/intellij-platform-plugin-template /tmp/iplt
cp -r /tmp/iplt/{build.gradle.kts,settings.gradle.kts,gradle.properties,gradle,gradlew,gradlew.bat,.gitignore} .
cp -r /tmp/iplt/src .
rm -rf /tmp/iplt
```

Edit `gradle.properties`:

```
pluginGroup = dev.cajeta.idea
pluginName = Cajeta
pluginVersion = 0.1.0
pluginSinceBuild = 242
pluginUntilBuild = 251.*
platformType = IC
platformVersion = 2024.2
```

Smoke test before adding anything Cajeta-specific:

```sh
./gradlew runIde
```

A sandbox IDE should launch with the template's "hello-world"
plugin loaded. Close it.

### Step 2 — Add ANTLR4 dependencies and code generation

Add to `build.gradle.kts`:

```kotlin
plugins {
    id("antlr")
    // …existing plugins…
}

dependencies {
    antlr("org.antlr:antlr4:4.13.2")
    implementation("org.antlr:antlr4-runtime:4.13.2")
    implementation("org.antlr:antlr4-intellij-adaptor:0.1")
}

// Point ANTLR at the compiler's grammar files
val grammarDir = file("../../antlr4")

tasks.generateGrammarSource {
    arguments = arguments + listOf(
        "-package", "dev.cajeta.idea.parser.antlr",
        "-visitor",
        "-long-messages"
    )
    source = fileTree(grammarDir) { include("*.g4") }
    outputDirectory = file("build/generated-src/antlr/main/dev/cajeta/idea/parser/antlr")
}

tasks.compileKotlin {
    dependsOn(tasks.generateGrammarSource)
}
```

Run:

```sh
./gradlew generateGrammarSource
ls build/generated-src/antlr/main/dev/cajeta/idea/parser/antlr
# expect CajetaLexer.java, CajetaParser.java, *Visitor.java
```

If this fails, the grammar files reference each other or import a
shared file in a way the standalone task can't resolve. Fix by
copying both `.g4` into a temp dir first or by passing `-lib`
pointing at `grammarDir`.

### Step 3 — Register the file type and language

`CajetaLanguage.kt`:

```kotlin
package dev.cajeta.idea

import com.intellij.lang.Language

object CajetaLanguage : Language("Cajeta")
```

`CajetaFileType.kt`:

```kotlin
package dev.cajeta.idea

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

object CajetaFileType : LanguageFileType(CajetaLanguage) {
    override fun getName() = "Cajeta"
    override fun getDescription() = "Cajeta source file"
    override fun getDefaultExtension() = "cajeta"
    override fun getIcon(): Icon = CajetaIcons.FILE
}
```

Register in `src/main/resources/META-INF/plugin.xml`:

```xml
<extensions defaultExtensionNs="com.intellij">
    <fileType
        name="Cajeta"
        implementationClass="dev.cajeta.idea.CajetaFileType"
        fieldName="INSTANCE"
        language="Cajeta"
        extensions="cajeta"/>
</extensions>
```

After this step `.cajeta` files show up with the file icon but
content is still plain text.

### Step 4 — Wire the lexer and parser into PSI

The adaptor needs three things: a token-type registry, an
element-type registry, and a `ParserDefinition` that ties the
generated lexer/parser to PSI nodes.

`CajetaTokenTypes.kt`:

```kotlin
package dev.cajeta.idea.parser

import com.intellij.psi.tree.IElementType
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.antlr.CajetaLexer
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory
import org.antlr.intellij.adaptor.lexer.TokenIElementType

object CajetaTokenTypes {
    init {
        PSIElementTypeFactory.defineLanguageIElementTypes(
            CajetaLanguage,
            CajetaLexer.tokenNames,
            CajetaLexer.ruleNames  // re-used by element types
        )
    }
    val TOKENS: List<TokenIElementType> =
        PSIElementTypeFactory.getTokenIElementTypes(CajetaLanguage)
}
```

`CajetaElementTypes.kt`:

```kotlin
package dev.cajeta.idea.parser

import dev.cajeta.idea.CajetaLanguage
import org.antlr.intellij.adaptor.lexer.PSIElementTypeFactory

object CajetaElementTypes {
    val RULES = PSIElementTypeFactory.getRuleIElementTypes(CajetaLanguage)
}
```

`CajetaParserDefinition.kt`:

```kotlin
package dev.cajeta.idea.parser

import com.intellij.lang.ASTNode
import com.intellij.lang.ParserDefinition
import com.intellij.lang.PsiParser
import com.intellij.lexer.Lexer
import com.intellij.openapi.project.Project
import com.intellij.psi.FileViewProvider
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IFileElementType
import com.intellij.psi.tree.TokenSet
import dev.cajeta.idea.CajetaFileType
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.antlr.CajetaLexer as AntlrCajetaLexer
import dev.cajeta.idea.parser.antlr.CajetaParser as AntlrCajetaParser
import org.antlr.intellij.adaptor.lexer.ANTLRLexerAdaptor
import org.antlr.intellij.adaptor.parser.ANTLRParserAdaptor
import org.antlr.intellij.adaptor.psi.ANTLRPsiNode
import org.antlr.v4.runtime.Parser as Antlr4Parser
import org.antlr.v4.runtime.tree.ParseTree

class CajetaParserDefinition : ParserDefinition {
    companion object {
        val FILE = IFileElementType(CajetaLanguage)
    }

    override fun createLexer(project: Project?): Lexer =
        ANTLRLexerAdaptor(CajetaLanguage, AntlrCajetaLexer(null))

    override fun createParser(project: Project?): PsiParser =
        object : ANTLRParserAdaptor(CajetaLanguage, AntlrCajetaParser(null)) {
            override fun parse(parser: Antlr4Parser, root: com.intellij.psi.tree.IElementType): ParseTree {
                parser.errorHandler = CajetaErrorStrategy()
                return (parser as AntlrCajetaParser).compilationUnit()  // change to the grammar's real start rule
            }
        }

    override fun getFileNodeType() = FILE

    override fun getCommentTokens(): TokenSet {
        // CajetaLexer.g4:232-233 route COMMENT and LINE_COMMENT to
        // channel(HIDDEN), so the adapter sees them as token types
        // even though the parser doesn't. Map both here.
        val byName = CajetaTokenTypes.TOKENS.associateBy { it.toString() }
        return TokenSet.create(
            byName.getValue("COMMENT"),
            byName.getValue("LINE_COMMENT")
        )
    }

    override fun getStringLiteralElements(): TokenSet = TokenSet.EMPTY
    override fun createElement(node: ASTNode): PsiElement = ANTLRPsiNode(node)
    override fun createFile(viewProvider: FileViewProvider): PsiFile = CajetaPsiFile(viewProvider)
}
```

`CajetaPsiFile.kt`:

```kotlin
package dev.cajeta.idea.parser

import com.intellij.extapi.psi.PsiFileBase
import com.intellij.openapi.fileTypes.FileType
import com.intellij.psi.FileViewProvider
import dev.cajeta.idea.CajetaFileType
import dev.cajeta.idea.CajetaLanguage

class CajetaPsiFile(viewProvider: FileViewProvider)
    : PsiFileBase(viewProvider, CajetaLanguage) {
    override fun getFileType(): FileType = CajetaFileType
    override fun toString() = "Cajeta File"
}
```

Register in `plugin.xml`:

```xml
<lang.parserDefinition
    language="Cajeta"
    implementationClass="dev.cajeta.idea.parser.CajetaParserDefinition"/>
```

**Error recovery — `CajetaErrorStrategy`.** ANTLR's
`DefaultErrorStrategy` does single-token insertion/deletion, which
in an IDE context produces noisy partial trees and chatty error
annotations while the user is typing. A custom strategy
synchronizes to known Cajeta-level anchors (statement terminators,
block braces, declaration-starting keywords) so recoveries land on
clean boundaries and the rest of the file still produces a usable
tree:

```kotlin
package dev.cajeta.idea.parser

import dev.cajeta.idea.parser.antlr.CajetaLexer
import org.antlr.v4.runtime.DefaultErrorStrategy
import org.antlr.v4.runtime.Parser
import org.antlr.v4.runtime.misc.IntervalSet

class CajetaErrorStrategy : DefaultErrorStrategy() {

    /** Tokens we treat as safe synchronization anchors. */
    private val anchorTokens: IntervalSet = IntervalSet.of(
        CajetaLexer.SEMI,       // ;
        CajetaLexer.RBRACE,     // }
        CajetaLexer.LBRACE,     // {
        // top-level / member declaration starters
        CajetaLexer.PACKAGE, CajetaLexer.IMPORT,
        CajetaLexer.CLASS, CajetaLexer.INTERFACE,
        CajetaLexer.STRUCT, CajetaLexer.ENUM,
        // visibility / modifiers that begin a member
        CajetaLexer.PUBLIC, CajetaLexer.PRIVATE, CajetaLexer.PROTECTED,
        CajetaLexer.STATIC, CajetaLexer.FINAL, CajetaLexer.ABSTRACT
        // exact constant names come from generated CajetaLexer.java —
        // verify against `tokenNames` and adjust if grammar evolves
    )

    override fun getErrorRecoverySet(recognizer: Parser): IntervalSet =
        super.getErrorRecoverySet(recognizer).or(anchorTokens)
}
```

This is intentionally a thin override — only `getErrorRecoverySet`,
which the default `recover()` / `sync()` already call. Anchor tokens
are unioned with ANTLR's follow-set heuristic rather than replacing
it, so well-formed cases keep behaving exactly as before.

**Maintenance note.** Anchor token IDs reference constants generated
by ANTLR from `CajetaLexer.g4`. Adding or renaming a declaration
keyword in the grammar requires touching this list. Acceptable cost
for the recovery-quality win; documented here so a future grammar
change doesn't silently degrade the recovery quality.

After this step the PSI Viewer (Tools → View PSI Structure of
Current File) shows a real parse tree for `.cajeta` files, and
mid-typing errors recover to the next statement or block boundary
rather than propagating across the rest of the file.

### Step 5 — Syntax highlighting

Lexer-based — runs on every keystroke, must not call into the
parser. Categorize tokens by ANTLR token name and map to
`TextAttributesKey`s. Keywords, types, numbers, strings, comments,
operators, identifiers, annotations.

`CajetaSyntaxHighlighter.kt`:

```kotlin
package dev.cajeta.idea.highlighting

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors as Colors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.psi.tree.IElementType
import dev.cajeta.idea.CajetaLanguage
import dev.cajeta.idea.parser.antlr.CajetaLexer as AntlrCajetaLexer
import org.antlr.intellij.adaptor.lexer.ANTLRLexerAdaptor

class CajetaSyntaxHighlighter : SyntaxHighlighterBase() {

    override fun getHighlightingLexer(): Lexer =
        ANTLRLexerAdaptor(CajetaLanguage, AntlrCajetaLexer(null))

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> =
        when (tokenName(tokenType)) {
            in KEYWORDS    -> pack(Colors.KEYWORD)
            in PRIMITIVES  -> pack(Colors.KEYWORD)
            in OPERATORS   -> pack(Colors.OPERATION_SIGN)
            "STRING_LITERAL"  -> pack(Colors.STRING)
            "INTEGER_LITERAL", "FLOAT_LITERAL" -> pack(Colors.NUMBER)
            "LINE_COMMENT"   -> pack(Colors.LINE_COMMENT)
            "BLOCK_COMMENT"  -> pack(Colors.BLOCK_COMMENT)
            "AT" -> pack(Colors.METADATA)
            "IDENTIFIER" -> pack(Colors.IDENTIFIER)
            else -> emptyArray()
        }

    private fun tokenName(t: IElementType): String =
        // adaptor stores the ANTLR token name in toString()
        t.toString()

    companion object {
        // TODO(codegen-keywords): generate these sets from
        // CajetaLexer.g4 at build time. Hand-maintained for v0.1;
        // see "Future work" in Plan.md.
        private val KEYWORDS = setOf(
            "CLASS","INTERFACE","ENUM","STRUCT","PUBLIC","PRIVATE","PROTECTED",
            "STATIC","FINAL","ABSTRACT","RETURN","IF","ELSE","FOR","WHILE","DO",
            "SWITCH","CASE","BREAK","CONTINUE","NEW","STACK","HEAP","PACKAGE",
            "IMPORT","EXTENDS","IMPLEMENTS","THIS","SUPER","NULL","TRUE","FALSE",
            "ASPECT","ASYNC","SCOPE","SPAWN","VIEW"
            // fill from CajetaLexer.g4
        )
        private val PRIMITIVES = setOf(
            "INT8","INT16","INT32","INT64","UINT8","UINT16","UINT32","UINT64",
            "FLOAT32","FLOAT64","BOOL","CHAR","VOID"
        )
        private val OPERATORS = setOf(
            "PLUS","MINUS","STAR","SLASH","PERCENT","ASSIGN","EQ","NEQ","LT","GT",
            "LE","GE","AND","OR","NOT","BITAND","BITOR","XOR","SHL","SHR"
        )
    }
}
```

The exact token names come from `CajetaLexer.g4` — verify by
opening the generated `CajetaLexer.java` and reading the
`tokenNames` array. Don't trust the names above; treat them as
illustrative.

Register:

```xml
<lang.syntaxHighlighterFactory
    language="Cajeta"
    implementationClass="dev.cajeta.idea.highlighting.CajetaSyntaxHighlighterFactory"/>
```

### Step 6 — Brace matching, commenter, structure view

Three small classes, three `plugin.xml` registrations:

```xml
<lang.braceMatcher
    language="Cajeta"
    implementationClass="dev.cajeta.idea.editor.CajetaBraceMatcher"/>
<lang.commenter
    language="Cajeta"
    implementationClass="dev.cajeta.idea.editor.CajetaCommenter"/>
<lang.psiStructureViewFactory
    language="Cajeta"
    implementationClass="dev.cajeta.idea.editor.CajetaStructureViewFactory"/>
```

`CajetaCommenter` returns `"//"` and `"/*" / "*/"`. `CajetaBraceMatcher`
returns the brace/paren/bracket pairs. `CajetaStructureViewFactory`
walks the PSI tree and surfaces nodes whose rule name is
`packageDeclaration`, `classDeclaration`, `methodDeclaration`,
`fieldDeclaration` (exact rule names from `CajetaParser.g4`).

### Step 7 — Compiler-driven linting via ExternalAnnotator

Two channels of red/yellow squigglies:

- **Syntax errors** — come from the ANTLR adaptor's error listener.
  Free; nothing to wire beyond confirming `BaseErrorListener` is
  attached. Already verified in
  [Step 4](#step-4--wire-the-lexer-and-parser-into-psi).
- **Lint warnings** — come from `cajetac --lint --json`, run on
  every edit (debounced). Surfaced via
  `com.intellij.lang.annotation.ExternalAnnotator`, which is the
  IDE's official extension point for shelling out to a slow,
  out-of-process linter without blocking the EDT.

`ExternalAnnotator` runs in three phases:

1. `collectInformation(file)` — runs on EDT under read lock; cheap.
   Snapshot the file's contents and path; return a payload object.
2. `doAnnotate(payload)` — runs off-EDT, no read lock. Spawn
   `cajetac`, write the snapshot to its stdin, parse the JSON
   diagnostics off its stdout. May take hundreds of ms; that's
   fine here.
3. `apply(file, payload, holder)` — back on EDT under read lock.
   Walk the diagnostics and call `holder.newAnnotation(severity, msg)
   .range(textRange).withFix(…).create()`.

Skeleton:

```kotlin
package dev.cajeta.idea.lint

import com.intellij.lang.annotation.AnnotationHolder
import com.intellij.lang.annotation.ExternalAnnotator
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.openapi.util.TextRange
import com.intellij.psi.PsiFile

data class LintInput(val path: String, val text: String)
data class Diagnostic(
    val ruleId: String,         // e.g. "uncaught-throws"
    val severity: String,       // "error" | "warning"
    val message: String,
    val startOffset: Int,
    val endOffset: Int
)

class CajetaLintAnnotator : ExternalAnnotator<LintInput, List<Diagnostic>>() {

    override fun collectInformation(file: PsiFile): LintInput? =
        file.virtualFile?.path?.let { LintInput(it, file.text) }

    override fun doAnnotate(input: LintInput): List<Diagnostic> =
        CajetacRunner.lint(input.path, input.text)

    override fun apply(file: PsiFile, diagnostics: List<Diagnostic>, holder: AnnotationHolder) {
        for (d in diagnostics) {
            val severity = when (d.severity) {
                "error"   -> HighlightSeverity.ERROR
                "warning" -> HighlightSeverity.WARNING
                else      -> HighlightSeverity.WEAK_WARNING
            }
            holder.newAnnotation(severity, "[${d.ruleId}] ${d.message}")
                .range(TextRange(d.startOffset, d.endOffset))
                .create()
        }
    }
}
```

`CajetacRunner` is a small helper that:

- Locates `cajetac` (config setting in Settings | Languages & Frameworks | Cajeta, with a sensible default of `build/src/cajeta` relative to the project root — see [Compilation.md](../../docs/Compilation.md)).
- Spawns it with `--lint --json --stdin --stdin-name=<path>`, writes
  the buffer to stdin, reads JSON diagnostics from stdout.
- Deserializes via kotlinx.serialization.
- Caches by content hash so repeated keystrokes within the debounce
  window don't re-spawn.

Register in `plugin.xml`:

```xml
<externalAnnotator
    language="Cajeta"
    implementationClass="dev.cajeta.idea.lint.CajetaLintAnnotator"/>
```

Compiler-side contract (this is the API the plugin depends on):

```
$ cajetac --lint --diag-format=json --stdin --stdin-name=src/foo.cajeta < buffer.cajeta
[
  {"ruleId":"uncaught-throws","severity":"warning",
   "message":"call to fetch can throw IOException but enclosing run() neither catches nor declares it",
   "file":"src/foo.cajeta",
   "startLine":12,"startCol":4,"endLine":12,"endCol":18,
   "startOffset":214,"endOffset":228},
  {"ruleId":"wildcard-capture-mismatch","severity":"warning",
   "message":"Box<? extends Animal> capture#3 from b1 is not the same capture as b2",
   "file":"src/foo.cajeta",
   "startLine":18,"startCol":8,"endLine":18,"endCol":24,
   "startOffset":340,"endOffset":356}
]
```

**Current state of cajetac (verified 2026-05-26 against
`build/src/cajeta`):** none of these flags exist yet.

- `cajetac --help` exposes diagnostic controls
  `--diag-verbosity={terse,normal,verbose}` and
  `--diag-hints={on,off}` — and that's it. No `--lint`, no
  `--diag-format`, no `--stdin`.
- Diagnostics are emitted as plain text directly to `std::cerr`
  from the point in the visitor where the rule fires. Example
  at `src/cajeta/asn/expression/MethodCallExpression.cpp:2690`:
  ```cpp
  std::cerr << "warning: [uncaught-throws] call to " << methodCallName
            << " can throw " << thrownType->toCanonical()
            << " but enclosing " << currentMethod->getName()
            << " neither catches nor declares it" << std::endl;
  ```
- Suppression already works: `Method::isLintSuppressed("…")` is
  checked at the call site before printing (line 2616 of the same
  file). The JSON path inherits this for free.
- The compiler is wired top-to-bottom around producing a build
  artifact; there's no front-end-only entry point yet that runs
  through type/lint passes and stops.

**Compiler-side prerequisites — add these before the plugin can
wire Step 7.** Each is its own commit in the cajeta repo, not the
plugin repo:

1. **Centralize diagnostic emission.** Introduce a
   `DiagnosticSink` abstraction (interface + default
   `StderrTextSink` keeping current behavior). Replace every
   `std::cerr << "warning: [...]"` site with
   `sink->emit(Diagnostic{ruleId, severity, message, sourceRange})`.
   `sourceRange` must carry file path, start/end line, start/end
   col, and start/end byte offsets — the plugin needs byte offsets
   to call `TextRange()`. Each call site already has an ANTLR
   token / AST node in scope so the range is recoverable.
   *Scope:* mechanical refactor across the ~dozen lint sites
   (grep for `std::cerr <<` near `LintRules.md` comments).
2. **`--diag-format=json` flag.** Add `JsonDiagnosticSink`
   that buffers diagnostics and prints them as a JSON array to
   stdout at shutdown. Default stays `text` (current behavior).
3. **`--lint` mode.** New flag that runs `parse → resolve →
   type-check → lint pass` and exits without invoking codegen,
   linker, or artifact emission. Wire this into the existing
   `Compiler` driver as an early-exit branch. Lint emission
   already happens during these passes, so this is mostly about
   *not* doing the rest.
4. **`--stdin` + `--stdin-name=<path>`.** Read source from stdin;
   treat the buffer as if it were at the given path for the
   purpose of diagnostic file references. Mutually exclusive with
   the positional source-root argument. Needed because the IDE
   buffer is dirty and not on disk.

Cross-reference these in `docs/Compilation.md` and link
the JSON schema from `docs/LintRules.md` so the plugin and
compiler stay in sync over time.

**Compiler-side cost estimate:** roughly 1–2 days of focused work
for prerequisites (1) and (2), half a day each for (3) and (4).
Total ~3 days. The mechanical refactor in (1) is the bulk of it.

**If we want a faster v0.1 before the compiler work lands:**
ship a degraded mode that parses cajetac's stderr text output
with a regex (`warning: \[(?<id>[^\]]+)\] (?<msg>.*)`). Position
information would be missing or approximate, so squigglies cover
the whole line containing the warning rather than the precise
token. Mark this in code as a temporary path and remove it once
the JSON sink lands. Not recommended unless the compiler work
slips materially.

After this step, edits in the sandbox produce yellow squigglies under
flagged code with the rule ID and message in the tooltip, and entries
in the Problems tool window (Alt-6).

### Step 8 — Markdown rendering in comments (Obsidian-style)

The user's mental model: a comment is markdown. The editor shows it
rendered (formatted headings, bold, lists, links, code spans, fenced
code blocks) by default. When the caret enters a comment, *that
comment alone* drops back to raw source so it can be edited. Caret
leaves → re-render.

This is the same architecture IntelliJ uses for "Render
documentation comments" (View → Appearance → Rendered Doc Comments),
extended to all comments and made caret-reactive. The relevant
platform classes are `CustomFoldingBuilder` (to identify the comment
ranges as foldable units) and the editor's inlay/custom-renderer API
(to draw rendered markdown in place of the folded text).

**Pieces:**

1. **Comment-range detection.** A `FoldingBuilder` walks the PSI
   tree and produces one `FoldingDescriptor` per markdown block.
   A **block** is either:
   - a single `BLOCK_COMMENT` token (`/* … */`), or
   - a *contiguous run* of `LINE_COMMENT` tokens — any sequence of
     `//` lines with no non-comment, non-whitespace token between
     them is grouped into one rendered markdown block. This is the
     authoring model the user wants: a multi-line markdown list
     written across N `//` lines renders as one list, not N
     fragments. A blank source line (no `//`) terminates the run.

2. **Custom renderer.** Each fold region installs an
   `EditorCustomElementRenderer` (or, more precisely, the
   doc-comment-style "rendered fold region" introduced in 2021.2 —
   `CustomFoldRegion` with a custom `CustomFoldRegionRenderer`) that:
   - Parses the comment's *content* (stripped of `//` / `/* */`
     markers) as markdown via the bundled
     `org.jetbrains:markdown` library.
   - Renders to HTML.
   - Draws the HTML inline using `JBHtmlPaneFactory` /
     `JBHtmlEditorKit` so it picks up the IDE's font, color
     scheme, and dark/light mode.

3. **Caret-following toggle.** A project-level `CaretListener` keeps
   a reference to the currently-unrendered comment. On
   `caretPositionChanged`:
   - If the new caret offset is inside any comment fold region,
     expand that region (raw source visible).
   - Re-collapse the previously-expanded region, if any.
   This is exactly the Obsidian live-preview model: at most one
   comment is "raw" at any time, the rest are rendered.

4. **Gutter + action toggle.** Two escape hatches for users who
   prefer manual control:
   - Gutter icon next to each rendered comment that toggles its
     state.
   - Editor action **Cajeta | Toggle Markdown Rendering in
     Comments** that flips the feature project-wide.

Skeleton (abbreviated):

```kotlin
package dev.cajeta.idea.markdown

import com.intellij.lang.ASTNode
import com.intellij.lang.folding.CustomFoldingBuilder
import com.intellij.lang.folding.FoldingDescriptor
import com.intellij.openapi.editor.Document
import com.intellij.openapi.util.TextRange
import dev.cajeta.idea.parser.CajetaTokenTypes

class CajetaCommentFoldingBuilder : CustomFoldingBuilder() {
    override fun buildLanguageFoldRegions(
        descriptors: MutableList<FoldingDescriptor>,
        root: com.intellij.psi.PsiElement,
        document: Document,
        quick: Boolean
    ) {
        for (run in collectCommentRuns(root)) {
            descriptors += FoldingDescriptor(
                run.first,
                TextRange(run.first.startOffset, run.last.endOffset),
                null,
                renderedPlaceholder(run, document)
            )
        }
    }

    override fun getLanguagePlaceholderText(node: ASTNode, range: TextRange): String =
        "…"   // unused — the custom renderer paints actual rendered markdown
    override fun isRegionCollapsedByDefault(node: ASTNode): Boolean = true
}
```

Plus a `CaretListener` registered via
`EditorFactory.getInstance().eventMulticaster.addCaretListener(…)`
inside a `ProjectActivity` (or the post-startup activity API for the
target platform version).

Register the folding builder:

```xml
<lang.foldingBuilder
    language="Cajeta"
    implementationClass="dev.cajeta.idea.markdown.CajetaCommentFoldingBuilder"/>
```

**Dialect.** CommonMark + GFM (tables, strikethrough, task lists,
autolinks). Pin this in docs so authors know what they can rely on
across editors and future renderers.

**Engine — modular by design.** Don't hard-code the markdown library.
Define a thin `MarkdownEngine` interface and route every rendering
call through it. Default implementation uses
`org.jetbrains:markdown:0.7.3` with `GFMFlavourDescriptor`, since
it's pure-Kotlin, ships with IntelliJ, and matches the dialect we
just pinned. But if flexmark, commonmark-java, or a future
better-rendered option comes along, swap the engine without touching
the folding builder, caret listener, or renderer.

```kotlin
package dev.cajeta.idea.markdown

interface MarkdownEngine {
    /** Convert markdown source to HTML suitable for JBHtmlEditorKit. */
    fun renderToHtml(markdown: String): String

    /** Stable identifier for caching and settings UI. */
    val id: String

    /** Human-readable name shown in Settings | Languages & Frameworks | Cajeta. */
    val displayName: String
}
```

```kotlin
package dev.cajeta.idea.markdown.engines

import dev.cajeta.idea.markdown.MarkdownEngine
import org.intellij.markdown.flavours.gfm.GFMFlavourDescriptor
import org.intellij.markdown.html.HtmlGenerator
import org.intellij.markdown.parser.MarkdownParser

class JetBrainsMarkdownEngine : MarkdownEngine {
    override val id = "jetbrains-markdown"
    override val displayName = "JetBrains Markdown (CommonMark + GFM)"

    private val flavour = GFMFlavourDescriptor()
    private val parser = MarkdownParser(flavour)

    override fun renderToHtml(markdown: String): String {
        val tree = parser.buildMarkdownTreeFromString(markdown)
        return HtmlGenerator(markdown, tree, flavour).generateHtml()
    }
}
```

Selection happens through an application-level service that reads
the active engine ID from settings (default `"jetbrains-markdown"`)
and returns the matching instance:

```kotlin
package dev.cajeta.idea.markdown

import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import dev.cajeta.idea.markdown.engines.JetBrainsMarkdownEngine

@Service(Service.Level.APP)
class MarkdownEngineRegistry {
    private val engines: Map<String, MarkdownEngine> = listOf(
        JetBrainsMarkdownEngine()
        // additional engines register here, or via extension point
    ).associateBy { it.id }

    fun active(): MarkdownEngine {
        val id = CajetaSettings.instance.markdownEngineId
        return engines[id] ?: engines.values.first()
    }

    fun all(): Collection<MarkdownEngine> = engines.values

    companion object {
        fun getInstance(): MarkdownEngineRegistry = service()
    }
}
```

Callers (the fold-region renderer, the gutter hover preview, any
future surface) never name a concrete engine — they always go
through `MarkdownEngineRegistry.getInstance().active()`. The
HTML-cache key includes the engine ID so changing engines
invalidates stale cached output automatically.

If a third party wants to plug a different engine in later, promote
the `engines` map to an `ExtensionPointName<MarkdownEngine>`
declared in `plugin.xml`. For v0.1, the hard-coded list is enough —
swapping for the extension-point version is a one-file change.

Don't pull in flexmark or commonmark-java *now*; they'd just
duplicate capability the bundled library already provides. The
abstraction is what matters; the second engine can wait until
there's a concrete reason for one.

**Performance constraints.** Folding rebuilds on every PSI change.
The markdown→HTML conversion runs inside `buildLanguageFoldRegions`
and must finish in single-digit ms on a long comment, or typing
inside a file with many comments will feel laggy. Cache HTML by the
comment's text hash; only re-render on actual content change.

After this step, opening a `.cajeta` file with markdown-formatted
comments shows them rendered. Moving the caret into a comment
reveals its source; moving out re-renders.

### Step 9 — Run in a sandbox IDE

```sh
./gradlew runIde
```

This downloads (cached after first run) IntelliJ IDEA Community,
launches a clean sandbox with the plugin pre-installed, and tails
logs to the console. Open one of the `samples/` `.cajeta` files
in the sandbox and verify:

- File icon shows the Cajeta icon (not the default text icon).
- Tokens are colored.
- Tools → View PSI Structure shows a non-trivial tree.
- Brace matching highlights `{` ↔ `}` when the cursor sits on one.
- Ctrl-/ toggles `//` line comments.
- Structure view (Alt-7) lists classes/methods.

Iterate. Edits to Kotlin code or `plugin.xml` get picked up by
`runIde`; grammar edits require `./gradlew generateGrammarSource`
first (or just re-run `runIde`, which depends on it).

### Step 10 — Package as a distributable .zip

```sh
./gradlew buildPlugin
ls build/distributions
# cajeta-0.1.0.zip
```

The zip is signed only if you've configured signing keys in
`gradle.properties`; for local installation that's not required.
For distribution through the JetBrains Marketplace it is — but
that's a v1.0 concern.

### Step 11 — Install into a real IntelliJ

Two paths:

**A. From the .zip (recommended for the first install):**

In IntelliJ IDEA → Settings → Plugins → ⚙ → Install Plugin from
Disk… → select `build/distributions/cajeta-0.1.0.zip` → Restart.

**B. From the Marketplace (later, after publishing):**

Settings → Plugins → Marketplace → search "Cajeta" → Install.

Verify by opening any project containing a `.cajeta` file. The
file type registration, highlighting, and structure view should
behave the same as in the sandbox.

## Verification checklist

v0.1 is done when all of the following hold in a real installed IDE
on a non-trivial sample (e.g. one of the files under `samples/`).
**Status: v0.1 shipped** — boxes ticked below are confirmed; the few
unticked carry an inline note (degraded or deferred).

**Syntax tier**

- [x] `.cajeta` files open with the Cajeta file icon.
- [x] Keywords, primitives, literals, comments, operators are
      colored distinctly and match the IDE's current color scheme.
- [x] PSI Viewer shows a parse tree with no error nodes for
      well-formed input.
- [x] Introducing a deliberate syntax error produces red error
      markers (from the ANTLR adaptor's error listener).
- [x] After a syntax error mid-file, code *below* the error still
      highlights, folds, and appears in the structure view — i.e.
      `CajetaErrorStrategy` recovers to the next `;` / `}` /
      declaration keyword instead of poisoning the rest of the
      buffer. Test by deleting a `;` halfway down a long sample
      and verifying everything beyond the broken statement still
      parses.
- [x] Ctrl-/ toggles `//` comments; Ctrl-Shift-/ toggles `/* */`.
- [x] Brace, paren, and bracket matching works in both directions.
- [x] Structure view (Alt-7) lists top-level declarations and
      nests methods/fields under their owning class.

**Linting tier** — *shipped in degraded mode: the plugin scrapes
`cajetac` stderr (`warning: [id] msg`) with a regex and approximates
the span, since the compiler does not yet expose
`--lint --diag-format=json --stdin`. The items below hold with
approximate (whole-line) ranges; precise ranges + suppression-respect
arrive with the `cajeta dap`/`lsp` structured-diagnostic work (Phase 2).*

- [x] A program containing a `uncaught-throws` violation shows a
      yellow squiggly under the call site within ~300 ms of typing
      pause, with the rule ID and message in the tooltip. *(rule IDs
      are emitted by `cajetac` today; range is approximate.)*
- [x] A wildcard misuse from the
      [CaptureConversion.md](../../docs/CaptureConversion.md)
      examples (e.g. `holder.box.set(holder.box.get())` with
      distinct captures) is flagged with the rule ID in the message.
- [~] Adding `@SuppressLint("uncaught-throws")` on the enclosing
      method clears the squiggly — *holds only insofar as `cajetac`
      itself suppresses; full fidelity lands with structured diagnostics.*
- [x] Problems tool window (Alt-6) lists every diagnostic with
      file/line and double-click navigates to the range.
- [x] Editing a different file in the project does not re-lint
      this one (per-file caching).
- [x] Killing `cajetac` mid-run (kill -9 on the spawned process)
      leaves the editor responsive; the annotation pass logs a
      warning but doesn't error-dialog.

**Markdown-in-comments tier**

- [x] Comments containing markdown (`# Heading`, `**bold**`, fenced
      code blocks, `[link](…)`, lists) render as formatted output
      in the editor by default. *(full CommonMark+GFM → HTML via
      `MarkdownFoldRenderer` + `engines/JetBrainsMarkdownEngine`.)*
- [x] Moving the caret into a rendered comment reveals its raw
      source for that comment only; other comments stay rendered.
- [x] Moving the caret out of the comment re-renders it.
- [ ] Folding/unfolding a comment via gutter icon overrides the
      caret-following behavior until the caret moves again.
      *(deferred — no gutter-icon toggle; mouse-click + Settings
      toggle ship instead.)*
- [ ] The toggle action (Cajeta | Toggle Markdown Rendering in
      Comments) disables the feature globally and all comments
      revert to raw source. *(deferred as a menu action; the global
      Settings → Cajeta checkbox provides the same effect.)*
- [x] Typing inside a long file with many markdown comments stays
      responsive (no perceptible lag on keystroke).

**Build and run loop**

- [x] `./gradlew runIde` boots a sandbox IDE with the plugin
      loaded in under 30 seconds on warm caches.
- [x] `./gradlew buildPlugin` produces a `.zip` under 5 MB.

## Phase 2 — Debugging

The next phase. Goal: full Java/C++-grade debugging of Cajeta programs
from inside IntelliJ — set breakpoints (line, **conditional**,
**exception**), launch a process in debug mode, see the call stack and a
frame's variables on a breakpoint, **view and edit** those values, and
see **threads** (Cajeta schedules stackful **fibers** on carrier OS
threads — `ucontext` on POSIX, Win32 Fibers on Windows).

**Architecture (per [`Debugging.md`](../../docs/Debugging.md)).**
The compiler ships a **Debug Adapter Protocol** server, `cajeta dap`
(JSON over stdio, or `--port=<n>` for TCP); the IDE drives it. The plugin
implements a **custom `XDebugProcess`** that speaks DAP — not the generic
DAP client — so it can render both the standard debugger UI *and* the
Cajeta-specific panes (Fibers, drop-chain, ownership, capabilities).
`Debugging.md` (959 lines) is the authoritative wire contract; this phase
**implements** it, it does not redesign it.

**Hard dependency / sequencing.** As of this writing the compiler emits
**no DWARF** and has **no `cajeta dap`** (`src/main.cpp` only dispatches
the `archive` subcommand). Every plugin feature here is blocked on
compiler work, so this phase spans **both repos** and sequences the
compiler enablers first. Land a vertical slice — *line breakpoint → hit →
stack → view variables* — then layer conditional/exception breakpoints,
value editing, and the fiber/ownership panes. Each slice is verifiable
with `gdb`/`lldb` + a scripted DAP session *before* any IDE wiring.

### Part A — Compiler prerequisites

Build flavor `debug` = `-O0 --debug-info=full --bounds=on` (see
[`BuildTool.md`](../../docs/BuildTool.md)).

**A1. DWARF debug-info emission** *(the critical gap — everything depends
on it).* Cajeta already captures what DWARF needs (source file+line on
tokens, named locals/params in scope maps, LLVM struct layouts, readable
canonical symbol names); only the emission layer is missing.
- `--debug-info=off|line|full` (+ `-g` alias) in `src/main.cpp`; flag
  threaded through `src/cajeta/compile/CompilerMode.h`.
- `llvm::DIBuilder` + `DICompileUnit` per module in
  `src/cajeta/compile/CajetaModule.{h,cpp}` (finalize before object emit).
- `DISubprogram` per method (`src/cajeta/method/Method.cpp`); `DILocation`
  at statement/expression boundaries (`src/cajeta/asn/**`, esp.
  `LocalVariableDeclaration.cpp`); `DILocalVariable` for locals + params;
  `DICompositeType`/`DIDerivedType` for class layout + fields (map the
  type system in `src/cajeta/type/`).
- Confirm `.debug_*` survives into the linked `--emit=exe` (lld path
  exists): DWARF in ELF (Linux) and PE/COFF-mingw (Windows); macOS
  `.dSYM` a follow-up.

**A2. `cajeta dap` subcommand + DAP server skeleton.**
- Route `argv[1] == "dap"` in `src/main.cpp` (model on the `archive`
  dispatch); stdio default + `--port=<n>` TCP.
- New `src/cajeta/dap/` module (mirrors `src/cajeta/cli/`): Content-Length
  message framing, JSON-RPC loop, request router.
- Launch-config schema (`Debugging.md`): `manifest`, `flavor`,
  `entry-method`, `args`, `env`, `cwd`, `stopOnEntry`, `sourceMaps`.

**A3. Execution control: launch, breakpoints, stepping.** AOT path =
DWARF + native control backend (ptrace on Linux; platform equivalents
staged).
- `launch`/`attach`, `continue`/`pause`, `next`/`stepIn`/`stepOut`.
- `setBreakpoints` — line **plus conditional + hit-count**. The
  **condition is an expression evaluated in the stopped frame's context**,
  so it can reference that frame's locals/params **and static/global
  variables** (reuses the Cajeta parser + the frame-scoped evaluator, A5).
- `setFunctionBreakpoints`; `setDataBreakpoints` (watchpoints).
- **`setExceptionBreakpoints`** — break on thrown exceptions, optionally
  filtered by type. Hook the runtime throw path (the existing
  `--stack-trace-capture` machinery in `runtime/native/` is the seam).

**A4. Stack + threads/fibers.**
- `stackTrace` / `scopes` (DWARF unwind).
- `threads` maps **fibers** to the DAP thread abstraction by default
  (`cajeta:setThreadsView=fibers|os|both`); custom `cajeta:fibers` returns
  id/name/state/carrier/stackTop. Backed by the `ucontext`/Win32-Fiber
  scheduler in `runtime/native/`.
- `cajeta:setPauseMode` (pause-all default / single-fiber).

**A5. Variables: view, evaluate, edit.**
- `variables` — frame locals/params/fields from DWARF locations, with the
  `cajeta` ownership extension (owned/borrowed/moved-out/view, willDrop).
- `evaluate` — compile+run an expression in the paused frame (reuses the
  parser; JIT/interpret). Shared engine behind conditional breakpoints
  (A3) and the Watch/Evaluate window.
- **`setVariable`** — write a new value back into a frame variable.
- Nice-to-haves rendered as panes: `cajeta:dropChain`,
  `cajeta:capabilities`, `cajeta:asyncTasks`.

### Part B — IntelliJ plugin: XDebugger integration

New package `src/main/kotlin/dev/cajeta/idea/debugger/`. A custom
`XDebugProcess` speaks DAP to a spawned `cajeta dap`, reusing the
`ProcessBuilder` + background-task patterns in `lint/CajetacRunner.kt` and
`harness/FixtureTyper.kt`, and `settings/CajetaSettings.compilerPath`
(same binary gains the `dap` verb).

| Plugin class | XDebugger / execution API | plugin.xml | Drives |
|---|---|---|---|
| `CajetaRunConfiguration` (+ `…Type`/`…Factory`/`SettingsEditor`) | `RunConfiguration` family | `com.intellij.configurationType` | launch schema (manifest/entry-method/args/env/cwd/stopOnEntry) |
| `CajetaDebugRunner` (+ built-in Debug executor) | `ProgramRunner` | `com.intellij.programRunner` | spawns `cajeta dap`, opens session |
| `CajetaDebugProcess` | `XDebugProcess` (custom DAP client) | — | the DAP wire loop |
| `CajetaDapClient` | plain Kotlin | — | JSON-RPC framing over the process streams |
| `CajetaLineBreakpointType` (**conditional**) | `XLineBreakpointType` + condition | `com.intellij.xdebugger.breakpointType` | `setBreakpoints` (line + condition + hit-count) |
| `CajetaExceptionBreakpointType` | `XBreakpointType` | `com.intellij.xdebugger.breakpointType` | `setExceptionBreakpoints` (type-filtered) |
| `CajetaBreakpointHandler` | `XBreakpointHandler` | — | enable/disable/condition sync |
| `CajetaSuspendContext` / `CajetaExecutionStack` | `XSuspendContext` / `XExecutionStack` | — | threads **+ Fibers view** (`cajeta:fibers`) |
| `CajetaStackFrame` | `XStackFrame` | — | `stackTrace`/`scopes`; source-position mapping |
| `CajetaValue` / children / `CajetaValueModifier` | `XValue`/`XValueChildrenList`/`XValueModifier` | — | `variables` (view) + `setVariable` (**edit**) |
| `CajetaDebuggerEditorsProvider` | `XDebuggerEditorsProvider` | `com.intellij.xdebugger.editorsProvider` | Cajeta-highlighted **breakpoint-condition** + Watch/Evaluate editing (reuses `highlighting/CajetaSyntaxHighlighter`) |

- **Conditional breakpoints**: the condition is authored in Cajeta syntax
  via `CajetaDebuggerEditorsProvider`, sent as the DAP `condition`, and
  evaluated server-side in the frame (A3) — so it sees frame locals +
  statics, as required.
- **Exception breakpoints**: break on all throws or filter by exception
  type → `setExceptionBreakpoints`.
- **Threads**: one `XExecutionStack` per fiber (default) with an OS-thread
  toggle; selecting one re-targets `stackTrace`/`variables`.
- **Edit values**: `CajetaValueModifier` → `setVariable`.

### Part C — Checkpoint execution (status)

Parts A and B above are the original outline. The actual build ran
**checkpoint-by-checkpoint** (implement + verify one checkpoint, report, gate
the next; TDD-first; debugger tests live in the separate `cajeta_debug_test`
target, not the main suite) and has since **merged to `main`** — the checkpoint
commits below are on `main`. Note the implementation took the **JIT-in-process**
path (`cajeta dap` LLJIT-runs the target and parks the executing fiber
in-process) rather than the AOT/DWARF/ptrace path sketched in Part A — same DAP
wire contract, different backend.

**Overall Phase 2 status: landed through CP7-6.** Everything below is **DONE**
except the single CP6f-2d tail noted inline (hard carrier-quiesce for the
entry-thread-vs-live-fibers edge); the multi-fiber view works under the
stop-the-world model without it. The FR-1 checkpoint table in
[`ide-plugin-debug-fr-1.md`](ide-plugin-debug-fr-1.md) (all CP7 rows ✅) is the
authoritative per-checkpoint record.

**CP1–CP5 — compiler + DAP foundation (DONE).**
- CP1 JIT host (`cajeta jit-run`); CP2 statement safepoints + `DebugLocTable`;
  CP3 `DebugController` stop/resume + per-fiber id + breakpoint arming; CP4
  `cajeta dap` server (initialize/launch/setBreakpoints/configurationDone/
  threads/stackTrace/continue/disconnect + stopped/exited/terminated); CP5
  scopes/variables/setVariable + multi-frame stackTrace + the per-fiber
  `dbg_top` frame chain and the host `DebugVars` value layer.

**CP6 — IntelliJ XDebugger integration (DONE; CP6f-2d the lone open tail).**
Pattern: behavioral core is plain JVM (no `com.intellij.*`) so it
unit-tests without a platform fixture and drives the real `cajeta dap` binary;
platform classes are thin delegates.
- **CP6a** DAP client core (`Json`/`DapTransport`/`DapClient`). **DONE.**
- **CP6b** run config + ProgramRunner + `XDebugProcess` skeleton +
  `CajetaDebugSession` (launch handshake, run-control). **DONE.**
- **CP6c** line breakpoints + suspend context (stack frames + source
  positions). **DONE.**
- **CP6d** variables view (scopes → variables → `XValue`). **DONE.**
- **CP6e** variable value editing (`setVariable` via `XValueModifier`).
  **DONE.**
- **CP6f** — conditional / exception breakpoints + fibers view. Re-split after
  a read-only audit (only conditional bp is backed by the existing runtime;
  exception bp + fibers need C++ runtime work):
  - **CP6f-1** conditional breakpoints — server-side condition eval
    (`evaluateCondition`, a constrained `<local> <op> <literal>` grammar) +
    plugin wiring of `XLineBreakpoint` conditions. **DONE.**
  - **CP6f-2** fibers view (full multi-stack, stop-the-world). Sub-split:
    **2a** runtime live-fiber registry + `dbg_id` fix **(DONE)**; **2b** DAP
    `threads`/per-fiber `stackTrace` wiring **(DONE** — `DapServer.cpp` threads
    handler + 2b-ii per-thread `stackTrace` slice**)**; **2c** plugin
    multi-stack thread dropdown (`XExecutionStack` per fiber) **(DONE)**;
    **2d** hard carrier-quiesce for the entry-thread-vs-live-fibers edge case
    **(OPEN** — no dedicated commit; the multi-fiber view operates under the
    stop-the-world "carrier parked while enumerating" model without it. Tracked
    as W3.**)**.
  - **CP6f-3** exception breakpoints — runtime throw-interception hook into
    `DebugController` + `setExceptionBreakpoints` + `stopped{reason:"exception"}`.
    **DONE** (CP6f-3b plugin wiring + arm-before-start; CP6f-3c throw-hang fix).

**CP7 — ownership / allocation / lifetime visualization + drop breakpoints
(DONE — CP7-1a…CP7-6 all landed; FR-1 table rows all ✅).** Strong at-a-glance
indication of allocation and
lifetime in both the Variables view and inline in the editor. Full requirements
(FR-1…FR-9) and the CP7-1a…CP7-6 checkpoint mapping live in
[`ide-plugin-debug-fr-1.md`](ide-plugin-debug-fr-1.md). Summary:
- **CP7-1a..1d** metadata foundation — capture allocation class (stack/heap/
  shared) + ownership role (owner/borrow/transferred) + lifetime in debug-info
  codegen → runtime frame chain → DAP `variables` facets.
- **CP7-2** Variables-view rendering (icon + color + bold; moved-out struck/
  greyed; textual tag/tooltip backup).
- **CP7-3** editor gutter icons; **CP7-4** full inline decorations, live-
  updating as the user steps.
- **CP7-5** configuration, legend, accessibility.
- **CP7-6** drop / destructor breakpoints — break when an object is dropped,
  surfaced as a breakpoint on the class destructor (mechanism: the destructor /
  synthesized drop wrapper carries a breakable safepoint).

The CP7 group depends on the CP6f line completing first; CP7-1a..1d touch
different layers and may be authored in parallel. The language semantics being
visualized are specified in
[`MemoryModel.md`](../../docs/specification/lang/MemoryModel.md) (owned by a separate
workstream); this plan covers only the debugger surfacing of them.

### Verification — Debugging tier

*Compiler (Part A), independent of the IDE:*

- [ ] `cajeta --debug-info=full --emit=exe <sample>` then
      `llvm-dwarfdump`/`readelf -S` shows `.debug_info` with subprograms,
      a line table, and typed locals.
- [ ] `gdb`/`lldb` on the debug binary sets a source breakpoint, hits it,
      and prints a local — proves DWARF correctness without the plugin.
- [ ] A scripted `cajeta dap` (stdio) session runs initialize → launch →
      setBreakpoints → continue → stackTrace → variables → setVariable →
      setExceptionBreakpoints → continue and returns well-formed
      responses (VS Code's generic DAP client is a quick cross-check).

*Plugin (Part B), end-to-end in `./gradlew runIde` on a Tour sample:*

- [ ] Create a Cajeta debug run-config (entry-method chosen); **Debug**
      launches the process under `cajeta dap`.
- [ ] A line breakpoint hits; the **call stack** and the frame's
      **variables** appear with names and types.
- [ ] A **conditional breakpoint** (e.g. `i == 50 && total > limit`,
      referencing frame locals and a static) stops only when true.
- [ ] An **exception breakpoint** stops at a throw; a type filter is
      respected.
- [ ] **Editing a variable** value in the Variables tree takes effect in
      the resumed run.
- [ ] The **Threads/Fibers** view lists fibers; selecting one repopulates
      the frames + variables.
- [ ] Step over/into/out work; Watch/Evaluate evaluates an expression in
      the selected frame.

*CP7 — ownership / allocation / lifetime (planned, after CP6f):*

- [ ] At a stop, owners vs borrows and stack/heap/shared bindings render
      distinctly in the Variables pane (icon + color + bold) and a moved-out
      binding is shown consumed, not with a stale value.
- [ ] The same distinctions appear inline in the editor (gutter icons +
      inline decorations) and update as the user steps.
- [ ] A breakpoint on a class destructor fires when an instance is dropped,
      with the dropped object inspectable at the stop (CP7-6).

## Out of scope for v0.1

Deferred to a v0.2 plan (separate doc). These are listed so
contributors know what *not* to scope-creep into the syntax tier:

- **Semantic resolution.** Go-to-definition, find-usages,
  symbol search, import resolution. Requires either a Kotlin
  port of Cajeta's symbol table or an LSP server fronting
  `cajetac`. Recommendation: LSP, consumed via lsp4ij on
  Community or built-in LSP on Ultimate.
- **Type-aware completion.** Keyword completion comes for free
  from the lexer; identifier completion needs resolution.
- **Refactoring.** Rename, move, change signature. All require
  resolution as a prerequisite.
- **Formatter.** Beyond brace matching. IntelliJ's
  `FormattingModelBuilder` is non-trivial; tie it to whatever
  `cajeta fmt` does so they don't drift.
- **Quick fixes.** v0.1 displays lint diagnostics but doesn't offer
  one-click fixes (insert `@SuppressLint("…")`, swap `heap` →
  `stack`, etc.). Quick fixes require either compiler-supplied
  patches in the JSON diagnostic payload, or plugin-side codegen
  that knows Cajeta's syntax — both are v0.2.
- **Inspections beyond what `cajetac --lint` emits.** Any check
  that isn't already a rule in
  [LintRules.md](../../docs/LintRules.md) is a compiler
  task, not a plugin task. Add the rule there; the plugin will
  display it for free.
- ~~**Debugger.**~~ **Now in scope** — promoted to
  [Phase 2 — Debugging](#phase-2--debugging). Architecture is DAP via a
  compiler-shipped `cajeta dap` server (not the LLDB/MI sketch this bullet
  originally guessed), per
  [Debugging.md](../../docs/Debugging.md).
- **Build-tool integration.** Run configurations that invoke
  `cajeta build` / `cajeta test`; gutter "Run main" markers.
  Wait until the build tool ([BuildTool.md](../../docs/BuildTool.md))
  is stable enough that the contract won't churn.
- **Multiple IntelliJ versions.** Pinning to 2024.2+ for v0.1;
  cross-version compatibility is its own engineering project.

## Resolved design decisions

All open questions have been answered; recording the choices and
rationale here so future readers can understand the *why*, not just
the *what*.

- **Plugin namespace.** `dev.cajeta.idea` for all packages.
- **Comment-token visibility.** `CajetaLexer.g4:232-233` already
  routes `COMMENT` and `LINE_COMMENT` to `channel(HIDDEN)`, so the
  adapter sees them. No grammar change. `getCommentTokens()` maps
  to those two `IElementType`s directly (see Step 4).
- **Markdown dialect for comment rendering.** CommonMark + GFM,
  via the bundled `org.jetbrains:markdown` library's
  `GFMFlavourDescriptor`. Engine is modular (`MarkdownEngine`
  interface, see Step 8) so a different renderer can be swapped
  in later without touching the folding builder or caret listener.
- **Error recovery strategy.** Custom `CajetaErrorStrategy`
  installed up front, syncing to statement/block/declaration
  anchors. Picked over the do-nothing default because mid-typing
  noise compounds with the linting tier — error squigglies and
  lint squigglies stacking on the same line creates a poor first
  impression. The thin override (`getErrorRecoverySet` only) keeps
  the maintenance burden low. See Step 4.
- **Canonical keyword/operator token-name list.** Hand-maintained
  sets in `CajetaSyntaxHighlighter` for v0.1, with a
  `TODO(codegen-keywords)` marker. Grammar-driven codegen
  (option B from the design discussion) is the planned v0.2
  upgrade once the list of keywords actually stabilizes.

## Future work — v0.2 candidates

Listed so they're visible without re-running the design
discussion. None are blockers for v0.1 shipping.

- **`TODO(codegen-keywords)`** — Gradle task that parses
  `CajetaLexer.g4`, classifies tokens by naming convention, and
  emits `CajetaKeywords.kt`. Removes the hand-maintained sets in
  the syntax highlighter.
- **`MarkdownEngine` extension point** — promote the hard-coded
  engine map in `MarkdownEngineRegistry` to a JetBrains
  `ExtensionPointName<MarkdownEngine>` so third-party engines can
  register declaratively in `plugin.xml`.
- **Error-recovery telemetry** — log how often
  `CajetaErrorStrategy` fires and at which anchor tokens, so we
  know whether the anchor set needs expansion as the grammar
  evolves. Cheap to add once we have a real user base.
- **Typing-simulator test harness** — interactive IDE action and
  headless JUnit tests that type curated `.cajeta` fixtures into
  an editor character-by-character at configurable speed, asserting
  that "valid" fixtures never produce intermediate parse errors
  and that "invalid" fixtures produce only their known errors.
  Catches ANTLR error-recovery regressions, fold-region thrashing
  during edits, linter spam on partial input, and exceptions
  during incremental parse — none of which the existing per-file
  open-and-render testing surfaces. Fixtures live in
  `test-fixtures/{valid,invalid}/`. See P4 in the active task
  list.
