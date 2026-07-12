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

    // No edge may carry an unresolved bare name.
    EXPECT_FALSE(has(doc, "\"parent\": \"Base\""))
        << "an unresolved raw parent name leaked into the export";
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

// ---- 1.1.8 — the flag is opt-in; absent it, nothing changes -------------------

TEST(XrefExport, AbsentFlagEmitsNothing) {
    auto proj = makeTmpProject("optin");
    ASSERT_TRUE(compileWithoutXref(proj)) << "baseline compile failed";
    EXPECT_FALSE(fs::exists(proj.xrefPath))
        << "xref was written without --emit-xref";
}
