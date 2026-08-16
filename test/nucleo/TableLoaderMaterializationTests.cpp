//
// Loader materialization for synthesized generics (synth-class-loader-
// materialization spec). Two same-family gaps, both "the name is bound but
// the class isn't loaded when synthesis needs it":
//
//  A. The FIRST `Table<R>` instantiation inside a template-static method
//     body: member-synthesis imports (`DynCol`, …) were only name-bound by
//     `injectImportIfUnbound`, never materialized, so the synthesized
//     `fromColumns` ctor's `DynCol[]` local resolved to a null element type
//     — a compiler SIGSEGV. (This is what blocked nucleo-frame U16's
//     `Table.fromCsv<R>`.)
//
//  B. A record declared in a SEPARATE file from the first `Table<R>` use:
//     the schema synthesizer read the record flag before the record's unit
//     was materialized, throwing a spurious CAJETA_ERROR_FRAME_SCHEMA
//     "'test.Rec' is not a record". Same-file always worked (and the
//     JIT-prelude suites compile one string, which is why they never saw it).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// --- A: first instantiation inside a template-static body ------------------

TEST(TableLoaderMaterializationTests, firstInstantiationInsideTemplateStaticBody) {
    // `wrap<R>` is the fromCsv shape: the template-static body performs the
    // program's FIRST `Table<R>` instantiation, so the member-synthesized
    // ctor compiles inside the template context — where `DynCol` must be
    // materialized, not merely name-bound.
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public record P {\n"
        "    float64 v;\n"
        "}\n"
        "public final class D {\n"
        "    static #Table<R> wrap<R>(#Column<float64> c) {\n"
        "        Table<R> t = heap Table<R>(#c);\n"
        "        return #t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] vs = heap float64[3];\n"
        "        vs[0] = 1.5; vs[1] = 2.5; vs[2] = 3.5;\n"
        "        Table<P> t #= D.wrap<P>(Column.of<float64>(vs));\n"
        "        if (t.v.get(1) == 2.5) { return 42; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// --- B: cross-file record --------------------------------------------------

TEST(TableLoaderMaterializationTests, crossFileRecordSchemaMaterializes) {
    std::map<std::string, std::string> sources;
    sources["test.Rec"] =
        "package test;\n"
        "public record Rec {\n"
        "    float64 price;\n"
        "    float64 size;\n"
        "}\n";
    sources["test.D"] =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] pv = heap float64[2];\n"
        "        pv[0] = 1.0; pv[1] = 2.0;\n"
        "        float64[] sv = heap float64[2];\n"
        "        sv[0] = 10.0; sv[1] = 20.0;\n"
        "        Table<Rec> t = heap Table<Rec>(\n"
        "            Column.of<float64>(pv), Column.of<float64>(sv));\n"
        "        if (t.price.get(1) == 2.0 && t.size.get(0) == 10.0) { return 42; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// Ordering probe: identical shape, but the USING class sorts AFTER the record
// ("test.Zuser" > "test.Rec" in the harness's std::map file order). If this
// passes while crossFileRecordSchemaMaterializes fails, the defect is purely
// parse/walk file order — the record's declaration walk hasn't run when the
// earlier file's body walk instantiates Table<Rec>.
TEST(TableLoaderMaterializationTests, crossFileRecordUsingFileSortsAfter) {
    std::map<std::string, std::string> sources;
    sources["test.Rec2"] =
        "package test;\n"
        "public record Rec2 {\n"
        "    float64 price;\n"
        "}\n";
    sources["test.Zuser"] =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public final class Zuser {\n"
        "    public static int32 run() {\n"
        "        float64[] pv = heap float64[2];\n"
        "        pv[0] = 1.0; pv[1] = 2.0;\n"
        "        Table<Rec2> t = heap Table<Rec2>(Column.of<float64>(pv));\n"
        "        if (t.price.get(1) == 2.0) { return 42; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.Zuser");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// Nested-materialize prototype hazard (the ml v0.2.0 CI failure): the
// consumer sorts FIRST and declares an EXPLICIT no-arg ctor; its walk hits
// Table<Rec3> mid-body, the nested materialize compile runs, and any
// prototype sweep inside that nested compile (drainLazyStdlib's tail) used
// to prototype the still-BODYLESS consumer — fabricating its auto-default
// ctor, which collided with the declared one when the walk resumed
// (CAJETA_ERROR_DUPLICATE_CONSTRUCTOR). The declWalkInFlight marker keeps
// mid-walk classes out of every sweep.
TEST(TableLoaderMaterializationTests, explicitCtorConsumerBeforeRecord) {
    std::map<std::string, std::string> sources;
    sources["test.AConsumer"] =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public class AConsumer {\n"
        "    public AConsumer() { return; }\n"
        "    static #Tensorish helper() { return null; }\n"
        "    public static int32 run() {\n"
        "        float64[] pv = heap float64[2];\n"
        "        pv[0] = 1.5; pv[1] = 2.5;\n"
        "        Table<Rec3> t = heap Table<Rec3>(Column.of<float64>(pv));\n"
        "        if (t.price.get(1) == 2.5) { return 42; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "class Tensorish {\n"
        "    public Tensorish() { return; }\n"
        "}\n";
    sources["test.Rec3"] =
        "package test;\n"
        "public record Rec3 {\n"
        "    float64 price;\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.AConsumer");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// Negative control: materializing cross-file types must NOT soften the schema
// contract — a cross-file NON-record argument still fails loudly.
TEST(TableLoaderMaterializationTests, crossFileNonRecordStillRejected) {
    std::map<std::string, std::string> sources;
    sources["test.NotRec"] =
        "package test;\n"
        "public class NotRec {\n"
        "    public float64 v;\n"
        "    public NotRec() { this.v = 0.0; return; }\n"
        "}\n";
    sources["test.D"] =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] pv = heap float64[1];\n"
        "        pv[0] = 1.0;\n"
        "        Table<NotRec> t = heap Table<NotRec>(Column.of<float64>(pv));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        auto jit = CajetaJit::compile(sources, "test.D");
        FAIL() << "Table<NotRec> with a cross-file non-record compiled";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_FRAME_SCHEMA")
            << "wrong error: " << e.getMessage();
    }
}

// --- A+B combined: the fromCsv composition ---------------------------------

TEST(TableLoaderMaterializationTests, crossFileRecordThroughTemplateStaticBody) {
    // The full consumer shape: a cross-file record whose first Table<R>
    // instantiation happens inside a template-static body (exactly what
    // `Table.fromCsv<R>` / an ml `fit(Table<T>)` overload does).
    std::map<std::string, std::string> sources;
    sources["test.Q"] =
        "package test;\n"
        "public record Q {\n"
        "    float64 v;\n"
        "}\n";
    sources["test.D"] =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public final class D {\n"
        "    static #Table<R> wrap<R>(#Column<float64> c) {\n"
        "        Table<R> t = heap Table<R>(#c);\n"
        "        return #t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] vs = heap float64[2];\n"
        "        vs[0] = 4.5; vs[1] = 5.5;\n"
        "        Table<Q> t #= D.wrap<Q>(Column.of<float64>(vs));\n"
        "        if (t.v.get(0) == 4.5) { return 42; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

} // namespace
