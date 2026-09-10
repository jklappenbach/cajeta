// SOURCE-LEVEL synthesis of script units: a script-shaped compilation unit is
// spliced verbatim into the ordinary unit the rest of the pipeline understands —
// an implicit final class in `cajeta.script` with a synthetic static entry.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "CajetaParser.h"

namespace antlr4 { class CommonTokenStream; }
namespace llvm { class Value; }

#include "ScriptLineMap.h"

namespace cajeta {

    class CajetaModule;
    typedef std::shared_ptr<CajetaModule> CajetaModulePtr;
    class Expression;
    typedef std::shared_ptr<Expression> ExpressionPtr;

    inline const char* scriptEntryName() { return "__cajeta_script_entry"; }

    // The reserved default package for package-less script units.
    inline const char* scriptDefaultPackage() { return "cajeta.script"; }

    // True when the parsed unit took the script alternative.
    bool isScriptUnit(CajetaParser::CompilationUnitContext* ctx);

    // The implicit class name for a source path: the file stem sanitized to an
    // identifier, '_'-prefixed if it would start with a digit, "script" if empty.
    std::string scriptClassStem(const std::string& sourcePath);

    // --- the session seam; all three helpers self-gate to a no-op off a session -

    // Seeds the entry's root scope from the module's SessionState, one Field per
    // earlier-unit binding, moved ones pre-demoted so a read rejects. Call right
    // after the entry method's scope is created.
    void seedSessionScope(CajetaModulePtr module);

    // Writes this unit's top-level bindings and moved-state updates back to the
    // SessionState. Call before the entry method's scope is destroyed.
    void writeBackSessionState(CajetaModulePtr module);

    // Emits the runtime call disarming `name`'s session registry slot after a `#`
    // transfer, so a later drop_all cannot double-drop it.
    void maybeEmitSessionDisarm(CajetaModulePtr module,
                                const std::string& name);

    // Refuses a use of a session binding whose class was REDEFINED since the value
    // was made: both generations share a canonical name, so the type check passes and
    // the old object is read through the NEW layout. Reinterpreting positions only.
    void rejectStaleGenerationUse(CajetaModulePtr module,
                                  const ExpressionPtr& expr,
                                  const std::string& position);

    // Builds the wrapper source, reporting the implicit class's canonical name, the
    // scriptMember-level bindings, the wrapper-to-host line spans and whether the
    // trailing `return 0;` was APPENDED. Token text only: no type is known here.
    std::string synthesizeScriptUnit(antlr4::CommonTokenStream& tokens,
                                     CajetaParser::CompilationUnitContext* ctx,
                                     const std::string& stem,
                                     std::string* outCanonical,
                                     std::vector<std::string>* outBindings,
                                     ScriptLineMap* outLineMap = nullptr,
                                     bool* outSyntheticTail = nullptr);

    // Renders the unit RESULT (`Out[N]`) and parks it in the session runtime — the
    // half of the trailing-expression decision synthesis cannot make. A no-op off a
    // session or for void; an unrenderable type degrades to its name, never fails.
    void emitScriptUnitResult(CajetaModulePtr module, const ExpressionPtr& expr,
                              llvm::Value* value);

    // Rewrites a semantic exception into the host's coordinates through the module's
    // line map, stamping an unlocated one with the current statement's host line.
    // Called only at the Method::generateCode boundary, so it never double-translates.
    class Exception;
    void remapScriptException(CajetaModulePtr module, Exception& e);

}  // namespace cajeta
