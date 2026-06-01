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
    EXPECT_EQ(t.actions[0].kind, cajeta::buildtool::ActionEntry::Kind::Invocation);
    EXPECT_EQ(t.actions[0].invocation.action, "exec");
    auto cmd = t.actions[0].invocation.params.getString("command");
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
    EXPECT_EQ(t.actions[0].invocation.id, "v");
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
    // Phase 3b: parser checks for any of action/parallel/run-task
    // before validating action-specific fields.
    EXPECT_NE(msg.find("exactly one of"), std::string::npos);
}

TEST(TaskTests, parsesParallelGroup) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [
                    { "parallel": [
                        { "action": "exec", "command": "echo", "args": ["a"] },
                        { "action": "exec", "command": "echo", "args": ["b"] }
                    ]}
                ]
            }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);
    const auto& t = tasks->at("t");
    ASSERT_EQ(t.actions.size(), 1u);
    EXPECT_EQ(t.actions[0].kind,
              cajeta::buildtool::ActionEntry::Kind::Parallel);
    ASSERT_TRUE((bool)t.actions[0].parallel);
    EXPECT_EQ(t.actions[0].parallel->children.size(), 2u);
}

TEST(TaskTests, parsesRunTaskEntry) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "callee": { "actions": [{ "action": "exec", "command": "true" }] },
            "caller": {
                "actions": [
                    { "run-task": "callee", "id": "c",
                      "params": { "x": "hello" } }
                ]
            }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);
    const auto& t = tasks->at("caller");
    ASSERT_EQ(t.actions.size(), 1u);
    EXPECT_EQ(t.actions[0].kind,
              cajeta::buildtool::ActionEntry::Kind::RunTask);
    EXPECT_EQ(t.actions[0].runTask.taskName, "callee");
    EXPECT_EQ(t.actions[0].runTask.id, "c");
    EXPECT_EQ(t.actions[0].runTask.params.at("x"), "hello");
}

TEST(TaskTests, errorsOnEntryWithoutAnyKind) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": { "t": { "actions": [{ "command": "echo" }] } }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    EXPECT_NE(msg.find("exactly one of"), std::string::npos);
}

TEST(TaskTests, errorsOnEntryMixingActionAndParallel) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": { "t": { "actions": [
            { "action": "exec", "command": "echo", "parallel": [] }
        ] } }
    })");
    auto tasks = parseTasks(m);
    ASSERT_FALSE((bool)tasks);
    auto msg = errorText(tasks.takeError());
    EXPECT_NE(msg.find("more than one"), std::string::npos);
}

TEST(TaskTests, errorsOnDependsOnUnknownTask) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": { "depends-on": ["ghost"],
                   "actions": [{ "action": "exec", "command": "true" }] }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);  // parse succeeds; validate flags it
    auto e = cajeta::buildtool::validateTaskGraph(*tasks);
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("undefined task 'ghost'"), std::string::npos);
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

TEST(TaskTests, acceptsAcyclicDependsOnChain) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "a": { "actions": [{ "action": "exec", "command": "true" }] },
            "b": { "depends-on": ["a"], "actions": [{ "action": "exec", "command": "true" }] },
            "c": { "depends-on": ["a", "b"], "actions": [{ "action": "exec", "command": "true" }] }
        }
    })");
    auto tasks = parseTasks(m);
    ASSERT_TRUE((bool)tasks);
    EXPECT_FALSE((bool)cajeta::buildtool::validateTaskGraph(*tasks));
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
