#include "cajeta/buildtool/SourceDigest.h"

#include "cajeta/buildtool/Lockfile.h"  // sha256Hex

#include <llvm/Support/Error.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace cajeta::buildtool {

    namespace fs = std::filesystem;

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        bool readAll(const std::string& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::ostringstream buf;
            buf << in.rdbuf();
            out = buf.str();
            return true;
        }

        // Strip the "sha256:" prefix the sha256Hex helper adds.
        std::string bareHex(std::string s) {
            const std::string prefix = "sha256:";
            if (s.compare(0, prefix.size(), prefix) == 0) {
                s.erase(0, prefix.size());
            }
            return s;
        }

        // Pull import dotted-names out of `source`'s preamble. Same
        // shape as ImportExtractor.cajeta in the security-lint plugin
        // but a tiny C++ version — we don't depend on the plugin.
        std::vector<std::string> extractImports(const std::string& source) {
            std::vector<std::string> out;
            std::stringstream ss(source);
            std::string line;
            while (std::getline(ss, line)) {
                // Trim leading whitespace.
                size_t i = 0;
                while (i < line.size() &&
                       std::isspace(static_cast<unsigned char>(line[i]))) {
                    ++i;
                }
                if (i == line.size()) continue;          // blank
                if (line[i] == '/' || line[i] == '*') continue;  // comment
                if (line.compare(i, 8, "package ") == 0) continue;
                if (line.compare(i, 7, "import ") != 0) {
                    // First substantive non-import line — preamble done.
                    break;
                }
                size_t start = i + 7;
                // Strip trailing `;` and any whitespace.
                size_t end = line.size();
                while (end > start &&
                       (std::isspace(static_cast<unsigned char>(line[end - 1])) ||
                        line[end - 1] == ';')) {
                    --end;
                }
                if (end <= start) continue;
                std::string body = line.substr(start, end - start);
                // Trim trailing `.*` for wildcard imports.
                if (body.size() >= 2 &&
                    body.compare(body.size() - 2, 2, ".*") == 0) {
                    body.erase(body.size() - 2);
                }
                if (!body.empty()) out.push_back(body);
            }
            return out;
        }

    } // namespace

    SourceDigestRegistry::SourceDigestRegistry(
        std::vector<std::string> sourceRoots)
        : sourceRoots_(std::move(sourceRoots)) {}

    llvm::Expected<std::string> SourceDigestRegistry::leafDigest(
        const std::string& sourcePath) {
        auto it = leafCache_.find(sourcePath);
        if (it != leafCache_.end()) return it->second;
        std::string bytes;
        if (!readAll(sourcePath, bytes)) {
            return err("source-digest: cannot read '" + sourcePath + "'");
        }
        std::string h = bareHex(sha256Hex(bytes));
        leafCache_[sourcePath] = h;
        return h;
    }

    std::optional<std::string> SourceDigestRegistry::resolveImport(
        const std::string& dotted) const {
        // dotted → relative path with `/` separators + `.cajeta`.
        std::string relPath = dotted;
        std::replace(relPath.begin(), relPath.end(), '.', '/');
        relPath += ".cajeta";
        for (const auto& root : sourceRoots_) {
            fs::path candidate = fs::path(root) / relPath;
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                return candidate.string();
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> SourceDigestRegistry::importsOf(
        const std::string& sourcePath) {
        std::string bytes;
        if (!readAll(sourcePath, bytes)) return {};
        return extractImports(bytes);
    }

    llvm::Expected<std::string> SourceDigestRegistry::digestRec(
        const std::string& sourcePath) {
        // Memo hit.
        auto cached = transitiveCache_.find(sourcePath);
        if (cached != transitiveCache_.end()) return cached->second;

        // Cycle: leaf-only digest. Cycle members get the file-only
        // hash; any source change inside the cycle still busts the
        // dependents because their digest folds in the cycle members.
        if (visiting_.count(sourcePath)) {
            return leafDigest(sourcePath);
        }
        visiting_.insert(sourcePath);

        std::string bytes;
        if (!readAll(sourcePath, bytes)) {
            visiting_.erase(sourcePath);
            return err("source-digest: cannot read '" + sourcePath + "'");
        }
        auto imports = extractImports(bytes);

        // Resolve each import + recursively digest. Unresolved imports
        // (stdlib, deps) contribute the dotted name itself — gives
        // some keying signal without forcing us to model the
        // resolved dep graph here.
        std::vector<std::string> contributions;
        for (const auto& imp : imports) {
            auto resolved = resolveImport(imp);
            if (resolved) {
                auto r = digestRec(*resolved);
                if (!r) {
                    visiting_.erase(sourcePath);
                    return r.takeError();
                }
                contributions.push_back(*r);
            } else {
                contributions.push_back("ext:" + imp);
            }
        }
        std::sort(contributions.begin(), contributions.end());

        std::string canonical;
        canonical += bareHex(sha256Hex(bytes));
        canonical.push_back('\0');
        for (const auto& c : contributions) {
            canonical += c;
            canonical.push_back('\0');
        }
        std::string h = bareHex(sha256Hex(canonical));
        transitiveCache_[sourcePath] = h;
        visiting_.erase(sourcePath);
        return h;
    }

    llvm::Expected<std::string> SourceDigestRegistry::digestOf(
        const std::string& sourcePath) {
        return digestRec(sourcePath);
    }

} // namespace cajeta::buildtool
