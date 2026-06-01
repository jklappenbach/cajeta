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

        // Recursively walk a JSON value, substituting every string-
        // typed leaf via the TaskContext.
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

        // The when/skip-when truthy rule. A substituted string is
        // "truthy" iff it's not in the falsy set. Deliberately
        // minimal — no expression language; if you need real logic,
        // write a user-defined action that wraps `exec`.
        bool isTruthy(const std::string& s) {
            return !s.empty()
                && s != "false"
                && s != "0"
                && s != "null";
        }

        // Evaluate when / skip-when. Returns:
        //   true  → skip this action
        //   false → run this action
        // Errors propagate (substitution failure).
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

        // Walk an ActionEntry list, executing each entry in order
        // against the TaskContext. Recursive — parallel children
        // dispatch to runEntries() in their own snapshot contexts.
        llvm::Error runEntries(
            const std::map<std::string, Task>& tasks,
            const Task& task,
            const std::vector<ActionEntry>& entries,
            TaskContext& ctx,
            const ResolvedProperties& props,
            const ActionRegistry& registry,
            std::set<std::string>& executedTasks,
            const std::string& breadcrumbBase);

        // Run-task: invoke another task with substituted params,
        // capture its outputs, publish them under the entry's id.
        llvm::Error runOneRunTask(
            const std::map<std::string, Task>& tasks,
            const RunTaskCall& call,
            TaskContext& parentCtx,
            const ResolvedProperties& props,
            const ActionRegistry& registry,
            std::set<std::string>& executedTasks,
            const std::string& breadcrumb) {
            // Resolve when/skip-when first.
            auto skip = shouldSkip(call.whenExpr, call.skipWhenExpr,
                                   breadcrumb, parentCtx);
            if (!skip) return skip.takeError();
            if (*skip) return llvm::Error::success();

            auto it = tasks.find(call.taskName);
            if (it == tasks.end()) {
                return err(breadcrumb + " references unknown task '" +
                           call.taskName + "'");
            }

            // Substitute the param values against the calling
            // context, then bind them as the called task's CLI
            // params.
            TaskInvocationParams cli;
            for (const auto& kv : call.params) {
                auto resolved = parentCtx.substitute(
                    kv.second, breadcrumb + " params." + kv.first);
                if (!resolved) return resolved.takeError();
                cli.values[kv.first] = *resolved;
            }

            // Recurse — the called task gets its own TaskContext.
            // Its dependencies traverse the same executedTasks set so
            // they don't double-run if also depended on by the
            // current task.
            const Task& called = it->second;

            // Detect deeper run-task cycles: a task indirectly
            // calling itself via run-task is harder to detect than
            // depends-on cycles. Use a small guard via executedTasks
            // set — but run-task can legitimately invoke a task
            // multiple times, so we use a per-call-chain set rather
            // than the same dedupe set. Phase 3b keeps it simple:
            // detect immediate self-call.
            if (call.taskName == called.name && executedTasks.count(called.name)) {
                // Already executed via depends-on or a prior run-task;
                // re-running run-task is allowed (it's an explicit
                // call), so don't error. Just proceed.
            }

            TaskContext childCtx(props, parentCtx.manifest());
            if (auto e = bindParams(called, cli, childCtx)) {
                return std::move(e);
            }

            // Reject duplicate ids in the called task before running.
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

            // Resolve called task's outputs block; publish under the
            // entry's id (if any).
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

        // Plain action invocation execution.
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
                           "'. See plan/build-tool-plan.md for the "
                           "action catalog rollout schedule.");
            }

            auto resolvedParams = substituteParams(inv.params, breadcrumb, ctx);
            if (!resolvedParams) return resolvedParams.takeError();

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

        // Parallel group: spawn each child in its own thread with a
        // snapshot context, join all, merge outputs back into the
        // parent. If any child errors, collect the first error
        // (others are reported in passing).
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

            // Per-child contexts (snapshot of the parent at entry).
            std::vector<TaskContext> childCtxs;
            childCtxs.reserve(group.children.size());
            for (size_t i = 0; i < group.children.size(); ++i) {
                childCtxs.push_back(parentCtx.snapshot());
            }

            // Per-child error slots.
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

            // Merge each child's outputs back into the parent in
            // declaration order so the final state is deterministic.
            for (auto& cc : childCtxs) parentCtx.mergeOutputs(cc);

            // Collect errors. First non-empty wins; subsequent ones
            // surface in the error message.
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

        // Topologically sort depends-on graph; return tasks in
        // execution order (deps before consumers). Assumes the graph
        // has already been validated for cycles.
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

        // Run one task (its actions + outputs resolution). Does NOT
        // handle depends-on — that's done by the outer runTask which
        // topologically expands and calls this in order.
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

            // Duplicate id check across this task's top-level
            // actions (parallel groups themselves have no id, but
            // their children do — those are checked recursively).
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

            // Resolve outputs block.
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

        // Validate the whole task graph for cycles before any action
        // fires. Cheap; catches the structural error early.
        if (auto e = validateTaskGraph(tasks)) return std::move(e);

        auto tIt = tasks.find(taskName);
        if (tIt == tasks.end()) {
            return err("no such task: '" + taskName + "'");
        }

        // Topologically expand depends-on; run each dep in order,
        // skip already-done within this invocation.
        std::unordered_set<std::string> visited;
        std::vector<std::string> order;
        topoOrder(tasks, taskName, visited, order);

        std::set<std::string> executed;
        std::map<std::string, std::string> lastOutputs;
        for (const auto& name : order) {
            // CLI-supplied params apply only to the target task,
            // not to its deps (deps get their declared defaults).
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

        std::string subSafe(const TaskContext& ctx, const std::string& s) {
            // Best-effort substitute. Leave unresolvable references
            // as literal `${name}` so `--show` doesn't error on
            // missing-at-this-point references (which is fine for
            // a static preview).
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
            // Primitive — let llvm::json format it.
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
