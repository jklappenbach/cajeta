# CajetaReflect.md

A design for `cajeta.reflect`, the stdlib package for runtime
type introspection. Reads the per-class RTTI tables the compiler
already emits (see `StructureMetadata` in the compiler:
`#VTable` + `#RttiGlobal` globals exist for every class today)
and surfaces them through a user-facing API modeled on Java's
`java.lang.reflect` and C#'s `System.Reflection`, with cajeta-
specific improvements (generic retention, annotation-controlled
private access, fiber-aware invocation).

Lives in stdlib because it's foundational — DI containers, JSON
serializers, ORMs, mocking frameworks, debuggers, and most
metaprogramming tools cajeta will grow need it, and the RTTI
cost is already paid (every cajeta binary carries `#RttiGlobal`
entries today regardless). User code reaches for it via
`import cajeta.reflect.*`.

## Why now

The infrastructure exists; the API doesn't. The compiler emits
RTTI for every class:
- Canonical class name
- Properties (name + type + annotations + modifiers per field)
- Methods (name + parameter list + annotations + modifiers per method)
- Parent class name(s)
- Implemented interfaces
- Vtable pointer

`AspectModel.md` uses this for pointcut matching at compile time;
no public runtime API reads it. Until this design lands, users
who want a JSON serializer write one by hand per class, DI
containers do compile-time code generation, and "instantiate a
class by name" isn't a thing. All three are common-enough needs
that a real reflection API is overdue.

## Goals

- **Read-only introspection of every class.** Name, package,
  modifiers, fields, methods, constructors, annotations,
  superclass, implemented interfaces, type parameters. Available
  for any class without opt-in — the RTTI tables exist
  regardless.
- **Generic retention, not erasure.** Java erases generic
  information at runtime; cajeta retains it. A `Box<int32>`
  instance reports `int32` as its type argument; the substitution
  info already lives in the compiler at instantiation time and
  costs nothing to keep in RTTI.
- **Annotation introspection.** Read annotations on classes,
  fields, methods, parameters, and type parameters; access their
  element values by name. Same data AspectModel already consumes
  for advice matching; cajeta.reflect generalizes the API.
- **Reflective invocation.** `Method.invoke`, `Constructor.
  newInstance`, `Field.get` / `Field.set`. Slower than static
  dispatch (vtable lookup by hash + argument marshalling) but
  the right tool for framework code.
- **Annotation-controlled access to private members.** Default
  is "reflection respects visibility" — `Field.get` on a
  `private` field throws unless the declaring class is marked
  `@Reflectable`. Two opt-in paths exist for callers that
  need broader access (see "Access control" below).
- **Class lookup by name.** `Class.forName("com.example.User")`
  resolves a string at runtime to a `Class<?>` for any class
  the binary still contains. AOT-compiled cajeta strips unused
  classes by default; `@Retained` marks classes that must
  survive even if no static reference reaches them.
- **Fiber-safe reflection.** Reflective invocation respects
  cajeta.thread's scheduling — invoke runs in the caller's
  fiber, no thread-hop, no separate "reflection executor."

## Non-goals (v1)

- **Dynamic class generation.** No emitting new classes at
  runtime, no `Proxy.newProxyInstance` equivalent, no bytecode-
  style emission. Cajeta is AOT-compiled; runtime codegen is a
  separate (much larger) effort that would need a JIT backend.
- **Class loading from external sources.** No reading a `.cajeta`
  file from disk and adding its types to the running process.
  Plugins / hot-reload land later, separately.
- **MethodHandle-style call-site caching.** Java's `MethodHandle`
  and `LambdaMetafactory` provide reflection-fast-path performance
  comparable to direct invocation; v1 ships the slow-but-simple
  `Method.invoke` path. Fast-path is a follow-up.
- **Constant folding of reflective constants.** Reading
  `Color.RED` reflectively returns a real lookup, not a compile-
  time constant. The static-known case (where you'd want
  constant folding) just uses the static reference directly.
- **Unsafe memory access.** No `sun.misc.Unsafe` equivalent. The
  `UnsafeReflect` class in this design exposes private members
  via reflection but doesn't bypass type safety, the garbage
  collector, or memory bounds.

## Package layout

```
cajeta.reflect                — public surface (Class, Field, Method,
                                Constructor, Parameter, Annotation,
                                TypeArgument, Modifiers, Modifier flags)
cajeta.reflect.access         — @Reflectable / @Retained annotations,
                                UnsafeReflect escape hatch
cajeta.reflect.invoke         — invocation machinery, exception types
                                (NoSuchMethodException, IllegalAccessException,
                                ReflectiveInvocationException, ...)
cajeta.reflect.registry       — process-wide class lookup
                                (Class.forName backend), used to map
                                canonical names to Class<?> instances
```

---

## cajeta.reflect.Class — the entry point

```cajeta
public final class Class<T> {
    // ----- identity -----
    public String   getName();              // "com.example.User"
    public String   getShortName();         // "User"
    public String   getPackage();           // "com.example"
    public Modifiers getModifiers();

    // ----- relationships -----
    public Class<?>   getSuperclass();      // null only for Object
    public Class<?>[] getInterfaces();
    public Class<?>   getEnclosingClass();  // for inner classes; null otherwise

    // ----- members -----
    public Field[]       getFields();              // declared + inherited public
    public Field[]       getDeclaredFields();      // declared only, all visibility
    public Field         getField(String name);
    public Field         getDeclaredField(String name);

    public Method[]      getMethods();             // declared + inherited public
    public Method[]      getDeclaredMethods();
    public Method        getMethod(String name, Class<?>... parameterTypes);
    public Method        getDeclaredMethod(String name, Class<?>... parameterTypes);

    public Constructor<T>[] getConstructors();
    public Constructor<T>   getConstructor(Class<?>... parameterTypes);

    // ----- annotations -----
    public Annotation[] getAnnotations();
    public Annotation   getAnnotation(String name);
    public Annotation   getAnnotation(Class<? extends Annotation> annotationType);
    public boolean      isAnnotationPresent(String name);

    // ----- generics -----
    public TypeParameter[] getTypeParameters();    // for generic classes (Box<T>)
    public TypeArgument[]  getTypeArguments();     // for instances of generics
                                                   // (Box<int32>'s T = int32)
    public boolean         isGeneric();
    public boolean         isParameterized();      // generic and instantiated

    // ----- type predicates -----
    public boolean isAssignableFrom(Class<?> other);
    public boolean isInstance(Object obj);
    public boolean isInterface();
    public boolean isAbstract();
    public boolean isFinal();
    public boolean isEnum();
    public boolean isStruct();                     // cajeta-specific
    public boolean isAnnotationType();

    // ----- construction -----
    public T newInstance();                        // no-arg ctor; throws if absent
                                                   // or not accessible

    // ----- lookup -----
    public static Class<?> forName(String canonicalName);   // throws if not in registry
    public static Class<?> forNameOrNull(String canonicalName);
    public static Class<?>[] allClasses();         // every @Retained class in process
    public static Class<?>[] classesInPackage(String packageName);
    public static Class<?>[] classesAnnotated(String annotationName);

    // ----- equality / hash -----
    public boolean operator==(Object other);
    public int64   hash();
    public String  toString();
}
```

The `forName` lookup goes through the process-wide registry built
at link time by the compiler. Classes that no static code path
references can be stripped by the AOT linker (binary-size win),
so `forName` doesn't see them. `@Retained` (see "Access control")
keeps a class in the registry regardless of static use.

Access via `T.class` (compile-time, for any type) or
`instance.getClass()` (runtime, returns the dynamic type):

```cajeta
Class<User>  cls1 = User.class;                 // statically known
Class<?>     cls2 = obj.getClass();             // dynamic dispatch
Class<?>     cls3 = Class.forName("com.example.User");
```

---

## cajeta.reflect.Field

```cajeta
public final class Field {
    public String       getName();
    public Class<?>     getType();              // the field's declared type
    public Class<?>     getDeclaringClass();
    public Modifiers    getModifiers();
    public Annotation[] getAnnotations();
    public Annotation   getAnnotation(String name);

    // ----- value access -----
    // Reads / writes the field on `instance`. Static fields take
    // null for instance. Throws IllegalAccessException if the
    // field is private and the declaring class is not @Reflectable
    // (see "Access control"). Throws ReflectiveTypeMismatchException
    // if `value`'s type doesn't match the field type on set.
    public Object get(Object instance);
    public void   set(Object instance, Object value);

    // Typed accessors — avoid boxing when the field type is
    // primitive. Compiler-synthesized per Field instance based
    // on the field's static type.
    public boolean getBoolean(Object instance);
    public int8    getInt8(Object instance);
    public int32   getInt32(Object instance);
    public int64   getInt64(Object instance);
    public float32 getFloat32(Object instance);
    public float64 getFloat64(Object instance);
    public void    setBoolean(Object instance, boolean v);
    public void    setInt32(Object instance, int32 v);
    public void    setInt64(Object instance, int64 v);
    // ...

    // ----- access control -----
    public boolean isAccessible();
    public void    setAccessible(boolean flag);  // see "Access control"
}
```

---

## cajeta.reflect.Method

```cajeta
public final class Method {
    public String       getName();
    public Class<?>     getDeclaringClass();
    public Class<?>     getReturnType();
    public Parameter[]  getParameters();
    public Class<?>[]   getParameterTypes();
    public Modifiers    getModifiers();
    public Annotation[] getAnnotations();
    public Annotation   getAnnotation(String name);
    public Class<?>[]   getDeclaredThrows();    // exception types declared

    // ----- invocation -----
    // `instance` is null for static methods. `args` must match
    // getParameters() in count and type. Returns boxed primitives
    // when the return type is primitive; null for void.
    public Object invoke(Object instance, Object... args);

    // Typed return variants — skip boxing on the return path.
    public int32   invokeInt32(Object instance, Object... args);
    public int64   invokeInt64(Object instance, Object... args);
    public float64 invokeFloat64(Object instance, Object... args);
    // ...

    // Future fast-path slot (post-v1): a MethodHandle-style
    // cached call site. Same shape as invoke but with the
    // argument-marshalling work done once at handle creation.
    // public MethodHandle bindCallSite();
}
```

`Method.invoke` dispatches through the vtable like a normal
method call would, with the canonical-signature-hash lookup in
the receiver's vtable (the same path `__cajeta_vtable_lookup`
takes). Static methods resolve directly to the declaring class's
function. The cost is one runtime hash lookup + argument
marshalling; direct dispatch is one vtable load instead.

---

## cajeta.reflect.Constructor

```cajeta
public final class Constructor<T> {
    public Class<T>     getDeclaringClass();
    public Parameter[]  getParameters();
    public Class<?>[]   getParameterTypes();
    public Modifiers    getModifiers();
    public Annotation[] getAnnotations();
    public Class<?>[]   getDeclaredThrows();

    public T newInstance(Object... args);
}
```

`newInstance` allocates + runs the constructor, returning the
fresh instance. Implicit super-constructor chaining (already in
the language; see commit `37525df`) fires the same way it does
under static `new`. Constructor reflection respects access
control identically to fields and methods.

---

## cajeta.reflect.Parameter

```cajeta
public final class Parameter {
    public String        getName();           // formal parameter name as declared
    public Class<?>      getType();
    public int8          getIndex();          // 0-based position
    public boolean       isThis();            // true for the implicit `this` parameter
    public Object        getDefaultValue();   // for parameters with `= default`
    public boolean       hasDefaultValue();
    public Annotation[]  getAnnotations();
    public Annotation    getAnnotation(String name);
    public TypeArgument[] getTypeArguments(); // for parameter types like `Box<T>`
}
```

The `this` parameter shows up in the parameter list for non-
static methods (Method.parameterList includes it after Method::
generatePrototype runs); `Parameter.isThis()` discriminates.
Callers iterating `getParameters()` for a non-static method
typically skip the first one.

---

## cajeta.reflect.Annotation

```cajeta
public final class Annotation {
    public String       getName();             // "com.example.Transactional"
    public String       getShortName();        // "Transactional"
    public Class<? extends Annotation> getType();
    public Map<String, Object> getValues();    // element name -> value
    public Object       getValue(String elementName);
    public Object       getValue(String elementName, Object defaultIfAbsent);

    public boolean      hasElement(String name);

    // Typed value access — skip the Object boxing on read when
    // the element's declared type is primitive.
    public int32        getInt32Value(String name);
    public int64        getInt64Value(String name);
    public String       getStringValue(String name);
    public boolean      getBooleanValue(String name);
    public Class<?>     getClassValue(String name);
    // ...

    // Annotations can be nested. e.g. @Component(scope=@Scope("singleton"))
    public Annotation   getAnnotationValue(String name);
}
```

Annotation element values are whatever the user wrote at the
declaration site, after compile-time evaluation. Primitive
literals are primitives, class references are `Class<?>`,
strings are `String`, arrays are `Array<...>`, nested annotations
are `Annotation`.

---

## cajeta.reflect.TypeParameter / TypeArgument

Cajeta retains generic information at runtime (departure from
Java's erasure). A `Box<int32>` instance reports `int32` as its
type argument; the compile-time substitution lives in RTTI.

```cajeta
// Declaration-side: `class Box<T extends Comparable<T>> { ... }`
public final class TypeParameter {
    public String       getName();             // "T"
    public Class<?>[]   getBounds();           // declared upper bounds
    public boolean      hasBounds();
    public int8         getIndex();            // 0-based position
}

// Use-site: for an instance of Box<int32>, the [0] type
// argument resolves to int32.
public final class TypeArgument {
    public Class<?>     getResolvedType();     // int32.class for Box<int32>'s T
    public TypeParameter getParameter();       // the declaration-side T
    public TypeArgument[] getTypeArguments();  // nested (Box<List<int32>>)
}
```

```cajeta
// Walking the type tree at runtime:
Box<List<int32>> nested = new Box<List<int32>>();
Class<?>     cls    = nested.getClass();
TypeArgument outerT = cls.getTypeArguments()[0];   // List<int32>
Class<?>     listCls = outerT.getResolvedType();
TypeArgument innerT = outerT.getTypeArguments()[0]; // int32
String       deepName = innerT.getResolvedType().getName();  // "int32"
```

Generic retention means a JSON serializer can ask `List<int32>`
"what's your element type?" at runtime and pick `Int32.toString`
for each element, instead of falling back to `Object.toString`
the way Java would.

---

## cajeta.reflect.Modifiers

```cajeta
public final class Modifiers {
    public boolean isPublic();
    public boolean isPrivate();
    public boolean isProtected();
    public boolean isInternal();
    public boolean isStatic();
    public boolean isFinal();
    public boolean isAbstract();
    public boolean isSealed();
    public boolean isOverride();
    public boolean isTransient();           // @transient field annotation
    public boolean isSynthetic();           // compiler-generated
    public int32   flags();                 // raw bit field
    public String  toString();              // "public final"
}
```

---

## Access control

Defaults are restrictive; opt-in widens the surface.

**Default:** `Field.get` / `Field.set` / `Method.invoke` /
`Constructor.newInstance` respect declared visibility. Reflective
access to a `private` member from outside its declaring class
throws `IllegalAccessException`.

**Opt-in path A — `@Reflectable` on the declaring class.** Marks
the class as having explicitly consented to broader reflective
access. Reflection can read / write / invoke private members of
`@Reflectable` classes.

```cajeta
@Reflectable
public class User {
    private String passwordHash;
    public  String name;
}

// Now reflection can reach passwordHash too.
Field pwd = User.class.getDeclaredField("passwordHash");
String h = (String) pwd.get(userInstance);    // works because @Reflectable
```

**Opt-in path B — `UnsafeReflect` for the framework case.**
Sometimes the framework code (a DI container, a serializer)
needs to reach private members of classes it doesn't control.
`UnsafeReflect` is the explicit escape hatch:

```cajeta
public final class UnsafeReflect {
    // Construction logs a warning at runtime — this isn't a quiet
    // bypass.
    public UnsafeReflect(String reason);

    // Same APIs as Field / Method / Constructor but skip the
    // visibility check.
    public Object  getField(Field f, Object instance);
    public void    setField(Field f, Object instance, Object value);
    public Object  invokeMethod(Method m, Object instance, Object... args);
    public Object  newInstance(Constructor c, Object... args);
}

// Used like this:
UnsafeReflect unsafe = new UnsafeReflect("JSON serializer needs private fields");
for (field in someClass.getDeclaredFields()) {
    Object value = unsafe.getField(field, instance);
    // ...
}
```

The construction-time warning is logged through
`cajeta.thread.log` and surfaces in process telemetry —
framework authors can use it without surprise but it's
visible in the audit trail.

**`@Retained`** — orthogonal to access control, addresses
AOT-stripping. Cajeta's AOT linker removes classes no static
code path reaches, which is correct for binary size but breaks
`Class.forName("com.example.PluginX")` when nothing else
references `PluginX`. Marking a class `@Retained` keeps it in
the registry regardless:

```cajeta
@Retained
public class JsonSerializer { ... }      // forName-discoverable
```

Use sparingly — `@Retained` defeats stripping for that class
plus everything it transitively references.

---

## Performance

Reflection was one of Java's downfalls — first-call costs in the
microseconds, per-call argument-array allocation, JIT inflation
overhead, opaque to the optimizer. None of those costs are
fundamental to "reflection" the concept; they were specific
choices Java made for reasons cajeta doesn't share (dynamic class
loading, JIT compilation, type erasure, security manager). The
design below sidesteps each one.

### Java's specific pain points (none of which apply to cajeta)

1. **JIT inflation.** First reflective call goes through a slow
   JNI bridge while the JIT decides whether the call site is
   hot enough to "inflate" into generated bytecode. Tens of
   microseconds on the cold path.
2. **Argument-array allocation per call.** `Method.invoke(obj,
   42, "foo")` allocates an `Object[]`, boxes `42` to
   `Integer.valueOf(42)`. Two-to-three allocations for a one-
   arg int call.
3. **Symbolic-name lookups at runtime.** `Class.getField("name")`
   is a hash table walk over interned strings. Cajeta knows the
   field's index at compile time and never walks strings on the
   hot path.
4. **No inlining.** Java's JIT can't see through `Method.invoke`'s
   vtable indirection; reflection blocks escape analysis, scalar
   replacement, and constant propagation.
5. **Per-call accessibility check.** `setAccessible` flags +
   module exports + security manager consulted on every reflective
   access.

### Cajeta's structural advantages

- **AOT-compiled.** No JIT inflation; the reflective dispatch code
  is compiled once and lives in the binary like any other function.
- **No dynamic class loading.** Every class in the binary is known
  at link time; no "is this class loaded?" check per lookup, no
  class-loader hierarchy to walk.
- **Static memory model.** Field offsets are compile-time
  constants. The compiler can emit per-class adapters that bake
  in the offsets directly — no JVM-style indirection through a
  reflective `Field.offset` field.
- **No security manager.** Access control is a compile-time
  `@Reflectable` check plus `UnsafeReflect`'s construction-time
  audit log, not a per-call check.
- **Monomorphic generics with retention.** No type-erased
  cast-back-to-Object roundtrips.

### The seven strategies that close the gap

**1. Pre-built integer-indexed RTTI tables.** Replace string-keyed
lookups with compact arrays. The compiler knows each field's
ordinal index in its declaring class; `Class.getField("name")`
resolves the name to an index at lookup time (one perfect-hash
probe over a per-class table) and then returns a `Field` whose
internal representation is just the index. The `Field` carries
no string. Subsequent `Field.get(obj)` calls don't touch a string
at all.

**2. Compiler-synthesized per-class reflection adapters.** For
every class, the compiler emits a hidden accessor pair:

```
User_reflect_getField(User* obj, int32 fieldIdx) -> Object
  switch (fieldIdx) {
    case 0: return obj->name;     // direct field load
    case 1: return obj->id;
    case 2: return obj->email;
    default: throw IllegalArgument;
  }

User_reflect_invokeMethod(User* obj, int32 methodIdx, Object[] args) -> Object
  switch (methodIdx) {
    case 0: return obj->greet((String) args[0]);   // direct call
    case 1: return obj->ageInDays();
    default: throw IllegalArgument;
  }
```

`Field.get(obj)` dispatches to the adapter via one vtable hop
(the adapter is reached through the RttiGlobal). Each field
access is **one vtable load + one switch + one direct load** —
the same shape as a virtual method call. That's the floor for
"the dispatch target wasn't statically known"; Java pays orders
of magnitude more because the JIT can't see this pattern.

**3. Typed accessors that skip boxing.** Already in the design
(`Field.getInt32`, `Method.invokeInt64`). For each combination
of (declaring class, field/method, primitive type) the compiler
emits a specialized variant whose body is a direct field load
or call with no `Integer.valueOf`-style boxing in sight.

```cajeta
Field idField = User.class.getField("id");
int64 id = idField.getInt64(user);   // emits: load + return, no Object
```

**4. `MethodHandle`-style cached call sites — promoted into v1.**
A `Method` produces a `MethodHandle` once via `bindCallSite()`.
The handle is a typed function pointer wrapped in a small struct;
calling it is one vtable hop + direct call. Java's
`MethodHandle.invokeExact` already proves this approach hits
~95% of direct-call speed.

```cajeta
public final class Method {
    public Object invoke(Object instance, Object... args);   // slow path
    public MethodHandle<S> bindCallSite<S>();                // fast path
}

public abstract class MethodHandle<S> {
    // S is the typed signature shape. Subclass per (return type +
    // parameter type list); compiler synthesizes the concrete
    // class once per signature shape encountered.
    public abstract S invoke(Object instance, Object... args);
}

// Concrete shape synthesized by the compiler:
class MethodHandle_Int32_to_Int64 extends MethodHandle<Int64Returning> {
    Function pointer to the underlying method;
    public int64 call(Object instance, int32 arg0) {
        return underlying(instance, arg0);   // direct
    }
}

// Usage in user code:
Method m = User.class.getMethod("ageDelta", int32.class);
var handle = (MethodHandle_Int32_to_Int64) m.bindCallSite();
int64 r = handle.call(user, 30);   // one vtable hop, no boxing
```

Hot reflection paths (DI containers, ORM mappers, serializers)
build the handle once at framework init and call it directly
forever after. The slow `Method.invoke` path stays available
for one-off reflective calls where building a handle isn't
worth it.

**5. Constant folding of statically-known reflection.** When all
inputs to a reflective call are compile-time constants —
`User.class.getField("name").get(user)` with `"name"` as a
literal — the compiler folds the call to a direct field access
in the same pass it does for `String.equals("literal")`. Same
machinery, applied to reflective lookups. Caveat: gives up
visibility checks at the call site, so the fold only fires when
the access would be permitted anyway.

**6. Fiber-stack argument buffers, not heap allocations.** When
the argument count is small and the call site is statically
typed (`method.invoke(obj, a, b)` with 2 args), the args
collection uses fiber-stack-allocated storage instead of a heap
`Object[]`. Cajeta's existing alloca path covers this — the
compiler picks stack vs heap based on count and lifetime. For
large or escaping arg lists, heap fallback is identical to
Java's shape.

**7. Cached `Class<?>` instances.** One `Class<?>` per declared
type in the process, reused across every `getClass()` and
`T.class` call. The instance lives in static storage next to
the RttiGlobal; no allocation. (Java does this too in
HotSpot's `Klass::java_mirror`, but the indirection through
the JVMTI machinery hides it from optimization. Cajeta's
single-step access doesn't.)

### Realistic performance ceiling

Combining the above:

| Operation                              | Cost vs direct equivalent |
|----------------------------------------|---------------------------|
| `obj.getClass()`                       | 1 vtable load = direct    |
| `Class.getField(name)` (cold)          | 1 perfect-hash probe      |
| `Class.getField(name)` (repeated, same site) | inline-cached to direct |
| `Field.getInt32(obj)` via `@Reflectable` adapter | ~1.5× direct field load |
| `Method.invoke(obj, args)` slow path   | ~5-10× direct virtual call |
| `MethodHandle.call(obj, args)`         | ~2-3× direct virtual call |
| `Constructor.newInstance(args)`        | ~2× direct `new` (mostly arg marshalling) |
| Reflective enumeration of fields       | array iteration; constant per field |

The slow path stays available for one-off cases; the fast paths
(typed accessors, MethodHandles) cover everything inside a tight
loop. Frameworks built on cajeta.reflect can be as fast as
manual code generation — without users writing the manual code.

### Compile-time codegen as the alternative

For the workloads where even MethodHandle-level overhead is too
much (JSON serializing millions of records per second, ORM column
mapping in a hot query loop), the right answer is shifting work
from runtime to compile time. Cajeta's annotation processor
machinery (the AspectModel infrastructure) can be extended to
let library authors emit per-class direct-dispatch code at
compile time, in the same way Rust derive macros work.

A `@JsonSerializable` annotation processor produces:

```
User_toJson(User obj) -> String     // generated by the macro
  emits: "{\"name\":" + escape(obj.name) + ",\"id\":" + obj.id + ...
```

instead of using reflection at all. Reflection is the
discoverable, general-purpose path; codegen is the escape hatch
for the hot paths that can amortize compile-time work. Both ship,
neither is "the answer" — the user picks based on the workload.

---

## Worked examples

**JSON-style serializer in 30 lines:**

```cajeta
public String toJson(Object obj) {
    if (obj == null) return "null";
    Class<?> cls = obj.getClass();
    if (cls.isAnnotationPresent("Primitive")) return primitiveToJson(obj);

    StringBuilder out = new StringBuilder();
    out.append("{");
    boolean first = true;
    for (field in cls.getFields()) {
        if (field.getModifiers().isStatic()) continue;
        if (field.getModifiers().isTransient()) continue;
        if (!first) out.append(",");
        first = false;
        out.append("\"").append(field.getName()).append("\":");
        out.append(toJson(field.get(obj)));
    }
    out.append("}");
    return out.toString();
}
```

**Annotation-driven DI scan:**

```cajeta
for (cls in Class.classesAnnotated("Component")) {
    var injectMethod = cls.getMethod("__cajeta_inject");
    Object instance = injectMethod.invoke(null);   // static method
    container.register(cls, instance);
}
```

**Generic-aware deep equals:**

```cajeta
public boolean deepEquals(Object a, Object b) {
    if (a == null || b == null) return a == b;
    Class<?> ca = a.getClass();
    if (ca != b.getClass()) return false;
    if (ca.isPrimitive()) return a.equals(b);

    for (field in ca.getDeclaredFields()) {
        if (field.getModifiers().isStatic()) continue;
        if (!deepEquals(field.get(a), field.get(b))) return false;
    }
    return true;
}
```

---

## Implementation sequence

A reasonable order, given the existing RTTI infrastructure. The
v1 scope includes the performance paths described above —
typed accessors, MethodHandle, the per-class synthesized
adapters — because they're the difference between "Java
reflection" and "useful reflection," and skipping them would
land a slow API that nobody uses.

1. **`Class<T>` + the basic introspection surface.** `getName`,
   `getPackage`, `getModifiers`, `getSuperclass`, `getInterfaces`,
   `getFields`, `getMethods`, `getConstructors`. Reads existing
   RTTI tables; no codegen changes. The Class registry
   (`cajeta.reflect.registry`) is built from the same data the
   compiler already emits. Per-class integer-indexed lookup
   tables land here (Strategy 1 from "Performance").
2. **Compiler-synthesized per-class reflection adapters.**
   `User_reflect_getField` / `User_reflect_invokeMethod` /
   `User_reflect_newInstance` emitted per class as part of
   normal codegen. Switch over field/method index; direct field
   load / direct call for each case. Foundation for steps 3-5.
3. **`Field` read/write.** Reflective `get` / `set` plus the
   typed primitive accessors. Dispatches through the per-class
   adapter from step 2. Respects visibility; throws on private
   without `@Reflectable`.
4. **`Method.invoke` + `Constructor.newInstance` slow paths.**
   Reuse the vtable lookup path + per-class adapter. Argument
   marshalling is the new code; fiber-stack-allocated arg
   buffers for small calls (Strategy 6 from "Performance").
5. **`MethodHandle.bindCallSite`.** Per-signature-shape concrete
   subclasses synthesized lazily on first `bindCallSite()` call
   for each unique signature in the program. Caches per shape
   so the binding cost is paid once. Fast-path invocation that
   frameworks (DI, ORM, serializers) build at init and call
   forever.
6. **Annotations.** RTTI already carries annotation metadata
   (AspectModel reads it for advice matching); `cajeta.reflect.
   Annotation` exposes it generally.
7. **Generic retention.** Augment RTTI to record type-parameter
   substitutions per instantiation. `TypeParameter` /
   `TypeArgument` API reads them. Most generic-aware features
   (JSON serializer dtype inference, ORM column types) become
   reachable here.
8. **`Class.forName` + the `@Retained` marker.** Process-wide
   registry built at link time from `@Retained` classes plus
   every statically-referenced class. forName lookup is
   perfect-hash over canonical name.
9. **`@Reflectable` + `UnsafeReflect`.** Access-control opt-in
   paths. UnsafeReflect logs construction-time warnings through
   cajeta.thread.log.
10. **Package / annotation queries.** `Class.classesInPackage`,
    `Class.classesAnnotated`. Linear scan over the registry
    initially; tree- or trie-indexed if it becomes a hot path.
11. **Constant-fold known reflection.** Compiler pass that
    folds `User.class.getField("name").get(user)` to direct
    field access when all inputs are statically known and
    visibility permits. Strategy 5 from "Performance."

Deferred (separate efforts, post-v1):
- Compile-time codegen escape hatch (the `@JsonSerializable`-
  style annotation processor that emits per-class direct-
  dispatch code at compile time). Useful for workloads where
  even MethodHandle-level overhead is too much.
- Dynamic class generation / proxies.
- Plugin / hot-reload class loading.
- Reflection-emit (writing new classes at runtime).

---

## Open questions

- **Private-access default.** The proposal above defaults
  restrictive (`@Reflectable` opt-in for private members). The
  alternative — default open, `@Sealed` opt-out — is more
  Java-like and friendlier to frameworks. Java moved toward
  restrictive over time (the JEP 396 / modules story); cajeta
  starts there. Worth pressure-testing against the DI / ORM
  / serializer use cases before locking in.
- **Generic retention overhead.** Every instantiated generic
  class carries an extra few words of RTTI for its type
  arguments. Negligible for most programs but real for code
  that instantiates many distinct generics. An optional
  `-noGenericRetention` flag could strip the per-instantiation
  RTTI for binary-size-critical builds, at the cost of
  `TypeArgument` API throwing at runtime.
- **`Class.forName` failure mode.** Strict (throws if the class
  was stripped) or lenient (returns `null`)? Java throws
  `ClassNotFoundException`; the design above provides both
  `forName` (throws) and `forNameOrNull` (returns null). Pick
  one as the primary in documentation.
- **Reflection invocation through fibers.** A reflective invoke
  inherits the calling fiber's scheduling, the same as any
  static call would. But what about reflectively invoking
  `async`-marked methods (when async lands as a first-class
  feature)? Worth a placeholder design — probably the same
  fiber-pool dispatch async non-reflective calls use.
- **`Object.getClass()` performance.** Every reflective entry
  point starts with a vtable-pointer-to-RttiGlobal load. Java
  caches `Class` instances aggressively; cajeta should too —
  one `Class<?>` per declared type, reused on every getClass
  call. Worth confirming the implementation does this rather
  than allocating per call.
- **Generic methods (not classes).** `<T> T pick(T[] arr)` —
  the type parameter lives only on the method, not the class.
  RTTI doesn't currently emit per-method type parameters;
  `Method.getTypeParameters()` is in the API but the underlying
  data needs to land. Worth deciding now whether v1 ships this
  or punts to v2.
