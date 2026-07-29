//
// nucleo-frame U16 — Arrow interop at the TABLE boundary (plan 16.1.2-16.1.3;
// spec §10). A whole table enters/exits the Arrow world zero-copy over the
// per-column C-Data-Interface seam (nucleo-column §4):
//   - Table.exportArrow() hands one {ArrowSchema, ArrowArray} bundle per column
//     over the live buffers (no serialization, no copy);
//   - Table.importArrow<R>(schemas, arrays, n) adopts an external array set
//     zero-copy, CHECKED against R: the per-column airlock enforces the
//     null-count rule (§10.3), the positional rebind checks each field's
//     physical/nullability (a mismatch throws), like .as<R>()'s materialized
//     branch;
//   - an extension-typed (MXFP4) array carries through as its physical column
//     — the bytes move even where the semantics are unknown (§10.4).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kPrelude =
    "package test;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "import cajeta.nucleo.column.MxColumn;\n"
    "import cajeta.nucleo.column.ColumnTypeException;\n"
    "import cajeta.nucleo.column.ArrowFfi;\n"
    "public record Bar { float64 a; int64 k; Utf8 s; }\n"
    "public record BarBad { int64 a; int64 k; Utf8 s; }\n"
    "public record NRec { float64 a; @Nullable float64 nb; }\n"
    "public record NRecNN { float64 a; float64 nb; }\n"
    "public record URec { uint8 p; }\n";

// Split each export bundle into its (schema, array) address pair.
const char* kSplit =
    "        ArrowFfi f = stack ArrowFfi();\n"
    "        int64[] sc = heap int64[__nc];\n"
    "        int64[] ar = heap int64[__nc];\n"
    "        int32 __i = 0;\n"
    "        while (__i < __nc) {\n"
    "            sc[__i] = bundles[__i];\n"
    "            ar[__i] = f.bundleArrayAddr(bundles[__i]);\n"
    "            __i = __i + 1;\n"
    "        }\n";

} // namespace

// 16.1.2 — round-trip a non-null / utf8 table: export to Arrow bundles, split
// each into (schema, array) addresses, re-import as Table<Bar>, values intact.
TEST(TableArrowTests, exportImportRoundTrip) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] av = heap float64[3];\n"
        "        av[0] = 1.5; av[1] = 2.5; av[2] = 3.5;\n"
        "        int64[] kv = heap int64[3];\n"
        "        kv[0] = 10; kv[1] = 20; kv[2] = 30;\n"
        "        String[] sv = heap String[3];\n"
        "        sv[0] = \"x\"; sv[1] = \"y\"; sv[2] = \"z\";\n"
        "        Table<Bar> t = heap Table<Bar>(\n"
        "            Column.of<float64>(av), Column.of<int64>(kv),\n"
        "            StringColumn.of(sv));\n"
        "        int64[] bundles = t.exportArrow();\n"
        "        int32 __nc = 3;\n"
        + kSplit +
        "        Table<Bar> r = Table.importArrow<Bar>(sc, ar, 3);\n"
        "        int32 score = 0;\n"
        "        if (r.rowCount() == 3) { score = score + 1; }\n"
        "        if (r.a.get(0) == 1.5 && r.a.get(2) == 3.5) { score = score + 2; }\n"
        "        if (r.k.get(0) == 10 && r.k.get(2) == 30) { score = score + 4; }\n"
        "        if (r.s.get(0).equals(\"x\") && r.s.get(2).equals(\"z\")) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// 16.1.2 (nullable) — a @Nullable column round-trips with its validity bitmap:
// the missing element stays missing, distinct from a present value.
TEST(TableArrowTests, nullableColumnRoundTrip) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] av = heap float64[3];\n"
        "        av[0] = 1.0; av[1] = 2.0; av[2] = 3.0;\n"
        "        float64[] nv = heap float64[3];\n"
        "        nv[0] = 10.0; nv[1] = 0.0; nv[2] = 30.0;\n"
        "        boolean[] ok = heap boolean[3];\n"
        "        ok[0] = true; ok[1] = false; ok[2] = true;\n"
        "        Table<NRec> t = heap Table<NRec>(\n"
        "            Column.of<float64>(av), NullableColumn.of<float64>(nv, ok));\n"
        "        int64[] bundles = t.exportArrow();\n"
        "        int32 __nc = 2;\n"
        + kSplit +
        "        Table<NRec> r = Table.importArrow<NRec>(sc, ar, 2);\n"
        "        int32 score = 0;\n"
        "        if (r.rowCount() == 3 && r.a.get(1) == 2.0) { score = score + 1; }\n"
        "        if (r.nb.isValid(0) && r.nb.get(0) == 10.0) { score = score + 2; }\n"
        "        if (!r.nb.isValid(1)) { score = score + 4; }\n"
        "        if (r.nb.isValid(2) && r.nb.get(2) == 30.0) { score = score + 8; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// 16.1.2 (checks) — the boundary is CHECKED against R: a physical mismatch (a
// float64 array bound to an int64 field) and a null-count mismatch (a
// null-carrying array bound to a non-null field, §10.3) each fail loud.
TEST(TableArrowTests, importCheckedAgainstRecord) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        // dtype mismatch: float64 column 'a' bound to BarBad's int64 field
        "        float64[] av = heap float64[2];\n"
        "        av[0] = 1.5; av[1] = 2.5;\n"
        "        int64[] kv = heap int64[2];\n"
        "        kv[0] = 10; kv[1] = 20;\n"
        "        String[] sv = heap String[2];\n"
        "        sv[0] = \"x\"; sv[1] = \"y\";\n"
        "        Table<Bar> t = heap Table<Bar>(\n"
        "            Column.of<float64>(av), Column.of<int64>(kv),\n"
        "            StringColumn.of(sv));\n"
        "        int64[] bundles = t.exportArrow();\n"
        "        int32 __nc = 3;\n"
        + kSplit +
        "        try {\n"
        "            Table<BarBad> bad = Table.importArrow<BarBad>(sc, ar, 3);\n"
        "        } catch (ColumnTypeException e) {\n"
        "            score = score + 1;\n"
        "        }\n"
        // null-count mismatch: a null-carrying array bound to a non-null field
        "        float64[] a2 = heap float64[3];\n"
        "        a2[0] = 1.0; a2[1] = 2.0; a2[2] = 3.0;\n"
        "        float64[] n2 = heap float64[3];\n"
        "        n2[0] = 10.0; n2[1] = 0.0; n2[2] = 30.0;\n"
        "        boolean[] ok2 = heap boolean[3];\n"
        "        ok2[0] = true; ok2[1] = false; ok2[2] = true;\n"
        "        Table<NRec> tn = heap Table<NRec>(\n"
        "            Column.of<float64>(a2), NullableColumn.of<float64>(n2, ok2));\n"
        "        int64[] bn = tn.exportArrow();\n"
        "        int64 s0 = bn[0]; int64 s1 = bn[1];\n"
        "        ArrowFfi g = stack ArrowFfi();\n"
        "        int64[] sc2 = heap int64[2];\n"
        "        int64[] ar2 = heap int64[2];\n"
        "        sc2[0] = s0; ar2[0] = g.bundleArrayAddr(s0);\n"
        "        sc2[1] = s1; ar2[1] = g.bundleArrayAddr(s1);\n"
        "        try {\n"
        "            Table<NRecNN> bad2 = Table.importArrow<NRecNN>(sc2, ar2, 2);\n"
        "        } catch (ColumnTypeException e) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 16.1.3 — an MXFP4 extension array carries through the table boundary as its
// physical uint8 column: the bytes move even though the table does not model
// the extension's semantics (§10.4 graceful degradation).
TEST(TableArrowTests, extensionColumnCarriesThroughAsPhysical) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        uint8[] pv = heap uint8[4];\n"
        "        pv[0] = (uint8) 17; pv[1] = (uint8) 34;\n"
        "        pv[2] = (uint8) 51; pv[3] = (uint8) 68;\n"
        "        Column<uint8> p = Column.of<uint8>(pv);\n"
        "        MxColumn m = MxColumn.mxfp4(#p, 32);\n"
        "        int64 bundle = m.exportArrow();\n"
        "        ArrowFfi f = stack ArrowFfi();\n"
        "        int64[] sc = heap int64[1];\n"
        "        int64[] ar = heap int64[1];\n"
        "        sc[0] = bundle;\n"
        "        ar[0] = f.bundleArrayAddr(bundle);\n"
        "        Table<URec> r = Table.importArrow<URec>(sc, ar, 1);\n"
        "        int32 score = 0;\n"
        "        if (r.rowCount() == 4) { score = score + 1; }\n"
        "        if (r.p.get(0) == (uint8) 17 && r.p.get(3) == (uint8) 68) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}
