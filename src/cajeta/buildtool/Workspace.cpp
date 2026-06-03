#include "cajeta/buildtool/Workspace.h"

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Lockfile.h"

#include <llvm/Support/Path.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace cajeta::buildtool {

    namespace {

        llvm::Error cite(const std::string& where, const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                where + ": " + msg);
        }

        // Allowed keys under `workspace`. Mirrors the strict
        // unknown-block rejection the top-level Manifest loader uses.
        const std::set<std::string>& workspaceAllowedKeys() {
            static const std::set<std::string> k = {
                "members", "shared-dependencies",
            };
            return k;
        }

        std::string absolutize(const std::string& path,
                               const std::string& base) {
            std::filesystem::path p(path);
            if (p.is_absolute()) {
                return std::filesystem::weakly_canonical(p).string();
            }
            std::filesystem::path joined = std::filesystem::path(base) / p;
            return std::filesystem::weakly_canonical(joined).string();
        }

        std::string parentDir(const std::string& filePath) {
            std::filesystem::path p(filePath);
            return std::filesystem::absolute(p).parent_path().string();
        }

        // The library() rule for a member's name → member short name
        // resolution. Mirrors ManifestDetails::library() but operates
        // on a bare name string so we can re-use it for matching
        // dependency declarations to members.
        std::string lastSegment(const std::string& dotted) {
            auto pos = dotted.find_last_of('.');
            if (pos == std::string::npos) return dotted;
            return dotted.substr(pos + 1);
        }

    } // namespace

    std::string memberShortName(const WorkspaceMember& m) {
        std::filesystem::path p(m.declaredPath);
        std::string leaf = p.filename().string();
        if (leaf.empty() || leaf == "." || leaf == "..") {
            return m.manifest.details.library();
        }
        return leaf;
    }

    llvm::Expected<Workspace> parseWorkspace(const Manifest& root) {
        if (!root.hasWorkspace) {
            return cite(root.sourcePath.empty() ? "<manifest>" : root.sourcePath,
                        "no 'workspace' block — not a workspace root");
        }
        const auto& ws = root.workspaceRaw;
        for (const auto& kv : ws) {
            if (!workspaceAllowedKeys().count(kv.first.str())) {
                return cite(root.sourcePath,
                    "unknown 'workspace." + kv.first.str() +
                    "' (allowed: members, shared-dependencies)");
            }
        }
        const auto* membersVal = ws.get("members");
        if (!membersVal) {
            return cite(root.sourcePath,
                "'workspace.members' is required (array of member directories)");
        }
        const auto* membersArr = membersVal->getAsArray();
        if (!membersArr) {
            return cite(root.sourcePath,
                "'workspace.members' must be an array of strings");
        }
        Workspace out;
        out.rootManifest = root;
        out.manifestPath = root.sourcePath;
        out.rootPath = parentDir(root.sourcePath);
        out.memberPatterns.reserve(membersArr->size());
        for (size_t i = 0; i < membersArr->size(); ++i) {
            auto s = (*membersArr)[i].getAsString();
            if (!s) {
                return cite(root.sourcePath,
                    "'workspace.members[" + std::to_string(i) +
                    "]' must be a string (member directory path)");
            }
            out.memberPatterns.push_back(s->str());
        }
        if (out.memberPatterns.empty()) {
            return cite(root.sourcePath,
                "'workspace.members' is empty — a workspace must "
                "list at least one member");
        }

        if (const auto* sd = ws.get("shared-dependencies")) {
            const auto* sdObj = sd->getAsObject();
            if (!sdObj) {
                return cite(root.sourcePath,
                    "'workspace.shared-dependencies' must be an "
                    "object mapping name → version-constraint");
            }
            for (const auto& kv : *sdObj) {
                auto v = kv.second.getAsString();
                if (!v) {
                    return cite(root.sourcePath,
                        "'workspace.shared-dependencies." +
                        kv.first.str() +
                        "' must be a version-constraint string");
                }
                WorkspaceSharedDependency d;
                d.name = kv.first.str();
                d.versionConstraint = v->str();
                out.sharedDependencies.push_back(std::move(d));
            }
        }

        return out;
    }

    llvm::Expected<Workspace> loadWorkspace(
        const std::string& rootManifestPath) {
        auto root = loadManifestFile(rootManifestPath);
        if (!root) return root.takeError();
        if (!root->hasWorkspace) {
            return cite(rootManifestPath,
                "this manifest does not declare a 'workspace' block");
        }
        // parseWorkspace expects rootManifest.sourcePath populated.
        auto ws = parseWorkspace(*root);
        if (!ws) return ws.takeError();

        // Load each member.
        for (const auto& pattern : ws->memberPatterns) {
            std::string memberAbs = absolutize(pattern, ws->rootPath);
            std::string memberManifestPath =
                (std::filesystem::path(memberAbs) / "cajeta.json").string();
            auto mm = loadManifestFile(memberManifestPath);
            if (!mm) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << mm.takeError();
                return cite(rootManifestPath,
                    "loading workspace member '" + pattern + "': " + msg);
            }
            if (mm->hasWorkspace) {
                return cite(rootManifestPath,
                    "workspace member '" + pattern +
                    "' also declares a 'workspace' block — nested "
                    "workspaces are not supported");
            }
            WorkspaceMember wm;
            wm.declaredPath = pattern;
            wm.absPath = memberAbs;
            wm.manifestPath = memberManifestPath;
            wm.manifest = std::move(*mm);
            ws->members.push_back(std::move(wm));
        }

        // Reject duplicate member short names — they would collide
        // on `-p <name>` and `<name>:<task>` dispatch.
        std::unordered_map<std::string, std::string> seen;
        for (const auto& m : ws->members) {
            std::string nm = memberShortName(m);
            auto it = seen.find(nm);
            if (it != seen.end()) {
                return cite(rootManifestPath,
                    "workspace members '" + it->second + "' and '" +
                    m.declaredPath + "' both resolve to short name '" +
                    nm + "' — rename one to disambiguate");
            }
            seen[nm] = m.declaredPath;
        }

        // Overlay shared-dependencies into each member's settingsRaw
        // for names the member didn't already declare. Member-declared
        // pins always win — the workspace curation is "inert inherits,
        // active wins", same as the melt + property rule documented in
        // BuildTool.md "Relationship to workspace.shared-dependencies".
        //
        // We mutate `member.manifest.settingsRaw.dependencies` rather
        // than threading a separate view through the resolver so the
        // existing parseDependencies + resolver paths see the composed
        // map without further wiring.
        for (auto& m : ws->members) {
            auto* settings = m.manifest.settingsRaw.getObject("settings");
            // settingsRaw IS the settings object (the loader already
            // pulled it from the top-level manifest), not a wrapper.
            (void)settings;
            llvm::json::Object* deps =
                m.manifest.settingsRaw.getObject("dependencies");
            llvm::json::Object scratch;
            if (!deps) {
                m.manifest.settingsRaw["dependencies"] = std::move(scratch);
                deps = m.manifest.settingsRaw.getObject("dependencies");
            }
            for (const auto& sd : ws->sharedDependencies) {
                if (deps->get(sd.name)) continue;  // member's pin wins
                (*deps)[sd.name] = sd.versionConstraint;
            }
        }

        return ws;
    }

    std::optional<std::string> discoverWorkspaceRoot(
        const std::string& startDir) {
        std::error_code ec;
        std::filesystem::path cur = std::filesystem::absolute(startDir, ec);
        if (ec) return std::nullopt;
        cur = std::filesystem::weakly_canonical(cur);
        while (true) {
            std::filesystem::path candidate = cur / "cajeta.json";
            if (std::filesystem::exists(candidate)) {
                auto manifest = loadManifestFile(candidate.string());
                if (manifest && manifest->hasWorkspace) {
                    return candidate.string();
                }
                if (!manifest) llvm::consumeError(manifest.takeError());
            }
            if (!cur.has_parent_path()) break;
            std::filesystem::path parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
        return std::nullopt;
    }

    LockfileWorkspaceView extractWorkspaceLockView(const Lockfile& lf) {
        LockfileWorkspaceView v;
        if (!lf.isWorkspace) return v;
        for (const auto& m : lf.workspaceMembers) {
            v.recordedChecksums[m.name] = m.manifestChecksum;
        }
        return v;
    }

    namespace {

        std::string readFileBytes(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

    } // namespace

    std::set<std::string> membersNeedingRebuild(
        const Workspace& ws,
        const LockfileWorkspaceView* view) {
        std::set<std::string> dirty;
        // Pass 1: per-member manifest-checksum drift OR no prior
        // record → mark dirty directly.
        std::unordered_map<std::string, std::string> currentChecksums;
        for (const auto& m : ws.members) {
            std::string name = memberShortName(m);
            std::string bytes = readFileBytes(m.manifestPath);
            currentChecksums[name] = sha256Hex(bytes);
            if (!view) {
                dirty.insert(name);
                continue;
            }
            auto it = view->recordedChecksums.find(name);
            if (it == view->recordedChecksums.end() ||
                it->second != currentChecksums[name]) {
                dirty.insert(name);
            }
        }
        // Pass 2: propagate dirty downstream. Walk the dependency
        // edges (member A depends on member B → if B dirty, A
        // dirty). Iterate to fixpoint.
        std::unordered_map<std::string, size_t> shortIdx;
        std::unordered_map<std::string, size_t> nameIdx;
        for (size_t i = 0; i < ws.members.size(); ++i) {
            shortIdx[memberShortName(ws.members[i])] = i;
            nameIdx[ws.members[i].manifest.details.name] = i;
        }
        std::vector<std::set<size_t>> deps(ws.members.size());
        for (size_t i = 0; i < ws.members.size(); ++i) {
            auto parsed = parseDependencies(ws.members[i].manifest);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                continue;
            }
            for (const auto& d : *parsed) {
                auto it = nameIdx.find(d.name);
                if (it == nameIdx.end()) {
                    auto dot = d.name.find_last_of('.');
                    std::string leaf = (dot == std::string::npos)
                                          ? d.name : d.name.substr(dot + 1);
                    it = shortIdx.find(leaf);
                }
                if (it == nameIdx.end()) continue;
                if (it->second == i) continue;
                deps[i].insert(it->second);
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < ws.members.size(); ++i) {
                std::string nm = memberShortName(ws.members[i]);
                if (dirty.count(nm)) continue;
                for (size_t d : deps[i]) {
                    if (dirty.count(memberShortName(ws.members[d]))) {
                        dirty.insert(nm);
                        changed = true;
                        break;
                    }
                }
            }
        }
        return dirty;
    }

    llvm::Expected<std::vector<const WorkspaceMember*>>
    topologicallySortMembers(const Workspace& ws) {
        // Build short-name → member index, plus details.name →
        // member-index for matching `settings.dependencies` entries.
        std::unordered_map<std::string, size_t> shortIdx;
        std::unordered_map<std::string, size_t> nameIdx;
        for (size_t i = 0; i < ws.members.size(); ++i) {
            shortIdx[memberShortName(ws.members[i])] = i;
            nameIdx[ws.members[i].manifest.details.name] = i;
        }
        // Edges A → B mean A depends on B (B must build first).
        std::vector<std::set<size_t>> deps(ws.members.size());
        for (size_t i = 0; i < ws.members.size(); ++i) {
            const auto& m = ws.members[i];
            auto parsed = parseDependencies(m.manifest);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                continue;
            }
            for (const auto& d : *parsed) {
                auto it = nameIdx.find(d.name);
                if (it == nameIdx.end()) {
                    // Try short-name match (members often refer to
                    // each other by short name when the workspace
                    // doesn't use fully-qualified names).
                    it = shortIdx.find(lastSegment(d.name));
                }
                if (it == nameIdx.end()) continue;
                if (it->second == i) continue;
                deps[i].insert(it->second);
            }
        }
        // Kahn's algorithm — emit members with no remaining
        // dependencies; ties broken by declaration order so the
        // output is deterministic.
        std::vector<size_t> remaining(ws.members.size(), 0);
        std::vector<std::set<size_t>> reverse(ws.members.size());
        for (size_t i = 0; i < ws.members.size(); ++i) {
            for (auto d : deps[i]) reverse[d].insert(i);
            remaining[i] = deps[i].size();
        }
        std::vector<const WorkspaceMember*> out;
        out.reserve(ws.members.size());
        std::vector<bool> emitted(ws.members.size(), false);
        while (out.size() < ws.members.size()) {
            ssize_t pick = -1;
            for (size_t i = 0; i < ws.members.size(); ++i) {
                if (!emitted[i] && remaining[i] == 0) {
                    pick = static_cast<ssize_t>(i);
                    break;
                }
            }
            if (pick < 0) {
                std::string cycle;
                for (size_t i = 0; i < ws.members.size(); ++i) {
                    if (!emitted[i]) {
                        if (!cycle.empty()) cycle += ", ";
                        cycle += memberShortName(ws.members[i]);
                    }
                }
                return cite(ws.manifestPath,
                    "cyclic workspace-member dependency among: " + cycle);
            }
            emitted[pick] = true;
            out.push_back(&ws.members[pick]);
            for (auto consumer : reverse[pick]) {
                if (remaining[consumer] > 0) --remaining[consumer];
            }
        }
        return out;
    }

} // namespace cajeta::buildtool
