# 7 — Names, Scopes & Packages

This chapter defines how names are introduced, resolved, and controlled: the scopes declarations govern, shadowing, simple and qualified names, packages and imports, access control, and the visibility a compilation unit's declarations have to other code.

## 7.1 Declarations and Scopes

A local variable's scope runs from its declaration to the closing brace of the declaring block. A block-nested declaration may shadow a name from an enclosing scope. Inside the block, the inner declaration governs, and the outer binding is untouched and resumes at the block's end.

**Example 7.1-1.** Block shadowing.

```cajeta
public final class C {
    public static int32 run() {
        int32 x = 1;
        { int32 x = 2; System.stdout.println("inner " + x); }   // inner 2
        return x;                                                // 1
    }
}
System.stdout.println(C.run());
```

A block-local shadowing a script unit's session binding is an ordinary local: it drops at block exit and leaves the session binding untouched (Script Units §18).

Class members are in scope throughout their class body regardless of declaration order. Fields are read and written through `this` (or another receiver) — `this.field` — and `this` in an instance method or constructor denotes the receiver.

## 7.2 Simple and Qualified Names

A simple name resolves against the enclosing scopes: locals and formals first, then class members, then types made visible by imports and the implicit root package (§7.3).

A qualified name spells its path. A fully qualified type name is usable in any type position without an import:

```cajeta
cajeta.collection.ArrayList<int32> xs = heap cajeta.collection.ArrayList<int32>();
```

Under multiple inheritance of behavior, the qualified selectors `super<Base>.method()` and `this<Base>.field` name a specific base's member. Resolution among colliding base members is Classes §8.

## 7.3 Packages and Imports

A compilation unit may open with a `package` declaration naming the package its types belong to. `import a.b.C;` makes `C` usable by its simple name in the importing unit.

Every program implicitly has access to the `cajeta.lang` root types — `Object`, `String`, `Encoding`, `Optional<T>`, `Pair<K, V>` — and to the `System` and `Cajeta` namespaces, with no import.

> *Discussion.* As of 0.27.0 an unqualified standard-library name outside `cajeta.lang` may also resolve without its import in some contexts. That leniency is not a guarantee. Portable code imports every type it names, or qualifies it fully.

## 7.4 Access Control

Members declare their accessibility with `public`, `protected`, or `private`. A `private` member is accessible only within its declaring class. Invoking an inaccessible method is a compile-time error.

**Example 7.4-1.** A rejected program: a private method invoked from outside its class.

<!-- snippet: skip -->
```cajeta
public class Secret {
    private int32 hidden() { return 42; }
}

Secret s = heap Secret();
System.stdout.println(s.hidden());    // CAJETA_ERROR_METHOD_NOT_ACCESSIBLE
```

> *Discussion.* Two enforcement gaps are open as of 0.27.0: a `private` *field* read from outside the class is not yet diagnosed, and the accessibility of a member declared with no modifier is not yet specified. Both are to be bound here when enforcement lands. Until then, treat `private` state as private regardless of what the compiler accepts, and write modifiers explicitly.

## 7.5 Compilation Units and Archives

A compilation unit is one source file: either a set of type declarations, or a script unit (Script Units §18). A library ships as a `.cja` archive. A consumer that resolves the library through its project manifest can import the library's `public` types, exactly as it imports types from its own project (the archive format and resolution mechanics are the Runtime & ABI companion's and the toolchain's concern).
