// tensor-element-access 6.1.2, 6.1.3: copySmallBodiesIntoCallers on hand-written modules. A small
// body is copied available_externally, a large one is not, and a local the copy needs is
// promoted so both modules still verify.

#include <gtest/gtest.h>

#include "cajeta/compile/CrossModuleInline.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <memory>
#include <string>
#include "../PortableEnv.h"

namespace {

const char* kDefs = R"IR(
@greeting = private constant [3 x i8] c"hi\00"
define i32 @small(i32 %x) {
  %r = add i32 %x, 1
  ret i32 %r
}
define ptr @usesLocal() {
  ret ptr @greeting
}
define i32 @big(i32 %a0) {
BIGBODY
  ret i32 %a79
}
)IR";

const char* kUser = R"IR(
declare i32 @small(i32)
declare ptr @usesLocal()
declare i32 @big(i32)
define i32 @use(i32 %x) {
  %a = call i32 @small(i32 %x)
  %b = call i32 @big(i32 %a)
  %p = call ptr @usesLocal()
  ret i32 %b
}
)IR";

std::unique_ptr<llvm::Module> parse(const std::string& text, llvm::LLVMContext& ctx) {
    llvm::SMDiagnostic err;
    auto m = llvm::parseAssemblyString(text, err, ctx);
    if (!m) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        err.print("test", os);
        ADD_FAILURE() << msg;
    }
    return m;
}

bool verifies(llvm::Module& m) {
    std::string msg;
    llvm::raw_string_ostream os(msg);
    bool broken = llvm::verifyModule(m, &os);
    EXPECT_FALSE(broken) << msg;
    return !broken;
}

}  // namespace

TEST(CrossModuleInlinePass, copiesSmallBodiesOnlyAndPromotesTheirLocals) {
    std::string defs(kDefs);
    defs.replace(defs.find("BIGBODY"), 7, R"BIG(  %a1 = add i32 %a0, 1
  %a2 = add i32 %a1, 2
  %a3 = add i32 %a2, 3
  %a4 = add i32 %a3, 4
  %a5 = add i32 %a4, 5
  %a6 = add i32 %a5, 6
  %a7 = add i32 %a6, 7
  %a8 = add i32 %a7, 8
  %a9 = add i32 %a8, 9
  %a10 = add i32 %a9, 10
  %a11 = add i32 %a10, 11
  %a12 = add i32 %a11, 12
  %a13 = add i32 %a12, 13
  %a14 = add i32 %a13, 14
  %a15 = add i32 %a14, 15
  %a16 = add i32 %a15, 16
  %a17 = add i32 %a16, 17
  %a18 = add i32 %a17, 18
  %a19 = add i32 %a18, 19
  %a20 = add i32 %a19, 20
  %a21 = add i32 %a20, 21
  %a22 = add i32 %a21, 22
  %a23 = add i32 %a22, 23
  %a24 = add i32 %a23, 24
  %a25 = add i32 %a24, 25
  %a26 = add i32 %a25, 26
  %a27 = add i32 %a26, 27
  %a28 = add i32 %a27, 28
  %a29 = add i32 %a28, 29
  %a30 = add i32 %a29, 30
  %a31 = add i32 %a30, 31
  %a32 = add i32 %a31, 32
  %a33 = add i32 %a32, 33
  %a34 = add i32 %a33, 34
  %a35 = add i32 %a34, 35
  %a36 = add i32 %a35, 36
  %a37 = add i32 %a36, 37
  %a38 = add i32 %a37, 38
  %a39 = add i32 %a38, 39
  %a40 = add i32 %a39, 40
  %a41 = add i32 %a40, 41
  %a42 = add i32 %a41, 42
  %a43 = add i32 %a42, 43
  %a44 = add i32 %a43, 44
  %a45 = add i32 %a44, 45
  %a46 = add i32 %a45, 46
  %a47 = add i32 %a46, 47
  %a48 = add i32 %a47, 48
  %a49 = add i32 %a48, 49
  %a50 = add i32 %a49, 50
  %a51 = add i32 %a50, 51
  %a52 = add i32 %a51, 52
  %a53 = add i32 %a52, 53
  %a54 = add i32 %a53, 54
  %a55 = add i32 %a54, 55
  %a56 = add i32 %a55, 56
  %a57 = add i32 %a56, 57
  %a58 = add i32 %a57, 58
  %a59 = add i32 %a58, 59
  %a60 = add i32 %a59, 60
  %a61 = add i32 %a60, 61
  %a62 = add i32 %a61, 62
  %a63 = add i32 %a62, 63
  %a64 = add i32 %a63, 64
  %a65 = add i32 %a64, 65
  %a66 = add i32 %a65, 66
  %a67 = add i32 %a66, 67
  %a68 = add i32 %a67, 68
  %a69 = add i32 %a68, 69
  %a70 = add i32 %a69, 70
  %a71 = add i32 %a70, 71
  %a72 = add i32 %a71, 72
  %a73 = add i32 %a72, 73
  %a74 = add i32 %a73, 74
  %a75 = add i32 %a74, 75
  %a76 = add i32 %a75, 76
  %a77 = add i32 %a76, 77
  %a78 = add i32 %a77, 78
  %a79 = add i32 %a78, 79)BIG");
    llvm::LLVMContext ctx;
    auto lib = parse(defs, ctx);
    auto user = parse(kUser, ctx);
    ASSERT_TRUE(lib && user);
    int copies = cajeta::copySmallBodiesIntoCallers({lib.get(), user.get()});
    EXPECT_EQ(copies, 2);
    llvm::Function* small = user->getFunction("small");
    llvm::Function* usesLocal = user->getFunction("usesLocal");
    llvm::Function* big = user->getFunction("big");
    ASSERT_TRUE(small && usesLocal && big);
    EXPECT_FALSE(small->isDeclaration());
    EXPECT_EQ(small->getLinkage(), llvm::GlobalValue::AvailableExternallyLinkage);
    EXPECT_FALSE(usesLocal->isDeclaration());
    EXPECT_TRUE(big->isDeclaration()) << "a body over the size limit was copied";
    EXPECT_TRUE(lib->getFunction("small")->getLinkage() == llvm::GlobalValue::ExternalLinkage);
    for (llvm::GlobalVariable& g : lib->globals())
        EXPECT_FALSE(g.hasLocalLinkage()) << g.getName().str() << " was not promoted";
    verifies(*lib);
    verifies(*user);
}

TEST(CrossModuleInlinePass, theOptOutCopiesNothing) {
    llvm::LLVMContext ctx;
    std::string defs(kDefs);
    defs.replace(defs.find("BIGBODY"), 7, R"BIG(  %a1 = add i32 %a0, 1
  %a2 = add i32 %a1, 2
  %a3 = add i32 %a2, 3
  %a4 = add i32 %a3, 4
  %a5 = add i32 %a4, 5
  %a6 = add i32 %a5, 6
  %a7 = add i32 %a6, 7
  %a8 = add i32 %a7, 8
  %a9 = add i32 %a8, 9
  %a10 = add i32 %a9, 10
  %a11 = add i32 %a10, 11
  %a12 = add i32 %a11, 12
  %a13 = add i32 %a12, 13
  %a14 = add i32 %a13, 14
  %a15 = add i32 %a14, 15
  %a16 = add i32 %a15, 16
  %a17 = add i32 %a16, 17
  %a18 = add i32 %a17, 18
  %a19 = add i32 %a18, 19
  %a20 = add i32 %a19, 20
  %a21 = add i32 %a20, 21
  %a22 = add i32 %a21, 22
  %a23 = add i32 %a22, 23
  %a24 = add i32 %a23, 24
  %a25 = add i32 %a24, 25
  %a26 = add i32 %a25, 26
  %a27 = add i32 %a26, 27
  %a28 = add i32 %a27, 28
  %a29 = add i32 %a28, 29
  %a30 = add i32 %a29, 30
  %a31 = add i32 %a30, 31
  %a32 = add i32 %a31, 32
  %a33 = add i32 %a32, 33
  %a34 = add i32 %a33, 34
  %a35 = add i32 %a34, 35
  %a36 = add i32 %a35, 36
  %a37 = add i32 %a36, 37
  %a38 = add i32 %a37, 38
  %a39 = add i32 %a38, 39
  %a40 = add i32 %a39, 40
  %a41 = add i32 %a40, 41
  %a42 = add i32 %a41, 42
  %a43 = add i32 %a42, 43
  %a44 = add i32 %a43, 44
  %a45 = add i32 %a44, 45
  %a46 = add i32 %a45, 46
  %a47 = add i32 %a46, 47
  %a48 = add i32 %a47, 48
  %a49 = add i32 %a48, 49
  %a50 = add i32 %a49, 50
  %a51 = add i32 %a50, 51
  %a52 = add i32 %a51, 52
  %a53 = add i32 %a52, 53
  %a54 = add i32 %a53, 54
  %a55 = add i32 %a54, 55
  %a56 = add i32 %a55, 56
  %a57 = add i32 %a56, 57
  %a58 = add i32 %a57, 58
  %a59 = add i32 %a58, 59
  %a60 = add i32 %a59, 60
  %a61 = add i32 %a60, 61
  %a62 = add i32 %a61, 62
  %a63 = add i32 %a62, 63
  %a64 = add i32 %a63, 64
  %a65 = add i32 %a64, 65
  %a66 = add i32 %a65, 66
  %a67 = add i32 %a66, 67
  %a68 = add i32 %a67, 68
  %a69 = add i32 %a68, 69
  %a70 = add i32 %a69, 70
  %a71 = add i32 %a70, 71
  %a72 = add i32 %a71, 72
  %a73 = add i32 %a72, 73
  %a74 = add i32 %a73, 74
  %a75 = add i32 %a74, 75
  %a76 = add i32 %a75, 76
  %a77 = add i32 %a76, 77
  %a78 = add i32 %a77, 78
  %a79 = add i32 %a78, 79)BIG");
    auto lib = parse(defs, ctx);
    auto user = parse(kUser, ctx);
    ASSERT_TRUE(lib && user);
    setenv("CAJETA_XMODULE_INLINE", "0", 1);
    int copies = cajeta::copySmallBodiesIntoCallers({lib.get(), user.get()});
    unsetenv("CAJETA_XMODULE_INLINE");
    EXPECT_EQ(copies, 0);
    EXPECT_TRUE(user->getFunction("small")->isDeclaration());
}
