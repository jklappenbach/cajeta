//
// Source-synthesis facility (núcleo Layer-1a) — the ANTLR parse surface, split
// from SourceSynthesis.h so that header (deriveSynthName / injectImportIfUnbound)
// stays ANTLR-free and cheap to include. Callers here already depend on the
// generated parser. Plan: agents/cajeta/nucleo/source-synthesis-plan.md §1.
//
#pragma once

#include <string>

#include "CajetaParser.h"

namespace cajeta::synth {

    // Parse a `{ ... }` class-body fragment (the member-synthesis form, e.g. the
    // @Logged field or a record's synthesized `operator==`).
    //
    // The ANTLR pipeline is HEAP-allocated and DELIBERATELY LEAKED: the produced
    // parse tree holds token pointers that later codegen dereferences (a
    // synthesized field's clinit, a synthesized method's body), so the
    // lexer/stream/parser must outlive this call. This is compile-time and
    // bounded by the number of synthesized declarations. Returns the classBody
    // context (never null; a parse error yields an ANTLR error-node tree the
    // caller's visit step handles as today).
    CajetaParser::ClassBodyContext* parseClassBodyFragment(const std::string& src);

    // Parse a full compilation unit — the body-synthesis form, where a method
    // body is wrapped in a throwaway class and the concrete Method extracted.
    // Same deliberate leak (see above). Returns the compilationUnit context.
    CajetaParser::CompilationUnitContext* parseSynthesizedUnit(const std::string& src);

    // Parse a bare expression fragment — the seam for re-differentiating a
    // synthesized grad SOURCE string (second-order Grad, 3.1.6). The returned
    // context is fed to Expression::fromContext to rebuild the AST, which
    // buildDag walks structurally (no resolveTypes / scope needed). Same
    // deliberate leak (see above). Returns the expression context.
    CajetaParser::ExpressionContext* parseExpressionFragment(const std::string& src);

}
