#include "cajeta/buildtool/Task.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Recognized fields on a task entry. Anything else is a typo.
        const std::set<std::string> kTaskFields = {
            "description", "depends-on", "params", "actions",
            "outputs", "working-dir", "env",
        };

        // Recognized fields on a param-spec.
        const std::set<std::string> kParamFields = {
            "type", "default", "required", "doc",
        };

        // Recognized non-param keys on an action-invocation entry.
        // Anything else is treated as an action-specific param.
        const std::set<std::string> kActionMetaFields = {
            "action", "id", "when", "skip-when",
        };

        llvm::Error parseParamSpec(const std::string& taskName,
                                   const std::string& paramName,
                                   const llvm::json::Value& v,
                                   TaskParamSpec& out) {
            out.name = paramName;
            const auto* obj = v.getAsObject();
            if (!obj) {
                return err("task '" + taskName + "': param '" + paramName +
                           "' must be an object");
            }
            for (const auto& kv : *obj) {
                if (!kParamFields.count(kv.first.str())) {
                    return err("task '" + taskName + "': param '" + paramName +
                               "' has unknown field '" + kv.first.str() +
                               "' (allowed: type, default, required, doc)");
                }
            }
            if (auto t = obj->getString("type")) {
                out.type = t->str();
                if (out.type != "string" && out.type != "bool") {
                    return err("task '" + taskName + "': param '" + paramName +
                               "' has unsupported type '" + out.type +
                               "' (allowed: string, bool)");
                }
            }
            if (auto r = obj->getBoolean("required")) out.required = *r;
            if (const auto* d = obj->get("default")) {
                if (auto s = d->getAsString()) {
                    out.defaultValue = s->str();
                } else if (auto b = d->getAsBoolean()) {
                    out.defaultValue = *b ? std::string("true") : std::string("false");
                } else {
                    return err("task '" + taskName + "': param '" + paramName +
                               "': default must be a string or bool");
                }
            }
            if (auto doc = obj->getString("doc")) out.doc = doc->str();
            return llvm::Error::success();
        }

        // Forward decl; parseActionEntry recurses into parallel groups.
        llvm::Error parseActionEntry(const std::string& taskName,
                                     const std::string& breadcrumb,
                                     const llvm::json::Value& v,
                                     ActionEntry& out);

        llvm::Error parseActionInvocation(const std::string& taskName,
                                          const std::string& breadcrumb,
                                          const llvm::json::Object& obj,
                                          ActionInvocation& out) {
            auto actionStr = obj.getString("action");
            if (!actionStr) {
                return err("task '" + taskName + "': " + breadcrumb +
                           " missing required 'action' field");
            }
            out.action = actionStr->str();
            if (auto id = obj.getString("id")) out.id = id->str();
            if (auto w  = obj.getString("when"))      out.whenExpr     = w->str();
            if (auto sw = obj.getString("skip-when")) out.skipWhenExpr = sw->str();

            // Everything else is an action-specific param.
            for (const auto& kv : obj) {
                if (kActionMetaFields.count(kv.first.str())) continue;
                out.params[kv.first] = kv.second;
            }
            return llvm::Error::success();
        }

        llvm::Error parseParallelGroup(const std::string& taskName,
                                       const std::string& breadcrumb,
                                       const llvm::json::Object& obj,
                                       ParallelGroup& out) {
            // The `parallel` field is the array of children. Other
            // fields aren't allowed on a parallel entry.
            for (const auto& kv : obj) {
                if (kv.first.str() != "parallel") {
                    return err("task '" + taskName + "': " + breadcrumb +
                               " parallel entry has unexpected field '" +
                               kv.first.str() +
                               "' (parallel takes only the 'parallel' array)");
                }
            }
            const auto* arr = obj.getArray("parallel");
            if (!arr) {
                return err("task '" + taskName + "': " + breadcrumb +
                           " 'parallel' must be an array");
            }
            for (size_t i = 0; i < arr->size(); ++i) {
                ActionEntry child;
                std::string childBc = breadcrumb + ".parallel[" +
                                      std::to_string(i) + "]";
                if (auto e = parseActionEntry(taskName, childBc,
                                              (*arr)[i], child)) {
                    return std::move(e);
                }
                out.children.push_back(std::move(child));
            }
            return llvm::Error::success();
        }

        llvm::Error parseRunTaskCall(const std::string& taskName,
                                     const std::string& breadcrumb,
                                     const llvm::json::Object& obj,
                                     RunTaskCall& out) {
            // Allowed fields: run-task, params, id, when, skip-when.
            static const std::set<std::string> kRunTaskFields = {
                "run-task", "params", "id", "when", "skip-when",
            };
            for (const auto& kv : obj) {
                if (!kRunTaskFields.count(kv.first.str())) {
                    return err("task '" + taskName + "': " + breadcrumb +
                               " run-task entry has unexpected field '" +
                               kv.first.str() +
                               "' (allowed: run-task, params, id, when, skip-when)");
                }
            }
            auto name = obj.getString("run-task");
            if (!name) {
                return err("task '" + taskName + "': " + breadcrumb +
                           " 'run-task' must be a string (the called task's name)");
            }
            out.taskName = name->str();
            if (auto id = obj.getString("id")) out.id = id->str();
            if (auto w  = obj.getString("when"))      out.whenExpr     = w->str();
            if (auto sw = obj.getString("skip-when")) out.skipWhenExpr = sw->str();

            if (const auto* p = obj.getObject("params")) {
                for (const auto& kv : *p) {
                    auto s = kv.second.getAsString();
                    if (!s) {
                        return err("task '" + taskName + "': " + breadcrumb +
                                   " params." + kv.first.str() +
                                   " must be a string");
                    }
                    out.params[kv.first.str()] = s->str();
                }
            }
            return llvm::Error::success();
        }

        llvm::Error parseActionEntry(const std::string& taskName,
                                     const std::string& breadcrumb,
                                     const llvm::json::Value& v,
                                     ActionEntry& out) {
            const auto* obj = v.getAsObject();
            if (!obj) {
                return err("task '" + taskName + "': " + breadcrumb +
                           " must be an object");
            }
            const bool hasParallel = obj->get("parallel") != nullptr;
            const bool hasRunTask  = obj->get("run-task") != nullptr;
            const bool hasAction   = obj->get("action") != nullptr;
            const int count =
                (hasParallel ? 1 : 0) + (hasRunTask ? 1 : 0) + (hasAction ? 1 : 0);
            if (count == 0) {
                return err("task '" + taskName + "': " + breadcrumb +
                           " must declare exactly one of "
                           "'action', 'parallel', or 'run-task'");
            }
            if (count > 1) {
                return err("task '" + taskName + "': " + breadcrumb +
                           " declares more than one of "
                           "'action', 'parallel', or 'run-task' "
                           "(pick exactly one)");
            }
            if (hasParallel) {
                out.kind = ActionEntry::Kind::Parallel;
                out.parallel = std::make_shared<ParallelGroup>();
                return parseParallelGroup(taskName, breadcrumb, *obj,
                                          *out.parallel);
            }
            if (hasRunTask) {
                out.kind = ActionEntry::Kind::RunTask;
                return parseRunTaskCall(taskName, breadcrumb, *obj,
                                        out.runTask);
            }
            out.kind = ActionEntry::Kind::Invocation;
            return parseActionInvocation(taskName, breadcrumb, *obj,
                                         out.invocation);
        }

        llvm::Error parseStringMap(const std::string& taskName,
                                   const std::string& field,
                                   const llvm::json::Object& obj,
                                   std::map<std::string, std::string>& out) {
            for (const auto& kv : obj) {
                auto s = kv.second.getAsString();
                if (!s) {
                    return err("task '" + taskName + "': '" + field +
                               "." + kv.first.str() + "' must be a string");
                }
                out[kv.first.str()] = s->str();
            }
            return llvm::Error::success();
        }

        llvm::Error parseStringArray(const std::string& taskName,
                                     const std::string& field,
                                     const llvm::json::Array& arr,
                                     std::vector<std::string>& out) {
            for (size_t i = 0; i < arr.size(); ++i) {
                auto s = arr[i].getAsString();
                if (!s) {
                    return err("task '" + taskName + "': '" + field +
                               "[" + std::to_string(i) +
                               "]' must be a string");
                }
                out.push_back(s->str());
            }
            return llvm::Error::success();
        }

    } // namespace

    llvm::Expected<std::map<std::string, Task>> parseTasks(
        const llvm::json::Object& tasksBlock) {
        std::map<std::string, Task> out;

        for (const auto& tkv : tasksBlock) {
            const std::string name = tkv.first.str();
            const auto* tobj = tkv.second.getAsObject();
            if (!tobj) {
                return err("task '" + name + "' must be an object");
            }
            for (const auto& kv : *tobj) {
                if (!kTaskFields.count(kv.first.str())) {
                    return err("task '" + name + "': unknown field '" +
                               kv.first.str() +
                               "' (allowed: description, depends-on, "
                               "params, actions, outputs, working-dir, env)");
                }
            }

            Task t;
            t.name = name;
            if (auto d = tobj->getString("description")) t.description = d->str();

            if (const auto* deps = tobj->get("depends-on")) {
                const auto* arr = deps->getAsArray();
                if (!arr) {
                    return err("task '" + name + "': 'depends-on' must be an array");
                }
                if (auto e = parseStringArray(name, "depends-on", *arr, t.dependsOn)) {
                    return std::move(e);
                }
            }

            if (const auto* params = tobj->getObject("params")) {
                for (const auto& pkv : *params) {
                    TaskParamSpec spec;
                    if (auto e = parseParamSpec(name, pkv.first.str(),
                                                pkv.second, spec)) {
                        return std::move(e);
                    }
                    t.params.push_back(std::move(spec));
                }
            }

            // 'actions' is required (a task with no actions is
            // ill-formed — what would it do?).
            const auto* actionsArr = tobj->getArray("actions");
            if (!actionsArr) {
                return err("task '" + name +
                           "' missing required 'actions' array");
            }
            for (size_t i = 0; i < actionsArr->size(); ++i) {
                ActionEntry entry;
                std::string bc = "actions[" + std::to_string(i) + "]";
                if (auto e = parseActionEntry(name, bc,
                                              (*actionsArr)[i], entry)) {
                    return std::move(e);
                }
                t.actions.push_back(std::move(entry));
            }

            if (const auto* outputs = tobj->getObject("outputs")) {
                if (auto e = parseStringMap(name, "outputs", *outputs, t.outputs)) {
                    return std::move(e);
                }
            }
            if (auto wd = tobj->getString("working-dir")) {
                t.workingDir = wd->str();
            }
            if (const auto* env = tobj->getObject("env")) {
                if (auto e = parseStringMap(name, "env", *env, t.env)) {
                    return std::move(e);
                }
            }

            out[name] = std::move(t);
        }

        return out;
    }

    llvm::Expected<std::map<std::string, Task>> parseTasks(
        const Manifest& manifest) {
        return parseTasks(manifest.tasksRaw);
    }

    namespace {

        // DFS-based cycle detector. `path` carries the current
        // exploration chain so a cycle error names its members.
        // White / gray / black coloring: not-seen / visiting / done.
        bool detectCycle(const std::string& name,
                         const std::map<std::string, Task>& tasks,
                         std::unordered_set<std::string>& gray,
                         std::unordered_set<std::string>& black,
                         std::vector<std::string>& path,
                         std::vector<std::string>& cycleOut) {
            if (black.count(name)) return false;
            if (gray.count(name)) {
                // Found a cycle. Walk back through `path` until we hit
                // `name` to record the cycle members.
                auto it = std::find(path.begin(), path.end(), name);
                if (it != path.end()) {
                    cycleOut.assign(it, path.end());
                }
                cycleOut.push_back(name);
                return true;
            }
            auto it = tasks.find(name);
            if (it == tasks.end()) {
                // Reference to a task that doesn't exist — different
                // error class; caller flags it separately.
                return false;
            }
            gray.insert(name);
            path.push_back(name);
            for (const auto& dep : it->second.dependsOn) {
                if (detectCycle(dep, tasks, gray, black, path, cycleOut)) {
                    return true;
                }
            }
            path.pop_back();
            gray.erase(name);
            black.insert(name);
            return false;
        }

    } // namespace

    llvm::Error validateTaskGraph(const std::map<std::string, Task>& tasks) {
        // First: undefined-dep check. A task referencing a depends-on
        // target that isn't in the table is a load-time error.
        for (const auto& kv : tasks) {
            for (const auto& dep : kv.second.dependsOn) {
                if (!tasks.count(dep)) {
                    return err("task '" + kv.first +
                               "' depends on undefined task '" + dep + "'");
                }
            }
        }
        // Then: cycle detection.
        std::unordered_set<std::string> gray, black;
        std::vector<std::string> path;
        std::vector<std::string> cycle;
        for (const auto& kv : tasks) {
            if (black.count(kv.first)) continue;
            if (detectCycle(kv.first, tasks, gray, black, path, cycle)) {
                std::string cycleStr;
                for (const auto& n : cycle) {
                    if (!cycleStr.empty()) cycleStr += " → ";
                    cycleStr += n;
                }
                return err("cyclic depends-on: " + cycleStr);
            }
        }
        return llvm::Error::success();
    }

    namespace {
        // Whether a task produces a runnable native artifact, and (if the build
        // action declares one) its output-path. Lets `tasks --json` tell the IDE
        // which tasks can be Run-as-Debug and what binary to launch under
        // `cajeta dap` (widget spec §3, §5.2.2). Scans nested parallel groups.
        struct TaskRunInfo {
            bool runnable = false;
            std::optional<std::string> artifact;
        };

        void scanActionsForRun(const std::vector<ActionEntry>& actions, TaskRunInfo& info) {
            for (const auto& e : actions) {
                switch (e.kind) {
                    case ActionEntry::Kind::Invocation:
                        if (e.invocation.action == "build" ||
                            e.invocation.action == "exec" ||
                            e.invocation.action == "run") {
                            info.runnable = true;
                            if (e.invocation.action == "build") {
                                if (auto op = e.invocation.params.getString("output-path"))
                                    info.artifact = op->str();
                            }
                        }
                        break;
                    case ActionEntry::Kind::Parallel:
                        if (e.parallel) scanActionsForRun(e.parallel->children, info);
                        break;
                    case ActionEntry::Kind::RunTask:
                        break;  // delegates to another task; its own entry reports runnability
                }
            }
        }

        TaskRunInfo computeTaskRunInfo(const Task& t) {
            TaskRunInfo info;
            scanActionsForRun(t.actions, info);
            return info;
        }
    }  // namespace

    std::string renderTasksJson(const std::string& manifestPath,
                                const std::map<std::string, Task>& tasks,
                                const std::vector<BuiltinCommand>& builtins,
                                const DebugLaunchCoords& debugCoords) {
        llvm::json::Object root;
        root["manifest"] = manifestPath;

        // Project-level debug-launch coordinates (widget §5.2.2, unit 7): only
        // when an entry method is known can `cajeta dap` JIT-run the project, so
        // we gate the whole `build` object on it. The IDE forms a runnable
        // task's Debug launch from these; without them it disables Debug.
        if (debugCoords.entryMethod && !debugCoords.entryMethod->empty()) {
            llvm::json::Object build;
            build["entryMethod"] = *debugCoords.entryMethod;
            if (debugCoords.sourceRoot) build["sourceRoot"] = *debugCoords.sourceRoot;
            root["build"] = std::move(build);
        }

        llvm::json::Array taskArr;
        for (const auto& [name, t] : tasks) {  // std::map -> sorted by name
            (void) name;
            llvm::json::Object to;
            to["name"] = t.name;
            if (t.description) to["description"] = *t.description;

            // §3 (widget §5.2.2): expose whether the task yields a runnable
            // artifact (so the IDE can offer Debug) and, when declared, where.
            TaskRunInfo ri = computeTaskRunInfo(t);
            to["runnable"] = ri.runnable;
            if (ri.artifact) to["artifact"] = *ri.artifact;

            llvm::json::Array deps;
            for (const auto& d : t.dependsOn) deps.push_back(d);
            to["dependsOn"] = std::move(deps);

            llvm::json::Array params;
            for (const auto& p : t.params) {
                llvm::json::Object po;
                po["name"] = p.name;
                po["type"] = p.type;
                if (p.defaultValue) po["default"] = *p.defaultValue;
                po["required"] = p.required;
                if (p.doc) po["doc"] = *p.doc;
                params.push_back(std::move(po));
            }
            to["params"] = std::move(params);
            taskArr.push_back(std::move(to));
        }
        root["tasks"] = std::move(taskArr);

        llvm::json::Array builtinArr;
        for (const auto& b : builtins) {
            builtinArr.push_back(llvm::json::Object{
                {"name", b.name},
                {"description", b.description},
            });
        }
        root["builtins"] = std::move(builtinArr);

        std::string out;
        llvm::raw_string_ostream os(out);
        os << llvm::formatv("{0:2}", llvm::json::Value(std::move(root)));
        return os.str();
    }

} // namespace cajeta::buildtool
