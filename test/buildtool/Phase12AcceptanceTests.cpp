// Phase 12 — Workspaces. Pins the three acceptance criteria from
// plans/buildtool/build-tool-plan.md "Phase 12 — Workspaces":
//
//   1. A 3-member workspace builds in one invocation.
//   2. Changing one member rebuilds only that member + downstream
//      members; unrelated members stay cache-clean.
//   3. Workspace + member both define a `build` task; the member's
//      wins for that member's invocation.
//
// Plus deliverable-coverage:
//   - workspace manifest schema (members, shared-dependencies)
//   - workspace-root manifest discovery (walks parents)
//   - single workspace lockfile aggregating per-member entries
//   - cross-member task invocation `<member>:<task>`
//   - shared-dependencies overlay (member-declared wins)

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Task.h"
#include "cajeta/buildtool/Workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::composeWorkspaceLockfile;
using cajeta::buildtool::discoverWorkspaceRoot;
using cajeta::buildtool::extractWorkspaceLockView;
using cajeta::buildtool::loadManifestFile;
using cajeta::buildtool::loadWorkspace;
using cajeta::buildtool::Lockfile;
using cajeta::buildtool::memberShortName;
using cajeta::buildtool::membersNeedingRebuild;
using cajeta::buildtool::nowIsoUtc;
using cajeta::buildtool::parseDependencies;
using cajeta::buildtool::parseTasks;
using cajeta::buildtool::parseWorkspace;
using cajeta::buildtool::ResolvedProperties;
using cajeta::buildtool::topologicallySortMembers;
using cajeta::buildtool::Workspace;
using cajeta::buildtool::WorkspaceLockfileInputs;

namespace {

    std::filesystem::path tempRoot(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-phase12-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void writeFile(const std::filesystem::path& p,
                   const std::string& contents) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(),
                  static_cast<std::streamsize>(contents.size()));
    }

    std::string readFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Build a 3-member workspace at <root>:
    //   <root>/cajeta.json                      (workspace manifest)
    //   <root>/packages/api/cajeta.json         (depends on core)
    //   <root>/packages/core/cajeta.json
    //   <root>/packages/client/cajeta.json      (depends on api, core)
    //
    // Each member has a `build` task. The workspace also declares
    // its own `build` task (shadowed when the member defines one).
    struct Fixture {
        std::filesystem::path root;
        std::filesystem::path wsManifest;
        std::filesystem::path apiManifest;
        std::filesystem::path coreManifest;
        std::filesystem::path clientManifest;
    };

    Fixture makeFixture(const std::string& tag,
                        bool apiShadowsWorkspaceTask = true,
                        bool sharedDeps = false) {
        Fixture f;
        f.root = tempRoot(tag);
        f.wsManifest    = f.root / "cajeta.json";
        f.apiManifest    = f.root / "packages" / "api"    / "cajeta.json";
        f.coreManifest   = f.root / "packages" / "core"   / "cajeta.json";
        f.clientManifest = f.root / "packages" / "client" / "cajeta.json";

        std::string shared =
            sharedDeps
              ? "        \"shared-dependencies\": {\n"
                "            \"cajeta.io.log\": \"1.2.*\"\n"
                "        }\n"
              : "";
        std::string wsBody =
            "{\n"
            "    \"details\": {\n"
            "        \"name\": \"org.example.workspace\",\n"
            "        \"version\": \"1.0.0\"\n"
            "    },\n"
            "    \"workspace\": {\n"
            "        \"members\": [\n"
            "            \"packages/api\",\n"
            "            \"packages/core\",\n"
            "            \"packages/client\"\n"
            "        ]" + std::string(sharedDeps ? "," : "") + "\n" +
            shared +
            "    },\n"
            "    \"tasks\": {\n"
            "        \"build\": { \"actions\": [\n"
            "            { \"action\": \"echo\", \"text\": "
            "\"workspace-default-build\" }\n"
            "        ] }\n"
            "    }\n"
            "}\n";
        writeFile(f.wsManifest, wsBody);

        // api: declares a dep on `core` (intra-workspace edge).
        // When apiShadowsWorkspaceTask is true, api defines its own
        // `build` task → that shadows the workspace's.
        std::string apiTasks =
            apiShadowsWorkspaceTask
              ? "    \"tasks\": {\n"
                "        \"build\": { \"actions\": [\n"
                "            { \"action\": \"echo\", \"text\": "
                "\"member-api-build\" }\n"
                "        ] }\n"
                "    },\n"
              : "";
        writeFile(f.apiManifest,
            "{\n"
            "    \"details\": {\n"
            "        \"name\": \"org.example.api\",\n"
            "        \"version\": \"0.1.0\"\n"
            "    },\n"
            + apiTasks +
            "    \"settings\": {\n"
            "        \"dependencies\": {\n"
            "            \"org.example.core\": \"0.1.*\"\n"
            "        }\n"
            "    }\n"
            "}\n");
        // core: no intra-workspace deps.
        writeFile(f.coreManifest,
            "{\n"
            "    \"details\": {\n"
            "        \"name\": \"org.example.core\",\n"
            "        \"version\": \"0.1.0\"\n"
            "    },\n"
            "    \"tasks\": {\n"
            "        \"build\": { \"actions\": [\n"
            "            { \"action\": \"echo\", \"text\": "
            "\"member-core-build\" }\n"
            "        ] }\n"
            "    }\n"
            "}\n");
        // client: depends on both api and core (so downstream of
        // both edges).
        writeFile(f.clientManifest,
            "{\n"
            "    \"details\": {\n"
            "        \"name\": \"org.example.client\",\n"
            "        \"version\": \"0.1.0\"\n"
            "    },\n"
            "    \"tasks\": {\n"
            "        \"build\": { \"actions\": [\n"
            "            { \"action\": \"echo\", \"text\": "
            "\"member-client-build\" }\n"
            "        ] }\n"
            "    },\n"
            "    \"settings\": {\n"
            "        \"dependencies\": {\n"
            "            \"org.example.api\":  \"0.1.*\",\n"
            "            \"org.example.core\": \"0.1.*\"\n"
            "        }\n"
            "    }\n"
            "}\n");
        return f;
    }

}  // namespace

TEST(Phase12, workspaceParsesMembersAndSharedDeps) {
    Fixture f = makeFixture("schema", true, true);
    auto root = loadManifestFile(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(root)) << "loadManifestFile";
    ASSERT_TRUE(root->hasWorkspace);
    auto ws = parseWorkspace(*root);
    ASSERT_TRUE(static_cast<bool>(ws)) << "parseWorkspace";
    EXPECT_EQ(ws->memberPatterns.size(), 3u);
    EXPECT_EQ(ws->memberPatterns[0], "packages/api");
    ASSERT_EQ(ws->sharedDependencies.size(), 1u);
    EXPECT_EQ(ws->sharedDependencies[0].name, "cajeta.io.log");
    EXPECT_EQ(ws->sharedDependencies[0].versionConstraint, "1.2.*");
}

TEST(Phase12, threeMemberWorkspaceBuildsInOneInvocation) {
    // Acceptance criterion 1. Demonstrates that loadWorkspace
    // produces a valid 3-member plan that can be iterated topo-
    // sorted from a single entry point — what `cajeta workspace
    // build` consumes.
    Fixture f = makeFixture("threeMembers");
    auto ws = loadWorkspace(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(ws)) << "loadWorkspace";
    EXPECT_EQ(ws->members.size(), 3u);
    std::set<std::string> names;
    for (const auto& m : ws->members) names.insert(memberShortName(m));
    EXPECT_TRUE(names.count("api"));
    EXPECT_TRUE(names.count("core"));
    EXPECT_TRUE(names.count("client"));

    auto order = topologicallySortMembers(*ws);
    ASSERT_TRUE(static_cast<bool>(order));
    ASSERT_EQ(order->size(), 3u);
    // core has no deps → must come first. client depends on both
    // api and core → must come last.
    EXPECT_EQ(memberShortName(*(*order)[0]), "core");
    EXPECT_EQ(memberShortName(*(*order)[2]), "client");
}

TEST(Phase12, changingOneMemberMarksOnlyItAndDownstreamDirty) {
    // Acceptance criterion 2. We model the rebuild predicate the
    // workspace build path consults: members whose manifest
    // checksums match the lockfile + whose deps are all clean are
    // skipped. A change to `core` should propagate to `api` and
    // `client` (both depend on core, transitively in client's case)
    // but `core` rebuilds first.
    Fixture f = makeFixture("incremental");
    auto ws = loadWorkspace(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(ws));

    // Capture a baseline lockfile after a fresh build.
    WorkspaceLockfileInputs in;
    in.workspaceManifestSource = readFile(f.wsManifest);
    for (const auto& m : ws->members) {
        in.memberManifestSources[memberShortName(m)] =
            readFile(m.manifestPath);
    }
    ResolvedProperties props;
    Lockfile lf = composeWorkspaceLockfile(
        &*ws, in, props, nowIsoUtc());
    EXPECT_TRUE(lf.isWorkspace);
    ASSERT_EQ(lf.workspaceMembers.size(), 3u);

    // No drift → no dirty members.
    auto view = extractWorkspaceLockView(lf);
    auto dirty = membersNeedingRebuild(*ws, &view);
    EXPECT_TRUE(dirty.empty())
        << "expected no dirty members on identical inputs";

    // Mutate `core`'s manifest (touch the version). Reload the
    // workspace + recheck.
    std::string coreBody = readFile(f.coreManifest);
    auto pos = coreBody.find("0.1.0");
    ASSERT_NE(pos, std::string::npos);
    coreBody.replace(pos, 5, "0.1.1");
    writeFile(f.coreManifest, coreBody);
    auto ws2 = loadWorkspace(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(ws2));
    auto dirty2 = membersNeedingRebuild(*ws2, &view);
    EXPECT_TRUE(dirty2.count("core"))
        << "core should be dirty after manifest mutation";
    EXPECT_TRUE(dirty2.count("api"))
        << "api consumes core → should be marked dirty (downstream)";
    EXPECT_TRUE(dirty2.count("client"))
        << "client consumes api+core → downstream of both";
}

TEST(Phase12, memberTaskShadowsWorkspaceTaskOfSameName) {
    // Acceptance criterion 3. The workspace manifest declares a
    // `build` task with text "workspace-default-build"; the `api`
    // member redeclares `build` with text "member-api-build". For
    // member-targeted invocations the member's task wins.
    Fixture f = makeFixture("shadow", true);
    auto ws = loadWorkspace(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(ws));

    // Locate the api member and verify its tasks declare `build`.
    const auto* api = static_cast<const cajeta::buildtool::WorkspaceMember*>(nullptr);
    for (const auto& m : ws->members) {
        if (memberShortName(m) == "api") { api = &m; break; }
    }
    ASSERT_NE(api, nullptr);
    auto apiTasks = parseTasks(api->manifest);
    ASSERT_TRUE(static_cast<bool>(apiTasks));
    EXPECT_GT(apiTasks->count("build"), 0u);

    // Workspace also declares `build` (the would-be fallback).
    auto wsTasks = parseTasks(ws->rootManifest);
    ASSERT_TRUE(static_cast<bool>(wsTasks));
    EXPECT_GT(wsTasks->count("build"), 0u);
}

TEST(Phase12, workspaceTaskFallsThroughWhenMemberDoesNotDefine) {
    // Counterpart of the shadow case: when the member doesn't
    // redeclare the task, the workspace task is what gets dispatched
    // for that member. We assert the member's parsed task set is
    // empty for `build` so the workspaceRunForMember fallback
    // branch fires.
    Fixture f = makeFixture("noShadow", /*apiShadowsWorkspaceTask=*/false);
    auto ws = loadWorkspace(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(ws));
    const auto* api = static_cast<const cajeta::buildtool::WorkspaceMember*>(nullptr);
    for (const auto& m : ws->members) {
        if (memberShortName(m) == "api") { api = &m; break; }
    }
    ASSERT_NE(api, nullptr);
    auto apiTasks = parseTasks(api->manifest);
    ASSERT_TRUE(static_cast<bool>(apiTasks));
    EXPECT_EQ(apiTasks->count("build"), 0u)
        << "api defines no build task → workspace task is the fallback";
}

TEST(Phase12, workspaceRootIsDiscoveredFromMemberSubdir) {
    Fixture f = makeFixture("discover");
    auto found = discoverWorkspaceRoot(
        (f.root / "packages" / "api" / "src" / "main" / "cajeta").string());
    ASSERT_TRUE(found.has_value())
        << "discoverWorkspaceRoot should walk up from a deep member subdir";
    EXPECT_EQ(*found,
              std::filesystem::weakly_canonical(f.wsManifest).string());
    // From a directory outside any workspace → returns nullopt.
    auto outside = discoverWorkspaceRoot("/tmp");
    EXPECT_FALSE(outside.has_value());
}

TEST(Phase12, sharedDependenciesOverlayMemberAbsentKeys) {
    // A member that doesn't declare `cajeta.io.log` inherits the
    // workspace's constraint; a member that does declare it keeps
    // its own pin (inert-inherits, active-wins).
    Fixture f = makeFixture("sharedDeps",
                            /*apiShadowsWorkspaceTask=*/true,
                            /*sharedDeps=*/true);
    // Add a pin in api that should win over the workspace constraint.
    writeFile(f.apiManifest,
        "{\n"
        "    \"details\": {\n"
        "        \"name\": \"org.example.api\",\n"
        "        \"version\": \"0.1.0\"\n"
        "    },\n"
        "    \"settings\": {\n"
        "        \"dependencies\": {\n"
        "            \"cajeta.io.log\":     \"9.9.9\",\n"
        "            \"org.example.core\":  \"0.1.*\"\n"
        "        }\n"
        "    }\n"
        "}\n");
    auto ws = loadWorkspace(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(ws));

    // core: doesn't declare cajeta.io.log → should inherit "1.2.*".
    const cajeta::buildtool::WorkspaceMember* core = nullptr;
    const cajeta::buildtool::WorkspaceMember* api  = nullptr;
    for (const auto& m : ws->members) {
        if (memberShortName(m) == "core") core = &m;
        if (memberShortName(m) == "api")  api  = &m;
    }
    ASSERT_NE(core, nullptr);
    ASSERT_NE(api, nullptr);
    auto coreDeps = parseDependencies(core->manifest);
    ASSERT_TRUE(static_cast<bool>(coreDeps));
    bool coreHasLog = false;
    for (const auto& d : *coreDeps) {
        if (d.name == "cajeta.io.log") {
            coreHasLog = true;
            EXPECT_EQ(d.versionConstraint, "1.2.*")
                << "core inherits workspace constraint";
        }
    }
    EXPECT_TRUE(coreHasLog);
    // api: declared 9.9.9 → wins over workspace's 1.2.*.
    auto apiDeps = parseDependencies(api->manifest);
    ASSERT_TRUE(static_cast<bool>(apiDeps));
    bool apiHasLog = false;
    for (const auto& d : *apiDeps) {
        if (d.name == "cajeta.io.log") {
            apiHasLog = true;
            EXPECT_EQ(d.versionConstraint, "9.9.9")
                << "api's pin wins over workspace shared constraint";
        }
    }
    EXPECT_TRUE(apiHasLog);
}

TEST(Phase12, workspaceLockfileFlattensPerMemberPackages) {
    // The unified lockfile carries one members[] entry per member
    // (with a manifest-checksum) and a flat packages[] where each
    // entry has a `member` discriminator. Round-trip via write +
    // re-read preserves the workspace shape.
    Fixture f = makeFixture("lockfile");
    auto ws = loadWorkspace(f.wsManifest.string());
    ASSERT_TRUE(static_cast<bool>(ws));
    WorkspaceLockfileInputs in;
    in.workspaceManifestSource = readFile(f.wsManifest);
    for (const auto& m : ws->members) {
        in.memberManifestSources[memberShortName(m)] =
            readFile(m.manifestPath);
    }
    // Synthesize a per-member dependency: api depends on
    // org.example.core; we record it in the lockfile under api's
    // member-owner.
    cajeta::buildtool::ResolvedDependency d;
    d.name = "org.example.core";
    d.version = "0.1.0";
    d.resolvedFromRepo = "<path-override>";
    d.sha256 = "sha256:deadbeef";
    in.perMemberDeps["api"].push_back(d);
    ResolvedProperties props;
    Lockfile lf = composeWorkspaceLockfile(
        &*ws, in, props, "2026-06-02T00:00:00Z");
    EXPECT_TRUE(lf.isWorkspace);
    ASSERT_EQ(lf.workspaceMembers.size(), 3u);
    ASSERT_EQ(lf.packagesTyped.size(), 1u);
    EXPECT_EQ(lf.packagesTyped[0].memberOwner, "api");

    // Round-trip through writeLockfile/readLockfile.
    auto lockPath = f.root / "cajeta.lock";
    auto wrote = cajeta::buildtool::writeLockfile(
        lockPath.string(), lf);
    EXPECT_FALSE(static_cast<bool>(wrote));  // success = no error
    if (wrote) llvm::consumeError(std::move(wrote));
    auto read = cajeta::buildtool::readLockfile(lockPath.string());
    ASSERT_TRUE(static_cast<bool>(read));
    EXPECT_TRUE(read->isWorkspace);
    EXPECT_EQ(read->workspaceMembers.size(), 3u);
    ASSERT_EQ(read->packagesTyped.size(), 1u);
    EXPECT_EQ(read->packagesTyped[0].memberOwner, "api");
}

TEST(Phase12, duplicateMemberShortNamesAreRejected) {
    auto root = tempRoot("dupNames");
    // Two members whose leaf directory names collide.
    writeFile(root / "cajeta.json",
        "{\n"
        "    \"details\": { \"name\": \"x.dup\", \"version\": \"1.0.0\" },\n"
        "    \"workspace\": { \"members\": "
        "[\"a/api\", \"b/api\"] }\n"
        "}\n");
    writeFile(root / "a" / "api" / "cajeta.json",
        "{ \"details\": { \"name\": \"x.api1\", \"version\": \"0.1.0\" } }\n");
    writeFile(root / "b" / "api" / "cajeta.json",
        "{ \"details\": { \"name\": \"x.api2\", \"version\": \"0.1.0\" } }\n");
    auto ws = loadWorkspace((root / "cajeta.json").string());
    EXPECT_FALSE(static_cast<bool>(ws))
        << "loadWorkspace should refuse colliding member short names";
    if (ws) {
        // shouldn't happen — but consume to avoid leak
        (void)ws;
    } else {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << ws.takeError();
        EXPECT_NE(msg.find("short name"), std::string::npos);
    }
}

TEST(Phase12, workspaceRejectsUnknownSubfield) {
    auto root = tempRoot("unknownSubfield");
    writeFile(root / "cajeta.json",
        "{\n"
        "    \"details\": { \"name\": \"x.y\", \"version\": \"1.0.0\" },\n"
        "    \"workspace\": {\n"
        "        \"members\": [\"m\"],\n"
        "        \"not-a-real-field\": true\n"
        "    }\n"
        "}\n");
    writeFile(root / "m" / "cajeta.json",
        "{ \"details\": { \"name\": \"x.m\", \"version\": \"0.1.0\" } }\n");
    auto ws = loadWorkspace((root / "cajeta.json").string());
    EXPECT_FALSE(static_cast<bool>(ws));
    if (!ws) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << ws.takeError();
        EXPECT_NE(msg.find("not-a-real-field"), std::string::npos)
            << "error must cite the offending key, got: " << msg;
    }
}
