//
// Forward-declares the cajeta stdlib embed manifest produced by
// src/EmbedStdlib.cmake at compiler-build time.
//
// The CMake step concatenates every `.cajeta` file under
// runtime/src/cajeta/ into a generated .cpp that defines a
// cajeta::stdlib::g_files array of StdlibFile entries. Compiler::parse
// iterates this array at parse time, treating each entry as a stdlib
// source. Keeping the stdlib on disk (as real cajeta files) rather
// than inline C string literals makes it reviewable and editable like
// any other cajeta code; baking the bytes into the compiler binary
// keeps the distribution self-contained.
//

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
