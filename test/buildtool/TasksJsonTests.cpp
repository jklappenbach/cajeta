// Golden contract test for `cajeta tasks --json` (buildtool-widget plan unit 1,
// spec §3). The IDE plugin discovers tasks by parsing this document, so the
// shape is a contract: manifest path + tasks[{name,description,dependsOn,params}]
// + builtins[{name,description}]. We assert structurally (parse the emission
// back) rather than string-match, so key order / whitespace stay free.
#include "cajeta/buildtool/Task.h"

#include <gtest/gtest.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <string>
#include <vector>

using namespace cajeta::buildtool;

namespace {
    llvm::json::Value parseOrFail(const std::string& s) {
        auto v = llvm::json::parse(s);
        EXPECT_TRUE(static_cast<bool>(v))
            << "emission is not valid JSON: "
            << (v ? "" : llvm::toString(v.takeError()));
        return v ? std::move(*v) : llvm::json::Value(nullptr);
    }
}

// §3.1: the full shape with description, dependsOn, and a typed param.
TEST(TasksJson, GoldenShapeMatchesSpecSection3) {
    std::map<std::string, Task> tasks;
    Task build;
    build.name = "build";
    build.description = "Compile + link the project";
    build.dependsOn = {"check"};
    TaskParamSpec flavor;
    flavor.name = "flavor";
    flavor.type = "string";
    flavor.defaultValue = "debug";
    flavor.required = false;
    flavor.doc = "Build flavor";
    build.params = {flavor};
    tasks["build"] = build;

    std::vector<BuiltinCommand> builtins = {
        {"tasks", "List task names"},
        {"info", "Print dependency tree / capabilities"},
    };

    auto root = parseOrFail(renderTasksJson("/abs/path/cajeta.json", tasks, builtins));
    auto* obj = root.getAsObject();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->getString("manifest").value_or(""), "/abs/path/cajeta.json");

    auto* tarr = obj->getArray("tasks");
    ASSERT_NE(tarr, nullptr);
    ASSERT_EQ(tarr->size(), 1u);
    auto* t0 = (*tarr)[0].getAsObject();
    ASSERT_NE(t0, nullptr);
    EXPECT_EQ(t0->getString("name").value_or(""), "build");
    EXPECT_EQ(t0->getString("description").value_or(""), "Compile + link the project");

    auto* deps = t0->getArray("dependsOn");
    ASSERT_NE(deps, nullptr);
    ASSERT_EQ(deps->size(), 1u);
    EXPECT_EQ((*deps)[0].getAsString().value_or(""), "check");

    auto* params = t0->getArray("params");
    ASSERT_NE(params, nullptr);
    ASSERT_EQ(params->size(), 1u);
    auto* p0 = (*params)[0].getAsObject();
    ASSERT_NE(p0, nullptr);
    EXPECT_EQ(p0->getString("name").value_or(""), "flavor");
    EXPECT_EQ(p0->getString("type").value_or(""), "string");
    EXPECT_EQ(p0->getString("default").value_or(""), "debug");
    ASSERT_TRUE(p0->getBoolean("required").has_value());
    EXPECT_FALSE(*p0->getBoolean("required"));
    EXPECT_EQ(p0->getString("doc").value_or(""), "Build flavor");

    auto* barr = obj->getArray("builtins");
    ASSERT_NE(barr, nullptr);
    ASSERT_EQ(barr->size(), 2u);
    EXPECT_EQ((*barr)[0].getAsObject()->getString("name").value_or(""), "tasks");
    EXPECT_EQ((*barr)[1].getAsObject()->getString("name").value_or(""), "info");
}

// §3 (widget §5.2.2): a task with a build action is `runnable` and carries the
// build's output-path as `artifact`; a task without one is not runnable.
TEST(TasksJson, EmitsRunnableAndArtifactFromBuildAction) {
    std::map<std::string, Task> tasks;

    Task build;
    build.name = "build";
    ActionInvocation inv;
    inv.action = "build";
    inv.params["output-path"] = "build/exe/demo";
    ActionEntry entry;
    entry.kind = ActionEntry::Kind::Invocation;
    entry.invocation = inv;
    build.actions.push_back(entry);
    tasks["build"] = build;

    Task lint;            // no build action -> not runnable
    lint.name = "lint";
    tasks["lint"] = lint;

    auto root = parseOrFail(renderTasksJson("/m/cajeta.json", tasks, {}));
    auto* tarr = root.getAsObject()->getArray("tasks");
    ASSERT_EQ(tarr->size(), 2u);
    // sorted by name: build, lint
    auto* b = (*tarr)[0].getAsObject();
    EXPECT_EQ(b->getString("name").value_or(""), "build");
    ASSERT_TRUE(b->getBoolean("runnable").has_value());
    EXPECT_TRUE(*b->getBoolean("runnable"));
    EXPECT_EQ(b->getString("artifact").value_or(""), "build/exe/demo");

    auto* l = (*tarr)[1].getAsObject();
    EXPECT_EQ(l->getString("name").value_or(""), "lint");
    ASSERT_TRUE(l->getBoolean("runnable").has_value());
    EXPECT_FALSE(*l->getBoolean("runnable"));
    EXPECT_EQ(l->get("artifact"), nullptr);   // omitted when no build output-path
}

// §3.1.3 robustness: a minimal task (no description/deps/params) still emits
// valid JSON — optional fields omitted, arrays present but empty — so a sparse
// or forward-evolved manifest never produces broken output.
TEST(TasksJson, SparseTaskEmitsValidJsonWithEmptyArrays) {
    std::map<std::string, Task> tasks;
    Task clean;
    clean.name = "clean";
    tasks["clean"] = clean;

    auto root = parseOrFail(renderTasksJson("/m/cajeta.json", tasks, {}));
    auto* obj = root.getAsObject();
    ASSERT_NE(obj, nullptr);

    auto* t0 = (*obj->getArray("tasks"))[0].getAsObject();
    ASSERT_NE(t0, nullptr);
    EXPECT_EQ(t0->getString("name").value_or(""), "clean");
    EXPECT_EQ(t0->get("description"), nullptr);          // omitted when absent
    EXPECT_EQ(t0->getArray("dependsOn")->size(), 0u);
    EXPECT_EQ(t0->getArray("params")->size(), 0u);
    EXPECT_EQ(obj->getArray("builtins")->size(), 0u);
}

// map ordering: tasks emit sorted by name (std::map), a stable order for the UI.
TEST(TasksJson, TasksEmittedSortedByName) {
    std::map<std::string, Task> tasks;
    for (const char* n : {"zebra", "alpha", "mango"}) {
        Task t;
        t.name = n;
        tasks[n] = t;
    }
    auto root = parseOrFail(renderTasksJson("/m/cajeta.json", tasks, {}));
    auto* tarr = root.getAsObject()->getArray("tasks");
    ASSERT_EQ(tarr->size(), 3u);
    EXPECT_EQ((*tarr)[0].getAsObject()->getString("name").value_or(""), "alpha");
    EXPECT_EQ((*tarr)[1].getAsObject()->getString("name").value_or(""), "mango");
    EXPECT_EQ((*tarr)[2].getAsObject()->getString("name").value_or(""), "zebra");
}
