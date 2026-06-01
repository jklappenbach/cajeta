// Regression tests for the task model + parsing. See
// src/cajeta/buildtool/Task.h and plan/build-tool-plan.md Phase 3a.

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

TEST(TaskTests, parsesMinimalTask) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "hello": {
                "actions": [
                    { "action": "exec", "command": "echo", "args": ["hi"] }
                ]
            }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);
    ASSERT_EQ(tasks->size(), 1u);
    const auto& t = tasks->at("hello");
    EXPECT_EQ(t.name, "hello");
    EXPECT_EQ(t.actions.size(), 1u);
    EXPECT_EQ(t.actions[0].action, "exec");
    auto cmd = t.actions[0].params.getString("command");
    ASSERT_TRUE((bool)cmd);
    EXPECT_EQ(cmd->str(), "echo");
}

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
    EXPECT_EQ(t.actions[0].id, "v");
    EXPECT_EQ(t.outputs.at("version"), "${params.version}");
    EXPECT_EQ(t.workingDir.value_or(""), "build/");
    EXPECT_EQ(t.env.at("RELEASE_ENV"), "prod");
}

TEST(TaskTests, paramDefaultBoolNormalizesToString) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "params": {
                    "flag": { "type": "bool", "default": true }
                },
                "actions": [ { "action": "exec", "command": "echo" } ]
            }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);
    EXPECT_EQ(tasks->at("t").params[0].defaultValue.value_or(""), "true");
}

// ─── error cases ──────────────────────────────────────────────────────

TEST(TaskTests, errorsOnTaskWithoutActions) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": { "noaction": { "description": "nothing here" } }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    EXPECT_NE(msg.find("'actions'"), std::string::npos);
}

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
    EXPECT_NE(msg.find("missing required 'action' field"), std::string::npos);
}

TEST(TaskTests, errorsOnParallelGroupAtPhase3a) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": { "actions": [ { "parallel": [] } ] }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    EXPECT_NE(msg.find("Phase 3b"), std::string::npos);
}

TEST(TaskTests, errorsOnRunTaskAtPhase3a) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": { "actions": [ { "run-task": "other" } ] }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    EXPECT_NE(msg.find("Phase 3b"), std::string::npos);
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
