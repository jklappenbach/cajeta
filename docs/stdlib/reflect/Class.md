# Class\<T\>

`cajeta.reflect.Class` — runtime representation of a class, the entry point to
`cajeta.reflect`. Obtain one via `Class.of(obj)` (the object's dynamic type)
or `T.class` (a statically-known type); each is a borrow of a process-lifetime
cached instance — one `Class` per declared type, emitted by the compiler next
to the type's RTTI — that the caller never frees. From it you can read
identity and modifiers, enumerate fields, methods, constructors, template
parameters/arguments, and annotations, read and write primitive fields,
construct instances reflectively, and query the process-wide class registry.

```cajeta
ArrayList<int32> xs = heap ArrayList<int32>();
Class<?> c = Class.of(xs);
String name #= c.getName();       // fully-qualified canonical name
int32 nFields = c.getFieldCount();
int32 nMethods = c.getMethodCount();
```

## Methods

| Signature | |
|---|---|
| `static Class<?> of(Object o)` ⚑ | The runtime `Class` of `o` (its dynamic type) |
| `static Optional<Class<?>> forName(String name)` ⚑ | Resolve a canonical name (e.g. `"cajeta.lang.String"`) to its cached `Class`; empty when unregistered — primitives never resolve |
| `static Optional<T> heapInstance<T>(String name, Class<?> bound)` ⚑ | Bounded by-name allocation: resolve `name`, verify it is a subtype of the bound, construct via the no-arg constructor; empty on unknown name or non-subtype (the bound is injected by the compiler from the `<T>` token) |
| `#String getName()` | The fully-qualified canonical class name |
| `int64 getInstanceSize()` | Heap size of an instance in bytes |
| `int32 getModifierFlags()` | Raw modifier bit field for the class |
| `boolean isPublic()` | Class has the `public` modifier |
| `boolean isFinal()` | Class has the `final` modifier |
| `#Modifiers getModifiers()` | The modifiers as a typed `Modifiers` value |
| `int32 getFieldCount()` | Number of declared fields |
| `#String getFieldName(int32 index)` | Declared field name at `index` (0-based); empty if out of range |
| `int32 getFieldModifierFlags(int32 index)` | Raw modifier bit field for the field at `index` |
| `int32 getFieldOffset(int32 index)` | Byte offset of the field within an instance, or -1 for a static field |
| `int64 getFieldTypeFlags(int32 index)` | The field's type-flag word (size, int-vs-float, signed, primitive-vs-reference) |
| `#Field getField(int32 index)` | The declared field at `index` as a `Field` object |
| `int32 getInt32(Object o, int32 index)` | Read the int32 field at `index` from instance `o` |
| `void setInt32(Object o, int32 index, int32 value)` | Write the int32 field at `index` of instance `o` |
| `int64 getInt64(Object o, int32 index)` | Read the int64 field at `index` |
| `void setInt64(Object o, int32 index, int64 value)` | Write the int64 field at `index` |
| `boolean getBoolean(Object o, int32 index)` | Read the boolean field at `index` |
| `void setBoolean(Object o, int32 index, boolean value)` | Write the boolean field at `index` |
| `float32 getFloat32(Object o, int32 index)` | Read the float32 field at `index` |
| `void setFloat32(Object o, int32 index, float32 value)` | Write the float32 field at `index` |
| `float64 getFloat64(Object o, int32 index)` | Read the float64 field at `index` |
| `void setFloat64(Object o, int32 index, float64 value)` | Write the float64 field at `index` |
| `#Object getBoxed(Object o, int32 index)` | Read field `index` of `o` as a uniform owned `#Object`, boxing a primitive into its `cajeta.lang` wrapper |
| `int32 getMethodCount()` | Number of declared methods |
| `int32 getMethodParamCount(int32 index)` | Number of declared parameters of the method at `index` |
| `#String getMethodName(int32 index)` | Canonical signature of the method at `index` (e.g. `"bump()"`) |
| `#Method getMethod(int32 index)` | The declared method at `index` as a `Method` object |
| `static int64 invokeScalar0(Object o, int32 index)` | Reflectively invoke the no-argument method at `index` on `o`, result widened to int64 (`void` yields 0) |
| `int32 getParentCount()` | Number of direct superclasses (1 for most classes, 0 for Object) |
| `int32 getConstructorCount()` | Number of declared constructors |
| `int32 getConstructorParamCount(int32 index)` | Number of declared parameters of the constructor at `index` |
| `#Constructor getConstructor(int32 index)` | The declared constructor at `index` as a `Constructor` object |
| `#Object heapInstance(int32 index)` | Reflectively construct a new instance via the no-argument constructor at `index`; the result is owned |
| `int32 getTemplateParameterCount()` | Number of declared template parameters (`class Box<T>` → 1) |
| `#TemplateParameter getTemplateParameter(int32 index)` | The declared template parameter at `index` |
| `int32 getTemplateArgumentCount()` | Number of concrete template arguments this class was instantiated with (`Box<int32>` → 1) |
| `#TemplateArgument getTemplateArgument(int32 index)` | The concrete template argument at `index` |
| `boolean isTemplateInstantiation()` | True when this class is a materialized template instantiation |
| `int32 getAnnotationCount()` | Number of annotations declared on this class |
| `#String getAnnotationName(int32 index)` | Canonical name of the class annotation at `index` |
| `#Annotation getAnnotation(int32 index)` | The class annotation at `index` as an `Annotation` object |
| `boolean hasAnnotation(String name)` | Whether this class declares an annotation named `name` |
| `static #Class<?>[] allClasses()` | Every class the binary retains, as `Class` borrows |
| `static #Class<?>[] classesInPackage(String packageName)` | Every retained class whose package equals `packageName` |
| `static #Class<?>[] classesAnnotated(String annotationName)` | Every retained class declaring an annotation with that canonical name |
| `static #Class<?>[] subtypes(Class<?> bound)` | Every retained class that is a subtype of (or is) the bound — the closed-world subtype closure |

⚑ = `@EntryPoint`

## See also

- Tour: [ReflectionDemo](../../../samples/tour/src/main/cajeta/tour/lang/ReflectionDemo.cajeta)
- Source: [`runtime/src/cajeta/reflect/Class.cajeta`](../../../runtime/src/cajeta/reflect/Class.cajeta)
