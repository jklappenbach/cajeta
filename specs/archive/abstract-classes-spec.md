# abstract-classes — spec

## 1. Definition

`abstract` is a modifier on methods and on classes. On a method it declares a
signature with no body that every concrete descendant must implement. On a
class it declares that the class is not instantiable and may leave inherited
obligations to its descendants. This spec makes both meanings enforced,
documented, and demonstrated.

### 1.1 Why

Measured 2026-09-25 on the build at `db1f07d2`.

- **1.1.1** The `Modifier` enum has no `ABSTRACT` bit. The parser accepts
  `abstract` on a class and drops it. `abstract class` is decoration.
- **1.1.2** The method keyword is not read either. A method becomes abstract
  because its body is missing, so `public int32 foo();` is silently abstract.
- **1.1.3** An intermediate abstract class is unwritable. `abstract class
  Polygon extends Shape` that leaves `area()` to a subclass is rejected with
  `CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED`, because abstractness is derived
  from "declares an abstract method of its own". The workaround is to restate
  the parent's declaration.
- **1.1.4** An abstract class that implements an interface and leaves a method
  to its subclasses is rejected with `CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED`.
  The interfaces chapter promises the opposite.
- **1.1.5** `abstract int32 area() { return 1; }` compiles and the body is
  dropped.
- **1.1.6** A complete class declared `abstract` allocates.
- **1.1.7** The vtable of a class with an abstract method has no slot for it.
  A call that reaches it dereferences null.

### 1.2 Design

The two keywords say different things, so neither restates the other.

- On a method, `abstract` is the primitive: bodiless, obligation on
  descendants.
- On a class, `abstract` is two independent assertions: not allocatable, and
  exempt from the completeness checks. Neither is derivable from the methods,
  which is why the class keyword is kept.

A class that declares an abstract method must say `abstract`. Once the class
keyword gates the completeness check, silence has to mean something, and an
inferred keyword cannot tell "forgot" from "meant concrete".

### 1.3 Scope

Compiler enforcement, reflection, the language specification, the guide, the
tour, and the cajeta.dev site, which renders `docs/`.

### 1.4 Non-goals

- **1.4.1** Default methods on interfaces. Interfaces stay bodiless.
- **1.4.2** An `override` keyword. Overriding stays by signature.
- **1.4.3** Enforcing that an override matches its abstract declaration's
  ownership stance. That is the stdlib ownership convention's job.

## 2. The method modifier

- **2.1** When a method is declared `abstract` with no body, it contributes a
  signature and a dispatch obligation and emits no function.
- **2.2** When a method is declared `abstract` with a body, compilation fails
  with `CAJETA_ERROR_ABSTRACT_METHOD_HAS_BODY`.
- **2.3** When a method has no body, is not declared `abstract`, and carries
  no annotation that supplies its body (`@Native`, `@Intrinsic`, a
  synthesizer), compilation fails with `CAJETA_ERROR_METHOD_MISSING_BODY`.
- **2.4** When a method is declared both `abstract` and `static`, compilation
  fails with `CAJETA_ERROR_ABSTRACT_STATIC_METHOD`. There is nothing to
  dispatch.
- **2.5** When a method is declared both `abstract` and `private`, compilation
  fails with `CAJETA_ERROR_ABSTRACT_PRIVATE_METHOD`. A descendant cannot
  override it.
- **2.6** When a class that is not declared `abstract` declares an abstract
  method, compilation fails with
  `CAJETA_ERROR_ABSTRACT_METHOD_IN_CONCRETE_CLASS`, at the class.
- **2.7** When an interface method is declared `abstract`, the keyword is
  accepted and redundant.
- **2.8** When a record declares an abstract method, the existing
  `CAJETA_ERROR_RECORD_ABSTRACT_METHOD` fires.
- **2.9** When a method is reflected, `Method.getModifiers().isAbstract()`
  reports the keyword.

## 3. The class modifier

- **3.1** When a class is declared `abstract`, the modifier is stored and
  `Class.isAbstract()` and `Class.getModifiers().isAbstract()` report it.
- **3.2** When an abstract class is allocated with `heap` or `stack`,
  compilation fails with `CAJETA_ERROR_ABSTRACT_INSTANTIATION`, whether or not
  the class has an abstract method.
- **3.3** When an abstract class inherits an abstract method and does not
  override it, the obligation passes to its descendants and no error fires.
- **3.4** When an abstract class implements an interface and leaves part of
  the contract unimplemented, the obligation passes to its descendants and no
  error fires.
- **3.5** When a class that is not declared `abstract` inherits an unmet
  obligation from any base or interface, the existing
  `CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED` or
  `CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED` fires.
- **3.6** When a class is declared both `abstract` and `final`, compilation
  fails with `CAJETA_ERROR_ABSTRACT_FINAL_CLASS`.
- **3.7** When a record is declared `abstract`, the existing
  `CAJETA_ERROR_RECORD_ABSTRACT` fires.
- **3.8** When an abstract type is used as a parameter, a local, a field, a
  return, or an array element, it is legal. Only allocation is refused.
- **3.9** When an abstract class template is instantiated, the instantiation
  is abstract.
- **3.10** When an abstract class comes from a dependency, every rule above
  holds for its consumers.
- **3.11** When a `@GenerateMock` target is abstract, the mock supplies a body
  for every abstract method and is instantiable.
- **3.12** When an abstract class is named to `Class.heapInstance<T>`, the
  result is empty. Reflection does not allocate what source cannot.

## 4. Dispatch

- **4.1** When a concrete descendant is bound to an abstract base type, calls
  reach the descendant's implementation.
- **4.2** When a method left to a leaf by an abstract class is called through
  an interface-typed binding, the call reaches the leaf's implementation.
- **4.3** When a vtable is emitted for a class with an abstract method, the
  slot holds a stub that panics naming the class and method, rather than
  null. The compile-time refusals make it unreachable from cajeta source, so
  the stub is what a reflective or foreign caller hits instead of a null
  dereference.

## 5. Documentation and examples

- **5.1** The language specification chapter on classes states every rule in
  §2 to §4, with an example of an intermediate abstract class and of a
  complete abstract class, and its discussion note about unenforced checks is
  removed.
- **5.2** The interfaces chapter's statement that an abstract class may leave
  part of a contract unimplemented is true and cross-referenced.
- **5.3** The guide's inheritance chapter has a section on abstract classes
  and methods with a runnable example and a link to the tour demo.
- **5.4** The tour has an `AbstractClassesDemo` covering the base, an
  intermediate abstract class, a concrete leaf, a complete abstract class,
  and dispatch through base and interface bindings, registered in
  `Tour.cajeta` and passing `check-tour.sh`.
- **5.5** The site builds from the updated `docs/` and the classes chapter
  renders on cajeta.dev after the next deploy.

## 6. Migration

- **6.1** `samples/tour/.../DemoClass.cajeta` declares an abstract method and
  is not declared `abstract`. It gains the keyword. A survey of the stdlib,
  the tools, and every sibling repo on this machine found no other case.
