// Native-dependency AOT link inputs (native-deps unit 7).
//
// DCE-aware: a `@Native(symbol, lib)` binding records (lib, symbol) as the
// module metadata `cajeta.native.reqs` (unit 2). After tree-shaking, a pruned
// forwarder leaves its extern symbol with NO uses — so we resolve + link a
// native lib ONLY when its symbol is still live (referenced). Native libs are
// linked as static archives so the linker's `--gc-sections` + archive-member
// semantics still dead-strip unused native code. No live native reqs → the link
// line is unchanged (strict no-op for every program that uses no @Native lib).
//
// See docs/specification/buildtool/native-deps-spec.md §5.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llvm { class Module; }

namespace cajeta {

    // lib-ids whose `@Native` symbol is still referenced (live) in `m` after
    // tree-shaking. A recorded requirement whose symbol has no uses (pruned
    // forwarder) is excluded — this is the DCE-aware gate.
    std::set<std::string> collectLiveNativeLibs(const llvm::Module& m);

    // Host platform triple in the native/ convention (e.g. "linux-x64").
    std::string hostNativePlatform();

    // Search dirs for resolving a native archive at AOT link: the
    // colon-separated `CAJETA_NATIVE_PATH`, then `~/.cajeta/native`.
    std::vector<std::string> nativeLinkSearchDirs();

    // Resolve each live lib to a static archive (`lib<id>.a` / `<id>.a`, with or
    // without a `<platform>/` subdir) under one of `searchDirs`. A live lib that
    // resolves nowhere fails loud (named, with the dirs searched and the fix).
    llvm::Expected<std::vector<std::string>> resolveNativeArchivesForLink(
        const std::set<std::string>& liveLibs,
        const std::string& platform,
        const std::vector<std::string>& searchDirs);

    // A JIT-loadable native artifact: a shared lib (`isStatic=false`, loaded via
    // an ORC DynamicLibrarySearchGenerator) or a static archive
    // (`isStatic=true`, via StaticLibraryDefinitionGenerator).
    struct NativeJitArtifact {
        std::string path;
        bool isStatic = false;
    };

    // Find a JIT-loadable artifact for `lib` under `searchDirs` (prefers a
    // shared lib `.so`/`.dylib`, falls back to a static archive `.a`). Empty if
    // none is present locally — the JIT NEVER fetches at run time.
    std::optional<NativeJitArtifact> findNativeJitArtifact(
        const std::string& lib, const std::string& platform,
        const std::vector<std::string>& searchDirs);

} // namespace cajeta
