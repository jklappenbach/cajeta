//
// Script units (script-units spec §2-§3): a script-shaped compilation unit —
// loose statements and top-level methods interleaved with type declarations —
// compiles as an implicit final class with a synthetic static entry. This
// header owns the SOURCE-LEVEL synthesis: given the parsed script tree and
// its token stream, produce the ordinary compilation unit the rest of the
// pipeline already understands. The original token text is spliced verbatim
// (whitespace and comments ride the HIDDEN channel, so intervals reproduce
// the source exactly); the wrapper adds only the package default, the class
// shell, and the entry method.
//
// The synthesized entry is `__cajeta_script_entry` and the default package
// is the reserved `cajeta.script` (spec §3.2). Line fidelity across the
// splice is script-units U5's diagnostics-mapping work, not done here.
//
#pragma once

#include <string>
#include <vector>

#include "CajetaParser.h"

namespace antlr4 { class CommonTokenStream; }

namespace cajeta {

    // The synthesized entry-method name, shared with hosts (`cajeta run`,
    // the Jupyter kernel) and tests.
    inline const char* scriptEntryName() { return "__cajeta_script_entry"; }

    // The reserved default package for package-less script units.
    inline const char* scriptDefaultPackage() { return "cajeta.script"; }

    // True when the parsed unit took the script alternative.
    bool isScriptUnit(CajetaParser::CompilationUnitContext* ctx);

    // Derive the implicit class name from a source path: file stem,
    // sanitized to an identifier ([A-Za-z0-9_], '_'-prefixed if it would
    // start with a digit; "script" if empty).
    std::string scriptClassStem(const std::string& sourcePath);

    // Build the wrapper compilation-unit source. `outCanonical` receives the
    // implicit class's canonical name (package + '.' + stem) so the caller
    // can mark it script-synthesized after registration. `outBindings`
    // receives the names declared at scriptMember level — the unit's
    // session bindings (spec §4); block-nested locals are not collected.
    std::string synthesizeScriptUnit(antlr4::CommonTokenStream& tokens,
                                     CajetaParser::CompilationUnitContext* ctx,
                                     const std::string& stem,
                                     std::string* outCanonical,
                                     std::vector<std::string>* outBindings);

}  // namespace cajeta
