//
// transform-intrinsics Unit 6 — Jit (spec §6.3, §6.4). `Jit(f)` is a drop-in
// faster `f`: same call signature, same result, body FUSED (temporaries
// eliminated). U1's identity placeholder returned f's record unchanged; U6
// clones f's specialized function as `__JitFused_<name>`, runs the O2
// function-simplification pipeline over the clone, and returns a record
// dispatching to it. Composition (`Jit(Vmap(Grad(f)))`) fuses the synthesized,
// already-batched, already-differentiated functions in place.
//
// The fusion probes ride the captureIr channel (post-codegen, post-O0 IR): the
// fused clone must exist and carry strictly fewer allocas than the original —
// the "fewer intermediate allocations" probe the plan names.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// The alloca count inside the body of the first `define` whose signature line
// contains `fnNeedle` and for which `exclude` (when non-empty) does NOT appear
// in the signature line. -1 when no such function exists.
int allocasIn(const std::string& ir, const std::string& fnNeedle,
              const std::string& exclude = "") {
    size_t d = ir.find("define ");
    while (d != std::string::npos) {
        size_t nl = ir.find('\n', d);
        size_t end = ir.find("\n}", d);
        if (nl == std::string::npos || end == std::string::npos) break;
        std::string sig = ir.substr(d, nl - d);
        if (sig.find(fnNeedle) != std::string::npos
                && (exclude.empty() || sig.find(exclude) == std::string::npos)) {
            int n = 0;
            for (size_t p = ir.find(" = alloca ", d);
                 p != std::string::npos && p < end;
                 p = ir.find(" = alloca ", p + 1)) ++n;
            return n;
        }
        d = ir.find("define ", end);
    }
    return -1;
}

// The body text of the first `define` whose signature contains `fnNeedle`.
std::string bodyOf(const std::string& ir, const std::string& fnNeedle) {
    size_t d = ir.find("define ");
    while (d != std::string::npos) {
        size_t nl = ir.find('\n', d);
        size_t end = ir.find("\n}", d);
        if (nl == std::string::npos || end == std::string::npos) break;
        if (ir.substr(d, nl - d).find(fnNeedle) != std::string::npos) {
            return ir.substr(d, end - d);
        }
        d = ir.find("define ", end);
    }
    return "";
}

} // namespace

// 6.1.1 (drop-in) — Jit(f) has f's exact signature and produces f's result.
TEST(JitFuse, dropInSameSignatureSameResult) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 g = Jit((int32 x) -> (x + 1) * (x - 1));\n"
        "        return g(5);\n"
        "    }\n"
        "}\n", "test.T");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 24);
}

// 6.1.1 (fusion probe) — the fused clone exists in the emitted IR and carries
// strictly FEWER allocas than the original lambda function (temporaries
// eliminated; at O0 the original keeps its param/temp slots).
TEST(JitFuse, fusedCloneHasFewerAllocas) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 g = Jit((int32 x) -> (x + 1) * (x - 1));\n"
        "        return g(5);\n"
        "    }\n"
        "}\n", "test.T", opts);
    ASSERT_EQ(jit->lookup<int32_t (*)()>("run")(), 24);

    const std::string& ir = jit->getModuleIr();
    int fused = allocasIn(ir, "__JitFused_");
    ASSERT_GE(fused, 0) << "no __JitFused_ function in the emitted IR";

    // The original: the same lambda define, minus the fused prefix. The first
    // __JitFused_ occurrence may be a REFERENCE (the record initializer), so the
    // name ends at any of quote/paren/comma/space/newline.
    size_t tag = ir.find("__JitFused_");
    ASSERT_NE(tag, std::string::npos);
    size_t nameEnd = ir.find_first_of("\"(), \n", tag);
    std::string orig = ir.substr(tag + std::string("__JitFused_").size(),
                                 nameEnd - tag - std::string("__JitFused_").size());
    int unfused = allocasIn(ir, orig, /*exclude=*/"__JitFused_");
    ASSERT_GT(unfused, 0) << "original lambda '" << orig
                          << "' not found or already alloca-free at O0";
    EXPECT_LT(fused, unfused)
        << "fusion did not reduce allocas (fused=" << fused
        << ", original=" << unfused << ")";
}

// 6.1.2 — Jit(Vmap(Grad(f))) fuses the already-batched, already-differentiated
// form and still produces the per-example gradients (the U5 5.1.3 result).
// The probe: a __JitFused_ function whose ONE body carries both the batch loop
// (a back-edge branch) and the differentiated arithmetic (fmul) — one fused
// form over the whole composition.
TEST(JitFuse, composesOverVmapGrad) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        float32[] xs = [1.0f, 2.0f, 3.0f];\n"
        "        (float32[]) -> #GradResult<float32,float32>[] g =\n"
        "            Jit(Vmap(Grad((float32 x) -> x * x)));\n"
        "        GradResult<float32,float32>[] rs = g(xs);\n"
        "        return rs[0].grads + rs[1].grads * 10.0f + rs[2].grads * 100.0f;\n"
        "    }\n"
        "}\n", "test.T", opts);
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 642.0f);

    const std::string& ir = jit->getModuleIr();
    // Walk every __JitFused_ function; at least one must hold loop + grad math.
    bool oneFusedForm = false;
    size_t at = 0;
    while ((at = ir.find("__JitFused_", at)) != std::string::npos) {
        size_t nameEnd = ir.find_first_of("\"(), \n", at);
        std::string body = bodyOf(ir, ir.substr(at, nameEnd - at));
        if (body.find("fmul") != std::string::npos
                && body.find("br label") != std::string::npos) {
            oneFusedForm = true;
            break;
        }
        at = nameEnd;
    }
    EXPECT_TRUE(oneFusedForm)
        << "no single __JitFused_ function carries both the batch loop and the "
           "differentiated arithmetic";
}

// 3.3.3 (closed by U6) — the synthesized backward participates in ordinary IR
// optimization: Jit(Grad(f)) runs the standard function pipeline over the
// backward's fresh functions (tagged __JitFused_) and the gradient survives it
// unchanged — the backward is ordinary IR, not something opt must tiptoe past.
TEST(JitFuse, backwardParticipatesInOrdinaryOpt) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        (float32) -> GradResult<float32,float32> g =\n"
        "            Jit(Grad((float32 x) -> x * x));\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;\n"
        "    }\n"
        "}\n", "test.T", opts);
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 906.0f);
    EXPECT_NE(jit->getModuleIr().find("__JitFused_"), std::string::npos)
        << "the backward's synthesized functions were not run through the "
           "ordinary optimization pipeline";
}

// 6.2.2 (signature preservation, second shape) — the Jit'd function is stored,
// passed through a second call, and still dispatches with f's signature.
TEST(JitFuse, jitOfVmapAloneStillBatches) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        float32[] xs = [2.0f, 5.0f];\n"
        "        (float32[]) -> #float32[] g = Jit(Vmap((float32 x) -> x + x));\n"
        "        float32[] ys = g(xs);\n"
        "        return ys[0] + ys[1] * 10.0f;\n"
        "    }\n"
        "}\n", "test.T");
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 104.0f);
}
