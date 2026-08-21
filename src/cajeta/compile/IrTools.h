// `cajeta lower` / `cajeta disasm` — the LLVM tool surface coco needs, served
// by the compiler's own linked-in LLVM.
//
// WHY THIS EXISTS
//
// cajeta-coco drives LLVM directly: it harvests `--emit=ir`, instruments each
// module, then lowers and links with its own hands. That is what makes its
// numbers affordable — a front-end pass costs ~80s, an `llc` of one module
// ~0.03s, so a mutant is a re-lower plus a relink rather than a recompile.
// Dropping the tools is not an option.
//
// Spawning them was. coco called `llc` and `llvm-dis` off PATH, which is fine
// on a machine that built cajeta from source and silently wrong everywhere
// else. The .deb ships a compiler with LLVM linked IN; it does not ship the
// LLVM command-line tools. So after `apt install cajeta`, `llc` resolves to
// whatever the distro packages — LLVM 21 against a compiler emitting LLVM 23
// IR — and the failure surfaces as
//
//     error: unterminated attribute group
//
// from LLVM's parser, naming nothing that would lead anyone to a version
// mismatch. Requiring adopters to build LLVM from source to measure coverage
// is not a real answer.
//
// The compiler already holds the exactly-matching LLVM. Exposing it removes
// the dependency instead of packaging a second copy of it: coco spawns
// `cajeta` (which it already does, 15 times) and `cc`, and a version mismatch
// becomes structurally impossible rather than merely unlikely.

#pragma once

#include <string>

namespace cajeta {

    /**
     * `cajeta lower <in.ll|in.bc> -o <out.o>` — textual or bitcode IR to a
     * native object, the equivalent of `llc -filetype=obj`.
     *
     * Uses this compiler's TargetMachine, so the object matches what an
     * ordinary build produces. Returns a process exit code.
     */
    int irLowerCommand(int argc, const char* argv[]);

    /**
     * `cajeta disasm <in.bc> -o <out.ll>` — bitcode to textual IR, the
     * equivalent of `llvm-dis`.
     */
    int irDisasmCommand(int argc, const char* argv[]);

}
