#include "cajeta/buildtool/TaskRunner.h"

#include <llvm/Support/Error.h>

#include <atomic>
#include <future>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Materializes a task's params into `ctx` from the CLI-bound values,
        // falling back to each spec's default.
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
            }
            return llvm::Error::success();
        }

        // Recursively substitutes every string-typed leaf of a JSON value through
        // the TaskContext.
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
            return v;
        }

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

        // The when/skip-when truthy rule: anything outside the falsy set. There is
        // deliberately no expression language here.
        bool isTruthy(const std::string& s) {
            return !s.empty()
                && s != "false"
                && s != "0"
                && s != "null";
        }

        // Evaluates when / skip-when: TRUE means skip this action, false means run
        // it. A substitution failure propagates as an error.
        llvm::Expected<bool> shouldSkip(
            const std::optional<std::string>& whenExpr,
            const std::optional<std::string>& skipWhenExpr,
            const std::string& whereContext,
            const TaskContext& ctx) {
            if (whenExpr) {
                auto r = ctx.substitute(*whenExpr, whereContext + " when");
                if (!r) return r.takeError();
                if (!isTruthy(*r)) return true;
            }
            if (skipWhenExpr) {
                auto r = ctx.substitute(*skipWhenExpr, whereContext + " skip-when");
                if (!r) return r.takeError();
                if (isTruthy(*r)) return true;
            }
            return false;
        }

        // Executes an ActionEntry list in order against the TaskContext. Recursive:
        // parallel children re-enter here on their own snapshot contexts.
        llvm::Error runEntries(
            const std::map<std::string, Task>& tasks,
            const Task& task,
            const std::vector<ActionEntry>& entries,
            TaskContext& ctx,
            const ResolvedProperties& props,
            const ActionRegistry& registry,
            std::set<std::string>& executedTasks,
            const std::string& breadcrumbBase);

        // Invokes another task with substituted params, publishing its outputs
        // under the entry's id.
        llvm::Error runOneRunTask(
            const std::map<std::string, Task>& tasks,
            const RunTaskCall& call,
            TaskContext& parentCtx,
            const ResolvedProperties& props,
            const ActionRegistry& registry,
            std::set<std::string>& executedTasks,
            const std::string& breadcrumb) {
            auto skip = shouldSkip(call.whenExpr, call.skipWhenExpr,
                                   breadcrumb, parentCtx);
            if (!skip) return skip.takeError();
            if (*skip) return llvm::Error::success();

            auto it = tasks.find(call.taskName);
            if (it == tasks.end()) {
                return err(breadcrumb + " references unknown task '" +
                           call.taskName + "'");
            }

            TaskInvocationParams cli;
            for (const auto& kv : call.params) {
                auto resolved = parentCtx.substitute(
                    kv.second, breadcrumb + " params." + kv.first);
                if (!resolved) return resolved.takeError();
                cli.values[kv.first] = *resolved;
            }

            const Task& called = it->second;

            if (call.taskName == called.name && executedTasks.count(called.name)) {
                // An explicit run-task call may re-run an already-executed task.
            }

            TaskContext childCtx(props, parentCtx.manifest());
            if (auto e = bindParams(called, cli, childCtx)) {
                return std::move(e);
            }

            std::set<std::string> seen;
            for (const auto& entry : called.actions) {
                std::string id;
                switch (entry.kind) {
                    case ActionEntry::Kind::Invocation: id = entry.invocation.id; break;
                    case ActionEntry::Kind::RunTask:    id = entry.runTask.id;    break;
                    case ActionEntry::Kind::Parallel:   continue;  // parallel itself has no id
                }
                if (!id.empty() && !seen.insert(id).second) {
                    return err("task '" + called.name + "': duplicate id '" +
                               id + "' across actions");
                }
            }

            std::set<std::string> childExecuted;
            childExecuted.insert(call.taskName);
            if (auto e = runEntries(tasks, called, called.actions, childCtx,
                                    props, registry, childExecuted,
                                    "task '" + called.name + "'")) {
                return std::move(e);
            }

            std::map<std::string, std::string> calledOutputs;
            for (const auto& kv : called.outputs) {
                auto resolved = childCtx.substitute(
                    kv.second,
                    "task '" + called.name + "' outputs." + kv.first);
                if (!resolved) return resolved.takeError();
                calledOutputs[kv.first] = *resolved;
            }
            if (!call.id.empty()) {
                parentCtx.publishOutputs(call.id, calledOutputs);
            }
            return llvm::Error::success();
        }

        // A `-p` value arrives as a string but action params are typed JSON, so an
        // uncoerced overlay silently changes nothing. Non-coercible stays a string.
        llvm::json::Value coerceCliParam(const std::string& raw) {
            if (raw == "true")  return llvm::json::Value(true);
            if (raw == "false") return llvm::json::Value(false);
            llvm::StringRef s(raw);
            long long i = 0;
            if (!s.empty() && !s.getAsInteger(10, i)) {
                return llvm::json::Value(static_cast<int64_t>(i));
            }
            double d = 0;
            if (!s.empty() && !s.getAsDouble(d)) return llvm::json::Value(d);
            return llvm::json::Value(raw);
        }

        // Overlays the task's CLI `-p` params onto one action's params, the CLI
        // winning. Every action sees every override but reads only its own keys.
        void applyCliParamOverrides(llvm::json::Object& params, const TaskContext& ctx) {
            for (const auto& [name, value] : ctx.cliParams()) {
                params[name] = coerceCliParam(value);
            }
        }

        // Runs one plain action invocation from the registry.
        llvm::Error runOneInvocation(
            const ActionInvocation& inv,
            const ActionRegistry& registry,
            TaskContext& ctx,
            const std::string& breadcrumb) {
            auto skip = shouldSkip(inv.whenExpr, inv.skipWhenExpr,
                                   breadcrumb, ctx);
            if (!skip) return skip.takeError();
            if (*skip) return llvm::Error::success();

            const Action* action = registry.get(inv.action);
            if (!action) {
                return err(breadcrumb + " references unknown action '" +
                           inv.action +
                           "'. See plans/buildtool/build-tool-plan.md for the "
                           "action catalog rollout schedule.");
            }

            auto resolvedParams = substituteParams(inv.params, breadcrumb, ctx);
            if (!resolvedParams) return resolvedParams.takeError();
            applyCliParamOverrides(*resolvedParams, ctx);

            auto result = action->run(*resolvedParams, ctx);
            if (!result) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << breadcrumb << " (" << inv.action << "): "
                   << result.takeError();
                return err(msg);
            }
            if (!inv.id.empty()) {
                ctx.publishOutputs(inv.id, result->outputs);
            }
            return llvm::Error::success();
        }

        // Runs a parallel group: one thread per child on a snapshot context, then
        // joins, merges outputs back in declaration order, and reports the first
        // child error with the rest appended.
        llvm::Error runParallel(
            const std::map<std::string, Task>& tasks,
            const Task& task,
            const ParallelGroup& group,
            TaskContext& parentCtx,
            const ResolvedProperties& props,
            const ActionRegistry& registry,
            std::set<std::string>& executedTasks,
            const std::string& breadcrumb) {
            if (group.children.empty()) return llvm::Error::success();

            std::vector<TaskContext> childCtxs;
            childCtxs.reserve(group.children.size());
            for (size_t i = 0; i < group.children.size(); ++i) {
                childCtxs.push_back(parentCtx.snapshot());
            }

            std::vector<std::string> childErrors(group.children.size());
            std::vector<std::thread> threads;
            threads.reserve(group.children.size());

            for (size_t i = 0; i < group.children.size(); ++i) {
                threads.emplace_back([&, i]() {
                    std::vector<ActionEntry> single = { group.children[i] };
                    std::set<std::string> childExec = executedTasks;
                    if (auto e = runEntries(tasks, task, single, childCtxs[i],
                                            props, registry, childExec,
                                            breadcrumb + ".parallel[" +
                                                std::to_string(i) + "]")) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << e;
                        childErrors[i] = msg;
                        llvm::consumeError(std::move(e));
                    }
                });
            }
            for (auto& t : threads) t.join();

            for (auto& cc : childCtxs) parentCtx.mergeOutputs(cc);

            std::string combined;
            for (size_t i = 0; i < childErrors.size(); ++i) {
                if (childErrors[i].empty()) continue;
                if (combined.empty()) combined = childErrors[i];
                else combined += "\n  also: " + childErrors[i];
            }
            if (!combined.empty()) return err(combined);
            return llvm::Error::success();
        }

        llvm::Error runEntries(
            const std::map<std::string, Task>& tasks,
            const Task& task,
            const std::vector<ActionEntry>& entries,
            TaskContext& ctx,
            const ResolvedProperties& props,
            const ActionRegistry& registry,
            std::set<std::string>& executedTasks,
            const std::string& breadcrumbBase) {
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                std::string bc = breadcrumbBase + " actions[" +
                                 std::to_string(i) + "]";
                switch (e.kind) {
                    case ActionEntry::Kind::Invocation:
                        if (auto err2 = runOneInvocation(
                                e.invocation, registry, ctx, bc)) {
                            return std::move(err2);
                        }
                        break;
                    case ActionEntry::Kind::Parallel:
                        if (auto err2 = runParallel(
                                tasks, task, *e.parallel, ctx,
                                props, registry, executedTasks, bc)) {
                            return std::move(err2);
                        }
                        break;
                    case ActionEntry::Kind::RunTask:
                        if (auto err2 = runOneRunTask(
                                tasks, e.runTask, ctx,
                                props, registry, executedTasks, bc)) {
                            return std::move(err2);
                        }
                        break;
                }
            }
            return llvm::Error::success();
        }

        // Appends `root`'s depends-on closure to `order`, deps before consumers.
        // Assumes the graph has already been validated for cycles.
        void topoOrder(const std::map<std::string, Task>& tasks,
                       const std::string& root,
                       std::unordered_set<std::string>& visited,
                       std::vector<std::string>& order) {
            if (visited.count(root)) return;
            visited.insert(root);
            auto it = tasks.find(root);
            if (it == tasks.end()) return;
            for (const auto& dep : it->second.dependsOn) {
                topoOrder(tasks, dep, visited, order);
            }
            order.push_back(root);
        }

        // Runs one task's actions and resolves its outputs. Does NOT handle
        // depends-on: runTask expands that and calls this in order.
        llvm::Expected<std::map<std::string, std::string>> runOneTask(
            const std::map<std::string, Task>& tasks,
            const Task& task,
            const TaskInvocationParams& cliParams,
            const ResolvedProperties& props,
            const ActionRegistry& registry,
            std::set<std::string>& executedTasks,
            const Manifest* manifest) {
            TaskContext ctx(props, manifest);
            if (auto e = bindParams(task, cliParams, ctx)) {
                return std::move(e);
            }
            ctx.setCliParams(cliParams.values);

            std::set<std::string> seen;
            for (const auto& e : task.actions) {
                std::string id;
                switch (e.kind) {
                    case ActionEntry::Kind::Invocation: id = e.invocation.id; break;
                    case ActionEntry::Kind::RunTask:    id = e.runTask.id;    break;
                    case ActionEntry::Kind::Parallel:   continue;
                }
                if (!id.empty() && !seen.insert(id).second) {
                    return err("task '" + task.name + "': duplicate id '" +
                               id + "' across actions");
                }
            }

            if (auto e = runEntries(tasks, task, task.actions, ctx,
                                    props, registry, executedTasks,
                                    "task '" + task.name + "'")) {
                return std::move(e);
            }

            std::map<std::string, std::string> outputs;
            for (const auto& kv : task.outputs) {
                auto resolved = ctx.substitute(
                    kv.second,
                    "task '" + task.name + "' outputs." + kv.first);
                if (!resolved) return resolved.takeError();
                outputs[kv.first] = *resolved;
            }
            return outputs;
        }

    } // namespace

    llvm::Expected<std::map<std::string, std::string>> runTask(
        const std::map<std::string, Task>& tasks,
        const std::string& taskName,
        const TaskInvocationParams& cliParams,
        const ResolvedProperties& props,
        const ActionRegistry& registry,
        const Manifest* manifest) {

        if (auto e = validateTaskGraph(tasks)) return std::move(e);

        auto tIt = tasks.find(taskName);
        if (tIt == tasks.end()) {
            return err("no such task: '" + taskName + "'");
        }

        std::unordered_set<std::string> visited;
        std::vector<std::string> order;
        topoOrder(tasks, taskName, visited, order);

        std::set<std::string> executed;
        std::map<std::string, std::string> lastOutputs;
        for (const auto& name : order) {
            // CLI params reach the target task only; deps get their defaults.
            const TaskInvocationParams& p =
                (name == taskName) ? cliParams : TaskInvocationParams{};
            auto it = tasks.find(name);
            if (it == tasks.end()) {
                return err("no such task: '" + name + "'");
            }
            auto outs = runOneTask(tasks, it->second, p, props, registry,
                                   executed, manifest);
            if (!outs) return outs.takeError();
            executed.insert(name);
            if (name == taskName) lastOutputs = std::move(*outs);
        }
        return lastOutputs;
    }

    // ─── Show ───────────────────────────────────────────────────────

    namespace {

        // Best-effort substitution for `--show`: an unresolvable reference is left
        // as a literal `${name}` rather than erroring on a static preview.
        std::string subSafe(const TaskContext& ctx, const std::string& s) {
            std::string r;
            r.reserve(s.size());
            for (size_t i = 0; i < s.size(); ) {
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '$') {
                    r += '$'; i += 2; continue;
                }
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
                    size_t close = s.find('}', i + 2);
                    if (close == std::string::npos) { r += s.substr(i); break; }
                    std::string name = s.substr(i + 2, close - (i + 2));
                    auto v = ctx.lookup(name);
                    r += v ? *v : ("${" + name + "}");
                    i = close + 1;
                    continue;
                }
                r += s[i++];
            }
            return r;
        }

        void showValue(const llvm::json::Value& v,
                       const TaskContext& ctx,
                       std::ostream& out) {
            if (auto s = v.getAsString()) {
                out << '"' << subSafe(ctx, s->str()) << '"';
                return;
            }
            if (const auto* o = v.getAsObject()) {
                out << "{";
                bool first = true;
                for (const auto& kv : *o) {
                    if (!first) out << ", ";
                    first = false;
                    out << '"' << kv.first.str() << "\": ";
                    showValue(kv.second, ctx, out);
                }
                out << "}";
                return;
            }
            if (const auto* a = v.getAsArray()) {
                out << "[";
                for (size_t i = 0; i < a->size(); ++i) {
                    if (i) out << ", ";
                    showValue((*a)[i], ctx, out);
                }
                out << "]";
                return;
            }
            std::string buf;
            llvm::raw_string_ostream os(buf);
            os << v;
            out << buf;
        }

        void showEntries(const std::vector<ActionEntry>& entries,
                         const TaskContext& ctx,
                         std::ostream& out,
                         int indent) {
            std::string pad(indent, ' ');
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                switch (e.kind) {
                    case ActionEntry::Kind::Invocation: {
                        const auto& inv = e.invocation;
                        out << pad << "- action: " << inv.action;
                        if (!inv.id.empty()) out << "  (id: " << inv.id << ")";
                        out << "\n";
                        for (const auto& kv : inv.params) {
                            out << pad << "    " << kv.first.str() << ": ";
                            showValue(kv.second, ctx, out);
                            out << "\n";
                        }
                        if (inv.whenExpr) {
                            out << pad << "    when: \""
                                << subSafe(ctx, *inv.whenExpr) << "\"\n";
                        }
                        if (inv.skipWhenExpr) {
                            out << pad << "    skip-when: \""
                                << subSafe(ctx, *inv.skipWhenExpr) << "\"\n";
                        }
                        break;
                    }
                    case ActionEntry::Kind::Parallel: {
                        out << pad << "- parallel:\n";
                        showEntries(e.parallel->children, ctx, out, indent + 4);
                        break;
                    }
                    case ActionEntry::Kind::RunTask: {
                        const auto& rt = e.runTask;
                        out << pad << "- run-task: " << rt.taskName;
                        if (!rt.id.empty()) out << "  (id: " << rt.id << ")";
                        out << "\n";
                        for (const auto& kv : rt.params) {
                            out << pad << "    " << kv.first << ": \""
                                << subSafe(ctx, kv.second) << "\"\n";
                        }
                        break;
                    }
                }
            }
        }

    } // namespace

    llvm::Error showTask(
        const std::map<std::string, Task>& tasks,
        const std::string& taskName,
        const TaskInvocationParams& cliParams,
        const ResolvedProperties& props,
        std::ostream& out,
        const Manifest* manifest) {
        if (auto e = validateTaskGraph(tasks)) return std::move(e);
        auto it = tasks.find(taskName);
        if (it == tasks.end()) return err("no such task: '" + taskName + "'");
        const Task& task = it->second;

        TaskContext ctx(props, manifest);
        if (auto e = bindParams(task, cliParams, ctx)) return std::move(e);

        out << "task: " << taskName;
        if (task.description) out << "  — " << *task.description;
        out << "\n";
        if (!task.dependsOn.empty()) {
            out << "  depends-on: ";
            for (size_t i = 0; i < task.dependsOn.size(); ++i) {
                if (i) out << ", ";
                out << task.dependsOn[i];
            }
            out << "\n";
        }
        out << "  actions:\n";
        showEntries(task.actions, ctx, out, 4);
        if (!task.outputs.empty()) {
            out << "  outputs:\n";
            for (const auto& kv : task.outputs) {
                out << "    " << kv.first << ": \""
                    << subSafe(ctx, kv.second) << "\"\n";
            }
        }
        return llvm::Error::success();
    }

} // namespace cajeta::buildtool
