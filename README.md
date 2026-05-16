# Cajeta

Cajeta is a computer language that is a hybrid of C++ and Java, incorporating some of the memory management philosophies 
of Rust.  Like Java, all objects are allocated from the heap, and creating an object involves a transparent stack variable,
called a "stack reference", that maintains the pointer to heap memory, as well as the object metadata of ownership and 
the actual type for polymorphism.  A actual reference may be to a base type, but the allocation was of a child type.  
The stack reference memory will carry the information as to the actual type.

Like Java, Cajeta will feature only pass-by-value parameters to methods.  Even object references are passed by value.  If 
an object reference is passed into a method, whereby it is assigned a new value, the calling scope will remain unchanged.
Updating the object memory that reference in the callee scope, however, will be observed in the calling scope as both
references will point to the same heap reference.

For memory management, instead of reference counting, "smart" pointers, Cajeta is based on a simplified borrower / owner
strategy whereby only one reference can own heap memory allocations, and when it falls out of scope the memory is 
automatically allocated.

## Building

The compiler is configured via CMake and built with Ninja. Two scripts wrap the typical flow:

```sh
./setup.sh    # one-time: runs cmake into build/, picks LLVM from LLVM_DIR env or default /usr/lib/llvm-18
./build.sh    # incremental ninja build; runs every time you change code
```

### Build prerequisites

System packages (Ubuntu 24.04 names — adapt for other distros):

```sh
sudo apt install \
    cmake ninja-build clang-18 llvm-18-dev libllvm18 \
    libantlr4-runtime-dev openjdk-17-jre \
    libgtest-dev libglog-dev libzstd-dev vim-common
```

Notes:
- `clang-18` is required at compiler-build time to compile `runtime/native/cajeta_runtime.c` to
  LLVM bitcode, which is then embedded into the Cajeta compiler binary.
- `vim-common` provides `xxd`, used to convert the bitcode bytes into a C array.
- `openjdk-17-jre` is needed to run the bundled ANTLR4 jar in `tools/antlr/`.
- To target a different LLVM version, set `LLVM_DIR` before running setup, e.g.
  `LLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm ./setup.sh`.

### Build outputs

- `build/src/cajeta` — the compiler binary.
- `build/test/cajeta_test` — the test executable.
- `build/src/cajeta_runtime.bc` — the C runtime compiled to LLVM bitcode.
- `build/src/cajeta_runtime_embedded.cpp` — generated C array of the bitcode bytes,
  linked into the compiler. The compiler links this bitcode into every output module
  via `CajetaModule::linkRuntime()` so cross-compilation needs no per-target runtime.

## Running the tests

Tests are written with GoogleTest. The fixture in `test/jit/JitTestHelper.{h,cpp}` compiles a
Cajeta source string, links the embedded runtime into the produced LLVM module, JITs it via
`llvm::orc::LLJIT`, and exposes generated functions to the C++ test as callable pointers.

> **Do not invoke `c++` / `clang++` directly on `test/main.cpp`.**
> The test executable depends on libgtest, the Cajeta static library, the ANTLR4 runtime,
> and roughly a hundred LLVM static libs — the CMake build assembles that link line. A direct
> compile produces the error you'd expect: `undefined reference to testing::InitGoogleTest`,
> `undefined reference to testing::UnitTest::Run`, etc.
>
> If your IDE (CLion, VS Code, etc.) is trying to compile `main.cpp` as a standalone file,
> point it at this repo's `CMakeLists.txt` instead — that's the only supported build path.
> Always build the test target via:
>
> ```sh
> ./setup.sh      # once, to generate build/
> ./build.sh      # every time you change code
> ```
>
> The test binary lands at `build/test/cajeta_test`.

```sh
# Run the full suite. CAJETA_SOURCE_ROOT lets the parse-flow tests locate sample .cajeta files.
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test

# Compact output (one line per test result).
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test --gtest_brief=1

# Filter to specific suites or tests (gtest syntax).
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test '--gtest_filter=BinaryOpTests.*'
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test '--gtest_filter=CompareTests.lessThanTrue'

# Skip the parse-flow tests (CompilerTests) when iterating on expression codegen.
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test '--gtest_filter=-CompilerTests.*'
```

### Debugging tip

Set `CAJETA_DUMP_IR=1` to have the JIT helper print the generated LLVM IR to stderr just
before passing it to the JIT. Useful when a test fails verification:

```sh
CAJETA_DUMP_IR=1 CAJETA_SOURCE_ROOT="$PWD" \
    ./build/test/cajeta_test '--gtest_filter=BinaryOpTests.intAdd'
```

### Test suites

| Suite                       | What it covers                                                |
| --------------------------- | ------------------------------------------------------------- |
| `CompilerTests`             | End-to-end parse-flow tests on `.cajeta` files in `test/compile/code/src/` |
| `LiteralExpressionTests`    | Integer / float / bool literal codegen                        |
| `BinaryOpTests`             | `+ - * / %`, bitwise, shifts, compound `*=` forms             |
| `CompareTests`              | `< <= > >= == !=` over ints and floats                        |
| `LogicalTests`              | Short-circuit `&&` and `\|\|`                                 |
| `PrefixTests`               | Unary `+ - ~ !` and `++x / --x`                               |
| `PostfixTests`              | `x++ / x--`                                                   |
| `CastTests`                 | int↔float and width-changing casts                            |
| `TernaryTests`              | `cond ? a : b` with int/float/coercion/nested/side-effect     |
| `InstanceOfTests`           | Compile-time `instanceof` static-type matching                |
| `UnsupportedExpressionTests`| Lambda / super / inner-class / method-ref / explicit-template-invocation throw NOT_IMPLEMENTED |
| `SwitchExpressionTests`     | Java 17 arrow-form switch expressions with multi-label cases and a default arm |
| `ArrayTests`                | `new T[n]`, indexing, `arr.size()`, bounds checks, nested `T[][]` |
| `ControlFlowTests`          | if/else, while, for, do-while, break, continue, nested loops, early-return |
| `ForInitDeclTests`          | Loop-variable declaration inside `for (...)` init |
| `EnhancedForTests`          | `for (T x : arr)` and Cajeta-extended `for (int i, T x : arr)` with break/continue |
| `SwitchTests`               | switch/case/default with fall-through and no-match |
| `TryCatchTests`             | throw / try / catch via setjmp/longjmp runtime |
| `SystemIoTests`             | `System.{stdout,stderr,stdin}.{print,println,printf}` with SLF4J-style `{}` templating, `+` String concatenation with auto-stringified primitives |
| `StringMethodsTests`        | `String.{size,length,isEmpty,equals,charAt,indexOf,startsWith,endsWith,contains,substring,toUpperCase,toLowerCase,trim,replace}` |
| `MathIntrinsicTests`        | `Math.{abs,max,min,sqrt,pow,floor,ceil,round,sin,cos,tan,log,log10,exp}`, `Math.{PI,E}`, `Integer.{MAX,MIN}_VALUE` |
| `CharLiteralTests`          | Single-quoted `char` literals — ASCII, escape sequences (\n, \t, …), octal, `\\uXXXX` |
| `ConversionIntrinsicTests`  | `Integer.parseInt`, `Long.parseLong`, `Double.parseDouble`, `Boolean.parseBoolean`, `*.toString`, `String.valueOf` |
| `NullHandlingTests`         | null assignment / comparison and null-safe String built-ins |
| `SystemUtilTests`           | `System.{exit,currentTimeMillis}`, `Math.random` |
| `CompilerOptionTests`       | `--bounds=off` toggle, `--emit` mode round-trip, target-triple setter |
| `FpTests`                   | fp32/fp64 arithmetic, fp16 declare/store, fp4/fp6/fp8 storage |

#### Memory model + wire formats (Sessions 1–3.5 of the memory-model rollout — see `cajeta-docs/history/ImplementationStatus.md`)

| Suite                       | What it covers                                                |
| --------------------------- | ------------------------------------------------------------- |
| `Session1ParseTests`        | Parser-level recognition of `#` in three positions, `struct` keyword, `@BigEndian` / `@LittleEndian` / `@Align` annotations; invalid syntax rejected |
| `UseAfterMoveTests`         | `#` transfer marks the source moved; subsequent reads throw `CAJETA_ERROR_USE_AFTER_MOVE` |
| `DropChainTests`            | Owned-array drops fire on normal return, are suppressed on move-out, and unwind on `throw` |
| `ElisionTests`              | Multi-parameter free functions can't return a borrow (`CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM`); single-param / `#`-returning / primitive-returning signatures pass |
| `PathBorrowTests`           | `#person.name` records the path; reads through that path (or any prefix) are rejected |

#### Aggregate types — views + structs + interface dispatch (Sessions 1–12 of the struct/view rollout — see `cajeta-docs/history/StructsViewsStatus.md`)

| Suite                       | What it covers                                                |
| --------------------------- | ------------------------------------------------------------- |
| `KeywordEquivalenceTests`   | `view` and `struct` keywords compile through their own codegen paths; positive pin for declare + use of each (S1, S2, S6) |
| `StructViewTests`           | View declaration, `MyView(byte[])` borrow construction, field reads/writes through the view (legacy view tests, migrated to `view` keyword in S1) |
| `StructViewBoundsTests`     | View constructor's `count * elem_size >= sizeof(view)` check fires on undersize buffer, catchable via `try/catch` |
| `EndianAlignTests`          | `@BigEndian` emits `bswap` on field access (verified by reading the same bytes through a host-order view); `@LittleEndian` on a little-host is a no-op |
| `VariableSizeStructTests`   | Inline `String` field reads materialize an owned copy; reassignment rejected; layout rule (variable-size must be last) enforced |
| `ViewOwningTests`           | Owning-form view (`MyView(#buf)`) takes ownership of the buffer and drops it at scope exit; borrow form rejects escape; owning form allows escape (S3) |
| `ViewEndiannessRequiredTests` | View declarations must carry `@BigEndian` / `@LittleEndian` / `@HostEndian` — missing annotation is rejected at layout time (S3) |
| `ViewMethodsTests`          | Methods declared on views compile and dispatch directly (no vtable); intra-method `this.field` reads and writes work (S4) |
| `NestedViewTests`           | Views containing other views inline at layout; per-view endianness independent across nesting; direct-recursion rejected (S4) |
| `MultiVarSizeViewTests`     | Multiple trailing variable-size fields per view; per-field walk-the-prefixes access; length-prefix attacks throw at construction (S5) |
| `PostVariableFieldTests`    | Fixed-size fields after a variable-size field; `T[]` view fields with element round-trip (S5b) |
| `StructStackTests`          | Stack-allocated structs with primitive and class-ref fields; aggregate-initializer syntax; drop chain fires owned-class-ref drops in reverse declaration order; use-after-move on struct fields; struct returns by value (S6) |
| `StructCompositionTests`    | Structs embedded inline as class fields; drops recurse through embedded structs in reverse declaration order; field access through chained GEPs; path-borrow through embedded struct fields (S7) |
| `StructMethodsTests`        | Methods on structs (direct calls only — aggregates skip vtable dispatch); mutating methods, intra-struct dispatch, struct-returning methods, struct-by-value parameters (S8) |
| `StructInterfaceTests`      | `struct implements Iface { ... }` — vtable synthesis per (struct, iface) pair; signature mismatch and missing-method rejected; interface-method-level generics rejected at declaration (S9) |
| `StructInterfaceDispatchTests` | Struct → interface assignment (BORROWED_STRUCT), class → interface (BORROWED_CLASS / `#` for OWNED_CLASS); kind-tag dispatched drop chain; struct-rooted interface value escape rejection (S10) |
| `FatPointerDispatchTests`   | Through-interface dispatch lands on the correct struct + class implementers; same-concrete-type return through dyn dispatch rejected at call site; mixed class/struct dispatch is uniform; vtable method-indexing pinned across multi-method interfaces (S11) |

- Type
    - Number
        - Integer
            - int8
            - uint8
            - int16
            - uint16
            - int32
            - uint32
            - int64
            - uint64
            - int128
            - uint128
        - Float
            - float16
            - float32
            - float64
            - float128
    - Class / User Defined

This hierarchy 

## Types

All values in Cajeta are Types.  Another way of stating this is that ``cajeta.lang.Type`` is the base "class" for all
types in the Cajeta language.  Types include all "native" types, as well as Objects, and 

All types should have methods for:

- toString

## Number extends Type

Numbers are a Type representing all "native" type values in the language, from int8 to float 128.  The Number type can be
used by developers as an abstract representation.

Number inherits the Type method toString, and adds:

- static parse*

There will be a static parse implementation for each type, allowing Strings to be converted to numbers

## Object extends Type

All Classes implicitly derive from Object.  The methods implemented by Objects are:
- '=='
- hashCode
- toString
- clone
- getClass
- ~ (destructor)

### Class Identity Contract

Like Java, Cajeta will have the hashCode and equals ('==') to override so that developers can fine-tune the behavior of 
their objects in collection classes like HashMap or HashSet, where a hashcode is used to distinguish objects.  Also, 
the default hashCode implementation may not be rigorous enough to prevent collisions.  With hashCode, this can be 
replaced with a more expensive and rigorous algorithm (xxHash, Murmur).  

The method ``equals`` is not necessary as classes will have operator overloading, supporting the '==' operator.  Initially,
'==' will return true if the object has the same instance memory, or if the object is a reference to the owning instance.

### Memory Management

Objects can have multiple references, but only one owner.  This ownership is established, by default, by the initial allocation
assignment.  When this reference falls out of scope, the memory associated with it is deallocated.

```c
    {
        Foo foo = new Foo();
        ...
    } // foo has fallen out of scope and is deallocated
```

Ownership is transferred automatically through the assignment operator, or by passing the instance as an argument:

#### Ownership Examples

```c
    void main(String[] args) {
        Foo foo;
        {
            Foo bar = new Foo();
            foo = bar;
        } // foo is owner, no deallocation
    } // foo falls out of scope, memory is finally freed
    
    class AClass {
        Map<uint32, Foo> myMap = new HashMap<>();    
        void someMethod() {
            Foo bar = new Foo();
            myMap[1] = bar;
        } // ownership is assigned to myMap, no memory freed   
    }

    class AnotherClass {
        Foo foo;
        public void someMethod(Foo foo) {
            this.foo = foo;
        }
    }
    
    class YetAnotherClass {
        AClass aClass = new AClass();
        public void anotherMethod() {
            Foo foo = new Foo();
            aClass.someMethod(foo); // Ownership is transfered to AClass.foo 
        }
    }
```

#### Simple Scope Example

```c
    void main(String[] args) {
        Foo foo;
        {
            Foo bar = new Foo();
            foo = #bar;
        } // bar is deallocated.  foo now points to null
        foo.doSomething();  // ERROR
    } 
```

#### Assignment Example

```c
    class AClass {
        Foo bar;
        void someMethod() {
            Map<uint32, Foo> myMap = new HashMap<>();    
            bar = new Foo();
            myMap[1] = #bar;
        } // ownership maintained by AClass.  myMap only possesses a reference.  When myMap is deallocated, bar will remain. 
    }
```
The simple scope example here also demonstrates what can happen when ownership isn't managed correctly.  It is up to the compiler
to keep track of when objects are deallocated, and make it clear to the developer when references are incorrectly used, where 
the actual deallocation occurred, and how to fix the problem.

#### Orphan Detection & Mitigation

In assignments, the compiler shall evaluate both the target and source and will flag a warning if it is detected that neither
own a memory reference if either were originally an owner.  At the point that a memory allocation no longer has a reference, the 
compiler shall issue a free on the block.  But the developer should be informed that this is occurring.  Here's an example:

```c
    class AClass {
        Foo bar = new Foo();
        void someMethod() {
            bar = new Foo();  // <-- This will generate a free on bar's initial allocation, and a warning
            ...
        }
    }            
```

### Class Destructor

Unlike Java, Cajeta will have destructors.  These are not necessary to facilitate the freeing of memory associated with 
the class or its contained objects.  Rather, this is to allow a class to free resources from dependencies upon destruction.  
This can include operating system handles, resources maintained by frameworks, engines, and libraries, etc.  Destructors are
only called for the references that own the memory associated with the allocation.

### Class Operator Overloading

Classes support operator overloading, using the same syntax as C++, where the form is:
```c
    class MyClass {
        public bool operator==(MyClass myClass);
    }
```
The following operators can be overridden:
- ==
- !=
- =
- \+
- \+=
- \-
- \-=
- \*
- \*=
- \
- \=
- ^
- ^=
- %
- %=
- &
- &=
- \--
- \++
- !
- |
- |=
- \>>=
- <<=
- \>>>=
- ~
- \>
- \>=
- <
- <=

Mono operators are implemented without an argument, and operate on the local object.  Binomial operators accept a Type,
which may anything that the developer desires to interop with the target class.  Unfortunately, global operators aren't
supported, which means that the arguments in an binomial expression will be order dependent.  Thus,```A + B``` is not 
equal to  ```B + A```, as the former must have an overload on A for ```A.operator+(B b)```, and the latter will not 
compile unless ```B.operator+(A a)```has been provided.

To provide for more efficient compiler operation, the compiler will maintain maps of any overridden operators for each class, and will leverage this lookup
during expression processing, invoking the operator instead of leveraging the standard IR emitters for the operator.

#### Assignment Operator Overloading

Cajeta, like Java, differs from C++ in how objects are managed.  In C++, objects can be allocated on the stack or using the 
new operator from the heap.  In stack based allocations, the default assignment operator executes a shallow copy.  For
heap based objects, developers can deal directly with pointers, or dereference them to invoke the assignment operator.

In Cajeta, all objects are allocated from the heap, with a local component that maintains the pointer to the heap memory,
as well as object metadata (class type, ownership).  When an assignment between two objects occurs, the l-value's stack
component is updated to the r-value's pointer and metadata.  If the l-value previously owned its own heap memory, that is 
immediately freed. 

While the semantics of the two languages are similar, the underlying mechanics are completely different.

Because of this, no overloading will be permitted on the assignment operator for homogenous assignments, including valid
polymorphic casts.  

For cases where it is permitted, the operator will return an instance of the callee, allowing operator execution to
participate in chaining.

## Templates

Cajeta offers true templates, akin to what C++ offers — not Java-style generics.  Templates accept any type as a
parameter, including all primitives.  Each class declaration involving a template produces a new implicitly
defined type with the template parameters substituted; there is no type erasure, and template-produced types
are present at runtime as first-class concrete types.

Template syntax resembles Java's `<T>` parameter declarations, allowing the developer to limit the scope of
class types used in the template to an inheritance hierarchy (`<T extends Foo>`).

**Templates are class-level only.** A method, constructor, operator overload, or interface method cannot
declare its own type parameters (the `<U> U someFunc(U x)` shape).  Cajeta methods are all virtual — like
Java — so the vtable has to be laid out at class-instantiation time, with one slot per method.  A method
template would need a vtable slot per (method, type-argument-list) pair, but the call site has no way to
tell the receiver's vtable which slot to install without runtime type-argument metadata, which contradicts
the monomorphization-per-instantiation design.  C++ forbids template virtual methods for the same reason.

Class-level type parameters can be used freely in method signatures, though: `class Box<T> { T value(); }`
is fine — `T` is bound by the enclosing class, not by the method, and gets substituted at
class-instantiation time.

The following demonstrates basic usage:

### Class Templates

```c
    public class TemplateClass<T> {
        LinkedList<T> list = new LinkedList<>();
        public void doSomething(T argument) {
            list.add(argument);
        }
        public static void main(String[] args) {
            TemplateClass<?> aClass = new TemplateClass<String>();
        }
    }
    
    public class AnotherClass() {
        public void doSomethingElse() {
        }
    }
            
    public class AnotherTemplateClass<T extends AnotherClass> {
        public void someMethod(T argument) {
            argument.doSomethingElse();
        }
    }
    
    public class ConcreteClass extends AnotherTemplateClass<AnotherClass> {
        public void someOtherMethod() {
        }
    }
    
    public class AppClass {
        private MyTemplate<ConcreteChildClass> myInstance = new MyTemplate<>();
        private ConcreteClass concreteClass = new ConcreteClass();
    }
```

### Method Templates

Method templates will be achieved without leveraging the creation of new types.  Instead, support will be in the form of 
a cast to either a Type for unscoped templates, or the filtering class for scoped templates.  Only one version of the 
method will be created regardless of how many different types are used.  Instead, the method will be compiled with a 
template parameter of either the base ``cajeta.lang.Type``, or the type specified in the template scope.  Any variables 
passed in will be cast from their type to Type / Template Scope.  Similarly, any return values will be automatically
converted back to their appropriate type.

```c
    public class AppClass {
        public <T> T someMethod(T argument) {
            ...
        } 
    }
```

### Template Wildcards

To provide flexibility, wildcard templates are supported using the '?' character.  Wildcards support inheritance scoping
with the extends keyword.  It's arguable that most cases where a scoped wildcard is used, simple polymorphism would be 
better, but wildcards do provide value in certain use cases.

```c
    public class AppClass {
        private Class<?> someClass;
        public <? extends MyClass> myClass;
```

#### Template Compiler

In order to facilitate templates, we'll need the ability to create a new class based on the template with all the template
elements substituted.  Types for the class will be defined in the template module.  These classes will be created by the 
compiler each time a unique use of the template is made.  The naming convention will include the list of parameters and 
their assignments for a particular instance:

```com.mydomain.myapp.component.MyClass<T=MyTemplateType,V=uint32>```

## Arrays

Open Question:  How will arrays be implemented?  How do various languages handle them:

- CLang Arrays
  - Can be allocated on the stack, or on the heap
  - Either approach uses fairly similar syntax
- C++ Arrays
  - Includes basic arrays, but includes STL and Boost collections
- Java
  - Arrays are basically special classes, always allocated on the heap
  - We also have Array collection hierarchy (Array, ArrayList)

For novice developers, having more than one array implementation can be confusing.  With a memory managed language that
doesn't support pointers, its awkward to incorporate structures as stack variables.  For example:

```c
    public void someMethod() {
        Structure a();
        Structure b = new Structure();
        a = b; // Error, we can't assign a heap variable to a stack variable
    }
```
But why is this troublesome?  A heap variable will have a stack component as well as heap memory.  The variable, 'a', would 
have only a stack component containing the memory.  A developer would have little indication that 'b' was a heap variable
and was unassignable to 'a' or vice-versa, from the syntax alone.  A mixed approach would also introduce collateral like
requiring two entirely different string processing libraries.

Since a heap based array is really the most feasible, Cajeta will focus on only collection library based arrays.  For cases
where the compiler needs arrays for strings or lists of numbers, LLVM has built in array types.

## Compilation

Cajeta will feature several compilation modes to facilitate the various use cases it will be designed to serve:

### Compilation Dimensions

- Encoding Output
    - IR
    - Binary
- Release Level
    - Debug
    - Release
- Archive Format
    - Archived
    - Exploded

#### Compilation Encoding

The default encoding format will be the conversion of code to IR, which can then be processed by LLVM's JIT compilers to
execute on any supported environment.  IR format compilation will have a release format in the form of a compressed archive
using LZ77, or whatever is current best of breed.  In the archive hierarchy, generated IR modules will be stored in a hierarchical
structure mirroring their location in the source tree.  Any non-source files will be included in the archive.

Binary compilations will be targeted to a specific computing environment (architecture and operating system), and will
be a single binary application file capable of execution in that environment.

#### Release Level

All release levels will share the following metadata bound to execution to support useful stack traces:
- Line number of code associated with binary execution
- Class and method metadata associated with binary execution

The default release format will be Debug, which will include the following artifacts in compilation:
- Memory allocation tracking: for each allocation, track the code ref (module and line) that allocation occurred
- Reference passing tracking (optional): each time a reference is passed, the previous instance and code ref is recorded

See the following for LLVM's strategy for storing debugging information: https://llvm.org/docs/SourceLevelDebugging.html

In addition, debug releases may differ on the optimizations that can be performed for IR passes.  IR optimizations are
directly supported in the compiler SDK, and range from dead-code elimination to optimizations for specific architectures.
These, of course, should be configurable.

### Compilation Responses

In response to events, developers will have the following notifications:

#### Errors

Errors will have the Class, Method, Line Number, and Column associated with compilation errors.  In compilation, this
information will be provided by ANTLR4, with the node information passed down the call graph.  For runtime,

## Class Metadata

Each class will have a global variable, a structure, declared in its module with fields that can be
used for instantiation, including:
- Amount of memory to allocate
- A list of properties, including all metadata
    - Annotations
    - Modifiers
    - Type
- A list of methods
    - All methods defined within the class
- A VirtualTable, populated with
    - All public and protected methods in its parents
    - All public, private, and protected methods in the current class
    - Process of collation will overwrite entries from parents with method of child where it exists
- Able to generate a cajeta.Structure from metadata and vice-versa

All of this metadata, save the VirtualTable, will be available via "Reflection" API

### Metadata Structure

#### Basic Metadata Types

- String
    - length: uint16
    - data: char[]
- Type
    - name: String
    - parentCount: uint8
    - parents: Type[]
- Parameter
    - type: Type
    - name: String
    - annotationCount: uint8
    - annotations: Type[]
- Argument
    - name: String
    - value: String
- AnnotationInstance
    - type: Type
    - argumentCount: int8
    - arguments: Argument[]
- Import
    - type: Type
- Property
    - type: Type
    - name: String
    - annotationCount: uint8
    - annotations: AnnotationInstances[]
- Method
    - name: String
    - returnType: Type
    - annotationCount: uint8
    - annotationInstances: AnnotationInstances[]
    - parameterCount: uint8
    - parameters: Parameter[]
- Function
    - type: [Local|Remote]
    - name: String
    - pointer: Function*
- VirtualTable
    - functionCount: uint8
    - functions: Function[]

#### Class Metadata

1. allocationSize: uint64
2. type: Type
3. annotationCount: uint8
4. AnnotationInstance[]
5. importCount: uint16
6. imports: Import[]
7. propertyCount: uint8
8. properties: Property[]
9. methodCount: uint8
10. methods: Method[]
11. virtualTable: VirtualTable

## Compilation Phase

When Compiling, the class path is first scanned for all available cajeta archives.  For each:
- Iterate through the archive format, and for each entry in the archive:
    - If configured, use a certificate to verify the application signature against the archives
        - Stored in the root directory, as signature
            - Signed MD5 hash of the published archive (without the inserted signature)
    - Construct the package string for the module and map:
        - String of package.Class to filePath/archivePath
- Compilation starts with the main class
    - Indicated through compiler cli argument (multiple mains allowed per archive)
    - Start with main class
        - Load @AspectImport chains
            - This will define the active aspects for the compilation context
            - Unreferenced Aspects will be ignored, enhancing security and control
            - Alternate, debug / mocked
        - Load all class imports recursively (check for cycles)
            - If the class is uncompiled:
                - Parse to cajeta.Structure
                - Process Aspects
            - If the class is compiled:
                - Load module through LLVM
                - Parse module globals into cajeta.Structure
                - Process Aspects
        - Process the main method
            - Handle @Interception if method signature matches @Aspect target
            - Resolve method calls
                - Local methods through standard linking in IR
                - Remote module methods through external linkage in IR (https://groups.google.com/g/llvm-dev/c/3MKelrUEq7A)
            - Convert logic to LLVM IR
        - Write LLVM IR for each module to:
            - Exploded: a corresponding path in a target path
            - Archive: a corresponding path in a target archive (7z)
            - CompoundArchive: a corresponding path in a target archive (7z), along with all files from all referenced jars
                - Only include jars with active imports in the compilation process

## Class Loader

For compiled classes, the ClassLoader is used by the compiler to read the LLVM modules and convert them into
the Structure types that are native to the compiler.  This is done through a combination of:

- Read the metadata from the module
    - Properties
    - Methods / VirtualTable
    - Allocation Size
- Construct Structure from module metadata
- Implement Aspects

## Aspects

### Aspect Requirements

In Cajeta, Aspects will be implemented with the following capabilities:

- Designated through the explicit loading of ``lang.cajeta.aspect.@Aspect`` annotated classes
- Add a property to a class - Aspect
- Add a method to a class - Aspect
- Set the value of a property - Injection (Spring Bean)
- Manage an interceptor that is
    - Called before a target method, can invoke business logic before the call
    - Can invoke (or not invoke) target method
    - Can invoke business logic after the call
    - Returns any return value from the call to the caller
- Manage code globally
    - Search criteria
        - Package scans
        - Direct Import
        - Allow exclusions
- Detect collisions and throw meaningful errors

### Aspect Implementation

Aspects will be implemented through ClassLoader behavior.  See the Clas

- Property and Method Addition will be accomplished through module manipulation.
    - Aspects are accomplished by using:
        - An arbitrary annotation to mark the target class or method
        - An ``lang.cajeta.aspect.@Aspect`` annotated class will provide aspect behavior
            - Arguments to @Aspect define the target annotation
                - Initially, the annotation to target will be the default argument
                - Later, allow regex to identify classes to target or exclude
        - A property annotated by ``lang.cajeta.aspect.@AddProperty`` will add the decorated property to the target class
            - All associated annotations (sans lang.cajeta.aspect.*), modifiers, and initializers of the property will be used.
        - A method annotated by ``lang.cajeta.aspect.@AddMethod`` will add the decorated method to the target class
            - All associated annotations (sans lang.cajeta.aspect.*) and modifiers of the method will be used.
        - A method annotated by ``lang.cajeta.aspect.@Intercept`` will allow a method to be intercepted
            - The method must have the correct signature (accept a ``lang.cajeta.aspect.Invocation`)
                - Invocation contains method reflection info, arguments, and execution method.
                    - Information needed to make the call will be encoded in the ``lang.cajeta.aspect.Invocation``
                        - Instance
                        - Method pointer
                        - Arguments
                            - Reference vs Own
                        - This information will be used to make a call from within Invocation.exec()
                    - Information needed to quickly reflect the call
                        - ``lang.cajeta.type.Method``
                            - From this, one can get the type and method information
                                - Method info
                                - Args info
                                - Type info
                                - Annotations for all
            - The method will have an Object as a result, allowing a return value (if any) to be returned from calling Invocation.exec()
            - The method needs to be accessible (public) to the Aspect class.
                - If we ignore security, this would make Aspect programming an unstoppable threat vector
    - Processing passes will directly modify the target module, updating the compiler structures that will be responsible for generating module output.
        - Adding Methods, Properties & Interceptions simple, not requiring llvm IR modification
        - Scope limited to compilation, will not allow Aspects to operate on already compiled archives
            - This may not be an issue, as initial compilation of any included archive will have already processed aspects
- Dependency Injection will require the coordination of runtime code, executing at the ClassLoader
    - The ``lang.cajeta.aspect.@Aspect`` annotated class denotes a class capable of generating / producing values
    - Methods on the class are labeled with the ``lang.cajeta.aspect.@Value`` are registered by the classloader in a global static map
        - Can provide a discriminator (name / type)
            - Allows multiple emitters of a value type
        - Can be singleton, instance per injection
            - Corresponding map entry should have getter that returns either new value, or a cached value lazily loaded





