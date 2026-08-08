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

#include <memory>
#include <string>
#include <vector>

#include "CajetaParser.h"

namespace antlr4 { class CommonTokenStream; }

#include "ScriptLineMap.h"

namespace cajeta {

    class CajetaModule;
    typedef std::shared_ptr<CajetaModule> CajetaModulePtr;

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

    // --- U4: the session seam -------------------------------------------
    // All three helpers self-gate (no-op unless the module is a script unit
    // compiling into a session / the builder sits in the script entry), so
    // call sites stay one line.

    // Seed the script entry's root scope from the module's SessionState:
    // one Field per earlier-unit binding (sessionSeeded, no alloca), with
    // moved bindings pre-demoted so reads reject (spec §4.2). Call right
    // after the entry method's scope is created.
    void seedSessionScope(CajetaModulePtr module);

    // Write ownership facts back to the SessionState after the entry's body
    // codegen: this unit's top-level bindings (name, canonical type, moved
    // state, transfer site) plus moved-state updates for seeded names. Call
    // before the entry method's scope is destroyed.
    void writeBackSessionState(CajetaModulePtr module);

    // A `#` transfer moved `name`'s title to a new owner: emit the runtime
    // call that disarms the session registry slot so drop_all doesn't
    // double-drop (spec §4.2). No-op unless `name` is a session binding and
    // the builder is inside the script entry.
    void maybeEmitSessionDisarm(CajetaModulePtr module,
                                const std::string& name);

    // Build the wrapper compilation-unit source. `outCanonical` receives the
    // implicit class's canonical name (package + '.' + stem) so the caller
    // can mark it script-synthesized after registration. `outBindings`
    // receives the names declared at scriptMember level — the unit's
    // session bindings (spec §4); block-nested locals are not collected.
    // `outLineMap` (U5) receives the wrapper→host line spans for diagnostic
    // translation; pass null to skip.
    std::string synthesizeScriptUnit(antlr4::CommonTokenStream& tokens,
                                     CajetaParser::CompilationUnitContext* ctx,
                                     const std::string& stem,
                                     std::string* outCanonical,
                                     std::vector<std::string>* outBindings,
                                     ScriptLineMap* outLineMap = nullptr);

    // U5 (spec §6.1) — rewrite a semantic exception into the host's
    // coordinates: host source name as the file, wrapper lines translated
    // through the module's line map; an unlocated exception is stamped with
    // the current statement's host line (Block stamps it per statement).
    // No-op for ordinary modules. Called at the Method::generateCode
    // boundary — the single remap point, so it never double-translates.
    class Exception;
    void remapScriptException(CajetaModulePtr module, Exception& e);

}  // namespace cajeta
