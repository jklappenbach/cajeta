// Line-info codegen (diagnostic-exceptions Unit 3, mechanism B). When
// --line-info is on (the default), each Cajeta method body is wrapped with:
//   __cajeta_line_enter(&#FrameDesc)  — at the prologue
//   __cajeta_line_mark(line)          — at each statement boundary
//   __cajeta_line_leave()             — on every normal return path
// so a captured stack trace resolves to Package.Class.method(File.cajeta:NN)
// with no debug info. Mirrors DebugCodegen's frame-enter/leave shape but is
// gated on CompilerFlags::lineInfo and carries type/method/file, not loc ids.
#pragma once

#include <string>
#include "cajeta/compile/CajetaModule.h"

namespace cajeta::dbg {

    // Emit the prologue __cajeta_line_enter(&desc), building a per-method
    // #FrameDesc { typeName, methodName, fileName } constant. No-op unless
    // --line-info is on and the insert block is live.
    void emitLineEnter(cajeta::CajetaModulePtr module, const std::string& typeName,
                       const std::string& methodName, const std::string& fileName);

    // Emit __cajeta_line_leave() on a return path. No-op unless --line-info on.
    void emitLineLeave(cajeta::CajetaModulePtr module);

    // Emit __cajeta_line_mark(line) at a statement boundary. No-op unless
    // --line-info on or line <= 0.
    void emitLineMark(cajeta::CajetaModulePtr module, int line);

} // namespace cajeta::dbg
