// Exact-instrumentation codegen (cajeta-profiler §3). With
// `--profiler=instrument`, each SELECTED Cajeta method body is wrapped with:
//   %t0 = __cajeta_prof_instr_enter(&#ProfMethod)   — at the prologue
//        __cajeta_prof_instr_exit(&#ProfMethod, %t0) — on every return path
// so call counts are exact and inclusive time is measured rather than sampled.
//
// The enter RETURNS its timestamp and the exit takes it back, held in an alloca
// in the method's own frame. That is what makes this fiber-safe for free: the
// timing state lives on the stack that the fiber carries with it, so a yield in
// the middle of a probed method cannot misattribute the span to whatever
// resumed on the same carrier. It is also what makes §3.6 work — when the body
// is inlined, the alloca and both probes travel with it.
//
// Modeled on dbg/LineInfoCodegen, which is the same shape.
#pragma once

#include <string>
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/prof/ProfileFrame.h"
#include "cajeta/prof/ProfileSelection.h"

namespace llvm { class Function; }

namespace cajeta::prof {

    // The selection in force for this build, parsed from
    // CompilerFlags::profilerSelect. Cached per text, so the per-method
    // membership test does not re-parse the file for every method.
    const ProfileSelection& selectionFor(const cajeta::CajetaModulePtr& module);

    // Is `canonicalClassName` instrumented in this build? False whenever the
    // profiler is off, so callers need only ask this one question.
    bool isInstrumented(const cajeta::CajetaModulePtr& module,
                        const std::string& canonicalClassName);

    // Emit the prologue probe, building a per-method #ProfMethod global (the
    // counters live IN it, so there is no id assignment and no side table).
    // Returns an empty frame when no probe was emitted.
    ProfileFrame emitProfileEnter(cajeta::CajetaModulePtr module,
                                  const std::string& typeName,
                                  const std::string& methodName,
                                  const std::string& fileName);

    // Emit the matching exit at the current insert point. No-op on an empty
    // frame or a terminated block.
    void emitProfileExit(cajeta::CajetaModulePtr module, const ProfileFrame& frame);

    // Emit the matching exit immediately before every `ret` in `fn`. Used where
    // the return sites are not visible to the statement walker — the inline
    // lambda bodies of Expression.cpp, which finish their own function.
    void emitProfileExitAtReturns(cajeta::CajetaModulePtr module,
                                  llvm::Function* fn, const ProfileFrame& frame);

} // namespace cajeta::prof
