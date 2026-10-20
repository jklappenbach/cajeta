// Makes small functions defined in one module of an --emit=exe program inlinable in
// the others without LTO, by giving each caller module an available_externally copy.

#pragma once

#include <vector>

namespace llvm {
    class Module;
}

namespace cajeta {

    /** Instruction count above which a function is not copied into its callers' modules. */
    constexpr unsigned kCrossModuleInlineMaxInstructions = 64;

    /** Unoptimized hot-path size (blocks that do not end in `unreachable`, line-frame calls excluded) at or
     *  below which a copy is always inlined: accessor-sized bodies such as Storage.get. */
    constexpr unsigned kCrossModuleAlwaysInlineHotInstructions = 32;

    /** For each module in `mods`, adds an available_externally copy of every small function it
     *  calls directly that another module in `mods` defines; returns how many copies were made.
     *  A no-op when CAJETA_XMODULE_INLINE=0. `mods` must be every module of the program. */
    int copySmallBodiesIntoCallers(const std::vector<llvm::Module*>& mods);

}
