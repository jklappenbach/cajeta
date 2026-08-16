// Regression tests for the task model + parsing. See
// src/cajeta/buildtool/Task.h and plans/buildtool/build-tool-plan.md Phase 3a.

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Task.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseTasks;
using cajeta::buildtool::Task;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << "manifest load failed: "
                          << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

} // namespace

// ─── happy-path parsing ────────────────────────────────────────────────


TEST(TaskTests, parsesFullTaskShape) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "release": {
                "description": "Release build",
                "depends-on": ["test", "integration"],
                "params": {
                    "version": { "type": "string", "required": true, "doc": "Release version" },
                    "skip-upload": { "type": "bool", "default": false }
                },
                "actions": [
                    { "action": "exec", "command": "echo", "args": ["${params.version}"], "id": "v" }
                ],
                "outputs": { "version": "${params.version}" },
                "working-dir": "build/",
                "env": { "RELEASE_ENV": "prod" }
            }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);
    const auto& t = tasks->at("release");
    EXPECT_EQ(t.description.value_or(""), "Release build");
    EXPECT_EQ(t.dependsOn.size(), 2u);
    EXPECT_EQ(t.dependsOn[0], "test");
    EXPECT_EQ(t.dependsOn[1], "integration");
    EXPECT_EQ(t.params.size(), 2u);
    EXPECT_EQ(t.actions.size(), 1u);
    EXPECT_EQ(t.actions[0].invocation.id, "v");
    EXPECT_EQ(t.outputs.at("version"), "${params.version}");
    EXPECT_EQ(t.workingDir.value_or(""), "build/");
    EXPECT_EQ(t.env.at("RELEASE_ENV"), "prod");
}


// ─── error cases ──────────────────────────────────────────────────────


TEST(TaskTests, errorsOnUnknownTaskField) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [ { "action": "exec", "command": "echo" } ],
                "bogus": "field"
            }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    EXPECT_NE(msg.find("unknown field 'bogus'"), std::string::npos);
}

TEST(TaskTests, errorsOnActionWithoutActionField) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": { "actions": [ { "command": "echo" } ] }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    // Phase 3b: parser checks for any of action/parallel/run-task
    // before validating action-specific fields.
    EXPECT_NE(msg.find("exactly one of"), std::string::npos);
}





TEST(TaskTests, errorsOnCyclicDependsOn) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "a": { "depends-on": ["b"], "actions": [{"action":"exec","command":"true"}] },
            "b": { "depends-on": ["c"], "actions": [{"action":"exec","command":"true"}] },
            "c": { "depends-on": ["a"], "actions": [{"action":"exec","command":"true"}] }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);
    auto e = cajeta::buildtool::validateTaskGraph(*tasks);
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("cyclic depends-on"), std::string::npos);
}


TEST(TaskTests, errorsOnUnknownParamType) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "params": { "p": { "type": "intMaybe" } },
                "actions": [ { "action": "exec", "command": "echo" } ]
            }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    EXPECT_NE(msg.find("unsupported type 'intMaybe'"), std::string::npos);
}
