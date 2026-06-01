#include "cajeta/buildtool/TaskRunner.h"

#include <llvm/Support/Error.h>

#include <set>
#include <string>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Apply CLI-bound values + defaults to a task's params,
        // producing the materialized map for the TaskContext.
        llvm::Error bindParams(const Task& task,
                               const TaskInvocationParams& cli,
                               TaskContext& ctx) {
            for (const auto& spec : task.params) {
                auto it = cli.values.find(spec.name);
                if (it != cli.values.end()) {
                    ctx.setParam(spec.name, it->second);
                    continue;
                }
                if (spec.defaultValue) {
                    // Defaults can themselves contain ${...} references;
                    // resolve them in the task's context (so e.g.
                    // `"default": "${env.RELEASE_BUCKET}"` works).
                    auto resolved = ctx.substitute(
                        *spec.defaultValue,
                        "task '" + task.name + "' params." + spec.name + " default");
                    if (!resolved) return resolved.takeError();
                    ctx.setParam(spec.name, *resolved);
                    continue;
                }
                if (spec.required) {
                    return err("task '" + task.name +
                               "' requires param '" + spec.name +
                               "' (none supplied, no default)");
                }
                // Optional param with no value and no default — leave
                // unset. References to `${params.<name>}` will fail
                // substitution with a clean error citing the action.
            }
            return llvm::Error::success();
        }

        // Recursively walk a JSON value, substituting every string-
        // typed leaf via the TaskContext. Objects and arrays are
        // recursed into; non-string scalars pass through unchanged.
        // Errors at the first failed substitution.
        llvm::Expected<llvm::json::Value> substituteValue(
            const llvm::json::Value& v,
            const std::string& whereContext,
            const TaskContext& ctx) {
            if (auto s = v.getAsString()) {
                auto resolved = ctx.substitute(s->str(), whereContext);
                if (!resolved) return resolved.takeError();
                return llvm::json::Value(*resolved);
            }
            if (const auto* obj = v.getAsObject()) {
                llvm::json::Object out;
                for (const auto& kv : *obj) {
                    auto child = substituteValue(
                        kv.second,
                        whereContext + "." + kv.first.str(), ctx);
                    if (!child) return child.takeError();
                    out[kv.first] = std::move(*child);
                }
                return llvm::json::Value(std::move(out));
            }
            if (const auto* arr = v.getAsArray()) {
                llvm::json::Array out;
                for (size_t i = 0; i < arr->size(); ++i) {
                    auto child = substituteValue(
                        (*arr)[i],
                        whereContext + "[" + std::to_string(i) + "]", ctx);
                    if (!child) return child.takeError();
                    out.push_back(std::move(*child));
                }
                return llvm::json::Value(std::move(out));
            }
            // Numbers, booleans, null — pass through.
            return v;
        }

        // Substitute every string-leaf in an action invocation's
        // params object. Returns a new Object whose strings have
        // ${...} fully resolved against the current TaskContext.
        llvm::Expected<llvm::json::Object> substituteParams(
            const llvm::json::Object& params,
            const std::string& whereContext,
            const TaskContext& ctx) {
            llvm::json::Object out;
            for (const auto& kv : params) {
                auto resolved = substituteValue(
                    kv.second,
                    whereContext + "." + kv.first.str(), ctx);
                if (!resolved) return resolved.takeError();
                out[kv.first] = std::move(*resolved);
            }
            return out;
        }

    } // namespace

    llvm::Expected<std::map<std::string, std::string>> runTask(
        const std::map<std::string, Task>& tasks,
        const std::string& taskName,
        const TaskInvocationParams& cliParams,
        const ResolvedProperties& props,
        const ActionRegistry& registry) {

        auto tIt = tasks.find(taskName);
        if (tIt == tasks.end()) {
            return err("no such task: '" + taskName + "'");
        }
        const Task& task = tIt->second;

        // Phase 3a constraint: depends-on isn't honored yet. If a
        // task declares it, fail loudly rather than silently
        // ignoring it.
        if (!task.dependsOn.empty()) {
            return err("task '" + taskName + "' declares depends-on but "
                       "depends-on traversal lands in Phase 3b "
                       "(plan/build-tool-plan.md)");
        }

        TaskContext ctx(props);

        // Bind params (CLI > default > required-or-error).
        if (auto e = bindParams(task, cliParams, ctx)) {
            return std::move(e);
        }

        // Reject duplicate ids up front — two actions publishing
        // under the same id would shadow each other, almost
        // certainly a manifest mistake.
        std::set<std::string> seenIds;
        for (const auto& inv : task.actions) {
            if (inv.id.empty()) continue;
            if (!seenIds.insert(inv.id).second) {
                return err("task '" + taskName + "': duplicate id '" +
                           inv.id + "' across actions");
            }
        }

        // Walk actions linearly.
        for (size_t i = 0; i < task.actions.size(); ++i) {
            const auto& inv = task.actions[i];

            // Reject when/skip-when at Phase 3a — full evaluation
            // lands in Phase 3b. If a task uses them today, fail
            // explicitly rather than ignoring.
            if (inv.whenExpr || inv.skipWhenExpr) {
                return err("task '" + taskName + "': actions[" +
                           std::to_string(i) +
                           "] uses when/skip-when, which lands in "
                           "Phase 3b");
            }

            const Action* action = registry.get(inv.action);
            if (!action) {
                return err("task '" + taskName + "': actions[" +
                           std::to_string(i) +
                           "] references unknown action '" + inv.action +
                           "'. Built-in actions in Phase 3a: exec. "
                           "Other actions land in later phases per "
                           "plan/build-tool-plan.md.");
            }

            std::string where = "task '" + taskName + "' actions[" +
                                std::to_string(i) + "] (" + inv.action + ")";
            auto resolvedParams = substituteParams(inv.params, where, ctx);
            if (!resolvedParams) return resolvedParams.takeError();

            auto result = action->run(*resolvedParams, ctx);
            if (!result) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << where << ": " << result.takeError();
                return err(msg);
            }
            if (!inv.id.empty()) {
                ctx.publishOutputs(inv.id, result->outputs);
            }
        }

        // Resolve the task's outputs block.
        std::map<std::string, std::string> taskOutputs;
        for (const auto& kv : task.outputs) {
            auto resolved = ctx.substitute(
                kv.second,
                "task '" + taskName + "' outputs." + kv.first);
            if (!resolved) return resolved.takeError();
            taskOutputs[kv.first] = *resolved;
        }
        return taskOutputs;
    }

} // namespace cajeta::buildtool
