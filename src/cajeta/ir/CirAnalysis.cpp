//
// CirSpecializationAnalysis — see CirAnalysis.h and spec §3.5, §3.6.
//

#include "CirAnalysis.h"

#include <sstream>
#include <unordered_map>

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaFunctionType.h"

namespace cajeta {
namespace ir {

namespace {

    // A first-class function type `(T…)->R`. Detected via the resolved CajetaType
    // when present (lowered IR) or the textual spelling (hand-built IR).
    bool isClosureType(const CirType& t) {
        if (t.resolved && std::dynamic_pointer_cast<CajetaFunctionType>(t.resolved))
            return true;
        return t.spelling.find("->") != std::string::npos;
    }

    // Per-function index: value -> defining instruction, and memory-cell ->
    // values stored into it (for tracing closures through a local slot).
    struct FnIndex {
        std::unordered_map<CirValue*, CirInstPtr> defOf;
        std::unordered_map<CirValue*, std::vector<CirValuePtr>> cellStores;

        explicit FnIndex(const CirFunctionPtr& fn) {
            for (auto& bb : fn->blocks) {
                auto record = [&](const CirInstPtr& inst) {
                    if (!inst) return;
                    if (inst->result) defOf[inst->result.get()] = inst;
                    if (inst->op == CirOp::Store && inst->operands.size() >= 2)
                        cellStores[inst->operands[0].get()].push_back(inst->operands[1]);
                };
                for (auto& inst : bb->insts) record(inst);
                record(bb->terminator);
            }
        }
    };

    // §3.5.1/§3.5.3 — trace a value back to the unique make.closure it resolves
    // to, through the defining inst and (one level) a single-store memory cell.
    // Returns the make.closure inst, or null for "unknown / not direct".
    CirInstPtr traceClosureTarget(const CirValuePtr& v, const FnIndex& idx, int depth = 0) {
        if (!v || depth > 8) return nullptr;
        auto it = idx.defOf.find(v.get());
        if (it == idx.defOf.end()) return nullptr;     // param / block-param / undef
        const CirInstPtr& def = it->second;
        if (def->op == CirOp::MakeClosure) return def;
        if (def->op == CirOp::AllocStack) {            // a local slot
            auto sit = idx.cellStores.find(v.get());
            if (sit != idx.cellStores.end() && sit->second.size() == 1)
                return traceClosureTarget(sit->second.front(), idx, depth + 1);
        }
        return nullptr;
    }

    // §3.5.4/§3.5.5 — inside callee F, collect the apply.closure sites that invoke
    // parameter P (operand[0] == P), and detect whether P escapes (appears as any
    // other operand: stored, returned, passed on). Returns invocation-site count;
    // sets `escapes` if P is used anywhere but as an apply.closure target.
    int invocationMap(const CirFunctionPtr& F, const CirValuePtr& P, bool& escapes) {
        escapes = false;
        int sites = 0;
        auto scan = [&](const CirInstPtr& inst) {
            if (!inst) return;
            for (size_t k = 0; k < inst->operands.size(); ++k) {
                if (inst->operands[k].get() != P.get()) continue;
                if (inst->op == CirOp::ApplyClosure && k == 0) ++sites;
                else escapes = true;         // any other use is an escape
            }
        };
        for (auto& bb : F->blocks) {
            for (auto& inst : bb->insts) scan(inst);
            scan(bb->terminator);
        }
        return sites;
    }

} // namespace

CirAnalysisResult CirSpecializationAnalysis::analyze(
        const std::vector<CirFunctionPtr>& slice) {
    CirAnalysisResult result;

    std::unordered_map<std::string, CirFunctionPtr> byName;
    for (auto& fn : slice) if (fn) byName[fn->name] = fn;

    for (auto& caller : slice) {
        if (!caller) continue;
        FnIndex idx(caller);

        auto visit = [&](const CirInstPtr& inst) {
            if (!inst) return;
            if (inst->op != CirOp::Call && inst->op != CirOp::CallGeneric) return;

            const std::string& calleeName = inst->symbol;
            auto fit = byName.find(calleeName);
            const bool calleeKnown = (fit != byName.end());
            CirFunctionPtr F = calleeKnown ? fit->second : nullptr;

            for (size_t j = 0; j < inst->operands.size(); ++j) {
                const CirValuePtr& arg = inst->operands[j];
                if (!arg) continue;

                // The authoritative "this is a closure argument" signal is the
                // callee's parameter type at this position; for an unknown callee
                // fall back to the arg's own type / a make.closure definition.
                const bool paramIsClosure =
                    calleeKnown && j < F->params.size() && isClosureType(F->params[j]->type);
                CirInstPtr mk = traceClosureTarget(arg, idx);
                const bool argLooksClosure = isClosureType(arg->type) || mk != nullptr;
                if (!paramIsClosure && !argLooksClosure) continue;

                // Callee body must be visible to guarantee a *complete* invocation
                // set (§3.5.4); otherwise we cannot specialize safely.
                if (!calleeKnown) {
                    result.leaveIndirect.push_back(
                        {caller->name, calleeName,
                         "callee body not in slice (incomplete invocation set)"});
                    continue;
                }
                if (!paramIsClosure) continue;   // arg at a non-closure parameter position
                CirValuePtr P = F->params[j];

                // §3.5.1/§3.5.3 — known, directly-supplied target?
                if (!mk) {
                    result.leaveIndirect.push_back(
                        {caller->name, calleeName,
                         "closure argument not a directly-supplied known closure"});
                    continue;
                }
                // §3.5.2 — capture-free?
                if (!mk->targetKnown || !mk->operands.empty()) {
                    result.leaveIndirect.push_back(
                        {caller->name, calleeName, "closure target is capturing (not capture-free)"});
                    continue;
                }
                // §3.5.5 — escape/ownership safety on the parameter.
                if (P->ownership == CirOwnership::Owned) {
                    result.leaveIndirect.push_back(
                        {caller->name, calleeName, "closure parameter is owned (drop semantics)"});
                    continue;
                }
                bool escapes = false;
                int sites = invocationMap(F, P, escapes);
                if (escapes) {
                    result.leaveIndirect.push_back(
                        {caller->name, calleeName,
                         "closure parameter escapes (used outside apply.closure)"});
                    continue;
                }
                if (sites < 1) {
                    result.leaveIndirect.push_back(
                        {caller->name, calleeName, "no invocation sites for the closure parameter"});
                    continue;
                }

                // All probes hold -> SPECIALIZE (§3.6.1/§3.6.2).
                CirSpecRequest req;
                req.callerFn = caller->name;
                req.callee = F->name;
                req.typeArgs = inst->typeArgs;
                req.closureParam = P->name;
                req.targetFn = mk->symbol;
                req.invocationSites = sites;
                result.specialize.push_back(std::move(req));
            }
        };

        for (auto& bb : caller->blocks) {
            for (auto& inst : bb->insts) visit(inst);
            visit(bb->terminator);
        }
    }

    return result;
}

std::string CirSpecializationAnalysis::print(const CirAnalysisResult& result) {
    std::ostringstream out;
    for (auto& r : result.specialize) {
        out << "SPECIALIZE " << r.callee;
        if (!r.typeArgs.empty()) {
            out << '<';
            for (size_t i = 0; i < r.typeArgs.size(); ++i) {
                if (i) out << ", ";
                out << r.typeArgs[i];
            }
            out << '>';
        }
        out << "  param " << r.closureParam << " -> " << r.targetFn
            << "  (sites: " << r.invocationSites << ", caller: " << r.callerFn << ")\n";
    }
    for (auto& l : result.leaveIndirect) {
        out << "LEAVE-INDIRECT " << l.callee
            << "  (caller: " << l.callerFn << ")  reason: " << l.reason << "\n";
    }
    return out.str();
}

} // namespace ir
} // namespace cajeta
