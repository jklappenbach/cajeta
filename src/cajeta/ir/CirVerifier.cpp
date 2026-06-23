//
// CirVerifier — well-formedness checks. See CirVerifier.h and spec §2.1.4.
//

#include "CirVerifier.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace cajeta {
namespace ir {

std::vector<std::string> CirVerifier::verify(const CirFunction& fn) {
    std::vector<std::string> errors;

    // Block-label lookup (also catches duplicate labels).
    std::unordered_map<std::string, const CirBlock*> byLabel;
    for (auto& bb : fn.blocks) {
        if (byLabel.count(bb->label))
            errors.push_back("duplicate block label '" + bb->label + "'");
        byLabel[bb->label] = bb.get();
    }

    // SSA single-definition: a value is defined by being a function param, a
    // block param, or an instruction result. No value may be defined twice.
    std::unordered_set<const CirValue*> defined;
    auto def = [&](const CirValuePtr& v, const std::string& where) {
        if (!v) return;
        if (!defined.insert(v.get()).second)
            errors.push_back("value %" + v->name + " defined more than once (SSA) at " + where);
    };
    for (auto& p : fn.params) def(p, "fn params");

    for (auto& bb : fn.blocks) {
        // Entry-block params alias the function params (same SSA values), so
        // only count block params as fresh definitions for non-entry blocks.
        bool isEntry = !fn.blocks.empty() && bb.get() == fn.blocks.front().get();
        if (!isEntry)
            for (auto& bp : bb->params) def(bp, "block " + bb->label + " params");

        for (auto& inst : bb->insts) {
            if (inst && cirIsTerminator(inst->op))
                errors.push_back("terminator '" + std::string(cirOpMnemonic(inst->op)) +
                                 "' in the middle of block " + bb->label);
            if (inst) def(inst->result, "block " + bb->label);
        }

        // Exactly one terminator, at the end.
        if (!bb->terminator) {
            errors.push_back("block " + bb->label + " has no terminator");
            continue;
        }
        if (!cirIsTerminator(bb->terminator->op))
            errors.push_back("block " + bb->label + " terminator is non-terminator op '" +
                             cirOpMnemonic(bb->terminator->op) + "'");
        def(bb->terminator->result, "block " + bb->label + " terminator");

        // Branch targets: arity + type-spelling match against the target's
        // block parameters.
        for (auto& s : bb->terminator->successors) {
            auto it = byLabel.find(s.label);
            if (it == byLabel.end()) {
                errors.push_back("block " + bb->label + " branches to unknown label '" +
                                 s.label + "'");
                continue;
            }
            const CirBlock* target = it->second;
            if (target->params.size() != s.args.size()) {
                std::ostringstream m;
                m << "branch " << bb->label << " -> " << s.label
                  << " block-param arity mismatch: passes " << s.args.size()
                  << ", target expects " << target->params.size();
                errors.push_back(m.str());
                continue;
            }
            for (size_t i = 0; i < s.args.size(); ++i) {
                const auto& got = s.args[i];
                const auto& want = target->params[i];
                if (got && want && got->type.spelling != want->type.spelling) {
                    std::ostringstream m;
                    m << "branch " << bb->label << " -> " << s.label << " param " << i
                      << " type mismatch: passes " << got->type.spelling
                      << ", target expects " << want->type.spelling;
                    errors.push_back(m.str());
                }
            }
        }
    }

    return errors;
}

} // namespace ir
} // namespace cajeta
