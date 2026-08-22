#include "cajeta/compile/CacheManifest.h"

#include "cajeta/buildtool/JsonC.h"

#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

namespace cajeta {

    using llvm::json::Object;
    using llvm::json::Value;

    static llvm::Error manifestError(const std::string& path,
                                     const std::string& what) {
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
            "cache-manifest %s: %s", path.c_str(), what.c_str());
    }

    llvm::Expected<CacheManifest> CacheManifest::load(const std::string& path) {
        auto parsed = buildtool::parseJsonCFile(path);
        if (!parsed) return parsed.takeError();

        Object* root = parsed->getAsObject();
        if (!root) return manifestError(path, "top level must be an object");

        auto version = root->getString("version");
        if (!version || *version != "cache-manifest-v1")
            return manifestError(path, "version must be \"cache-manifest-v1\"");

        auto discriminator = root->getString("discriminator");
        if (!discriminator)
            return manifestError(path, "missing string field `discriminator`");

        CacheManifest m;
        m.discriminator = discriminator->str();

        auto* sources = root->getArray("sources");
        if (!sources)
            return manifestError(path, "missing array field `sources`");

        for (Value& v : *sources) {
            Object* o = v.getAsObject();
            if (!o) return manifestError(path, "each source must be an object");
            CacheManifestEntry e;
            auto relPath = o->getString("path");
            auto clean = o->getBoolean("clean");
            auto bc = o->getString("bc");
            auto obligations = o->getString("obligations");
            if (!relPath || relPath->empty())
                return manifestError(path, "source missing `path`");
            if (!clean)
                return manifestError(path,
                    "source `" + relPath->str() + "` missing bool `clean`");
            if (!bc || bc->empty() || !obligations || obligations->empty())
                return manifestError(path,
                    "source `" + relPath->str()
                    + "` missing `bc`/`obligations` slot paths");
            e.relPath = relPath->str();
            e.clean = *clean;
            e.bcPath = bc->str();
            e.obligationsPath = obligations->str();
            if (auto obj = o->getString("obj")) e.objPath = obj->str();
            if (m.populateMode() && e.clean)
                return manifestError(path,
                    "populate mode (empty discriminator) is write-only, but `"
                    + e.relPath + "` is marked clean");
            m.sources.push_back(std::move(e));
        }
        return m;
    }

    std::vector<std::pair<std::string, std::string>> cacheFlagPairs(
            const CompilerFlags& f, const std::string& emit,
            const std::string& targetTriple) {
        auto onOff = [](bool b) { return b ? "on" : "off"; };
        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.emplace_back("emit", emit);
        pairs.emplace_back("target", targetTriple.empty()
            ? llvm::sys::getDefaultTargetTriple() : targetTriple);

        pairs.emplace_back("bounds",
            f.bounds == BoundsCheck::On ? "on"
            : f.bounds == BoundsCheck::Off ? "off" : "trap");
        pairs.emplace_back("null-checks",
            f.nullChecks == NullChecks::On ? "on"
            : f.nullChecks == NullChecks::Off ? "off" : "trap");
        pairs.emplace_back("overflow-checks",
            f.overflowChecks == OverflowChecks::On ? "on"
            : f.overflowChecks == OverflowChecks::Off ? "off" : "wrapping");
        pairs.emplace_back("live-set",
            f.liveSet == LiveSet::Strict ? "strict"
            : f.liveSet == LiveSet::Bounded ? "bounded" : "off");

        pairs.emplace_back("source-tags", onOff(f.sourceTags));
        pairs.emplace_back("poison-free", onOff(f.poisonFree));
        pairs.emplace_back("drop-chain-validate", onOff(f.dropChainValidate));
        pairs.emplace_back("ub-traps", onOff(f.ubTraps));
        pairs.emplace_back("use-after-move-rt", onOff(f.useAfterMoveRt));
        pairs.emplace_back("stack-trace-capture", onOff(f.stackTraceCapture));
        pairs.emplace_back("profile-counters", onOff(f.profileCounters));
        pairs.emplace_back("lazy-scope", onOff(f.lazyScope));
        pairs.emplace_back("line-info", onOff(f.lineInfo));
        // The level, not a bool: `off` and `line` differ in emitted IR (the
        // shadow stack) but would share a cache key under onOff(debugInfo),
        // so one's artifact would be re-published for the other.
        pairs.emplace_back("debug-info", debugInfoName(f.debugInfoLevel));
        // cajeta-profiler §3.7 — an instrumented build and a plain one must
        // never alias, because the difference is a probe pair in every
        // selected method.
        pairs.emplace_back("profiler", profilerName(f.profiler));
        // §3.10 — the selection's CONTENTS, not the path it was read from.
        // Keying on the path passes every other test in this area and then
        // hands back objects probed to a selection the user edited in place.
        // The origin string is deliberately absent: two build roots that read
        // the same selection from different paths must share objects.
        pairs.emplace_back("profiler-select", f.profilerSelect);

        pairs.emplace_back("opt",
            f.opt == OptLevel::O0 ? "0"
            : f.opt == OptLevel::O1 ? "1"
            : f.opt == OptLevel::O2 ? "2" : "3");
        pairs.emplace_back("lto",
            f.lto == LtoMode::Off ? "off"
            : f.lto == LtoMode::Thin ? "thin" : "full");
        pairs.emplace_back("link-mode",
            f.linkMode == LinkMode::Full ? "full" : "lean");
        pairs.emplace_back("tree-shake",
            f.treeShake == TreeShake::Off ? "off"
            : f.treeShake == TreeShake::Report ? "report" : "on");

        pairs.emplace_back("debug-prefix-map", f.debugPrefixMap);
        pairs.emplace_back("seed", f.seed);
        pairs.emplace_back("source-date-epoch", f.sourceDateEpoch);
        return pairs;
    }

} // namespace cajeta
