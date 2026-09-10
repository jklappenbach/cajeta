#include "cajeta/buildtool/RootTrust.h"

#include "cajeta/buildtool/Signature.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cajeta::buildtool {

    namespace {

        namespace fs = std::filesystem;

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        std::string readAll(const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            return buf.str();
        }

        // A repository name reaches the filesystem here, so it must not escape the pins directory.
        bool isSafeName(const std::string& name) {
            if (name.empty() || name == "." || name == "..") return false;
            for (char c : name) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || (c >= '0' && c <= '9')
                       || c == '-' || c == '_' || c == '.';
                if (!ok) return false;
            }
            return name.find("..") == std::string::npos;
        }

    } // namespace

    std::optional<std::string> pinFor(const RootTrustLayout& layout,
                                      const std::string& repositoryName) {
        if (!isSafeName(repositoryName)) return std::nullopt;
        for (const auto& dir : layout.searchDirs) {
            if (dir.empty()) continue;
            fs::path pin = fs::path(dir) / "pins" / repositoryName;
            std::error_code ec;
            if (!fs::is_regular_file(pin, ec)) continue;
            std::string id = readAll(pin);
            // Tolerate a trailing newline: an operator may write this file by hand.
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r'
                                   || id.back() == ' ')) {
                id.pop_back();
            }
            if (!id.empty()) return id;
        }
        return std::nullopt;
    }

    llvm::Expected<std::vector<RootKey>> rootsFor(
            const RootTrustLayout& layout,
            const std::string& repositoryName) {
        std::vector<RootKey> found;

        const RootKey& shipped = layout.shippedOverride
                               ? *layout.shippedOverride
                               : shippedRoot();
        if (!shipped.pem.empty()) found.push_back(shipped);

        // Highest-precedence directory first; the first tier holding a key id wins.
        for (const auto& dir : layout.searchDirs) {
            if (dir.empty()) continue;
            fs::path roots = fs::path(dir) / "roots";
            std::error_code ec;
            if (!fs::is_directory(roots, ec)) continue;
            for (const auto& entry : fs::directory_iterator(roots, ec)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".pem") continue;
                std::string id = entry.path().stem().string();
                bool seen = false;
                for (const auto& r : found) {
                    if (r.id == id) { seen = true; break; }
                }
                if (seen) continue;
                RootKey k;
                k.id = id;
                k.pem = readAll(entry.path());
                if (!k.pem.empty()) found.push_back(std::move(k));
            }
        }

        auto pin = pinFor(layout, repositoryName);
        if (!pin) return found;

        // A pin NARROWS: falling back to the full set when the pinned root is absent
        // would invert the operator's intent, so it is an error, not a widening.
        for (const auto& r : found) {
            if (r.id == *pin) return std::vector<RootKey>{r};
        }
        return err("repository '" + repositoryName + "' is pinned to root key '"
                   + *pin + "', which is not installed. Install it with "
                     "`cajeta trust add-root`, or clear the pin — refusing "
                     "rather than falling back to the other "
                   + std::to_string(found.size()) + " trusted root(s).");
    }

    llvm::Error addRootKey(const RootTrustLayout& layout,
                           const std::string& keyId,
                           const std::string& pemPath) {
        if (!isSafeName(keyId)) {
            return err("'" + keyId + "' is not a usable key id (letters, "
                       "digits, dot, dash and underscore)");
        }
        if (layout.searchDirs.empty() || layout.searchDirs.front().empty()) {
            return err("no writable trust directory");
        }
        std::string pem = readAll(pemPath);
        if (pem.empty()) return err("cannot read '" + pemPath + "'");

        // Reject a non-ed25519 key here, rather than at every later verification.
        auto probe = verifyDetachedEd25519PemBytes("probe", "", pem);
        if (!probe) {
            llvm::consumeError(probe.takeError());
            return err("'" + pemPath + "' is not an ed25519 public key");
        }

        fs::path dest = fs::path(layout.searchDirs.front()) / "roots"
                      / (keyId + ".pem");
        std::error_code ec;
        if (fs::exists(dest, ec)) {
            return err("root key '" + keyId + "' is already installed; "
                       "remove it first");
        }
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream(dest, std::ios::binary) << pem;
        return llvm::Error::success();
    }

    llvm::Error removeRootKey(const RootTrustLayout& layout,
                              const std::string& keyId) {
        if (!isSafeName(keyId)) return err("'" + keyId + "' is not a key id");
        if (keyId == shippedRoot().id) {
            return err("'" + keyId + "' is the root shipped with this "
                       "toolchain and is part of the binary. Pin the "
                       "repository to a different root instead — a 'remove' "
                       "that the next lookup undoes is worse than a refusal.");
        }
        for (const auto& dir : layout.searchDirs) {
            if (dir.empty()) continue;
            fs::path p = fs::path(dir) / "roots" / (keyId + ".pem");
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) {
                fs::remove(p, ec);
                if (ec) return err("cannot remove '" + p.string() + "'");
                return llvm::Error::success();
            }
        }
        return err("root key '" + keyId + "' is not installed");
    }

    llvm::Error pinRepository(const RootTrustLayout& layout,
                              const std::string& repositoryName,
                              const std::string& keyId) {
        if (!isSafeName(repositoryName)) {
            return err("'" + repositoryName + "' is not a usable repository "
                       "name for a pin");
        }
        if (layout.searchDirs.empty() || layout.searchDirs.front().empty()) {
            return err("no writable trust directory");
        }
        fs::path pin = fs::path(layout.searchDirs.front()) / "pins"
                     / repositoryName;
        std::error_code ec;
        if (keyId.empty()) {
            fs::remove(pin, ec);
            return llvm::Error::success();
        }
        if (!isSafeName(keyId)) return err("'" + keyId + "' is not a key id");
        fs::create_directories(pin.parent_path(), ec);
        std::ofstream(pin, std::ios::binary) << keyId << "\n";
        return llvm::Error::success();
    }

} // namespace cajeta::buildtool
