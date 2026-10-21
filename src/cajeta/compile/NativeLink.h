// Native-dependency AOT link inputs, DCE-aware: a lib is resolved and linked
// only while its `@Native` symbol is still live after tree-shaking, and always
// as a static archive so `--gc-sections` can still strip unused native code.

#pragma once

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llvm { class Module; }

namespace cajeta {

    // lib-ids whose `@Native` symbol is still referenced in `m` after pruning.
    std::set<std::string> collectLiveNativeLibs(const llvm::Module& m);

    std::string hostNativePlatform();   // native/ convention, e.g. "linux-x64"

    // The project's own `native/`, when the build tool passed one
    // (`--native-dir`), then the colon-separated `CAJETA_NATIVE_PATH`, then
    // `~/.cajeta/native`. The project dir comes from the manifest's root,
    // never from the working directory, so a build's inputs do not depend on
    // where the command ran (build-cache-native-key spec 3.2).
    std::vector<std::string> nativeLinkSearchDirs();

    // Records the project `native/` dir for this process; empty clears it.
    void setProjectNativeDir(const std::string& dir);
    const std::string& projectNativeDir();

    // Every file a bake or link could pick up under `searchDirs`, labelled
    // for a cache key: each dir contributes `dir=<path>`, then one entry per
    // regular file directly under each `<dir>/<platform>/` but `include`,
    // then each `.so`, `.dylib`, `.dll` and `.a` directly under `<dir>` (the
    // shared-library fallback's places). Sorted within a dir, dirs in search
    // order. A missing dir contributes its `dir=` line only. The build tool
    // digests the paths; this only says what the bake would see
    // (build-cache-native-key spec 3.1).
    struct NativeInputFile {
        std::string label;   // "dir=<dir>" or "<dir>|<platform>/<file>"
        std::string path;    // empty for a dir line
    };
    std::vector<NativeInputFile> nativeInputFiles(
        const std::vector<std::string>& searchDirs);

    // Explode `native/<platform>/…` out of each classpath archive under
    // `<stageRoot>/native/<platform>/`, returning `<stageRoot>/native` when
    // anything landed. A dependency ships its static library INSIDE its `.cja`,
    // the linker takes files rather than archive members, and nothing else
    // unpacks them, so without this a dependency that carries native code is
    // unlinkable unless the caller has already staged it by hand.
    std::optional<std::string> stageClasspathNativeArtifacts(
        const std::vector<std::string>& classpath,
        const std::string& platform,
        const std::string& stageRoot);

    // Resolve each live lib to a static archive under one of `searchDirs`,
    // failing loud (naming the lib and the dirs searched) when one resolves nowhere.
    llvm::Expected<std::vector<std::string>> resolveNativeArchivesForLink(
        const std::set<std::string>& liveLibs,
        const std::string& platform,
        const std::vector<std::string>& searchDirs);

    // Every platform under `searchDirs` carrying a STATIC archive for `lib`,
    // as platform -> path. A `native/` tree names each platform with a
    // subdirectory, so the directory name IS the platform id; `include` holds
    // headers and is not one. Earlier dirs win, matching the resolver.
    //
    // This is what lets a `.cja` be built once and carry every platform its
    // publisher provisioned, rather than only the machine that ran the build.
    std::map<std::string, std::string> findNativeArchivesByPlatform(
        const std::string& lib, const std::vector<std::string>& searchDirs);

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
