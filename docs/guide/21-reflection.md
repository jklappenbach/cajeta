# 21 — Reflection

Every class carries fixed-layout RTTI: name, fields, methods, constructors,
parameters, annotations. `cajeta.reflect` reads it at runtime. Access is
data-driven — classes you never reflect over pay no per-class codegen tax.

Tour demo: [ReflectionDemo](../../samples/tour/src/main/cajeta/tour/lang/ReflectionDemo.cajeta).

## Getting a Class

`Class.of(obj)` returns the cached `Class` for an object's dynamic type:

```cajeta
import cajeta.reflect.Class;

public class Probe {
    public void describe(Object o) {
        Class<?> c = Class.of(o);
        System.stdout.println(c.getName() + ": "
            + c.getFieldCount() + " field(s), "
            + c.getMethodCount() + " method(s)");
    }
}
```

`Class.forName(name)` looks a class up by canonical name in the process-wide
registry, returning an empty `Optional` on a miss:

```cajeta
import cajeta.lang.Optional;
import cajeta.reflect.Class;

public class Finder {
    public int32 fieldsOf(String name) {
        Optional<Class<?>> c = Class.forName(name);
        if (c.isPresent()) {
            return c.get().getFieldCount();
        }
        return -1;
    }
}
```

The registry also answers bulk queries: `Class.allClasses()`,
`classesInPackage(...)`, `classesAnnotated(...)`, and `subtypes(bound)`.

## Members, instantiation, invocation

`Class` hands out `Field`, `Method`, `Constructor`, and `Parameter` objects.
Constructing reflectively is always heap — the type isn't known at the call
site, so no stack allocation is possible:

```cajeta
import cajeta.reflect.Class;
import cajeta.reflect.Constructor;
import cajeta.reflect.Field;

public class Widget {
    public int32 v;
    public Widget() {
        this.v = 0;
    }
}

public class Cloner {
    public int32 cloneWidget() {
        Widget seed = heap Widget();
        Class<?> c = Class.of(seed);
        Constructor ctor #= c.getConstructor(0);
        Object obj #= ctor.heapInstance();
        Field f #= c.getField(0);
        f.setInt32(obj, 42);
        return f.getInt32(obj);
    }
}
```

Methods invoke through packed scalar arguments (`m.invokeScalar(obj, args)`
with an `int64[]`), and every annotatable owner — class, field, method,
constructor, parameter — reports its annotations, including argument values
(`a.getInt("value")`, `a.getString("value")`, list elements). The
[tour demo](../../samples/tour/src/main/cajeta/tour/lang/ReflectionDemo.cajeta)
walks all of it.

## Reflection and lean linking

The default executable link (`--link-mode=lean`) strips classes nothing
references. Reflection sites are roots in that analysis: the compiler builds
an embedded keep-set from them, so `Class.forName("app.Plugin")` with a
string literal keeps `app.Plugin` alive, `classesInPackage(...)` keeps the
package, `classesAnnotated(...)` keeps the annotated classes, and
`subtypes(bound)` keeps the bound's subtype closure. `@Retained` pins a class
unconditionally. A selector the compiler can't narrow (a computed `forName`
string, say) forces keep-all, with a warning pointing at the site.

When you're unsure why a class survived — or why `forName` can't find it —
ask the linker:

```bash
$ cajeta --emit=exe --why-kept=app.Plugin app.Main.run src/ out/
why-kept: app.Plugin — kept by forName("app.Plugin")
```

`--keepset-json=<path>` dumps the whole keep-set with per-class provenance.

Full surface: [the reflection specification](../specification/reflect/Reflection.md)
and [annotations](../specification/reflect/Annotations.md).

Next: Part III — the standard library. See the [guide index](README.md).
