//
// Forward-declares the cajeta stdlib SKILL corpus embedded at compiler-build
// time by tools/skillembed (wired in src/CMakeLists.txt). The bytes are a
// single zstd-compressed blob; the framing is documented in
// tools/skillembed/SkillEmbed.cpp. EmbeddedStdlibSkills.cpp decompresses and
// parses it on first access so stdlib skills are always available (no .cja /
// lockfile) while the binary carries them compressed (skill-discovery §2.4/§2.5).
//

#pragma once

#include <cstddef>

namespace cajeta {
    namespace stdlib {

        extern const unsigned char g_stdlibSkillsCompressed[];
        extern const size_t g_stdlibSkillsCompressedLen;
        extern const size_t g_stdlibSkillsUncompressedLen;

    }
}
