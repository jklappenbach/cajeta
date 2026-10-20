// Whole-program devirtualization for --emit=exe: a vtable lookup whose every entry in
// the program names one function becomes a direct call.

#pragma once

#include <vector>

namespace llvm {
    class Module;
}

namespace cajeta {

    /** Replaces each __cajeta_vtable_lookup with a constant hash that every #VTable in `mods`
     *  maps to the same function by that function; returns how many were replaced.
     *  A no-op when CAJETA_DEVIRT=0. `mods` must be every module of the program. */
    int devirtualizeUniqueSlots(const std::vector<llvm::Module*>& mods);

}
