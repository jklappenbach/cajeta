// Regression tests for the Phase 3a task runner. See
// src/cajeta/buildtool/TaskRunner.h and
// plan/build-tool-plan.md Phase 3a.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Task.h"
#include "cajeta/buildtool/TaskRunner.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <string>

using cajeta::buildtool::ActionRegistry;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseTasks;
using cajeta::buildtool::PropertyOverrides;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::runTask;
using cajeta::buildtool::TaskInvocationParams;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    struct Loaded {
        cajeta::buildtool::Manifest manifest;
        cajeta::buildtool::ResolvedProperties props;
        std::map<std::string, cajeta::buildtool::Task> tasks;
    };

    Loaded mustLoad(const std::string& src) {
        Loaded l;
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << "manifest load failed: "
                          << errorText(m.takeError());
            return l;
        }
        l.manifest = std::move(*m);
        auto props = resolveProperties(l.manifest);
        if (!props) {
            ADD_FAILURE() << "property resolve failed: "
                          << errorText(props.takeError());
            return l;
        }
        l.props = std::move(*props);
        auto tasks = parseTasks(l.manifest);
        if (!tasks) {
            ADD_FAILURE() << "task parse failed: "
                          << errorText(tasks.takeError());
            return l;
        }
        l.tasks = std::move(*tasks);
        return l;
    }

} // namespace

// ─── happy paths ──────────────────────────────────────────────────────

TEST(TaskRunnerTests, runsSingleExecAction) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [
                    { "action": "exec", "command": "true" }
                ]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_TRUE((bool)outputs);
    EXPECT_TRUE(outputs->empty());  // no task-level outputs declared
}

TEST(TaskRunnerTests, threadsActionOutputsToLaterActions) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [
                    { "action": "exec", "command": "echo", "args": ["payload"], "id": "src" },
                    { "action": "exec", "command": "test",
                      "args": ["${src.stdout}", "=", "payload\n"] }
                ]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_TRUE((bool)outputs) << errorText(outputs.takeError());
}

TEST(TaskRunnerTests, propertiesAndBuiltinsResolveInActionParams) {
    auto l = mustLoad(R"({
        "details": { "name": "com.example.foo", "version": "0.1.0" },
        "properties": { "msg": "hi from ${details.name}" },
        "tasks": {
            "t": {
                "actions": [
                    { "action": "exec", "command": "test",
                      "args": ["${msg}", "=", "hi from com.example.foo"] }
                ]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_TRUE((bool)outputs) << errorText(outputs.takeError());
}

TEST(TaskRunnerTests, paramDefaultUsedWhenCliOmitsIt) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "params": {
                    "x": { "type": "string", "default": "default-x" }
                },
                "actions": [
                    { "action": "exec", "command": "test",
                      "args": ["${params.x}", "=", "default-x"] }
                ]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_TRUE((bool)outputs) << errorText(outputs.takeError());
}

TEST(TaskRunnerTests, paramCliBindingOverridesDefault) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "params": {
                    "x": { "type": "string", "default": "default-x" }
                },
                "actions": [
                    { "action": "exec", "command": "test",
                      "args": ["${params.x}", "=", "from-cli"] }
                ]
            }
        }
    })");
    ActionRegistry registry;
    TaskInvocationParams cli;
    cli.values["x"] = "from-cli";
    auto outputs = runTask(l.tasks, "t", cli, l.props, registry);
    ASSERT_TRUE((bool)outputs) << errorText(outputs.takeError());
}

TEST(TaskRunnerTests, taskOutputsBlockResolves) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [
                    { "action": "exec", "command": "echo",
                      "args": ["produced"], "id": "x" }
                ],
                "outputs": { "result": "${x.stdout}" }
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_TRUE((bool)outputs) << errorText(outputs.takeError());
    EXPECT_EQ(outputs->at("result"), "produced\n");
}

// ─── error paths ──────────────────────────────────────────────────────

TEST(TaskRunnerTests, errorsOnUnknownTaskName) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": { "real": { "actions": [{ "action": "exec", "command": "true" }] } }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "ghost", {}, l.props, registry);
    ASSERT_FALSE((bool)outputs);
    auto msg = errorText(outputs.takeError());
    EXPECT_NE(msg.find("no such task: 'ghost'"), std::string::npos);
}

TEST(TaskRunnerTests, errorsOnUnknownActionName) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": { "actions": [{ "action": "nope" }] }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_FALSE((bool)outputs);
    auto msg = errorText(outputs.takeError());
    EXPECT_NE(msg.find("unknown action 'nope'"), std::string::npos);
}

TEST(TaskRunnerTests, errorsOnRequiredParamMissing) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "params": { "v": { "type": "string", "required": true } },
                "actions": [{ "action": "exec", "command": "echo" }]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_FALSE((bool)outputs);
    auto msg = errorText(outputs.takeError());
    EXPECT_NE(msg.find("requires param 'v'"), std::string::npos);
}

TEST(TaskRunnerTests, errorsOnUndefinedSubstitution) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [
                    { "action": "exec", "command": "echo", "args": ["${nope}"] }
                ]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_FALSE((bool)outputs);
    auto msg = errorText(outputs.takeError());
    EXPECT_NE(msg.find("'nope'"), std::string::npos);
}

TEST(TaskRunnerTests, errorsOnDuplicateIds) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [
                    { "action": "exec", "command": "echo", "id": "shared" },
                    { "action": "exec", "command": "echo", "id": "shared" }
                ]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_FALSE((bool)outputs);
    auto msg = errorText(outputs.takeError());
    EXPECT_NE(msg.find("duplicate id 'shared'"), std::string::npos);
}

TEST(TaskRunnerTests, errorsWhenExecChildExitsNonZero) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "t": {
                "actions": [
                    { "action": "exec", "command": "false" }
                ]
            }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "t", {}, l.props, registry);
    ASSERT_FALSE((bool)outputs);
    auto msg = errorText(outputs.takeError());
    EXPECT_NE(msg.find("exited"), std::string::npos);
}

TEST(TaskRunnerTests, errorsOnDependsOnAtPhase3a) {
    auto l = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "tasks": {
            "build": { "actions": [{ "action": "exec", "command": "true" }] },
            "test":  { "depends-on": ["build"],
                       "actions": [{ "action": "exec", "command": "true" }] }
        }
    })");
    ActionRegistry registry;
    auto outputs = runTask(l.tasks, "test", {}, l.props, registry);
    ASSERT_FALSE((bool)outputs);
    auto msg = errorText(outputs.takeError());
    EXPECT_NE(msg.find("Phase 3b"), std::string::npos);
}
