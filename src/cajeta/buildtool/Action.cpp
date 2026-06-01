#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <cctype>
#include <functional>
#include <sstream>
#include <utility>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Single-pass substitution over `s`. References are
        // `${NAME}` or `${id.field}`. `$$` escapes a literal `$`.
        // `lookup` returns nullopt for unknown names; on failure the
        // caller produces a citation-style error.
        bool substituteOnce(
            const std::string& s,
            const std::function<std::optional<std::string>(const std::string&)>& lookup,
            std::string& out,
            std::string& missing) {
            std::string r;
            r.reserve(s.size());
            for (size_t i = 0; i < s.size(); ) {
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '$') {
                    r += '$';
                    i += 2;
                    continue;
                }
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
                    size_t close = s.find('}', i + 2);
                    if (close == std::string::npos) {
                        r += s.substr(i);
                        i = s.size();
                        continue;
                    }
                    std::string name = s.substr(i + 2, close - (i + 2));
                    auto v = lookup(name);
                    if (!v) {
                        missing = name;
                        return false;
                    }
                    r += *v;
                    i = close + 1;
                    continue;
                }
                r += s[i++];
            }
            out = std::move(r);
            return true;
        }

    } // namespace

    TaskContext::TaskContext(const ResolvedProperties& props)
        : props_(props) {}

    void TaskContext::setParam(const std::string& name,
                               const std::string& value) {
        params_[name] = value;
    }

    void TaskContext::publishOutputs(
        const std::string& id,
        const std::map<std::string, std::string>& outputs) {
        if (id.empty()) return;
        actionOutputs_[id] = outputs;
    }

    TaskContext TaskContext::snapshot() const {
        TaskContext out(props_);
        out.params_ = params_;
        out.actionOutputs_ = actionOutputs_;
        return out;
    }

    void TaskContext::mergeOutputs(const TaskContext& other) {
        for (const auto& kv : other.actionOutputs_) {
            actionOutputs_[kv.first] = kv.second;
        }
    }

    std::optional<std::string> TaskContext::lookup(
        const std::string& name) const {
        // params.<name>
        if (name.size() > 7 && name.compare(0, 7, "params.") == 0) {
            std::string p = name.substr(7);
            auto it = params_.find(p);
            if (it == params_.end()) return std::nullopt;
            return it->second;
        }
        // <id>.<field>  →  prior action's output
        auto dot = name.find('.');
        if (dot != std::string::npos) {
            std::string id = name.substr(0, dot);
            std::string field = name.substr(dot + 1);
            auto it = actionOutputs_.find(id);
            if (it != actionOutputs_.end()) {
                auto f = it->second.find(field);
                if (f != it->second.end()) return f->second;
                // id matched but field didn't — fall through to props
                // lookup; that lets `details.name` etc. work even if
                // a task happens to have an id named "details".
            }
        }
        // Fall back to manifest properties (built-ins + user).
        return props_.lookup(name);
    }

    llvm::Expected<std::string> TaskContext::substitute(
        const std::string& s,
        const std::string& whereContext) const {
        std::string result;
        std::string missing;
        bool ok = substituteOnce(s,
            [&](const std::string& name) -> std::optional<std::string> {
                return lookup(name);
            }, result, missing);
        if (!ok) {
            return err(whereContext + ": references undefined property '" +
                       missing + "'");
        }
        return result;
    }

    // ──── Action registry ────────────────────────────────────────────

    // Forward decls for action implementations registered below.
    class ExecAction;
    std::unique_ptr<Action> makeExecAction();

    ActionRegistry::ActionRegistry() {
        // Phase 3a: register `exec`. Subsequent phases add more.
        auto reg = [&](std::unique_ptr<Action> a) {
            actions_[a->name()] = std::move(a);
        };
        reg(makeExecAction());
    }

    const Action* ActionRegistry::get(const std::string& name) const {
        auto it = actions_.find(name);
        if (it == actions_.end()) return nullptr;
        return it->second.get();
    }

    std::vector<std::string> ActionRegistry::list() const {
        std::vector<std::string> names;
        names.reserve(actions_.size());
        for (const auto& kv : actions_) names.push_back(kv.first);
        return names;
    }

} // namespace cajeta::buildtool
