// The kernel language inventory (xpu-kernel-independence 4.7, spec §3.1).
//
// A kernel is cajeta, and an author should not have to learn which part. A
// construct that host code accepts and kernel lowering refuses is a defect
// with a tracked item, or a stated reason it can never apply. This test is
// the list: every expression and statement kind the parser produces, each
// with a snippet that lowers, or an entry in the refusal list naming its
// item. The kinds are read off the AST headers, so a node class added later
// fails here until it is placed; a refusal entry without an item fails; and
// a refused kind that starts lowering fails until its entry moves, so the
// list says what the lowerer does today, not what it did once.
#include "gtest/gtest.h"

#include "KernelLoweringProbe.h"
#include "cajeta/error/Exception.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

using cajeta::xpu::probe::Lowered;
using cajeta::xpu::probe::lowerForNvptx;

namespace {

// One kernel, one snippet in its body. The class carries what a snippet may
// need around it: a constant, a scalar device helper, a device dispatch
// table and a callable, a value type.
std::string program(const std::string& body) {
    return R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.collection.HashMap;
import cajeta.xpu.Group;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
public class M {
    public static final float32 SCALE = 2.0f;
    public enum Mode { Fast, Slow }
    @Device
    public static float32 twice(float32 v) { return v * 2.0f; }
    @Kernel
    public static void k(KernelBuffer<float32> y, KernelBuffer<int32> z, uint32 which) {
        uint32 i = KernelThread.globalIdX();
)CJ" + body + R"CJ(
    }
}
)CJ";
}

struct Entry {
    const char* kind;      // the AST class
    const char* body;      // what exercises it in a kernel body
    const char* item;      // empty: it lowers; else the refusal's item or reason
};

// The inventory. A plan item (`4.8.1.1`) is a refusal that closes; `never:`
// names why a kind cannot apply to a kernel at all (no heap, no fibers, no
// exceptions, no objects on a device).
const Entry kInventory[] = {
    // ---- statements ----
    {"Block",                    "        { y[i] = 1.0f; }\n", ""},
    {"LabelStatement",           "        { y[i] = 1.0f; } { z[i] = 2; }\n", ""},
    {"IdentifierLabel",          "        outer: for (uint32 a = 0; a < 2; a = a + 1) { if (a == 1) { break outer; } y[i] = y[i] + 1.0f; }\n", ""},
    {"LocalVariableDeclaration", "        float32 t = y[i]; y[i] = t;\n", ""},
    {"ExpressionStatement",      "        y[i] = 3.0f;\n", ""},
    {"IfStatement",              "        if (which == 0) { y[i] = 1.0f; } else { y[i] = 2.0f; }\n", ""},
    {"ForStatement",             "        for (uint32 a = 0; a < 3; a = a + 1) { y[i] = y[i] + 1.0f; }\n", ""},
    {"EnhancedForStatement",     "        for (int32 bb : Group.stripe(4)) { z[i] = z[i] + bb; }\n", ""},
    {"WhileStatement",           "        uint32 a = 0; while (a < 3) { y[i] = y[i] + 1.0f; a = a + 1; }\n", ""},
    {"DoStatement",              "        uint32 a = 0; do { y[i] = y[i] + 1.0f; a = a + 1; } while (a < 3);\n", ""},
    {"BreakStatement",           "        for (uint32 a = 0; a < 3; a = a + 1) { if (a == 1) { break; } y[i] = y[i] + 1.0f; }\n", ""},
    {"ContinueStatement",        "        for (uint32 a = 0; a < 3; a = a + 1) { if (a == 1) { continue; } y[i] = y[i] + 1.0f; }\n", ""},
    {"ReturnStatement",          "        if (which == 0) { return; } y[i] = 1.0f;\n", ""},
    {"ScopeStatement",           "        scope { y[i] = 1.0f; }\n", "4.8.1.7"},
    {"SwitchStatement",          "        switch (which) { case 0: y[i] = 1.0f; break; default: y[i] = 2.0f; break; }\n", "4.8.1.6"},
    {"TryStatement",             "        try { y[i] = 1.0f; } catch (Exception e) { y[i] = 2.0f; }\n", "never: a kernel raises nothing; a device has no exception"},
    {"ThrowStatement",           "        throw heap Exception(\"no\");\n", "never: a kernel raises nothing; a device has no exception"},
    {"YieldStatement",           "        yield 1;\n", "never: a kernel is not a fiber"},
    // ---- expressions ----
    {"IdentifierExpression",     "        y[i] = (float32) which;\n", ""},
    {"IntegerLiteralExpression", "        z[i] = 7;\n", ""},
    {"FloatLiteralExpression",   "        y[i] = 1.5f;\n", ""},
    {"TextLiteralExpression",    "        boolean b = true; if (b) { y[i] = 1.0f; }\n", ""},
    {"TextLiteralExpression",    "        String s = \"a\"; y[i] = 1.0f;\n", ""},
    {"BinaryOpExpression",       "        y[i] = y[i] * 2.0f + 1.0f;\n", ""},
    {"PrefixExpression",         "        y[i] = -y[i];\n", ""},
    {"PostfixExpression",        "        uint32 c = which; c++; z[i] = (int32) c;\n", ""},
    {"CastExpression",           "        y[i] = (float32) z[i];\n", ""},
    {"DotExpression",            "        if (which == (uint32) Mode.Fast) { y[i] = 1.0f; }\n", ""},
    {"DotExpression",            "        y[i] = y[i] * M.SCALE;\n", "4.8.1.8"},
    {"IdentifierExpression",     "        y[i] = y[i] * SCALE;\n", "4.8.1.8"},
    {"MethodCallExpression",     "        y[i] = twice(y[i]);\n", ""},
    {"CallExpression",           "        (float32) -> float32 f = M::twice; y[i] = f(y[i]);\n", ""},
    {"NewExpression",            "        Shared<float32> s = shared float32[8]; s[i] = y[i]; y[i] = s[i];\n", ""},
    {"ArrayIndexExpression",     "        y[i] = y[i + 1];\n", ""},
    {"ArraySliceExpression",     "        Shared<int32> tile = shared [1, 2, 3]; Slice<int32> part = tile[1:3]; z[i] = 1;\n", "never: a slice is a heap view; a kernel has no heap"},
    {"ArrayLiteralExpression",   "        Shared<int32> tile = shared [1, 2, 3]; z[i] = tile[1];\n", ""},
    {"ArrayLiteralExpression",   "        int32[] arr = [1, 2, 3]; z[i] = arr[1];\n", "4.8.1.9"},
    {"AggregateInitializerExpression", "        int32[] arr = heap int32[] {1, 2, 3}; z[i] = arr[1];\n", "never: `heap T[] {...}` allocates; a kernel has no heap (the literal `[1, 2, 3]` lowers)"},
    {"MapLiteralExpression",     "        HashMap<int32, int32> m = [1: 2]; z[i] = 1;\n", "never: a map is a heap object; a kernel has no heap"},
    {"MoveExpression",           "        int64 c #= 0; z[i] = (int32) c;\n", ""},
    {"ThisExpression",           "        y[i] = this.SCALE;\n", "never: a kernel is static; there is no receiver"},
    {"SuperExpression",          "        y[i] = super.SCALE;\n", "never: a kernel is static; there is no receiver"},
    {"ClassLiteralExpression",   "        Class c = M.class; y[i] = 1.0f;\n", "never: a kernel has no runtime type objects"},
    {"BooleanSwitchExpression",  "        y[i] = which == 0 ? 1.0f : 2.0f;\n", "4.8.1.1"},
    {"InstanceOfExpression",     "        boolean b = y instanceof KernelBuffer; y[i] = 1.0f;\n", "never: a kernel has no runtime type objects"},
    {"MethodReferenceExpression","        (float32) -> float32 f = M::twice; y[i] = 1.0f;\n", ""},
    {"LambdaExpression",         "        y[i] = 1.0f; (float32) -> float32 f = (float32 v) -> v;\n", "never: a lambda is a closure; a function-typed local in a kernel takes a @Device method reference or a dispatch table"},
    {"SwitchExpression",         "        y[i] = switch (which) { case 0 -> 1.0f; default -> 2.0f; };\n", "4.8.1.6"},
    {"AwaitExpression",          "        y[i] = await 1.0f;\n", "never: a kernel is not a fiber"},
    {"SpawnExpression",          "        spawn twice(1.0f); y[i] = 1.0f;\n", "never: a kernel is not a fiber"},
    {"DetachExpression",         "        detach twice(1.0f); y[i] = 1.0f;\n", "never: a kernel is not a fiber"},
    {"UnsupportedExpression",    "        y[i] = 1.0f; super.foo();\n", "never: the parser's own stub for a form the language does not have"},
};

// Classes the headers declare that the parser never constructs. They are not
// kinds a kernel could meet, and the test says so rather than demanding a
// snippet for a node that cannot exist.
const std::set<std::string> kNotProduced = {
    "AssignmentStatement", "SynchronizedStatement", "SemiStatement",
    "DefaultBlockStatement",
};

// Every concrete node class the AST headers declare under Expression or
// Statement. Read from the source tree, so the inventory cannot drift from
// the parser by omission.
std::set<std::string> kindsInHeaders() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    EXPECT_NE(root, nullptr) << "CAJETA_SOURCE_ROOT names the checkout";
    std::set<std::string> kinds;
    if (!root) return kinds;
    const std::regex decl(
        R"(class\s+(\w+)\s*:\s*public\s+(Expression|PrimaryExpression|LiteralExpression|Statement|BlockStatement)\b)");
    const std::set<std::string> abstractKinds = {
        "Expression", "PrimaryExpression", "LiteralExpression", "Statement",
        "BlockStatement"};
    for (const char* dir : {"src/cajeta/asn", "src/cajeta/asn/expression"}) {
        for (auto& e : std::filesystem::directory_iterator(std::filesystem::path(root) / dir)) {
            if (e.path().extension() != ".h") continue;
            std::ifstream in(e.path());
            std::string text((std::istreambuf_iterator<char>(in)), {});
            for (auto it = std::sregex_iterator(text.begin(), text.end(), decl);
                 it != std::sregex_iterator(); ++it)
                if (!abstractKinds.count((*it)[1]))
                    kinds.insert((*it)[1]);
        }
    }
    return kinds;
}

} // namespace

// 4.7.1.1: every kind the parser produces is placed, and placed right.
TEST(XpuKernelInventory, everyNodeKindLowersOrIsRefusedWithItsItem) {
    std::set<std::string> placed;
    for (const Entry& e : kInventory) placed.insert(e.kind);

    for (const std::string& kind : kindsInHeaders()) {
        if (kNotProduced.count(kind)) continue;
        EXPECT_TRUE(placed.count(kind))
            << kind << " is a node kind the parser produces and the inventory does not place it";
    }
    for (const Entry& e : kInventory) {
        Lowered l;
        try {
            l = lowerForNvptx(program(e.body), "k", "test.M", "sm_89", "inventory");
        } catch (cajeta::Exception& ex) {
            l.ok = false; l.why = "host: " + ex.getMessage();
        } catch (const std::exception& ex) {
            l.ok = false; l.why = std::string("host: ") + ex.what();
        }
        if (std::string(e.item).empty()) {
            EXPECT_TRUE(l.ok) << e.kind << " is listed as lowering and was refused: " << l.why
                              << "\n" << e.body;
        } else {
            EXPECT_FALSE(l.ok) << e.kind << " is listed as refused under '" << e.item
                               << "' and lowers now; move it\n" << e.body;
            // The reason, so a refusal that moved from the lowerer to the
            // host compiler (or back) is visible in the run.
            fprintf(stderr, "[inventory] %s refused: %s\n", e.kind, l.why.c_str());
        }
    }
}

// 4.7.1.2: a refusal carries its item. An entry whose item is empty is a
// kind that lowers, so the only way to list a refusal is with the plan item
// that closes it or the reason it never applies.
TEST(XpuKernelInventory, aRefusalNamesItsItemOrItsReason) {
    for (const Entry& e : kInventory) {
        std::string item = e.item;
        if (item.empty()) continue;
        const bool planItem = std::regex_match(item, std::regex(R"(\d+\.\d+\.\d+\.\d+)"));
        const bool reason = item.rfind("never: ", 0) == 0 && item.size() > 7;
        EXPECT_TRUE(planItem || reason)
            << e.kind << ": a refusal names a plan item (4.8.1.1) or a reason (never: ...), not '"
            << item << "'";
    }
}
