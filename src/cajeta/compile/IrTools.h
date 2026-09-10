// `cajeta lower` / `cajeta disasm` - the LLVM tool surface coco needs, served by the
// compiler's own linked-in LLVM. Spawning `llc` / `llvm-dis` off PATH picks up whatever
// LLVM the distro packages, which mismatches the IR version this compiler emits.
#pragma once

#include <string>

namespace cajeta {

    /** `cajeta lower <in.ll|in.bc> -o <out.o>` - textual or bitcode IR to a native
     *  object, the equivalent of `llc -filetype=obj`. Uses this compiler's
     *  TargetMachine, so the object matches an ordinary build. Returns an exit code. */
    int irLowerCommand(int argc, const char* argv[]);

    /** `cajeta disasm <in.bc> -o <out.ll>` - bitcode to textual IR (`llvm-dis`). */
    int irDisasmCommand(int argc, const char* argv[]);

}
