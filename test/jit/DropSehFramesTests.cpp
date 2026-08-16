// Unit tests for cajeta::jit::dropSehFrames — the PrePrune pass that strips
// COFF SEH unwind tables out of JIT'd objects.
//
// These build a LinkGraph by hand rather than going through the JIT, for two
// reasons: the shape that matters (an Edge::KeepAlive pointing from .text into
// .pdata, which is how COFF associative COMDATs are modelled) is trivial to
// state directly, and LinkGraph is target-independent enough to construct on
// any host — so the COFF-only pass still gets tested on the Linux dev box and
// in CI, not only on the Windows runners where the bug actually bit.
//
// The regression under test is a use-after-free: LinkGraph::removeSection()
// destroys the section's Symbols and Blocks without scrubbing edges that point
// into it, and jitlink::prune() then reads and writes through those dangling
// Symbol*s. It presented as 13 of 434 release-subset tests dying with a bare
// SIGSEGV on Windows — the other 421 linked fine over the freed memory.

#include <gtest/gtest.h>

#include "../../src/cajeta/jit/JitCoffLinking.h"

#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>
#include <string>

using namespace llvm;
using namespace llvm::jitlink;

namespace {

const char* edgeKindName(Edge::Kind) { return "test-edge"; }

// A minimal stand-in for what COFFLinkGraphBuilder produces for one COMDAT
// function compiled with -fasynchronous-unwind-tables:
//
//   .text$fn   : the function body        <- live, promised by the JIT
//   .pdata$fn  : its RUNTIME_FUNCTION     <- associative COMDAT of .text$fn
//   .xdata$fn  : its unwind info          <- referenced only by .pdata$fn
//
// with the two edges COFF actually emits: a KeepAlive from .text into .pdata
// (the associative-COMDAT link, inbound to the section we remove) and an
// ordinary relocation from .pdata into .xdata.
struct SehGraph {
    std::unique_ptr<LinkGraph> g;
    Section* text;
    Section* pdata;
    Section* xdata;
    Symbol* fn;
    Symbol* pdataSym;

    SehGraph() {
        auto ssp = std::make_shared<orc::SymbolStringPool>();
        g = std::make_unique<LinkGraph>(
            "seh-test", std::move(ssp), Triple("x86_64-w64-windows-gnu"),
            SubtargetFeatures(), edgeKindName);

        text = &g->createSection(".text$fn", orc::MemProt::Read |
                                                 orc::MemProt::Exec);
        pdata = &g->createSection(".pdata$fn", orc::MemProt::Read);
        xdata = &g->createSection(".xdata$fn", orc::MemProt::Read);

        auto content = g->allocateContent(ArrayRef<char>("\xc3\x00\x00\x00", 4));
        auto& textBlock = g->createContentBlock(*text, content,
                                                orc::ExecutorAddr(0x1000), 16, 0);
        auto& pdataBlock = g->createContentBlock(
            *pdata, g->allocateContent(ArrayRef<char>("\x00\x00\x00\x00", 4)),
            orc::ExecutorAddr(0x2000), 4, 0);
        auto& xdataBlock = g->createContentBlock(
            *xdata, g->allocateContent(ArrayRef<char>("\x01\x00\x00\x00", 4)),
            orc::ExecutorAddr(0x3000), 4, 0);

        fn = &g->addDefinedSymbol(textBlock, 0, "fn", 4, Linkage::Strong,
                                  Scope::Default, true, /*IsLive=*/true);
        pdataSym = &g->addDefinedSymbol(pdataBlock, 0, "$pdata$fn", 4,
                                        Linkage::Strong, Scope::Local, false,
                                        /*IsLive=*/false);
        auto& xdataSym =
            g->addDefinedSymbol(xdataBlock, 0, "$xdata$fn", 4, Linkage::Strong,
                                Scope::Local, false, /*IsLive=*/false);

        // The associative-COMDAT link, in the direction COFF records it.
        textBlock.addEdge(Edge::KeepAlive, 0, *pdataSym, 0);
        // .pdata's RUNTIME_FUNCTION points at the unwind info in .xdata.
        pdataBlock.addEdge(Edge::FirstRelocation, 0, xdataSym, 0);
    }
};

// Every edge in the graph must resolve to a symbol the graph still owns.
// Without the inbound-edge scrub this walk is exactly what prune() does, and
// it reads freed memory.
void expectNoDanglingEdges(LinkGraph& g) {
    SmallPtrSet<const Symbol*, 16> reachable;
    for (auto* sym : g.defined_symbols())
        reachable.insert(sym);
    for (auto* sym : g.external_symbols())
        reachable.insert(sym);
    for (auto* sym : g.absolute_symbols())
        reachable.insert(sym);

    for (auto* b : g.blocks())
        for (auto& e : b->edges())
            EXPECT_TRUE(reachable.contains(&e.getTarget()))
                << "edge in section " << b->getSection().getName().str()
                << " targets a symbol the graph no longer owns";
}

} // namespace

// The pass must remove .pdata.
TEST(DropSehFramesTests, removesPdataSections) {
    SehGraph s;
    ASSERT_FALSE((bool) cajeta::jit::dropSehFrames(*s.g));

    for (auto& sec : s.g->sections())
        EXPECT_FALSE(sec.getName().starts_with(".pdata"))
            << "section " << sec.getName().str() << " survived";
}

// ...and must not leave the .text block pointing at the destroyed .pdata
// symbol. This is the regression: the KeepAlive edge outlives its target.
TEST(DropSehFramesTests, dropsInboundKeepAliveEdgesBeforeRemovingSection) {
    SehGraph s;
    ASSERT_FALSE((bool) cajeta::jit::dropSehFrames(*s.g));

    expectNoDanglingEdges(*s.g);

    // Specifically: the .text block has no edges left at all, since its only
    // edge was the associative-COMDAT KeepAlive into .pdata.
    EXPECT_EQ(std::distance(s.fn->getBlock().edges().begin(),
                            s.fn->getBlock().edges().end()),
              0);
}

// Edges that do not point into a removed section must be left alone — the
// scrub is targeted, not a blanket edge purge.
TEST(DropSehFramesTests, preservesEdgesThatDoNotTargetPdata) {
    SehGraph s;
    auto& other = s.g->createSection(".rdata", orc::MemProt::Read);
    auto& otherBlock = s.g->createContentBlock(
        other, s.g->allocateContent(ArrayRef<char>("\x00\x00\x00\x00", 4)),
        orc::ExecutorAddr(0x4000), 8, 0);
    auto& otherSym = s.g->addDefinedSymbol(otherBlock, 0, "g", 4,
                                           Linkage::Strong, Scope::Default,
                                           false, /*IsLive=*/true);
    s.fn->getBlock().addEdge(Edge::FirstRelocation, 0, otherSym, 0);

    ASSERT_FALSE((bool) cajeta::jit::dropSehFrames(*s.g));

    expectNoDanglingEdges(*s.g);
    EXPECT_EQ(std::distance(s.fn->getBlock().edges().begin(),
                            s.fn->getBlock().edges().end()),
              1);
    EXPECT_EQ(&s.fn->getBlock().edges().begin()->getTarget(), &otherSym);
}

// A graph with no unwind tables must come through untouched, and cheaply.
TEST(DropSehFramesTests, noOpWhenNoPdataPresent) {
    auto ssp = std::make_shared<orc::SymbolStringPool>();
    LinkGraph g("no-seh", std::move(ssp), Triple("x86_64-w64-windows-gnu"),
                SubtargetFeatures(), edgeKindName);
    auto& text = g.createSection(".text", orc::MemProt::Read | orc::MemProt::Exec);
    auto& b = g.createContentBlock(
        text, g.allocateContent(ArrayRef<char>("\xc3\x00\x00\x00", 4)),
        orc::ExecutorAddr(0x1000), 16, 0);
    auto& sym = g.addDefinedSymbol(b, 0, "fn", 4, Linkage::Strong,
                                   Scope::Default, true, /*IsLive=*/true);
    b.addEdge(Edge::FirstRelocation, 0, sym, 0);

    ASSERT_FALSE((bool) cajeta::jit::dropSehFrames(g));

    expectNoDanglingEdges(g);
    EXPECT_EQ(std::distance(b.edges().begin(), b.edges().end()), 1);
}
