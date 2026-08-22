// ide-symbol-index Unit 1 — the compiler exports its resolved view as a
// cross-reference index (spec §2).
//
// Why this exists: the IntelliJ plugin parses Cajeta but does not understand it
// (every PSI node is an untyped ANTLRPsiNode). Rather than reimplement Cajeta's
// semantics in Kotlin — which would drift, and has no oracle to be pinned against,
// since `cajeta doc --emit-model-json` parses but does not RESOLVE — the compiler
// exports what it already computed and the IDE presents it.
//
// This unit covers the half that needs no new resolution plumbing: declarations,
// and inheritance edges as RESOLVED FQNs. That last part is the thing cajetadoc
// structurally cannot produce (its extends/implements are "raw declared names as
// parsed"), and it is what the hierarchy and gutter features rest on.
//
// Pins (plan 1.1.1 - 1.1.8):
//   1.1.1  --emit-xref writes a JSON document carrying the schema version.
//   1.1.2  Every declaring form appears with FQN, kind, and a SourceRef.
//   1.1.3  Methods carry an overload key; same-name/same-arity overloads differ.
//   1.1.4  Operator declarations appear (cajetadoc omits them entirely).
//   1.1.5  extends/implements are RESOLVED FQNs — through an import, and through
//          a template instantiation.
//   1.1.6  Multiple interfaces emit one edge each.
//   1.1.7  Determinism: same input twice -> byte-identical output.
//   1.1.8  Without the flag: no xref output, build unchanged.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define CAJETA_DEVNULL "NUL"
#else
#  define CAJETA_DEVNULL "/dev/null"
#endif

namespace {

    namespace fs = std::filesystem;

    std::string compilerPath() {
        const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
        std::string r = (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
            CAJETA_SOURCE_ROOT_DEFAULT;
#else
            ".";
#endif
        return r + "/build/src/cajeta";
    }

    struct TmpProject {
        fs::path base;
        fs::path sourceRoot;
        fs::path buildRoot;
        fs::path xrefPath;
    };

    void writeUnit(const fs::path& root, const std::string& relPath,
                   const std::string& text) {
        auto file = root / relPath;
        fs::create_directories(file.parent_path());
        std::ofstream out(file);
        out << text;
    }

    // A fixture exercising every relation Unit 1 claims:
    //   - a base class in ANOTHER package, reached by import (1.1.5)
    //   - two interfaces on one class (1.1.6)
    //   - two same-arity overloads (1.1.3)
    //   - an operator declaration (1.1.4)
    //   - a template and an instantiation-parent (1.1.5)
    TmpProject makeTmpProject(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto base = fs::temp_directory_path()
                  / ("cajeta_xref_" + tag + "_" + std::to_string(rng()));
        auto src = base / "src";
        auto build = base / "build";
        fs::create_directories(src);
        fs::create_directories(build);

        writeUnit(src, "demo/base/Base.cajeta",
            "package demo.base;\n"
            "public class Base {\n"
            "    int32 v;\n"
            "    public Base(int32 x) {\n"
            "        this.v = x;\n"
            "    }\n"
            "    public int32 ping() {\n"
            "        return this.v;\n"
            "    }\n"
            "}\n");

        writeUnit(src, "demo/Greeter.cajeta",
            "package demo;\n"
            "public interface Greeter {\n"
            "    int32 greet();\n"
            "}\n");

        writeUnit(src, "demo/Named.cajeta",
            "package demo;\n"
            "public interface Named {\n"
            "    int32 id();\n"
            "}\n");

        // extends across a package boundary, via import -> the edge must carry
        // the RESOLVED FQN `demo.base.Base`, not the raw token `Base`.
        writeUnit(src, "demo/Derived.cajeta",
            "package demo;\n"
            "import demo.base.Base;\n"
            "public class Derived extends Base implements Greeter, Named {\n"
            "    public Derived(int32 x) {\n"
            "        super(x);\n"
            "    }\n"
            "    public int32 greet() {\n"
            "        return 1;\n"
            "    }\n"
            "    public int32 id() {\n"
            "        return 2;\n"
            "    }\n"
            "    public static int32 f(int32 a) {\n"
            "        return a;\n"
            "    }\n"
            "    public static int32 f(boolean b) {\n"
            "        return 0;\n"
            "    }\n"
            "}\n");

        writeUnit(src, "demo/Main.cajeta",
            "package demo;\n"
            "public final class Main {\n"
            "    public static int32 run() {\n"
            "        Derived d = heap Derived(7);\n"
            "        return d.greet() + Derived.f(1);\n"
            "    }\n"
            "}\n");

        return TmpProject{base, src, build, base / "xref.json"};
    }

    // Compile with --emit-xref; return the document text ("" if the compile failed
    // or nothing was written).
    std::string emitXref(const TmpProject& proj, const std::string& extraFlags = "") {
        std::string cmd = compilerPath()
            + " --emit-xref=" + proj.xrefPath.string() + " "
            + extraFlags
            + " --emit=ir demo.Main.run "
            + proj.sourceRoot.string() + " "
            + proj.buildRoot.string()
            + " > " CAJETA_DEVNULL " 2>&1";
        if (std::system(cmd.c_str()) != 0) return "";
        if (!fs::exists(proj.xrefPath)) return "";
        std::ifstream f(proj.xrefPath);
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }

    // Compile WITHOUT the flag; true if it succeeded.
    bool compileWithoutXref(const TmpProject& proj) {
        std::string cmd = compilerPath()
            + " --emit=ir demo.Main.run "
            + proj.sourceRoot.string() + " "
            + proj.buildRoot.string()
            + " > " CAJETA_DEVNULL " 2>&1";
        return std::system(cmd.c_str()) == 0;
    }

    bool has(const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    }

    // Count occurrences of `needle`.
    int count(const std::string& hay, const std::string& needle) {
        int n = 0;
        for (size_t p = hay.find(needle); p != std::string::npos;
             p = hay.find(needle, p + needle.size())) ++n;
        return n;
    }

    // Extract the JSON object (brace-balanced) that contains `anchor`, so a test
    // can assert on the fields of one specific record rather than the whole file.
    std::string recordContaining(const std::string& doc, const std::string& anchor) {
        auto at = doc.find(anchor);
        if (at == std::string::npos) return "";
        auto open = doc.rfind('{', at);
        if (open == std::string::npos) return "";
        int depth = 0;
        for (size_t i = open; i < doc.size(); ++i) {
            if (doc[i] == '{') ++depth;
            else if (doc[i] == '}' && --depth == 0)
                return doc.substr(open, i - open + 1);
        }
        return "";
    }

} // namespace

// ---- 1.1.1 — the document exists, parses, and declares its schema version ----

TEST(XrefExport, EmitsVersionedDocument) {
    auto proj = makeTmpProject("version");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty()) << "--emit-xref produced nothing";

    EXPECT_TRUE(has(doc, "\"version\"")) << doc.substr(0, 400);
    // Major version is the compatibility gate the plugin enforces (spec §2.0.6).
    EXPECT_TRUE(has(doc, "\"major\": 1")) << doc.substr(0, 400);
    // The five relations are all present as arrays, even when empty (units 2+
    // populate references/overrides/calls).
    for (const char* rel : {"declarations", "inheritance", "references",
                            "overrides", "calls"}) {
        EXPECT_TRUE(has(doc, std::string("\"") + rel + "\""))
            << "missing relation: " << rel;
    }
}

// ---- 1.1.2 — every declaring form, with FQN, kind, and a SourceRef -----------

TEST(XrefExport, DeclarationsCarryFqnKindAndSourceRef) {
    auto proj = makeTmpProject("decls");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // Types.
    EXPECT_TRUE(has(doc, "\"demo.base.Base\""));
    EXPECT_TRUE(has(doc, "\"demo.Derived\""));
    EXPECT_TRUE(has(doc, "\"demo.Greeter\""));
    EXPECT_TRUE(has(doc, "\"demo.Named\""));

    // Kinds are distinguished.
    EXPECT_TRUE(has(doc, "\"kind\": \"class\""));
    EXPECT_TRUE(has(doc, "\"kind\": \"interface\""));
    EXPECT_TRUE(has(doc, "\"kind\": \"method\""));
    EXPECT_TRUE(has(doc, "\"kind\": \"field\""));
    EXPECT_TRUE(has(doc, "\"kind\": \"constructor\""));

    // A specific declaration carries a usable SourceRef. `Derived` is declared on
    // line 3 of its file (package, import, then the class).
    auto rec = recordContaining(doc, "\"demo.Derived\"");
    ASSERT_FALSE(rec.empty()) << "no record for demo.Derived";
    EXPECT_TRUE(has(rec, "Derived.cajeta")) << rec;
    EXPECT_TRUE(has(rec, "\"line\": 3")) << rec;
    EXPECT_TRUE(has(rec, "\"col\"")) << rec;

    // Members resolve under their owner.
    EXPECT_TRUE(has(doc, "\"demo.base.Base.ping\""));
    EXPECT_TRUE(has(doc, "\"demo.base.Base.v\""));
}

// ---- 1.1.2b — a `view` is a view, not a class -------------------------------
//
// CajetaView DERIVES from CajetaClass, so a naive isInterface/isRecord check
// reports every view as a class. That is a WRONG value, not a missing one — the
// exact failure mode this export exists to prevent (spec §1.3: a resolver that
// disagrees with the compiler sends you somewhere with total confidence).

TEST(XrefExport, ViewsAreNotReportedAsClasses) {
    auto proj = makeTmpProject("views");
    writeUnit(proj.sourceRoot, "demo/Header.cajeta",
        "package demo;\n"
        "public view Header {\n"
        "    int32 magic;\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    auto rec = recordContaining(doc, "\"demo.Header\"");
    ASSERT_FALSE(rec.empty()) << "the view is missing from the export entirely";
    EXPECT_TRUE(has(rec, "\"kind\": \"view\""))
        << "a `view` must not be reported as a `class`; got: " << rec;
}

// ---- 1.4.1 / 1.4.2 — enums and their constants -------------------------------
//
// An enum registers as an i32-backed CajetaType carrying ENUM_FLAG, NOT a
// CajetaClass, so the declaration walk (which casts to CajetaClass) never saw one.
// Its constants live in a registry keyed by the SHORT name that stores only an
// ordinal — no source position at all. Until this landed, Ctrl-click on an enum,
// or on one of its constants, resolved to nothing.

TEST(XrefExport, EnumsAndTheirConstantsAreDeclared) {
    auto proj = makeTmpProject("enums");
    writeUnit(proj.sourceRoot, "demo/Color.cajeta",
        "package demo;\n"          // 1
        "public enum Color {\n"    // 2  <- the enum
        "    RED,\n"               // 3  <- constants, each on its own line
        "    GREEN,\n"             // 4
        "    BLUE\n"               // 5
        "}\n");                    // 6

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // The enum type itself.
    auto e = recordContaining(doc, "\"demo.Color\"");
    ASSERT_FALSE(e.empty()) << "the enum is missing from the export entirely";
    EXPECT_TRUE(has(e, "\"kind\": \"enum\"")) << e;
    EXPECT_TRUE(has(e, "Color.cajeta")) << e;
    EXPECT_TRUE(has(e, "\"line\": 2")) << e;

    // Each constant is its own declaration, owned by the enum, at its OWN line —
    // so Ctrl-click on `GREEN` lands on `GREEN`, not on `Color`.
    for (const auto& [name, line] : std::vector<std::pair<std::string, int>>{
            {"RED", 3}, {"GREEN", 4}, {"BLUE", 5}}) {
        auto c = recordContaining(doc, "\"demo.Color." + name + "\"");
        ASSERT_FALSE(c.empty()) << "missing enum constant: " << name;
        EXPECT_TRUE(has(c, "\"kind\": \"enumConstant\"")) << c;
        EXPECT_TRUE(has(c, "\"owner\": \"demo.Color\"")) << c;
        EXPECT_TRUE(has(c, "\"line\": " + std::to_string(line)))
            << name << " must point at its own line, not the enum's; got: " << c;
    }
}

// ---- 1.5 — methods of GENERIC types ------------------------------------------
//
// A template's body walk is skipped entirely (CajetaLlvmVisitor.h:726 — it "keeps
// the template out of getAllMethods' codegen worklist by way of having no methods
// at all"), so before this landed NO generic type exported a single method:
// ArrayList, HashMap, Comparable, the lot. `ArrayList.add` — the most-called method
// in the stdlib — was absent from the index, and Ctrl-click on `demos.add(...)`
// resolved to nothing.
//
// We export the TEMPLATE's method, not the instantiation's: an instantiation is
// monomorphized from the template and has no source of its own, so one record per
// instantiation would list the same source method N times, fragment "who calls add"
// across instantiations, and name FQNs that exist in no file.

TEST(XrefExport, GenericTypesExportTheirMethods) {
    auto proj = makeTmpProject("generics");
    writeUnit(proj.sourceRoot, "demo/Box.cajeta",
        "package demo;\n"                    // 1
        "public class Box<T> {\n"            // 2
        "    T value;\n"                     // 3
        "    public Box(T v) {\n"            // 4
        "        this.value = v;\n"          // 5
        "    }\n"                            // 6
        "    public T get() {\n"             // 7  <- the method under test
        "        return this.value;\n"       // 8
        "    }\n"                            // 9
        "}\n");                              // 10

    // Instantiate it TWICE, with different arguments.
    writeUnit(proj.sourceRoot, "demo/Main.cajeta",
        "package demo;\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        Box<int32> a = heap Box<int32>(1);\n"
        "        Box<boolean> b = heap Box<boolean>(true);\n"
        "        return a.get();\n"
        "    }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // 1.5.1 — the method exists, positioned in the TEMPLATE's source.
    auto m = recordContaining(doc, "\"demo.Box.get\"");
    ASSERT_FALSE(m.empty())
        << "a generic type's methods must be exported — otherwise Ctrl-click on a "
           "call through the template resolves to nothing";
    EXPECT_TRUE(has(m, "\"kind\": \"method\"")) << m;
    EXPECT_TRUE(has(m, "Box.cajeta")) << m;
    EXPECT_TRUE(has(m, "\"line\": 7")) << m;

    // 1.5.2 — TWO instantiations, but still exactly ONE declaration per source
    // method. The template is the navigation target; instantiations are not
    // separate declarations.
    EXPECT_EQ(count(doc, "\"demo.Box.get\""), 1)
        << "one source method must yield one declaration, not one per instantiation";

    // No declaration may name an instantiated type — `demo.Box<int32>.get` is not a
    // thing that exists in any source file.
    EXPECT_FALSE(has(doc, "\"demo.Box<"))
        << "instantiations must not appear as declarations";
}

// ---- 1.1.3 — overload keys distinguish same-name, same-arity methods ---------

TEST(XrefExport, OverloadsGetDistinctKeys) {
    auto proj = makeTmpProject("overloads");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // Both `f` overloads are declared, same name, same arity (1).
    EXPECT_EQ(count(doc, "\"demo.Derived.f\""), 2)
        << "expected exactly two `f` declarations";

    // Their overload keys must differ — this is the whole point. If they collide,
    // "who calls f(int32)" would return f(boolean)'s callers, and renaming one
    // overload would rewrite the other's call sites.
    EXPECT_TRUE(has(doc, "int32")) << "overload key should name the param type";
    EXPECT_TRUE(has(doc, "boolean")) << "overload key should name the param type";

    // Every method record carries the key.
    EXPECT_GE(count(doc, "\"overloadKey\""), 2);
}

// ---- 1.1.4 — operator declarations appear -----------------------------------
//
// cajetadoc's model omits operator overloads entirely
// (cajetadoc-model-fidelity-spec §2.1). They cannot be recovered from that
// export, so the xref index has to carry them or operators become invisible to
// every IDE feature.

TEST(XrefExport, OperatorDeclarationsAppear) {
    auto proj = makeTmpProject("operators");
    // Add a type with an operator overload.
    writeUnit(proj.sourceRoot, "demo/Vec.cajeta",
        "package demo;\n"
        "public class Vec {\n"
        "    int32 x;\n"
        "    public Vec(int32 x) {\n"
        "        this.x = x;\n"
        "    }\n"
        "    public static #Vec operator+ (Vec a, Vec b) {\n"
        "        return heap Vec(a.x + b.x);\n"
        "    }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    EXPECT_TRUE(has(doc, "\"demo.Vec\"")) << "the operator's owner is missing";
    EXPECT_TRUE(has(doc, "operator+"))
        << "operator declarations must appear — cajetadoc omits them, so this "
           "export is the only source for them";
}

// ---- 1.1.5 — inheritance edges are RESOLVED FQNs -----------------------------

TEST(XrefExport, InheritanceEdgesAreResolvedFqns) {
    auto proj = makeTmpProject("inherit");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // `Derived extends Base` where Base is imported from demo.base. The edge must
    // name demo.base.Base — NOT the raw token "Base" as written in the source.
    // This is precisely what cajetadoc cannot do.
    auto edge = recordContaining(doc, "\"parent\": \"demo.base.Base\"");
    ASSERT_FALSE(edge.empty())
        << "extends edge must carry the RESOLVED parent FQN (demo.base.Base), "
           "not the raw declared name; doc:\n" << doc;
    EXPECT_TRUE(has(edge, "\"child\": \"demo.Derived\"")) << edge;
    EXPECT_TRUE(has(edge, "\"kind\": \"extends\"")) << edge;

    // No edge may carry an unresolved bare name. This is the failure mode that
    // shipped in the first cut: getCanonicalMap() is keyed by BOTH the FQN and the
    // short name, so using the map key as the FQN emitted every class twice — once
    // under a bare name that is not a valid FQN at all.
    EXPECT_FALSE(has(doc, "\"parent\": \"Base\""))
        << "an unresolved raw parent name leaked into the export";
    EXPECT_FALSE(has(doc, "\"child\": \"Derived\""))
        << "a bare (non-FQN) child name leaked into the export";

    // A TEMPLATE-INSTANTIATED parent resolves too, with its type arguments — the
    // stdlib is compiled into every export, and it is full of these. `Comparable<T>`
    // must come out as e.g. `cajeta.lang.Comparable<cajeta.time.Duration>`, not as
    // the raw `Comparable`.
    EXPECT_TRUE(has(doc, "\"parent\": \"cajeta.lang.Comparable<"))
        << "a template-instantiated parent must be a resolved FQN carrying its "
           "type arguments";
}

// ---- 1.1.6 — multiple interfaces, one edge each ------------------------------

TEST(XrefExport, MultipleInterfacesEmitOneEdgeEach) {
    auto proj = makeTmpProject("ifaces");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    auto g = recordContaining(doc, "\"parent\": \"demo.Greeter\"");
    auto n = recordContaining(doc, "\"parent\": \"demo.Named\"");
    ASSERT_FALSE(g.empty()) << "missing implements edge -> demo.Greeter";
    ASSERT_FALSE(n.empty()) << "missing implements edge -> demo.Named";

    EXPECT_TRUE(has(g, "\"kind\": \"implements\"")) << g;
    EXPECT_TRUE(has(n, "\"kind\": \"implements\"")) << n;
    EXPECT_TRUE(has(g, "\"child\": \"demo.Derived\"")) << g;
    EXPECT_TRUE(has(n, "\"child\": \"demo.Derived\"")) << n;

    // One edge each, not one merged edge and not duplicates.
    EXPECT_EQ(count(doc, "\"parent\": \"demo.Greeter\""), 1);
    EXPECT_EQ(count(doc, "\"parent\": \"demo.Named\""), 1);
}

// ---- 2.1.7 — override edges, matched on SIGNATURE not name --------------------
//
// The relation behind Method Hierarchy and the up/down gutter icons. Derivable
// from what the compiler already holds (each class's methods + its resolved
// ancestors), so it needs no instrumentation of the resolution path.
//
// Matching on NAME alone would be wrong: an overload that merely shares a name
// with an ancestor's method does not override it, and claiming so would send
// "rename the override chain" through an unrelated method.

TEST(XrefExport, OverrideEdgesMatchOnSignature) {
    auto proj = makeTmpProject("overrides");
    writeUnit(proj.sourceRoot, "demo/Animal.cajeta",
        "package demo;\n"
        "public class Animal {\n"
        "    public int32 speak() {\n"
        "        return 0;\n"
        "    }\n"
        "    public int32 feed(int32 n) {\n"
        "        return n;\n"
        "    }\n"
        "}\n");
    writeUnit(proj.sourceRoot, "demo/Dog.cajeta",
        "package demo;\n"
        "public class Dog extends Animal {\n"
        "    public int32 speak() {\n"          // overrides Animal.speak()
        "        return 1;\n"
        "    }\n"
        "    public int32 feed(boolean b) {\n"  // SAME NAME, different signature —
        "        return 2;\n"                   // an overload, NOT an override
        "    }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // The genuine override.
    auto o = recordContaining(doc, "\"overrides\": \"demo.Animal::speak(pointer)\"");
    ASSERT_FALSE(o.empty())
        << "Dog.speak must record an override edge to Animal.speak; doc:\n" << doc;
    EXPECT_TRUE(has(o, "\"method\": \"demo.Dog::speak(pointer)\"")) << o;

    // `Dog.feed(boolean)` shares a name with `Animal.feed(int32)` but overrides
    // nothing. A name-only match would claim it does — and then "rename the
    // override chain" would rewrite an unrelated method.
    EXPECT_FALSE(has(doc, "\"overrides\": \"demo.Animal::feed(pointer,int32)\""))
        << "an overload must not be reported as an override";
}

// An interface method is the TARGET of an override edge — the Method Hierarchy's
// root for any interface-declared method.
TEST(XrefExport, ImplementingAnInterfaceMethodIsAnOverride) {
    auto proj = makeTmpProject("ifaceoverride");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // The base fixture: Derived implements Greeter { int32 greet(); }
    auto o = recordContaining(doc, "\"overrides\": \"demo.Greeter::greet(pointer)\"");
    ASSERT_FALSE(o.empty())
        << "implementing an interface method must record an override edge; doc:\n"
        << doc;
    EXPECT_TRUE(has(o, "\"method\": \"demo.Derived::greet(pointer)\"")) << o;
}

// ---- 2.1.1 - 2.1.4 — call edges ----------------------------------------------
//
// The compiler resolves every call site's callee and then throws it away. These
// pin that it now records it instead — which is what makes a Kotlin resolver
// unnecessary: overload selection and inherited-method lookup are exactly the
// things a second resolver gets subtly wrong.

TEST(XrefExport, CallEdgesRecordTheResolvedCallee) {
    auto proj = makeTmpProject("calls");
    writeUnit(proj.sourceRoot, "demo/Calls.cajeta",
        "package demo;\n"                             // 1
        "import demo.base.Base;\n"                    // 2
        "public final class Calls {\n"                // 3
        "    public static int32 run() {\n"           // 4
        "        Derived d = heap Derived(7);\n"      // 5
        "        int32 a = Derived.f(1);\n"           // 6  -> f(int32)
        "        int32 b = Derived.f(true);\n"        // 7  -> f(boolean)
        "        int32 c = d.greet();\n"              // 8  -> Derived.greet
        "        int32 e = d.ping();\n"               // 9  -> INHERITED from Base
        "        return a + b + c + e;\n"             // 10
        "    }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // 2.1.1 — a call edge from the call site to the resolved callee.
    EXPECT_TRUE(has(doc, "\"callee\": \"demo.Derived::greet(pointer)\""))
        << "no call edge for d.greet()";

    // 2.1.2 — OVERLOADS ARE NOT CONFLATED. If these collapse, "who calls f(int32)"
    // returns f(boolean)'s callers, and renaming one overload rewrites the other's
    // call sites.
    EXPECT_TRUE(has(doc, "\"callee\": \"demo.Derived::f(int32)\""))
        << "no call edge for Derived.f(1)";
    EXPECT_TRUE(has(doc, "\"callee\": \"demo.Derived::f(boolean)\""))
        << "no call edge for Derived.f(true)";

    // 2.1.3 — an INHERITED method resolves to the DECLARING ancestor (Base.ping),
    // not to the receiver's type (Derived). Getting this wrong sends Ctrl-click to
    // a method that does not exist.
    EXPECT_TRUE(has(doc, "\"callee\": \"demo.base.Base::ping(pointer)\""))
        << "d.ping() must resolve to the declaring ancestor demo.base.Base";
    EXPECT_FALSE(has(doc, "\"callee\": \"demo.Derived::ping(pointer)\""))
        << "an inherited call must not name the receiver's type as the declarer";

    // The caller is recorded, so the edge is walkable in both directions
    // (callers-of and callees-of both read this one relation).
    EXPECT_TRUE(has(doc, "\"caller\": \"demo.Calls::run()\""))
        << "call edges must name their enclosing method";
}

// A call THROUGH a generic type must land on the TEMPLATE's method — the thing
// that exists in source — not on a monomorphized instantiation (1.5).
TEST(XrefExport, CallsThroughGenericsTargetTheTemplate) {
    auto proj = makeTmpProject("genericcalls");
    writeUnit(proj.sourceRoot, "demo/Main.cajeta",
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        xs.add(1);\n"
        "        return xs.count();\n"
        "    }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    EXPECT_TRUE(has(doc, "\"callee\": \"cajeta.collection.ArrayList::add(T)\""))
        << "a call through ArrayList<int32> must target the TEMPLATE's add(T) — the "
           "method that actually exists in ArrayList.cajeta";
    // No callee may name an instantiation: `ArrayList<int32>::add` is in no file.
    EXPECT_FALSE(has(doc, "\"callee\": \"cajeta.collection.ArrayList<"))
        << "a callee must never name a monomorphized instantiation";
}

// 2.1.9 — a call the compiler cannot resolve is OMITTED, never guessed.
TEST(XrefExport, UnresolvedCallsAreOmittedNotGuessed) {
    auto proj = makeTmpProject("unresolved");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // Every callee must be a key we also declared. A callee naming nothing we
    // exported is a dangling target — Ctrl-click into the void.
    // (Structural check; the detailed assertion lives in the acceptance script.)
    EXPECT_FALSE(has(doc, "\"callee\": \"\""))
        << "an empty callee key means a guessed/unresolved edge leaked out";
}

// ---- 1.1.7 — determinism ------------------------------------------------------
//
// The project already holds this line for IR (verify-reproducible); the xref
// export is a build artifact and must hold it too, or it will churn caches and
// diffs for no reason.

TEST(XrefExport, IsDeterministic) {
    auto proj = makeTmpProject("determinism");
    auto first = emitXref(proj);
    ASSERT_FALSE(first.empty());
    fs::remove(proj.xrefPath);
    auto second = emitXref(proj);
    ASSERT_FALSE(second.empty());

    EXPECT_EQ(first, second) << "xref export is not byte-identical across runs";
}

// ---- annotations on declarations (2026-08-22) -------------------------------
//
// Added for the IDE's run-line markers: "which methods are @Test" is not
// answerable from a parse, because the answer depends on resolution. The
// compiler knows; it now says so.
//
// The RESOLUTION is the part worth pinning. A bare `@Marker` canonicalizes to
// package `code`, so the AST holds `code.Marker` regardless of where Marker is
// declared. Shipping that would name a package no source declares, and a
// consumer filtering on the real FQN would match nothing while appearing to
// work — a silent empty result, which is the failure mode this whole index
// exists to avoid.

TEST(XrefExport, AnnotationsAreRecordedWithTheirResolvedFqn) {
    auto proj = makeTmpProject("annotations");
    writeUnit(proj.sourceRoot, "mark/Marker.cajeta",
        "package mark;\n"
        "public annotation Marker { }\n");
    // NOTE: no `import mark.Marker` — a bare use is the case that canonicalizes
    // to `code.Marker` and therefore the case resolution has to handle.
    writeUnit(proj.sourceRoot, "app/Widget.cajeta",
        "package app;\n"
        "@Marker\n"
        "public class Widget {\n"
        "    @Marker\n"
        "    int32 tagged;\n"
        "    @Marker\n"
        "    public static int32 run() { return 1; }\n"
        "    public static int32 plain() { return 2; }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    // All three positions an annotation can appear on.
    for (const char* fqn : {"\"app.Widget\"", "\"app.Widget.tagged\"",
                            "\"app.Widget.run\""}) {
        auto rec = recordContaining(doc, fqn);
        ASSERT_FALSE(rec.empty()) << "no record for " << fqn;
        EXPECT_TRUE(has(rec, "\"annotations\": [\"mark.Marker\"]"))
            << fqn << " must carry the RESOLVED annotation name; got: " << rec;
        EXPECT_FALSE(has(rec, "code.Marker"))
            << fqn << " shipped the scope-canonical name instead of the declaring"
               " package, which names a package no source declares: " << rec;
    }
}

// The other half of the check. Without this, an implementation that stamped
// every declaration with the same annotation list would pass the test above.

TEST(XrefExport, AnUnannotatedDeclarationCarriesNoAnnotationsKey) {
    auto proj = makeTmpProject("noannotations");
    writeUnit(proj.sourceRoot, "mark/Marker.cajeta",
        "package mark;\n"
        "public annotation Marker { }\n");
    writeUnit(proj.sourceRoot, "app/Widget.cajeta",
        "package app;\n"
        "public class Widget {\n"
        "    @Marker\n"
        "    public static int32 run() { return 1; }\n"
        "    public static int32 plain() { return 2; }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    auto annotated = recordContaining(doc, "\"app.Widget.run\"");
    ASSERT_FALSE(annotated.empty());
    EXPECT_TRUE(has(annotated, "\"annotations\""))
        << "the control is not annotated, so the negative below proves nothing: "
        << annotated;

    auto plain = recordContaining(doc, "\"app.Widget.plain\"");
    ASSERT_FALSE(plain.empty()) << "no record for app.Widget.plain";
    EXPECT_FALSE(has(plain, "\"annotations\""))
        << "a declaration with no annotations must omit the key entirely, not "
           "carry an empty array: " << plain;

    // The class itself is unannotated here, unlike in the test above.
    auto klass = recordContaining(doc, "\"app.Widget\"");
    ASSERT_FALSE(klass.empty());
    EXPECT_FALSE(has(klass, "\"annotations\""))
        << "annotations leaked from a member onto its owner: " << klass;
}

// ---- 1.1.8 — the flag is opt-in; absent it, nothing changes -------------------

TEST(XrefExport, AbsentFlagEmitsNothing) {
    auto proj = makeTmpProject("optin");
    ASSERT_TRUE(compileWithoutXref(proj)) << "baseline compile failed";
    EXPECT_FALSE(fs::exists(proj.xrefPath))
        << "xref was written without --emit-xref";
}

// =============================================================================
// Unit 2 (continued) — display signatures, references, provenance, partial export
// =============================================================================

namespace {

    // Every JSON object inside the named top-level array. Crude but adequate: the
    // writer emits one flat object per line-group and never nests inside a record.
    std::vector<std::string> recordsIn(const std::string& doc, const std::string& rel) {
        std::vector<std::string> out;
        auto at = doc.find("\"" + rel + "\": [");
        if (at == std::string::npos) return out;
        auto end = doc.find("\n  ]", at);
        if (end == std::string::npos) end = doc.size();
        for (size_t i = doc.find('{', at); i != std::string::npos && i < end;
             i = doc.find('{', i + 1)) {
            auto close = doc.find('}', i);
            if (close == std::string::npos || close > end) break;
            out.push_back(doc.substr(i, close - i + 1));
        }
        return out;
    }

    std::string fieldOf(const std::string& rec, const std::string& key) {
        auto at = rec.find("\"" + key + "\": \"");
        if (at == std::string::npos) return "";
        auto start = at + key.size() + 5;
        auto end = rec.find('"', start);
        return end == std::string::npos ? "" : rec.substr(start, end - start);
    }

    int intFieldOf(const std::string& rec, const std::string& key) {
        auto at = rec.find("\"" + key + "\": ");
        if (at == std::string::npos) return -1;
        return std::atoi(rec.c_str() + at + key.size() + 4);
    }

    int lineCount(const fs::path& p) {
        std::ifstream f(p);
        if (!f) return -1;
        int n = 0;
        for (std::string l; std::getline(f, l); ) ++n;
        return n;
    }

} // namespace

// ---- 2.2.6 — the display signature is a LABEL, not the overload key -----------
//
// Unit 1 set signature = overloadKey = the compiler's canonical form, which renders
// as `demo.Derived::greet(pointer)`: the receiver leaks in as `pointer` and there is
// no return type. Exact as an identity, useless as the text a Type or Call Hierarchy
// node shows a developer. The two are now distinct.

TEST(XrefExport, SignatureIsHumanReadableAndDistinctFromTheOverloadKey) {
    auto proj = makeTmpProject("displaysig");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    std::string greet;
    for (const auto& rec : recordsIn(doc, "declarations")) {
        if (fieldOf(rec, "fqn") == "demo.Derived.greet") { greet = rec; break; }
    }
    ASSERT_FALSE(greet.empty()) << "demo.Derived.greet is not declared";

    // The key stays exactly what the compiler dispatches on.
    EXPECT_EQ(fieldOf(greet, "overloadKey"), "demo.Derived::greet(pointer)");

    // The label is what a human reads: return type, no receiver.
    const std::string sig = fieldOf(greet, "signature");
    EXPECT_EQ(sig, "int32 greet()") << "display signature was: " << sig;
    EXPECT_EQ(sig.find("pointer"), std::string::npos)
        << "the receiver leaked into the display signature: " << sig;
    EXPECT_NE(sig, fieldOf(greet, "overloadKey"))
        << "signature must not simply duplicate the overload key";
}

// ---- 2.1.5 / 2.2.2 — type references ------------------------------------------

TEST(XrefExport, TypeReferencesResolveToTheDeclaration) {
    auto proj = makeTmpProject("typerefs");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    bool derivedInMain = false;   // `Derived d = heap Derived(7);` in Main.cajeta
    bool baseInDerived = false;   // `extends Base` in Derived.cajeta -> RESOLVED fqn
    for (const auto& r : recordsIn(doc, "references")) {
        if (fieldOf(r, "kind") != "type") continue;
        const std::string target = fieldOf(r, "target");
        const std::string file = fieldOf(r, "file");
        if (target == "demo.Derived" && has(file, "Main.cajeta")) derivedInMain = true;
        // The base is named `Base` in the source and reached through an import; the
        // edge must carry the resolved FQN, which is the whole point of the export.
        if (target == "demo.base.Base" && has(file, "Derived.cajeta")) baseInDerived = true;
    }
    EXPECT_TRUE(derivedInMain) << "no type reference to demo.Derived from Main";
    EXPECT_TRUE(baseInDerived)
        << "no RESOLVED type reference to demo.base.Base from Derived's extends clause";
}

// ---- 2.1.6 — a field access targets the class that DECLARES the field ----------
//
// The sharp case is the INHERITED one: `this.v` inside Derived reads a field that
// demo.base.Base declares. Naming the receiver's class (demo.Derived.v) would send
// Ctrl-click to a type that declares nothing of the sort — a confident wrong answer.

TEST(XrefExport, FieldAccessTargetsTheDeclaringClassNotTheReceiver) {
    auto proj = makeTmpProject("fieldrefs");
    writeUnit(proj.sourceRoot, "demo/Reader.cajeta",
        "package demo;\n"                     // 1
        "import demo.base.Base;\n"            // 2
        "public class Reader extends Base {\n"// 3
        "    public Reader(int32 x) {\n"      // 4
        "        super(x);\n"                 // 5
        "    }\n"                             // 6
        "    public int32 peek() {\n"         // 7
        "        return this.v;\n"            // 8  <- inherited field
        "    }\n"                             // 9
        "}\n");

    writeUnit(proj.sourceRoot, "demo/Main.cajeta",
        "package demo;\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        Reader r = heap Reader(7);\n"
        "        return r.peek();\n"
        "    }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    bool declaringOwner = false;
    for (const auto& r : recordsIn(doc, "references")) {
        if (fieldOf(r, "kind") != "field") continue;
        if (!has(fieldOf(r, "file"), "Reader.cajeta")) continue;
        const std::string target = fieldOf(r, "target");
        EXPECT_NE(target, "demo.Reader.v")
            << "`this.v` was attributed to the RECEIVER's class, which does not "
               "declare it — Ctrl-click would land nowhere";
        if (target == "demo.base.Base.v" && intFieldOf(r, "line") == 8) {
            declaringOwner = true;
        }
    }
    EXPECT_TRUE(declaringOwner)
        << "the inherited read `this.v` did not resolve to demo.base.Base.v";
}

// ---- 2.2.8 — a position must be a position IN THE FILE IT NAMES ---------------
//
// Regression. Every call edge used to take its file from the module being compiled
// rather than from the file its AST node was parsed from. Stdlib and template bodies
// are generated while a USER module is active, so their internal calls were recorded
// against whichever user file triggered them: on samples/tour, 426 of 2589 call edges
// named the wrong file, and 181 of those cited a line past the end of it.
//
// This is the failure this whole export exists to prevent — not a missing answer but
// a confident wrong one.

TEST(XrefExport, NoRecordPointsPastTheEndOfTheFileItNames) {
    auto proj = makeTmpProject("provenance");
    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    int checked = 0;
    for (const char* rel : {"declarations", "references", "overrides", "calls"}) {
        for (const auto& rec : recordsIn(doc, rel)) {
            const std::string file = fieldOf(rec, "file");
            const int line = intFieldOf(rec, "line");
            if (file.empty() || line <= 0) continue;

            // Stdlib records name paths relative to the EXTRACTED stdlib root, which
            // is not on disk under the project. Only the project's own files can be
            // checked here — which is exactly where the bug put its wrong positions.
            auto path = proj.sourceRoot / file;
            if (!fs::exists(path)) continue;

            const int lines = lineCount(path);
            ++checked;
            EXPECT_LE(line, lines)
                << rel << " record cites " << file << ":" << line
                << " but that file has only " << lines << " lines: " << rec;
        }
    }
    EXPECT_GT(checked, 0) << "no on-disk records were checked; the test is vacuous";
}

// ---- 2.2.8 — a generic body's internal calls are not attributed to the call site --
//
// A template is monomorphized from a re-parsed snippet whose line numbers refer to
// the snippet, not to any file. `Box.twice()` calls `Box.get()` — that call happens
// inside Box, and must never be reported as happening at the line in Main where the
// instantiation was triggered.

TEST(XrefExport, AGenericBodysInternalCallsAreNotAttributedToTheCallSite) {
    auto proj = makeTmpProject("genericcalls");
    writeUnit(proj.sourceRoot, "demo/Box.cajeta",
        "package demo;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) {\n"
        "        this.value = v;\n"
        "    }\n"
        "    public T get() {\n"
        "        return this.value;\n"
        "    }\n"
        "    public T twice() {\n"
        "        return this.get();\n"       // <- internal call, lives in Box.cajeta
        "    }\n"
        "}\n");

    writeUnit(proj.sourceRoot, "demo/Main.cajeta",
        "package demo;\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = heap Box<int32>(3);\n"
        "        return b.twice();\n"        // line 5
        "    }\n"
        "}\n");

    auto doc = emitXref(proj);
    ASSERT_FALSE(doc.empty());

    bool sawTwiceFromMain = false;
    for (const auto& c : recordsIn(doc, "calls")) {
        const std::string callee = fieldOf(c, "callee");
        const std::string file = fieldOf(c, "file");
        if (!has(file, "Main.cajeta")) continue;

        if (has(callee, "demo.Box::twice")) sawTwiceFromMain = true;

        // The one that must never appear: Box.get() is called from inside Box, not
        // from Main. Attributing it here is the misattribution bug.
        EXPECT_FALSE(has(callee, "demo.Box::get"))
            << "a call made INSIDE the template body was attributed to the "
               "instantiation site in Main: " << c;
    }
    EXPECT_TRUE(sawTwiceFromMain)
        << "the call Main makes to Box.twice() is missing entirely";
}

// ---- 2.1.8 — a broken file must not sink the rest of the index ---------------
//
// The plugin re-runs the compiler on the user's buffer as they type, and a buffer
// mid-edit is broken most of the time. Two things used to go wrong, and both did:
//
//   1. The export ran at the END of Compiler::compile, which a SyntaxErrorException
//      skips entirely — so a broken file wrote NO index at all. It is now emitted
//      from a scope guard, on the unwind path too.
//   2. The parse aborted at the FIRST file with a syntax error, so every source
//      enumerated after it went unparsed — and since the walk is a directory walk,
//      WHICH files those were depended on readdir order. This very fixture indexed
//      all 17 of its declarations or none of them depending on where the broken file
//      landed. The parse loop now continues past a broken file and rethrows the
//      first error after the sweep: the build still fails, with the same diagnostic,
//      but everything parseable is parsed.
//
// So the guarantee is: every healthy file is indexed no matter where the broken one
// sits, nothing is invented for the file that did not parse, and the compile still
// fails.

TEST(XrefExport, ABrokenFileDoesNotSinkTheRestOfTheIndex) {
    auto proj = makeTmpProject("syntaxerror");
    writeUnit(proj.sourceRoot, "demo/Broken.cajeta",
        "package demo;\n"
        "public class Broken {\n"
        "    public int32 oops( {\n"        // <- unbalanced; the parse cannot recover
        "        return ;;;\n"
        "    }\n"
        "}\n");

    // The compile FAILS (correctly — the file really is broken), so emitXref()'s
    // exit-status check would report nothing. Read the document directly.
    std::string cmd = compilerPath()
        + " --emit-xref=" + proj.xrefPath.string()
        + " --emit=ir demo.Main.run "
        + proj.sourceRoot.string() + " " + proj.buildRoot.string()
        + " > " CAJETA_DEVNULL " 2>&1";
    const int rc = std::system(cmd.c_str());
    EXPECT_NE(rc, 0) << "the fixture is broken on purpose; the compile must fail";

    ASSERT_TRUE(fs::exists(proj.xrefPath))
        << "no xref was written at all — a failed compile sank the whole index";
    std::ifstream f(proj.xrefPath);
    std::string doc((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    // Well-formed and versioned, not a truncated fragment.
    ASSERT_FALSE(doc.empty()) << "the index was written but empty";
    EXPECT_TRUE(has(doc, "\"major\": 1"));
    for (const char* rel : {"declarations", "inheritance", "references",
                            "overrides", "calls"}) {
        EXPECT_TRUE(has(doc, std::string("\"") + rel + "\""))
            << "missing relation: " << rel;
    }

    // EVERY healthy file is still indexed — including the ones a directory walk may
    // well enumerate AFTER the broken one, which is precisely what used to be lost.
    for (const char* fqn : {"\"demo.Derived\"", "\"demo.Derived.greet\"",
                            "\"demo.Greeter\"", "\"demo.Named\"",
                            "\"demo.Main.run\"", "\"demo.base.Base\"",
                            "\"demo.base.Base.ping\"", "\"demo.base.Base.v\""}) {
        EXPECT_TRUE(has(doc, fqn))
            << "a healthy declaration is missing because another file failed to "
               "parse: " << fqn;
    }

    // And nothing was invented for the file that did not parse. A partial index is
    // fine; a fabricated one is not.
    EXPECT_FALSE(has(doc, "\"demo.Broken\""));
    EXPECT_FALSE(has(doc, "\"demo.Broken.oops\""));
}
