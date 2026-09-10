// Forward-declares the stdlib SKILL corpus embedded at compiler-build time by
// tools/skillembed: one zstd blob, framed as SkillEmbed.cpp documents, decompressed on
// first access so stdlib skills are always available with no `.cja` or lockfile.

#pragma once

#include <cstddef>

namespace cajeta {
    namespace stdlib {

        extern const unsigned char g_stdlibSkillsCompressed[];
        extern const size_t g_stdlibSkillsCompressedLen;
        extern const size_t g_stdlibSkillsUncompressedLen;

    }
}
