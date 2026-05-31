//
// Shared test helper: write a small Cajeta program to a fresh temp source root
// whose directory layout matches its `package` declaration, and clean it up on
// scope exit. Used by the debugger TDD harness's end-to-end JIT tests.
//
#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace cajeta::debugtest {

    struct TempProgram {
        std::filesystem::path root;

        // Write `source` at <root>/<pkgRelPath>/<fileName>. pkgRelPath mirrors
        // the program's package (e.g. "demo" for `package demo;`).
        TempProgram(const std::string& pkgRelPath,
                    const std::string& fileName,
                    const std::string& source) {
            static std::mt19937_64 rng(std::random_device{}());
            root = std::filesystem::temp_directory_path()
                 / ("cajeta_dbgtest_" + std::to_string(rng()));
            std::filesystem::path dir = root / pkgRelPath;
            std::filesystem::create_directories(dir);
            std::ofstream out(dir / fileName);
            out << source;
            out.close();
        }

        ~TempProgram() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        std::string sourceRoot() const { return root.string(); }
    };

} // namespace cajeta::debugtest
