// Exact-instrumentation codegen: a selected method body gets a prologue enter
// probe and an exit probe on every return path. The enter's timestamp rides an
// alloca in the method's own frame, which is what makes it fiber-safe for free.
#pragma once

#include <string>
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/prof/ProfileFrame.h"
#include "cajeta/prof/ProfileSelection.h"

namespace llvm { class Function; }

namespace cajeta::prof {

    // The selection in force for this build, cached per text.
    const ProfileSelection& selectionFor(const cajeta::CajetaModulePtr& module);

    // Is this class instrumented? False whenever the profiler is off, too.
    bool isInstrumented(const cajeta::CajetaModulePtr& module,
                        const std::string& canonicalClassName);

    // Emit the prologue probe, building a per-method #ProfMethod global that the
    // counters live in. Returns an empty frame when no probe was emitted.
    ProfileFrame emitProfileEnter(cajeta::CajetaModulePtr module,
                                  const std::string& typeName,
                                  const std::string& methodName,
                                  const std::string& fileName);

    // The matching exit at the insert point; a no-op on an empty frame.
    void emitProfileExit(cajeta::CajetaModulePtr module, const ProfileFrame& frame);

    // The matching exit before every `ret` in `fn`, for bodies whose return sites the statement walker cannot see.
    void emitProfileExitAtReturns(cajeta::CajetaModulePtr module,
                                  llvm::Function* fn, const ProfileFrame& frame);

} // namespace cajeta::prof
