// Native-deps unit 7 — DCE-aware native link input collection + resolution.
// See native-deps-plan.md unit 7, spec §5.
//
// The headline behavior (the DCE concern): a @Native(lib) requirement is
// recorded as module metadata, but a tree-shaken (pruned) forwarder leaves its
// extern symbol with NO uses — collectLiveNativeLibs must exclude it, so an
// unused native lib is never resolved/linked/fail-loud.

#include "cajeta/compile/NativeLink.h"

#include <gtest/gtest.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace cajeta;

namespace {
struct Built {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> m;
};
// Module recording @Native(lib, sym); `withCaller` adds a function that calls
// the extern (so it has a use → live); otherwise the extern is a use-less decl
// (the post-tree-shake "pruned forwarder" shape).
Built buildModule(const std::string& lib, const std::string& sym,
                  bool withCaller) {
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto m = std::make_unique<llvm::Module>("t", *ctx);
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx), false);
    auto* ext = llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, sym, *m);
    auto* nmd = m->getOrInsertNamedMetadata("cajeta.native.reqs");
    llvm::Metadata* e[] = {llvm::MDString::get(*ctx, lib),
                           llvm::MDString::get(*ctx, sym)};
    nmd->addOperand(llvm::MDNode::get(*ctx, e));
    if (withCaller) {
        auto* caller = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, "user", *m);
        auto* bb = llvm::BasicBlock::Create(*ctx, "e", caller);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(b.CreateCall(fnTy, ext));
    }
    return {std::move(ctx), std::move(m)};
}
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
} // namespace

// A live (referenced) @Native symbol contributes its lib.
TEST(NativeLinkTests, liveSymbolYieldsLib) {
    auto built = buildModule("zstd", "ZSTD_compress", /*withCaller=*/true);
    auto libs = collectLiveNativeLibs(*built.m);
    ASSERT_EQ(libs.size(), 1u);
    EXPECT_EQ(*libs.begin(), "zstd");
}

// THE DCE GATE: a recorded req whose symbol has no uses (pruned forwarder) is
// excluded — the native lib is not resolved/linked.
TEST(NativeLinkTests, prunedSymbolExcluded) {
    auto built = buildModule("zstd", "ZSTD_compress", /*withCaller=*/false);
    auto libs = collectLiveNativeLibs(*built.m);
    EXPECT_TRUE(libs.empty());
}

// No metadata at all → empty (no-op for non-@Native programs).
TEST(NativeLinkTests, noMetadataEmpty) {
    llvm::LLVMContext ctx;
    llvm::Module m("t", ctx);
    EXPECT_TRUE(collectLiveNativeLibs(m).empty());
}

// Resolution finds a static archive under a search dir's platform subdir.
TEST(NativeLinkTests, resolvesArchiveFromSearchDir) {
    auto base = std::filesystem::temp_directory_path() / "nd-link-search";
    auto pdir = base / "linux-x64";
    std::filesystem::create_directories(pdir);
    std::ofstream(pdir / "libfoo.a", std::ios::binary) << "ARCHIVE";

    auto r = resolveNativeArchivesForLink({"foo"}, "linux-x64", {base.string()});
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0], (pdir / "libfoo.a").string());
    std::filesystem::remove_all(base);
}

// A live lib that resolves nowhere fails loud, naming the lib + dirs searched.
TEST(NativeLinkTests, unresolvedLiveLibFailsLoud) {
    auto empty = std::filesystem::temp_directory_path() / "nd-link-empty";
    std::filesystem::create_directories(empty);
    auto r = resolveNativeArchivesForLink({"zstd"}, "linux-x64", {empty.string()});
    ASSERT_FALSE((bool) r);
    std::string msg = errText(r.takeError());
    EXPECT_NE(msg.find("zstd"), std::string::npos);
    EXPECT_NE(msg.find("not found"), std::string::npos);
    std::filesystem::remove_all(empty);
}

// hostNativePlatform produces an os-arch triple.
TEST(NativeLinkTests, hostPlatformShape) {
    std::string p = hostNativePlatform();
    EXPECT_NE(p.find('-'), std::string::npos);
}
