#include "cajeta/buildtool/InitTemplates.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <fstream>

namespace cajeta::buildtool {

    // Symbols defined by cajeta_init_embedded.cpp (CMake-generated
    // via cmake/EmbedInitTemplates.cmake). The shape is duplicated
    // here as plain structs because the generated file deliberately
    // pulls in only <cstddef> — keeping the embed compile-fast and
    // standalone from the rest of the build-tool headers.
    struct InitTemplateFileRow {
        const char* relativePath;
        const char* content;
        std::size_t contentBytes;
    };
    struct InitTemplateRow {
        const char* name;
        std::size_t firstFile;
        std::size_t fileCount;
    };
    extern const InitTemplateFileRow g_initTemplateFiles[];
    extern const InitTemplateRow     g_initTemplates[];
    extern const std::size_t         g_initTemplateCount;

    namespace {

        llvm::Error cite(const std::string& where, const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                where + ": " + msg);
        }

        const InitTemplateRow* findRow(std::string_view name) {
            for (std::size_t i = 0; i < g_initTemplateCount; ++i) {
                if (name == g_initTemplates[i].name) {
                    return &g_initTemplates[i];
                }
            }
            return nullptr;
        }

    } // namespace

    std::vector<std::string> availableInitTemplates() {
        std::vector<std::string> out;
        out.reserve(g_initTemplateCount);
        for (std::size_t i = 0; i < g_initTemplateCount; ++i) {
            out.emplace_back(g_initTemplates[i].name);
        }
        return out;
    }

    std::optional<InitTemplate> findInitTemplate(std::string_view name) {
        const auto* row = findRow(name);
        if (!row) return std::nullopt;
        InitTemplate t;
        t.name = row->name;
        t.files.reserve(row->fileCount);
        for (std::size_t i = 0; i < row->fileCount; ++i) {
            const auto& f = g_initTemplateFiles[row->firstFile + i];
            InitTemplateFile entry;
            entry.relativePath = f.relativePath;
            entry.contents = std::string_view(f.content, f.contentBytes);
            t.files.push_back(std::move(entry));
        }
        return t;
    }

    llvm::Expected<InitWriteResult> instantiateInitTemplate(
        std::string_view templateName,
        const std::string& destDir,
        bool force) {

        const auto* row = findRow(templateName);
        if (!row) {
            std::string available;
            for (std::size_t i = 0; i < g_initTemplateCount; ++i) {
                if (!available.empty()) available += ", ";
                available += g_initTemplates[i].name;
            }
            return cite("cajeta init",
                "unknown template '" + std::string(templateName) +
                "' (available: " + available + ")");
        }

        // Validate destDir: either doesn't exist (we'll create it)
        // or exists as a directory. Refuse files-where-a-dir-should-be
        // outright; that's almost certainly a typo.
        llvm::sys::fs::file_status destStat;
        std::error_code statEc = llvm::sys::fs::status(destDir, destStat);
        bool destExists = !statEc;
        if (destExists && !llvm::sys::fs::is_directory(destStat)) {
            return cite("cajeta init",
                "destination '" + destDir +
                "' exists and is not a directory");
        }

        // Pre-flight: collect every target path, check for collisions
        // up front. Writing nothing on collision (unless --force) is
        // safer than a half-written tree the user has to clean up.
        std::vector<std::string> targets;
        targets.reserve(row->fileCount);
        std::vector<std::string> collisions;
        for (std::size_t i = 0; i < row->fileCount; ++i) {
            const auto& f = g_initTemplateFiles[row->firstFile + i];
            llvm::SmallString<256> target(destDir);
            llvm::sys::path::append(target, f.relativePath);
            targets.emplace_back(target.c_str());
            if (!force && llvm::sys::fs::exists(target)) {
                collisions.push_back(target.c_str());
            }
        }
        if (!collisions.empty()) {
            std::string msg = "would overwrite existing file";
            if (collisions.size() > 1) msg += "s";
            msg += ":";
            for (const auto& c : collisions) {
                msg += "\n  " + c;
            }
            msg += "\n(pass --force to overwrite)";
            return cite("cajeta init", msg);
        }

        // Ensure destDir exists. create_directories is a no-op when
        // the directory is already there.
        if (auto ec = llvm::sys::fs::create_directories(destDir)) {
            return cite("cajeta init",
                "cannot create destination '" + destDir + "': " +
                ec.message());
        }

        InitWriteResult result;
        result.filesWritten.reserve(row->fileCount);
        for (std::size_t i = 0; i < row->fileCount; ++i) {
            const auto& f = g_initTemplateFiles[row->firstFile + i];
            const auto& target = targets[i];

            // Materialize any parent directories the file needs.
            llvm::StringRef parent = llvm::sys::path::parent_path(target);
            if (!parent.empty()) {
                if (auto ec = llvm::sys::fs::create_directories(parent)) {
                    return cite("cajeta init",
                        "cannot create directory '" + parent.str() + "': " +
                        ec.message());
                }
            }

            std::ofstream os(target, std::ios::binary | std::ios::trunc);
            if (!os) {
                return cite("cajeta init",
                    "cannot open '" + target + "' for writing");
            }
            os.write(f.content, static_cast<std::streamsize>(f.contentBytes));
            if (!os) {
                return cite("cajeta init",
                    "write failed for '" + target + "'");
            }

            // Caller expects forward-slash relative paths in the
            // result (matches how the template declares them, and
            // matches the writer-order contract on InitWriteResult).
            result.filesWritten.emplace_back(f.relativePath);
        }
        return result;
    }

} // namespace cajeta::buildtool
