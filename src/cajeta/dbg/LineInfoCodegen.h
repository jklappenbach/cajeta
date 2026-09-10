// Line-info codegen: under --line-info a method body is wrapped in runtime
// enter/mark/leave calls so a captured stack trace resolves to
// Package.Class.method(File.cajeta:NN) with no debug info present.
#pragma once

#include <string>
#include "cajeta/compile/CajetaModule.h"

namespace cajeta::dbg {

    // Emit the prologue enter, building a per-method #FrameDesc constant. No-op
    // unless --line-info is on and the insert block is live.
    void emitLineEnter(cajeta::CajetaModulePtr module, const std::string& typeName,
                       const std::string& methodName, const std::string& fileName);

    // Emit __cajeta_line_leave() on a return path. No-op unless --line-info on.
    void emitLineLeave(cajeta::CajetaModulePtr module);

    // Mark a statement boundary. No-op unless --line-info is on, or if line <= 0.
    void emitLineMark(cajeta::CajetaModulePtr module, int line);

    // Snippet line -> real file line: a template instantiation is re-parsed from a
    // synthetic snippet, so its token lines need the instantiator's dbgLineDelta.
    // Every consumer must go through here. Clamps to 1, since a delta can overshoot.
    int fileLineFor(const cajeta::CajetaModulePtr& module, int snippetLine);

} // namespace cajeta::dbg
