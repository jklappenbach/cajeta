// Native-dependency AOT link inputs, DCE-aware: a lib is resolved and linked
// only while its `@Native` symbol is still live after tree-shaking, and always
// as a static archive so `--gc-sections` can still strip unused native code.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llvm { class Module; }

namespace cajeta {

    // lib-ids whose `@Native` symbol is still referenced in `m` after pruning.
    std::set<std::string> collectLiveNativeLibs(const llvm::Module& m);

    std::string hostNativePlatform();   // native/ convention, e.g. "linux-x64"

    // The colon-separated `CAJETA_NATIVE_PATH`, then `~/.cajeta/native`.
    std::vector<std::string> nativeLinkSearchDirs();

    // Resolve each live lib to a static archive under one of `searchDirs`,
    // failing loud (naming the lib and the dirs searched) when one resolves nowhere.
    llvm::Expected<std::vector<std::string>> resolveNativeArchivesForLink(
        const std::set<std::string>& liveLibs,
        const std::string& platform,
        const std::vector<std::string>& searchDirs);

    struct NativeJitArtifact {
        std::string path;
        bool isStatic = false;   // false = a shared lib, true = a static archive
    };

    // A JIT-loadable artifact for `lib`, preferring a shared lib. Empty when
    // none is present locally: the JIT NEVER fetches at run time.
    std::optional<NativeJitArtifact> findNativeJitArtifact(
        const std::string& lib, const std::string& platform,
        const std::vector<std::string>& searchDirs);

} // namespace cajeta
