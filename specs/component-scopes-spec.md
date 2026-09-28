# Component scopes: published at an anchor, consumed by name

## 1. Definition

### 1.1 Purpose

A component's lifetime is a named scope. A library publishes a scope once, at
the method or class that bounds it. Everyone else consumes the scope by name
on `@Component` and never learns what bounds it. The compiler owns both ends
of every scope, so no framework code starts or stops one.

### 1.2 The problem

- `@Inject(allocate = ...)` picks a lifetime per injection site. Two sites can
  then disagree about one component, and a singleton site holding a
  request's component reads freed memory once the request ends.
- `ALLOCATE_CALL_SCOPE` is stubbed. The inject path throws
  `CAJETA_ERROR_NOT_IMPLEMENTED`.
- Request and session scopes exist only as primavera runtime calls
  (`RequestScope.materialize`/`lookup`). The compiler's generated accessor
  cannot reach them, so a request-scoped `@Component` cannot be declared
  (primavera-web plan 0.2.5).
- `allocate = ALLOCATE_SINGLETON` is a bare identifier the compiler reads as
  raw text. It looks like a symbol reference and is not one.

### 1.3 Decisions taken with Julian, 2026-09-28

- 1.3.1 A lifetime belongs to the component, not the site. This supersedes
  AspectModel.md's "the lifetime is declared at the site".
- 1.3.2 Every scope is bounded by a method activation or an instance's
  lifetime. There are no start and stop markers: a stop call is lost on an
  exception and puts the burden on the framework author, and any arbitrary
  scope can be written as a method.
- 1.3.3 A scope is introduced in one place and consumed by name. Consumers
  do not write scoping syntax and do not see the anchor.
- 1.3.4 No annotation type per scope. One compiler annotation publishes, one
  attribute consumes.
- 1.3.5 Scope names are strings, written as strings.
- 1.3.6 An instance scope hooks only non-private instance methods.
- 1.3.7 An anchor instance is current once its constructor completes.
  Concurrent first injections of a component park until it is built.
- 1.3.8 A structured task inherits its parent's scopes. A detached spawn does
  not.
- 1.3.9 The attribute is spelled `scope`. Annotation element names accept
  `scope` as a soft keyword, and the `scope { }` block statement is unchanged.

### 1.4 Constraints

- 1.4.1 Separate compilation, measured 2026-09-28 in `Compiler.cpp`: a
  consumer re-parses every archive's sources to resolve names, but links the
  archive's own bitcode, which is authoritative. So the compiler that
  compiles an anchor emits its hooks, and it can, because `@Scope` is in the
  anchor's own source. An implementation of an archived interface method is
  compiled by its own package, which sees the interface's `@Scope` through the
  re-parse. A scope's runtime identity is one symbol per qualified name, so
  separately compiled modules agree on it.
- 1.4.2 Ownership: a scope's table owns its components. A lookup returns a
  borrow. No component outlives its scope.
- 1.4.3 Cost is executed cost. Entry and exit hooks are inline IR, and only
  anchors pay them.

### 1.5 Non-goals

- Distributed or cross-process scopes. A session shared across nodes is
  cajeta-cluster's.
- Proxies. Nothing is injected as a generated delegate.
- Choosing among several live instances of an anchor class by key.

## 2. Publishing a scope

```cajeta
package cajeta.aot;

annotation Scope {
    String value();
    String within() default "";
}
```

- **2.1** When `@Scope("Request")` is placed on a method, `Request` is one
  activation of that method, static or instance.
- **2.2** When `@Scope("Session")` is placed on a class, `Session` is the
  lifetime of one instance of that class or of a subclass.
- **2.3** When a method publishes a scope, every override and every
  implementation of it is the same anchor. Publishing on an interface method
  makes each implementation an anchor.
- **2.4** When a class and one of its methods both publish, the method's
  scope is within the class's.
- **2.5** When `within = "Connection"` is given, entering the scope while no
  `Connection` is active fails on entry, with an error naming both scopes.
- **2.6** When a scope name clashes with one another package publishes, the
  consumer qualifies it by package: `"dev.cajeta.primavera.Request"`.
- **2.7** When an application needs its own boundary (a batch import, a CLI
  command, a game frame), it publishes a scope the same way a framework does.

## 3. Consuming a scope

```cajeta
annotation Component {
    String name() default "";
    String scope() default "Singleton";
}

annotation Inject {
    String name() default "";
    String scope() default "";
    boolean optional() default false;
}
```

- **3.1** When a component declares `scope = "Request"`, every injection of it
  inside one `Request` resolves to the same instance, created on the first
  injection.
- **3.2** When an injection site gives no scope, it takes the component's.
- **3.3** When an injection site asks for a scope that conflicts with the
  component's, compilation fails.
- **3.4** When a scope name is not published by any source the compiler sees,
  compilation fails, listing the published names.
- **3.5** When a name is published twice and used unqualified, compilation
  fails at the use, naming both publishers.

- **3.6** When a scope name appears in `@Component.scope`, `@Inject.scope` or
  `@Scope.within`, the compiler resolves it against the publications. The
  tie is the compiler's. Users see no mechanism beyond the name.
- **3.7** When a scope name is typed in the editor, the IDE plugin completes it
  from the published names and navigates from it to its `@Scope`.

## 4. Resolution

- **4.1** When a method scope's anchor is active on the current fiber, the
  innermost activation is the scope. Recursive and re-entrant calls each get
  their own.
- **4.2** When an instance method of an instance-scope anchor is active, its
  receiver is the current instance. The innermost such receiver wins.
- **4.3** When a private instance method runs, it adds no hook. It is reached
  only through a non-private method, which already made the instance current.
- **4.4** When a static method of an anchor class runs, no instance becomes
  current.
- **4.5** When an instance-scope anchor's constructor completes, the instance
  can become current. During its constructor it is not current.
- **4.6** When an injection runs outside every active scope of the
  component's kind, it throws a typed exception naming the component and the
  scope.

- **4.7** When a component has no instance yet in the current scope, the first
  injection constructs it. Any other fiber that injects it in the same scope
  meanwhile parks until the constructor completes, then gets that instance.
  A component is visible only after its constructor completes.
- **4.8** When that constructor throws, every parked fiber receives the same
  failure, and the slot stays empty, so a later injection constructs again.
- **4.9** When construction re-enters itself on the same fiber (A injects B,
  B injects A), it fails with an error naming the cycle instead of parking
  forever.
- **4.10** When the scope is `"Singleton"`, 4.7 to 4.9 hold for the process.

- **4.11** When a task is `spawn`ed inside a `scope { }` block within a scope,
  it sees that scope's components, shared with the parent. This follows
  `FiberLocal`'s inheritance on spawn.
- **4.12** When a scope ends, no inheriting task is still running, because a
  `scope { }` block joins its children before it exits and the block lies
  inside the anchored activation.
- **4.13** When a task is `detach`ed, it sees no scope it was spawned in. It
  receives what it needs as owned arguments, and an injection of a scoped
  component inside it fails as in 4.6.

## 5. Ends and ownership

- **5.1** When a method scope's activation returns or throws, its components
  are dropped, and `@PreDestroy` runs on each.
- **5.2** When an instance-scope anchor is dropped, its components are dropped
  with it, and `@PreDestroy` runs on each.
- **5.3** When a scope ends, no framework call is involved. The compiler emits
  the end on every exit path, as it does for owned locals.
- **5.4** When a component is looked up, the caller gets a borrow. A value
  that must outlive the scope is copied.
- **5.5** When an instance scope spans several calls, its components survive
  between them. A session's cart outlives each request.

## 6. Lifetime rules at compile time

- **6.1** When a field injection targets a component in the same scope, or a
  singleton, it is allowed.
- **6.2** When a field injection targets a component in a scope the holder's
  scope is provably within, it is allowed. Proof comes from 2.4 and `within`.
- **6.3** When a field injection targets a narrower or unrelated scope, it is a
  compile error. The site uses `Scoped<T>` instead.
- **6.4** When `Scoped<T>.get()` is called, it resolves in the scope active at
  that moment and returns a borrow.

## 7. Values produced at runtime

- **7.1** When a stage produces a value the DI graph cannot build (a
  `Principal` from a token), it calls `Components.provide(#value)`. The value
  lands in the active scope its type declares.
- **7.2** When a provided type is injected before it was provided, the
  injection throws a typed exception naming the type and the scope.
- **7.3** When a value is provided twice in one scope, the second call throws.

## 8. Built-in scopes and migration

- **8.1** When no scope is given, a component is `"Singleton"`. The compiler
  publishes `"Singleton"` and `"Transient"`.
- **8.2** When an injection site asks for `"Owner"` or `"Call"`, the scope is
  the injecting object or the injecting method's activation. These are
  site-relative, so only a site can ask for them.
- **8.3** When `allocate` appears, compilation fails with a message giving the
  `scope` spelling. Its only users are the compiler's own tests.
- **8.4** When `@Singleton` or `@Transient` marks a factory method, it means
  `scope = "Singleton"` or `"Transient"`.

## 9. Cost

- **9.1** When a method publishes a scope, its entry pushes one pointer onto a
  per-fiber anchor list and its exits pop it, inline.
- **9.2** When a class publishes a scope, every non-private instance method
  pays 9.1. Getters on an anchor class pay it too.
- **9.3** When no component in a scope is ever injected during an activation,
  the activation allocates nothing.

## 10. primavera's published scopes

- **10.1** `Request`: the method that runs one request's pipeline. It replaces
  `RequestScope.enter`.
- **10.2** `Session`: the session object held by the session store, dropped
  on eviction or invalidation.
- **10.3** `Connection`: the object that lives for one transport connection.
- **10.4** `Message`: the WebSocket dispatch method, `within = "Connection"`.
- **10.5** When an anchor is pooled or reused, its instance scope lasts as long
  as the reuse, not one unit of work. cajeta-http 0.4.0's `HttpRequest` is
  one per connection, so it is not the `Request` anchor. The docs state the
  rule: anchor on the method that runs once per unit of work.

