// Codegen helpers that emit the runtime calls building the per-fiber debug frame
// chain. Each is a no-op unless the module's debugInfo flag is set, so the
// scattered emission sites stay one-liners and the non-debug build is untouched.
#pragma once

#include <string>

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/dbg/MemoryFacets.h"
#include "llvm/IR/Value.h"

namespace cajeta::dbg {

    // Push a frame for cajeta-mangled `func`, returning the NODE the paired leave
    // consumes from an entry-block slot, or null when frames are off.
    llvm::Value* emitDbgFrameEnter(cajeta::CajetaModulePtr module,
                                   const std::string& func);

    // Pop the frame whose node `nodeSlot` holds; call it beside the drop teardown.
    void emitDbgFrameLeave(cajeta::CajetaModulePtr module, llvm::Value* nodeSlot);

    // Register a named local or parameter in the current frame. `slot` is its
    // alloca, `facets` the allocation class and ownership role, and `dropEntry` the
    // owner's drop-chain entry, or null: the runtime reads its `active` flag.
    void emitDbgLocal(cajeta::CajetaModulePtr module, const std::string& name,
                      const std::string& type, llvm::Value* slot,
                      MemoryFacets facets, llvm::Value* dropEntry);

    // Serialize the DbgLocTable into `module` with a ctor that registers it, so an
    // external debugger can map loc_id to (file, line). Call ONCE at end of codegen,
    // after every safepoint has claimed its id.
    void emitDbgLocTable(cajeta::CajetaModulePtr module);

} // namespace cajeta::dbg
