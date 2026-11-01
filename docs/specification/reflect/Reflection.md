# Reflection — `cajeta.reflect`

`cajeta.reflect` is the stdlib package for runtime type introspection. It
reads the per-class RTTI tables the compiler already emits and surfaces them
through a user-facing API. See `StructureMetadata` in the compiler, where
`#VTable`, `#RttiGlobal`, and a cached `#ClassObject` exist for every class
today.

> **Two layers, read this first.** A first slice, REFL-1, ships today,
> but its shape differs from the Java-mirror design that fills most of this
> document. The shipped surface is index-based and minimal. You walk
> members by integer index, and there are no `Field[]` / `Method[]` array
> returns, no `Object`-boxed `get`/`set` by default, no `@Reflectable`, no
> `UnsafeReflect`, and no `MethodHandle`. The detailed `Class` / `Field` /
> `Method` / `Constructor` / `Parameter` / `Annotation` sections further down
> describe the intended Java-style API, flavored by `java.lang.reflect` and
> `System.Reflection`, with template retention and fiber-aware invocation.
> Treat those as the design target and not as the current signatures.
> The next section documents exactly what ships.

It lives in stdlib because it is foundational. DI containers, JSON
serializers, ORMs, mocking frameworks, debuggers, and most
metaprogramming tools cajeta will grow need it, and the RTTI
cost is already paid, because every cajeta binary carries `#RttiGlobal`
entries today regardless. User code reaches for it via
`import cajeta.reflect.*`.

## Shipped surface (REFL-1)

This is what actually exists today, in `runtime/src/cajeta/reflect/`. Verify
against those sources and not against the design sections below. Members are
addressed by integer index, and accessors take that index directly.

```cajeta
public final class Class<T> {            // phantom T; layout is { vtable, rtti }
    public static Class<?> of(Object o);                 // dynamic type; also T.class
    public static Optional<Class<?>> forName(String name);   // Optional, not throw
    public static #Class<?>[] allClasses();
    public static #Class<?>[] classesInPackage(String packageName);
    public static #Class<?>[] classesAnnotated(String annotationName);

    public #String   getName();
    public int64     getInstanceSize();
    public int32     getModifierFlags();
    public #Modifiers getModifiers();
    public boolean   isPublic();  public boolean isFinal();

    public int32     getFieldCount();
    public #String   getFieldName(int32 index);
    public int32     getFieldModifierFlags(int32 index);
    public int32     getFieldOffset(int32 index);
    public int64     getFieldTypeFlags(int32 index);
    public #Field    getField(int32 index);

    // Typed field read/write by index (no boxing). i8 has no variant —
    // int32/int64/boolean/float32/float64 only.
    public int32  getInt32(Object o, int32 index);
    public void   setInt32(Object o, int32 index, int32 value);
    public int64  getInt64(Object o, int32 index);
    public void   setInt64(Object o, int32 index, int64 value);
    public boolean getBoolean(Object o, int32 index);
    public void    setBoolean(Object o, int32 index, boolean value);
    public float32 getFloat32(Object o, int32 index);
    public void    setFloat32(Object o, int32 index, float32 value);
    public float64 getFloat64(Object o, int32 index);
    public void    setFloat64(Object o, int32 index, float64 value);
    public #Object getBoxed(Object o, int32 index);      // reference-typed fields

    public int32     getMethodCount();
    public #String   getMethodName(int32 index);
    public int32     getMethodParamCount(int32 index);
    public #Method   getMethod(int32 index);
    public static int64 invokeScalar0(Object o, int32 index);

    public int32        getConstructorCount();
    public int32        getConstructorParamCount(int32 index);
    public #Constructor getConstructor(int32 index);
    public #Object      heapInstance(int32 ctorIndex);   // not `newInstance`

    public int32       getParentCount();

    public int32             getTemplateParameterCount();   // generic retention —
    public #TemplateParameter getTemplateParameter(int32 i); // "Template", not "Type"
    public int32             getTemplateArgumentCount();
    public #TemplateArgument getTemplateArgument(int32 i);
    public boolean           isTemplateInstantiation();

    public int32       getAnnotationCount();
    public #String     getAnnotationName(int32 index);
    public #Annotation getAnnotation(int32 index);
    public boolean     hasAnnotation(String name);
}
```

`Field`, `Method`, `Constructor`, `Parameter`, and `Annotation` are likewise
index-addressed wrappers over the raw `#Rtti` pointer:

- `Field` has `getName`, `getModifiers`, `getTypeFlags`, typed `getInt32`/`setInt32`/… +
  `getBoxed`, and annotation queries. Reads/writes go through a `checkAccess()`
  gate (see below).
- `Method` has `getName`, `getParameterCount`, `getParameter(i)`, `getModifiers`,
  and `invokeScalar` / `invokeInt32` / `invokeFloat32` / `invokeFloat64` /
  `invokeObject` / `invokeBoxed`, each taking an `int64[] args` (small-arity
  overloads exist). This differs from Java's `Object... args`.
- `Constructor` has `getParameterCount`, `getParameter(i)`, `getModifiers`,
  `heapInstance()` / `heapInstance(int64[] args)`.
- `Parameter` has `getName`, `getTypeName`, and annotation queries.
- `Annotation` has args by index (`getArgKind`/`getArgInt`/`getArgBool`/`getArgString`,
  list variants) and by key (`getInt(key)`, `getString(key)`, `getBool(key)`,
  `getClassRef(key)`).
- `Modifiers` has `isPublic` / `isPrivate` / `isProtected` / `isStatic` /
  `isFinal` / `isSealed` / `isRetained` / `isPackagePrivate` over a raw flag
  int. There is no `isOverride` / `isTransient` / `isInternal` / `isEnum` yet.

**Access control as shipped.** There is no `@Reflectable` annotation and no
`UnsafeReflect`. The `checkAccess()` gate blocks a member only when it is
`private` and its declaring class is `@Sealed`. The native
`__cajeta_reflect_field_blocked` / `_ctor_blocked` probes back this. Otherwise
access is allowed. Exceptions live in
`IllegalAccessException.cajeta` / `UnsupportedReflectionException.cajeta`.

**Not yet shipped from the design below.** `Object`-boxed `get`/`set` as the
primary path, `Class<?>[] getFields()`/`getMethods()` array enumerations,
`MethodHandle` / `bindCallSite`, `UnsafeReflect`, `@Reflectable`, `Class.forName`
that throws, and the constant-folding fast paths.

## Why now

The infrastructure exists, and the REFL-1 API above now reads it. The richer
Java-mirror surface is still landing. The compiler emits RTTI for every
class:
- Canonical class name
- Properties (name + type + annotations + modifiers per field)
- Methods (name + parameter list + annotations + modifiers per method)
- Parent class name(s)
- Implemented interfaces
- Vtable pointer

`AspectModel.md` uses this for pointcut matching at compile time. The REFL-1
runtime surface above now reads it too, for introspection, typed field access,
`heapInstance`, and annotation queries. The richer ergonomics the rest of this
doc describes are what remains to make a hand-written per-class JSON
serializer or a runtime DI container unnecessary. Those are array
enumerations, boxed `get`/`set`, `MethodHandle`, and `forName`-by-string with
retention.

## Goals

- **Read-only introspection of every class.** Name, package,
  modifiers, fields, methods, constructors, annotations,
  superclass, implemented interfaces, type parameters. This is
  available for any class without opt-in, because the RTTI tables
  exist regardless.
- **Template retention.** Java erases generic information at runtime,
  and cajeta retains it. A `Box<int32>` instance reports `int32` as
  its type argument. The substitution info already lives in the
  compiler at instantiation time and costs nothing to keep in RTTI.
- **Annotation introspection.** Read annotations on classes,
  fields, methods, parameters, and type parameters, and access their
  element values by name. This is the same data AspectModel already
  consumes for advice matching, and cajeta.reflect generalizes the
  API.
- **Reflective invocation.** `Method.invoke`, `Constructor.
  newInstance`, `Field.get` / `Field.set`. It is slower than static
  dispatch, because of the vtable lookup by hash and the argument
  marshalling, and it is the right tool for framework code.
- **Annotation-controlled access to private members.** The default
  is that reflection respects visibility, so `Field.get` on a
  `private` field throws unless the declaring class is marked
  `@Reflectable`. Two opt-in paths exist for callers that
  need broader access (see "Access control" below).
- **Class lookup by name.** `Class.forName("com.example.User")`
  resolves a string at runtime to a `Class<?>` for any class
  the binary still contains. AOT-compiled cajeta strips unused
  classes by default, and `@Retained` marks classes that must
  survive even if no static reference reaches them.
- **Fiber-safe reflection.** Reflective invocation respects
  cajeta.thread's scheduling. Invoke runs in the caller's
  fiber, with no thread-hop and no separate "reflection executor."

## Non-goals (v1)

- **Dynamic class generation.** There is no emitting of new classes at
  runtime, no `Proxy.newProxyInstance` equivalent, and no bytecode-
  style emission. Cajeta is AOT-compiled, and runtime codegen is a
  separate and much larger effort that would need a JIT backend.
- **Class loading from external sources.** There is no reading of a
  `.cajeta` file from disk and adding its types to the running process.
  Plugins / hot-reload land later, separately.
- **MethodHandle-style call-site caching.** Java's `MethodHandle`
  and `LambdaMetafactory` provide reflection-fast-path performance
  comparable to direct invocation. v1 ships the slow-but-simple
  `Method.invoke` path, and the fast path is a follow-up.
- **Constant folding of reflective constants.** Reading
  `Color.RED` reflectively returns a real lookup and never a compile-
  time constant. The static-known case, where you would want
  constant folding, just uses the static reference directly.
- **Unsafe memory access.** There is no `sun.misc.Unsafe` equivalent.
  The `UnsafeReflect` class in this design exposes private members
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
references can be stripped by the AOT linker, which is a binary-size
win, so `forName` doesn't see them. `@Retained` (see "Access control")
keeps a class in the registry regardless of static use.

You reach a class via `T.class` (compile-time, for any type) or
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
marshalling. Direct dispatch is one vtable load instead.

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
under a static `heap` allocation. Constructor reflection respects access
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
static methods, because Method.parameterList includes it after
Method::generatePrototype runs, and `Parameter.isThis()`
discriminates. Callers iterating `getParameters()` for a non-static
method typically skip the first one.

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

    // Annotations can be nested, e.g. @Retry(backoff = @Backoff(millis = 50))
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

> **Shipped names are `TemplateParameter` / `TemplateArgument`** (and
> `Class.getTemplateParameter(i)` / `getTemplateArgument(i)` /
> `isTemplateInstantiation()`). The `Type*` spelling and the array-returning
> `getTypeParameters()` / `getTypeArguments()` here are the design target. Map
> them onto the index-based `Template*` accessors when reading current code.

Cajeta retains generic information at runtime, which is a departure from
Java's erasure. A `Box<int32>` instance reports `int32` as its
type argument, and the compile-time substitution lives in RTTI.

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
Box<List<int32>> nested = heap Box<List<int32>>();
Class<?>     cls    = nested.getClass();
TypeArgument outerT = cls.getTypeArguments()[0];   // List<int32>
Class<?>     listCls = outerT.getResolvedType();
TypeArgument innerT = outerT.getTypeArguments()[0]; // int32
String       deepName = innerT.getResolvedType().getName();  // "int32"
```

Generic retention means a JSON serializer can ask `List<int32>`
"what's your element type?" at runtime and pick `Int32.toString`
for each element. Java would fall back to `Object.toString`.

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

> **Design target, differs from what ships.** The `@Reflectable` opt-in and
> `UnsafeReflect` escape hatch below are not implemented. The shipped
> REFL-1 model (see *Shipped surface*) gates on `@Sealed`. A member is blocked
> only when it is `private` and its declaring class is `@Sealed`. The
> annotation-driven model here is the intended evolution.

Defaults are restrictive, and opt-in widens the surface.

**Default.** `Field.get` / `Field.set` / `Method.invoke` /
`Constructor.newInstance` respect declared visibility. Reflective
access to a `private` member from outside its declaring class
throws `IllegalAccessException`.

**Opt-in path A, `@Reflectable` on the declaring class.** This marks
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

**Opt-in path B, `UnsafeReflect` for the framework case.**
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
UnsafeReflect unsafe = heap UnsafeReflect("JSON serializer needs private fields");
for (field in someClass.getDeclaredFields()) {
    Object value = unsafe.getField(field, instance);
    // ...
}
```

The construction-time warning is logged through
`cajeta.thread.log` and surfaces in process telemetry.
Framework authors can use it without surprise, and it is
visible in the audit trail.

**`@Retained`** is orthogonal to access control and addresses
AOT-stripping. Cajeta's AOT linker removes classes no static
code path reaches, which is correct for binary size but breaks
`Class.forName("com.example.PluginX")` when nothing else
references `PluginX`. Marking a class `@Retained` keeps it in
the registry regardless:

```cajeta
@Retained
public class JsonSerializer { ... }      // forName-discoverable
```

Use it sparingly, because `@Retained` defeats stripping for that class
plus everything it transitively references.

---

## Performance

Reflection was one of Java's downfalls. It had first-call costs in the
microseconds, per-call argument-array allocation, JIT inflation
overhead, and it was opaque to the optimizer. None of those costs are
fundamental to reflection as a concept. They were specific
choices Java made for reasons cajeta doesn't share, namely dynamic class
loading, JIT compilation, type erasure, and the security manager. The
design below sidesteps each one.

### Java's specific pain points (none of which apply to cajeta)

1. **JIT inflation.** The first reflective call goes through a slow
   JNI bridge while the JIT decides whether the call site is
   hot enough to "inflate" into generated bytecode. That is tens of
   microseconds on the cold path.
2. **Argument-array allocation per call.** `Method.invoke(obj,
   42, "foo")` allocates an `Object[]` and boxes `42` to
   `Integer.valueOf(42)`. That is two-to-three allocations for a one-
   arg int call.
3. **Symbolic-name lookups at runtime.** `Class.getField("name")`
   is a hash table walk over interned strings. Cajeta knows the
   field's index at compile time and never walks strings on the
   hot path.
4. **No inlining.** Java's JIT can't see through `Method.invoke`'s
   vtable indirection, so reflection blocks escape analysis, scalar
   replacement, and constant propagation.
5. **Per-call accessibility check.** `setAccessible` flags,
   module exports, and the security manager are consulted on every
   reflective access.

### Cajeta's structural advantages

- **AOT-compiled.** There is no JIT inflation. The reflective dispatch
  code is compiled once and lives in the binary like any other function.
- **No dynamic class loading.** Every class in the binary is known
  at link time. There is no "is this class loaded?" check per lookup
  and no class-loader hierarchy to walk.
- **Static memory model.** Field offsets are compile-time
  constants. The compiler can emit per-class adapters that bake
  in the offsets directly, with no JVM-style indirection through a
  reflective `Field.offset` field.
- **No security manager.** Access control is a compile-time
  `@Reflectable` check plus `UnsafeReflect`'s construction-time
  audit log. There is no per-call check.
- **Monomorphic generics with retention.** There are no type-erased
  cast-back-to-Object roundtrips.

### The seven strategies that close the gap

**1. Pre-built integer-indexed RTTI tables.** Compact arrays replace
string-keyed lookups. The compiler knows each field's ordinal index in
its declaring class. `Class.getField("name")` resolves the name to an
index at lookup time, with one perfect-hash probe over a per-class
table, and then returns a `Field` whose internal representation is just
the index. The `Field` carries no string, so subsequent
`Field.get(obj)` calls don't touch a string at all.

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

`Field.get(obj)` dispatches to the adapter via one vtable hop,
because the adapter is reached through the RttiGlobal. Each field
access is one vtable load + one switch + one direct load, which is
the same shape as a virtual method call. That is the floor for a
dispatch target that wasn't statically known. Java pays orders
of magnitude more because the JIT can't see this pattern.

**3. Typed accessors that skip boxing.** These are already in the design
(`Field.getInt32`, `Method.invokeInt64`). For each combination
of (declaring class, field/method, primitive type) the compiler
emits a specialized variant whose body is a direct field load
or call with no `Integer.valueOf`-style boxing in sight.

```cajeta
Field idField #= User.class.getField("id");
int64 id = idField.getInt64(user);   // emits: load + return, no Object
```

**4. `MethodHandle`-style cached call sites, promoted into v1.**
A `Method` produces a `MethodHandle` once via `bindCallSite()`.
The handle is a typed function pointer wrapped in a small struct,
and calling it is one vtable hop + direct call. Java's
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
Method m #= User.class.getMethod("ageDelta", int32.class);
var handle = (MethodHandle_Int32_to_Int64) m.bindCallSite();
int64 r = handle.call(user, 30);   // one vtable hop, no boxing
```

Hot reflection paths (DI containers, ORM mappers, serializers)
build the handle once at framework init and call it directly
forever after. The slow `Method.invoke` path stays available
for one-off reflective calls where building a handle isn't
worth it.

**5. Constant folding of statically-known reflection.** When all
inputs to a reflective call are compile-time constants, such as
`User.class.getField("name").get(user)` with `"name"` as a
literal, the compiler folds the call to a direct field access
in the same pass it does for `String.equals("literal")`. It is the
same machinery, applied to reflective lookups. The caveat is that
this gives up visibility checks at the call site, so the fold only
fires when the access would be permitted anyway.

**6. Fiber-stack argument buffers instead of heap allocations.** When
the argument count is small and the call site is statically
typed (`method.invoke(obj, a, b)` with 2 args), the args
collection uses fiber-stack-allocated storage instead of a heap
`Object[]`. Cajeta's existing alloca path covers this, and the
compiler picks stack vs heap based on count and lifetime. For
large or escaping arg lists, heap fallback is identical to
Java's shape.

**7. Cached `Class<?>` instances.** There is one `Class<?>` per declared
type in the process, reused across every `getClass()` and
`T.class` call. The instance lives in static storage next to
the RttiGlobal, with no allocation. Java does this too in
HotSpot's `Klass::java_mirror`, but the indirection through
the JVMTI machinery hides it from optimization. Cajeta's
single-step access doesn't.

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

The slow path stays available for one-off cases, and the fast paths
(typed accessors, MethodHandles) cover everything inside a tight
loop. Frameworks built on cajeta.reflect can be as fast as
manual code generation, without you writing the manual code.

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
discoverable, general-purpose path. Codegen is the escape hatch
for the hot paths that can amortize compile-time work. Both ship,
and neither is "the answer", so you pick based on the workload.

---

## Worked examples

**JSON-style serializer in 30 lines:**

```cajeta
public String toJson(Object obj) {
    if (obj == null) return "null";
    Class<?> cls = obj.getClass();
    if (cls.isAnnotationPresent("Primitive")) return primitiveToJson(obj);

    StringBuilder out = heap StringBuilder();
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
    var injectMethod #= cls.getMethod("__cajeta_inject");
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

> **Note on the shipped path.** REFL-1 took a data-driven route instead of
> the per-class synthesized adapters of Strategy 2 below. Typed field access
> goes through generic native helpers keyed on `(rtti, fieldIndex)`, namely
> `__cajeta_field_get_i32` / `_set_i32` / … reading at the field's recorded
> byte offset, with no per-class codegen. The `MethodHandle` fast path
> (step 5 / Strategy 4) is not built. The ordering below remains the design
> target for the richer surface.

This is a reasonable order, given the existing RTTI infrastructure. The
v1 scope includes the performance paths described above, meaning
typed accessors, MethodHandle, and the per-class synthesized
adapters, because they're the difference between "Java
reflection" and "useful reflection," and skipping them would
land a slow API that nobody uses.

1. **`Class<T>` + the basic introspection surface.** `getName`,
   `getPackage`, `getModifiers`, `getSuperclass`, `getInterfaces`,
   `getFields`, `getMethods`, `getConstructors`. This reads existing
   RTTI tables, with no codegen changes. The Class registry
   (`cajeta.reflect.registry`) is built from the same data the
   compiler already emits. Per-class integer-indexed lookup
   tables land here (Strategy 1 from "Performance").
2. **Compiler-synthesized per-class reflection adapters.**
   `User_reflect_getField` / `User_reflect_invokeMethod` /
   `User_reflect_newInstance` emitted per class as part of
   normal codegen. Each switches over field/method index, with a direct
   field load / direct call for each case. This is the foundation for
   steps 3-5.
3. **`Field` read/write.** Reflective `get` / `set` plus the
   typed primitive accessors. These dispatch through the per-class
   adapter from step 2. They respect visibility and throw on private
   without `@Reflectable`.
4. **`Method.invoke` + `Constructor.newInstance` slow paths.**
   These reuse the vtable lookup path + per-class adapter. Argument
   marshalling is the new code, with fiber-stack-allocated arg
   buffers for small calls (Strategy 6 from "Performance").
5. **`MethodHandle.bindCallSite`.** Per-signature-shape concrete
   subclasses are synthesized lazily on first `bindCallSite()` call
   for each unique signature in the program. They are cached per shape
   so the binding cost is paid once. This is the fast-path invocation
   that frameworks (DI, ORM, serializers) build at init and call
   forever.
6. **Annotations.** RTTI already carries annotation metadata, since
   AspectModel reads it for advice matching. `cajeta.reflect.
   Annotation` exposes it generally.
7. **Generic retention.** Augment RTTI to record type-parameter
   substitutions per instantiation. The `TypeParameter` /
   `TypeArgument` API reads them. Most generic-aware features
   (JSON serializer dtype inference, ORM column types) become
   reachable here.
8. **`Class.forName` + the `@Retained` marker.** A process-wide
   registry is built at link time from `@Retained` classes plus
   every statically-referenced class. The forName lookup is a
   perfect-hash over canonical name.
9. **`@Reflectable` + `UnsafeReflect`.** These are the access-control
   opt-in paths. UnsafeReflect logs construction-time warnings through
   cajeta.thread.log.
10. **Package / annotation queries.** `Class.classesInPackage`,
    `Class.classesAnnotated`. These start as a linear scan over the
    registry, and become tree- or trie-indexed if it becomes a hot path.
11. **Constant-fold known reflection.** A compiler pass that
    folds `User.class.getField("name").get(user)` to direct
    field access when all inputs are statically known and
    visibility permits. Strategy 5 from "Performance."

Deferred (separate efforts, post-v1):
- The compile-time codegen escape hatch, meaning the `@JsonSerializable`-
  style annotation processor that emits per-class direct-
  dispatch code at compile time. It is useful for workloads where
  even MethodHandle-level overhead is too much.
- Dynamic class generation / proxies.
- Plugin / hot-reload class loading.
- Reflection-emit (writing new classes at runtime).

---

## Open questions

- **Private-access default.** The proposal above defaults
  restrictive, with `@Reflectable` opt-in for private members. The
  alternative is default open with `@Sealed` opt-out, which is more
  Java-like and friendlier to frameworks. Java moved toward
  restrictive over time (the JEP 396 / modules story), and cajeta
  starts there. It is worth pressure-testing against the DI / ORM
  / serializer use cases before locking in.
- **Generic retention overhead.** Every instantiated generic
  class carries an extra few words of RTTI for its type
  arguments. That is negligible for most programs but real for code
  that instantiates many distinct generics. An optional
  `-noGenericRetention` flag could strip the per-instantiation
  RTTI for binary-size-critical builds, at the cost of the
  `TypeArgument` API throwing at runtime.
- **`Class.forName` failure mode.** Should it be strict (throws if the
  class was stripped) or lenient (returns `null`)? Java throws
  `ClassNotFoundException`. The design above provides both
  `forName` (throws) and `forNameOrNull` (returns null). Pick
  one as the primary in documentation.
- **Reflection invocation through fibers.** A reflective invoke
  inherits the calling fiber's scheduling, the same as any
  static call would. The open question is reflectively invoking
  `async`-marked methods, once async lands as a first-class
  feature. It is worth a placeholder design, probably the same
  fiber-pool dispatch async non-reflective calls use.
- **`Object.getClass()` performance.** Every reflective entry
  point starts with a vtable-pointer-to-RttiGlobal load. Java
  caches `Class` instances aggressively, and cajeta should too,
  with one `Class<?>` per declared type, reused on every getClass
  call. It is worth confirming the implementation does this rather
  than allocating per call.
- **Generic methods (not classes).** In `<T> T pick(T[] arr)`
  the type parameter lives only on the method and never on the class.
  RTTI doesn't currently emit per-method type parameters.
  `Method.getTypeParameters()` is in the API but the underlying
  data needs to land. It is worth deciding now whether v1 ships this
  or punts to v2.
