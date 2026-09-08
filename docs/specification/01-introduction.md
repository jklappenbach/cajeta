# 1 — Introduction

This specification defines the Cajeta programming language: which programs are legal, which are rejected and with what diagnostic, and what legal programs mean. It is the normative reference for the language; a construct's behavior is what this document says it is.

## 1.1 Scope

This specification defines the language itself — its capabilities and features: lexical structure, types, allocation and ownership, declarations and names, statements and expressions, errors, concurrency, accelerated compute, script units, the notebook execution model, and program lifecycle. It defines the requirements a program must meet to compile and the behavior a compiled program exhibits.

Coverage of the standard library and of external libraries built as part of cajeta.dev is out of scope; they are documented in their own references. Where a library type carries a language-level guarantee — `Mutex<T>`'s critical-section scoping (Concurrency §16), `Tile`'s role inference (Accelerated Compute §17) — this specification defines the guarantee, and the library reference defines the API.

> *Discussion.* The subdirectories of this documentation tree (`lang/`, `xpu/`, `concurrent/`, …) hold the internal design documents these chapters are distilled from. They record decisions and implementation detail, they remain reachable by direct link, and they are not normative: where an internal document and a chapter disagree, the chapter governs. The Runtime & ABI companion and the Stdlib Reference are being assembled from the same material.

## 1.2 Organization

- **Chapter 2, Grammar & Lexical Structure** — the grammar notation used throughout, source encoding, tokens, literals, and comments.
- **Chapter 3, Types, Values & Variables** — primitive and reference types, kinds of variables, null semantics, and definite assignment.
- **Chapter 4, Allocation & Storage** — the `stack` and `heap` allocation expressions, the drop chain, and reclamation.
- **Chapter 5, Ownership & the Borrow Checker** — the owned, borrow, and shared states; `=` as borrow; `#` as passthrough; the static analysis that enforces them.
- **Chapter 6, Conversions & Contexts** — every conversion, organized by the context in which it applies.
- **Chapter 7, Names, Scopes & Packages** — declarations, scoping, shadowing, packages, imports, and access control.
- **Chapter 8, Classes** — class declarations and members; single inheritance of state, multiple inheritance of behavior; dispatch; destructors.
- **Chapter 9, Interfaces** — interface declarations, implementation obligations, and dispatch through interface types.
- **Chapter 10, Annotations & Aspects** — declared metadata and what acts on it: annotations, aspects and advice, dependency injection.
- **Chapter 11, Templates & Wildcards** — the monomorphized template model, wildcards, deduction, and specialization.
- **Chapter 12, Arrays, Views & Slices** — array types, zero-copy `view` types, and slices with the `shared` state.
- **Chapter 13, Statements & Patterns** — every statement form and its borrow-checker obligations.
- **Chapter 14, Expressions** — every expression form, evaluation order, and operators.
- **Chapter 15, Errors & Stack Traces** — the throwable model, handler selection, traces, and the diagnostic philosophy.
- **Chapter 16, Concurrency** — `async`, `scope`, `spawn`, ownership across tasks, and the synchronization primitives.
- **Chapter 17, Accelerated Compute (XPU)** — kernels, the device subset, portable tiles, launch semantics, and the kernel scheduler.
- **Chapter 18, Script Units** — compilation units of loose statements, `cajeta run`, `System.args`, and script dependencies.
- **Chapter 19, Notebook Kernel & Jupyter Compatibility** — the persistent-session model, cells as script units, and protocol conformance.
- **Chapter 20, Execution & Program Lifecycle** — program shapes, startup, initialization order, and exit.
- **Chapter 21, Complete Grammar** — the full grammar, collected and cross-referenced.

## 1.3 Notation

### 1.3.1 Normative and informative text

Plain paragraphs are normative. Text in a block quote labeled *Discussion* is informative: it motivates, compares, or records status, and it imposes no requirement.

A feature that is designed but not yet implemented appears only in discussion text and is identified there as unimplemented. Normative text describes the shipped language.

### 1.3.2 Compile-time errors and diagnostic codes

The phrase "is a compile-time error" means the program is rejected and the implementation reports the named diagnostic code. Diagnostic codes have the form `CAJETA_ERROR_*`; warning codes have the form `CAJETA_WARN_*` and do not reject the program. The code is the normative, stable identifier of a diagnostic; the accompanying message text is informative and may change between releases (Errors §15).

### 1.3.3 Grammar

Syntax is defined by grammar productions in the notation of Grammar §2. Each construct's productions appear at the start of the section that defines it.

### 1.3.4 Cross-references

A reference of the form (§1.4) names a section of the current chapter. A reference of the form (Ownership §5.3) names a section of another chapter by chapter title.

### 1.3.5 Terminology

A term is defined at first use and used identically thereafter. Five terms recur across chapters:

- **title** — the ownership stake in a value (Ownership §5).
- **borrow** — a non-owning reference; `=` always produces one (Ownership §5).
- **passthrough** — what `#` does: it hands along whatever title the source holds — a transfer when the source owns, a borrow otherwise (Ownership §5).
- **drop** — the reclamation of an owned value at scope exit or claim time (Allocation §4).
- **formal** — a declared parameter; its ownership mode is fixed at the call site (Ownership §5).

## 1.4 Example Programs

Examples are numbered within a section — `Example 1.4-1` is the first example of §1.4 — and appear immediately after the rule they illustrate. Unless a class context is required, examples are written as script units (Script Units §18): loose statements, with method and type declarations as needed.

An example compiles and runs as presented unless it is a rejected program. A rejected program is an example too: it is shown with the exact diagnostic code it produces, on the line that produces it.

**Example 1.4-1.** An accepted program.

```cajeta
int32 x = 40;
System.stdout.println("answer = " + (x + 2));
```

**Example 1.4-2.** A rejected program. The second `#a` transfers from a source that no longer holds the title (Ownership §5).

<!-- snippet: skip -->
```cajeta
public class Sink {
    int8[] data;
    public Sink(#int8[] p) { this.data #= p; }
}

int8[] a = heap int8[4];
Sink s1 = heap Sink(#a);
Sink s2 = heap Sink(#a);    // CAJETA_ERROR_MOVE_OF_BORROW
```

## 1.5 Conformance

A conforming implementation:

1. accepts every program this specification admits;
2. rejects every program for which this specification specifies a compile-time error, reporting the named diagnostic code;
3. gives every accepted program the behavior this specification defines, in every program shape (Execution §20).

The `cajeta` compiler is the reference implementation. Where this specification is silent, the reference implementation's behavior is not thereby made normative; silence is a gap to be reported and closed in a later revision.

## 1.6 Versioning

This specification is versioned with the compiler. The release version is recorded in the `VERSION` file at the repository root, stamped into the binary at configure time, and reported by `cajeta --version`. A chapter documents the language as of the release it ships with; behavioral changes between releases appear in the release notes, and the corresponding chapters are revised in the same release.
