#include "cajeta/buildtool/Task.h"

#include <llvm/Support/Error.h>

#include <set>
#include <string>

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

        llvm::Error parseActionInvocation(const std::string& taskName,
                                          size_t index,
                                          const llvm::json::Value& v,
                                          ActionInvocation& out) {
            const auto* obj = v.getAsObject();
            if (!obj) {
                return err("task '" + taskName + "': actions[" +
                           std::to_string(index) + "] must be an object");
            }
            // Phase 3a only handles plain action-invocation entries.
            // Phase 3b adds parallel groups and run-task entries; for
            // now we reject those forms with a clear "not yet" message
            // so the parse failure points at what's missing rather
            // than mis-parsing them as actions.
            if (obj->get("parallel")) {
                return err("task '" + taskName + "': actions[" +
                           std::to_string(index) + "] is a parallel group; "
                           "parallel/run-task composition lands in "
                           "Phase 3b");
            }
            if (obj->get("run-task")) {
                return err("task '" + taskName + "': actions[" +
                           std::to_string(index) + "] is a run-task entry; "
                           "parallel/run-task composition lands in "
                           "Phase 3b");
            }

            auto actionStr = obj->getString("action");
            if (!actionStr) {
                return err("task '" + taskName + "': actions[" +
                           std::to_string(index) +
                           "] missing required 'action' field");
            }
            out.action = actionStr->str();
            if (auto id = obj->getString("id")) out.id = id->str();
            if (auto w  = obj->getString("when"))      out.whenExpr     = w->str();
            if (auto sw = obj->getString("skip-when")) out.skipWhenExpr = sw->str();

            // Everything else is an action-specific param. Pass through
            // verbatim; the action's own validator inspects them.
            for (const auto& kv : *obj) {
                if (kActionMetaFields.count(kv.first.str())) continue;
                out.params[kv.first] = kv.second;
            }
            return llvm::Error::success();
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
                ActionInvocation inv;
                if (auto e = parseActionInvocation(name, i, (*actionsArr)[i], inv)) {
                    return std::move(e);
                }
                t.actions.push_back(std::move(inv));
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

} // namespace cajeta::buildtool
