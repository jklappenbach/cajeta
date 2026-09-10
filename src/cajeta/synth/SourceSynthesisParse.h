// The source-synthesis ANTLR parse surface, split from SourceSynthesis.h so that
// header stays ANTLR-free and cheap to include.
#pragma once

#include <string>

#include "CajetaParser.h"

namespace cajeta::synth {

    // Parse a `{ ... }` class-body fragment, returning its classBody context
    // (never null; a parse error yields an error-node tree). The ANTLR pipeline
    // is DELIBERATELY LEAKED: codegen dereferences the tree's token pointers.
    CajetaParser::ClassBodyContext* parseClassBodyFragment(const std::string& src);

    // A full compilation unit — a method body wrapped in a throwaway class. Same leak.
    CajetaParser::CompilationUnitContext* parseSynthesizedUnit(const std::string& src);

    // A bare expression fragment, fed to Expression::fromContext to rebuild an
    // AST that is walked structurally, with no resolveTypes. Same leak.
    CajetaParser::ExpressionContext* parseExpressionFragment(const std::string& src);

}
