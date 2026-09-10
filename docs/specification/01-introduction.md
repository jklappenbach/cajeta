# 1 — Introduction

This specification defines the Cajeta programming language: which programs are legal, which are rejected and with what diagnostic, and what legal programs mean. It is the normative reference for the language. A construct's behavior is what this document says it is.

## 1.1 Scope

This specification defines the language itself — its capabilities and features: lexical structure, types, allocation and ownership, declarations and names, statements and expressions, errors, concurrency, accelerated compute, script units, the notebook execution model, and program lifecycle. It defines the requirements a program must meet to compile and the behavior a compiled program exhibits.

Coverage of the standard library and of external libraries built as part of cajeta.dev is out of scope. They are documented in their own references. Where a library type carries a language-level guarantee — `Mutex<T>`'s critical-section scoping (Concurrency §16), `Tile`'s role inference (Accelerated Compute §17) — this specification defines the guarantee, and the library reference defines the API.

> *Discussion.* The subdirectories of this documentation tree (`lang/`, `xpu/`, `concurrent/`, …) hold the internal design documents these chapters are distilled from. They record decisions and implementation detail, they remain reachable by direct link, and they are not normative: where an internal document and a chapter disagree, the chapter governs. The Runtime & ABI companion and the Stdlib Reference are being assembled from the same material.

## 1.2 Organization

Chapters 2 through 14 define the core language: grammar and lexical structure, types, allocation, ownership, conversions, names and scopes, classes, interfaces, annotations and aspects, templates, arrays and views, statements, and expressions. Chapters 15 through 20 define errors, concurrency, accelerated compute, script units, the notebook kernel, and program lifecycle. Chapter 21 collects the complete grammar.

## 1.3 Notation

### 1.3.1 Reading conventions

A plain paragraph is normative: it states a requirement or a defined behavior of the shipped language. A tinted panel — a quoted block opening with *Discussion.* — is informative: it motivates, compares, or records status, and imposes no requirement. A feature that is designed but not yet implemented appears only in a panel and is identified there as unimplemented. A bold phrase opening a paragraph names the rule that paragraph states.

### 1.3.2 Compile-time errors and diagnostic codes

The phrase "is a compile-time error" means the program is rejected and the implementation reports the named diagnostic code. Diagnostic codes have the form `CAJETA_ERROR_*`. Warning codes have the form `CAJETA_WARN_*` and do not reject the program. The code is the normative, stable identifier of a diagnostic. The accompanying message text is informative and may change between releases (Errors §15).

### 1.3.3 Grammar

Syntax is defined by grammar productions in the notation of Grammar §2. Each construct's productions appear at the start of the section that defines it.

### 1.3.4 Cross-references

A reference of the form (§1.4) names a section of the current chapter. A reference of the form (Ownership §5.3) names a section of another chapter by chapter title.

### 1.3.5 Terminology

A term is defined at first use and used identically thereafter. Five terms recur across chapters:

- **title** — the ownership stake in a value (Ownership §5).
- **borrow** — a non-owning reference. `=` always produces one (Ownership §5).
- **passthrough** — what `#=` does: it hands along whatever title the source holds — a transfer when the source owns, a borrow otherwise (Ownership §5).
- **drop** — the reclamation of an owned value at scope exit or claim time (Allocation §4).
- **formal** — a declared parameter whose ownership mode is fixed at the call site (Ownership §5).

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

1. accepts every program this specification admits
2. rejects every program for which this specification specifies a compile-time error, reporting the named diagnostic code
3. gives every accepted program the behavior this specification defines, in every program shape (Execution §20).

The `cajeta` compiler is the reference implementation. Where this specification is silent, the reference implementation's behavior is not thereby made normative. Silence is a gap to be reported and closed in a later revision.

## 1.6 Versioning

This specification is versioned with the compiler. The release version is recorded in the `VERSION` file at the repository root, stamped into the binary at configure time, and reported by `cajeta --version`. A chapter documents the language as of the release it ships with. Behavioral changes between releases appear in the release notes, and the corresponding chapters are revised in the same release.
