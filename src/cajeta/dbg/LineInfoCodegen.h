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

    // Snippet line -> real file line for the method being generated.
    //
    // A class- or method-template instantiation is re-parsed from a SYNTHETIC
    // SNIPPET, so its token lines are snippet lines that merely look plausible
    // (TemplateInstantiator §9.2). The instantiator records the correction as a
    // dbgLineDelta on the method, or on its class for class templates.
    //
    // The debugger's safepoint path applied this and the line-info path did
    // not, so F7 landed correctly while STACK TRACES and PROFILE SLICES for
    // every generic reported a snippet line — measured 2026-09-01 as
    // `Optional<int32>.get` at Optional.cajeta:83 for a throw on line 112, and
    // as a controlled 1-line miss in a project generic beside a non-generic
    // control that was exact. Both land in the doc comment above the method,
    // which is the symptom §9.2 already names for F7.
    //
    // One function so the two paths cannot drift apart again. Clamps to 1: a
    // delta can overshoot on a deeply nested instantiation, and a non-positive
    // line means "no line" to every consumer.
    int fileLineFor(const cajeta::CajetaModulePtr& module, int snippetLine);

} // namespace cajeta::dbg
