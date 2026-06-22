---
id: cajetadoc-render-md
applies-to: [cajetadoc/render-md]
title: cajetadoc --render-md (Markdown → HTML fragment)
description: Render a Markdown fragment from stdin to an HTML fragment on stdout; debug / golden-bless helper.
---

# `cajetadoc --render-md`

Renders the doc-comment Markdown subset (the same `renderMarkdown` engine used to
build doc pages) from **stdin** to an **HTML fragment** on **stdout**. It is a
debug / golden-blessing helper — no source root, no output dir, no file I/O.

## Invocation & I/O contract

```sh
echo '# Hi' | ./build/tools/cajetadoc/cajetadoc --render-md
```

- **Input**: all of stdin, read to EOF, treated as one Markdown document.
- **Output**: an HTML *fragment* (just the block elements — `<p>`, `<ul>`,
  `<h1>`… — no `<html>`/`<head>`/`<body>` wrapper, no CSS) on stdout.
- **Args**: `--render-md` returns the moment it is seen in the arg scan, so any
  source root or other flags on the same line are ignored. Give it nothing else.
- **Exit code**: always `0` (it never reports parse errors — unrecognized
  Markdown is emitted as escaped paragraph text, never a failure).

Worked example (this is the checked-in golden input/output):

```sh
$ printf 'Hello **world**.\n\n- a\n- b\n\n`code` and a [link](http://x).\n' \
    | ./build/tools/cajetadoc/cajetadoc --render-md
<p>Hello <strong>world</strong>.</p>
<ul>
<li>a</li>
<li>b</li>
</ul>
<p><code>code</code> and a <a href="http://x">link</a>.</p>
```

`cajeta doc --render-md` behaves identically (same engine), but the standalone
`cajetadoc` binary builds without LLVM and is the fast path for this.

## Re-blessing the golden

After an intentional renderer change, regenerate the fixture by piping the same
input into the freshly built binary (from `tools/cajetadoc/`):

```sh
printf 'Hello **world**.\n\n- a\n- b\n\n`code` and a [link](http://x).\n' \
  | ./build/tools/cajetadoc/cajetadoc --render-md > test/golden/inline_snippet.html
```

Then re-run `./build/tools/cajetadoc/cajetadoc_test` (or `ctest -L cajetadoc`).
Bless only when the diff is the change you intended.

## Supported Markdown subset

Block: ATX headings `#`..`######` (need a space after the `#`s; a bare `#Foo` is
paragraph text), fenced code ```` ``` ```` (optional lang → `class="language-…"`,
contents escaped, no highlighting), thematic break (`---`/`***`/`___`), GFM
tables (header row + `|---|` separator), blockquotes `>`, unordered lists
(`-`/`*`/`+ `), ordered lists (`1.`/`1) `), paragraphs. Inline: `` `code` ``,
`**strong**`, `*em*`/`_em_`, `[text](url)`, `![alt](url)`, and JavaDoc inline
tags `{@code …}`, `{@literal …}`, `{@link …}`, `{@linkplain …}`, `{@value …}`,
`{@index …}`, `{@summary …}`, `{@docRoot}`. All other text is HTML-escaped
(`& < > "`).

## What it does NOT do

- **No cross-reference resolution.** In `--render-md` mode no `linkResolver` is
  set, so `` `Type` ``, `[Target]` (bracket-only), and `{@link Target}` render as
  plain `<code>`/text, never `<a href>` links. Resolved cross-refs only appear in
  full site generation.
- **No heading offset / wrapping HTML / CSS / theming.** Fragment only; the page
  shell and stylesheet come from site generation (`Render`/`Stylesheet`).
- **No nested lists, no setext (`===`) headings, no reference-style links, no
  inline HTML passthrough, no syntax highlighting** inside code fences.
- **Reads stdin only** — there is no file-input flag; pipe or redirect.

For generating an actual doc site instead of a fragment, see the overview /
`cajetadoc <source-root> -o <dir>`; this command is purely for inspecting and
golden-testing the Markdown renderer.
