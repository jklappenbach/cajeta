// Native-dependency AOT link inputs (native-deps unit 7). See NativeLink.h.

#include "NativeLink.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace cajeta {

    std::set<std::string> collectLiveNativeLibs(const llvm::Module& m) {
        std::set<std::string> out;
        const llvm::NamedMDNode* nmd =
            m.getNamedMetadata("cajeta.native.reqs");
        if (!nmd) return out;
        for (unsigned i = 0; i < nmd->getNumOperands(); ++i) {
            const llvm::MDNode* op = nmd->getOperand(i);
            if (!op || op->getNumOperands() != 2) continue;
            auto* lib = llvm::dyn_cast<llvm::MDString>(op->getOperand(0));
            auto* sym = llvm::dyn_cast<llvm::MDString>(op->getOperand(1));
            if (!lib || !sym) continue;
            // DCE-aware gate: the recorded requirement counts only if its
            // extern symbol still has a use after tree-shaking.
            const llvm::Function* f = m.getFunction(sym->getString());
            if (f && !f->use_empty()) out.insert(lib->getString().str());
        }
        return out;
    }

    std::string hostNativePlatform() {
        llvm::Triple t(llvm::sys::getDefaultTargetTriple());
        std::string arch;
        switch (t.getArch()) {
            case llvm::Triple::x86_64:  arch = "x64";   break;
            case llvm::Triple::aarch64: arch = "arm64"; break;
            default:                    arch = t.getArchName().str(); break;
        }
        std::string os;
        if (t.isOSWindows())                 os = "windows";
        else if (t.isMacOSX() || t.isOSDarwin()) os = "macos";
        else                                 os = "linux";
        return os + "-" + arch;
    }

    std::vector<std::string> nativeLinkSearchDirs() {
        std::vector<std::string> dirs;
        if (const char* p = std::getenv("CAJETA_NATIVE_PATH")) {
            std::string s(p), cur;
            for (char c : s) {
                if (c == ':') {
                    if (!cur.empty()) dirs.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) dirs.push_back(cur);
        }
        if (const char* home = std::getenv("HOME"))
            dirs.push_back(std::string(home) + "/.cajeta/native");
        return dirs;
    }

    llvm::Expected<std::vector<std::string>> resolveNativeArchivesForLink(
            const std::set<std::string>& liveLibs,
            const std::string& platform,
            const std::vector<std::string>& searchDirs) {
        std::vector<std::string> out;
        for (const auto& lib : liveLibs) {
            std::string found;
            for (const auto& dir : searchDirs) {
                std::vector<std::string> cands = {
                    dir + "/" + platform + "/lib" + lib + ".a",
                    dir + "/" + platform + "/" + lib + ".a",
                    dir + "/lib" + lib + ".a",
                    dir + "/" + lib + ".a",
                };
                for (const auto& c : cands) {
                    std::error_code ec;
                    if (std::filesystem::exists(c, ec)) { found = c; break; }
                }
                if (!found.empty()) break;
            }
            if (found.empty()) {
                std::string searched;
                for (const auto& d : searchDirs)
                    searched += (searched.empty() ? "" : ", ") + d;
                if (searched.empty()) searched = "(none)";
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    "native library '" + lib + "' (" + platform +
                    ") not found; searched: " + searched +
                    ". Provide it via CAJETA_NATIVE_PATH, the project native/ "
                    "dir, or ~/.cajeta/native/.");
            }
            out.push_back(found);
        }
        return out;
    }

} // namespace cajeta
