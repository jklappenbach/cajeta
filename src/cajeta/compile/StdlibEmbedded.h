// Forward-declares the stdlib embed manifest generated at compiler-build time: every
// `.cajeta` file under runtime/src/cajeta/ is concatenated into a cajeta::stdlib::g_files
// array that Compiler::parse walks as stdlib sources, keeping the binary self-contained.

#pragma once

#include <cstddef>

namespace cajeta {
    namespace stdlib {

        struct StdlibFile {
            const char* relativePath;
            const char* content;
            size_t contentBytes;
        };

        extern const StdlibFile g_files[];
        extern const size_t g_fileCount;

    }
}
