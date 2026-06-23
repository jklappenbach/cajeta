//
// CirPrinter — textual dump of a CirFunction. See CirPrinter.h and spec §2.5.
//

#include "CirPrinter.h"

#include <sstream>

namespace cajeta {
namespace ir {

namespace {

    // "%name" reference.
    std::string vref(const CirValuePtr& v) {
        return v ? ("%" + v->name) : "%<null>";
    }

    // Comma-joined "%a, %b, ...".
    std::string refList(const std::vector<CirValuePtr>& vs, size_t from = 0) {
        std::string s;
        for (size_t i = from; i < vs.size(); ++i) {
            if (i > from) s += ", ";
            s += vref(vs[i]);
        }
        return s;
    }

    // " : <type>" suffix for an instruction that produces a result.
    void printResultType(std::ostringstream& out, const CirInstPtr& inst) {
        if (inst->result) out << " : " << inst->result->type.spelling;
    }

    // A function/block parameter declaration: "%name: <type>[ owned|borrowed]".
    void printParamDecl(std::ostringstream& out, const CirValuePtr& v) {
        out << vref(v) << ": " << v->type.spelling;
        const char* own = cirOwnershipWord(v->ownership);
        if (own[0] != '\0') out << ' ' << own;
    }

    // One control-flow edge: "label" or "label(%a, %b)"; switch cases prefix
    // the case value: "7 -> label".
    void printSuccessor(std::ostringstream& out, const CirSuccessor& s) {
        if (s.caseValue.has_value()) out << *s.caseValue << " -> ";
        out << s.label;
        if (!s.args.empty()) out << '(' << refList(s.args) << ')';
    }

    void printInst(std::ostringstream& out, const CirInstPtr& inst) {
        out << "    ";
        if (inst->result) out << vref(inst->result) << " = ";

        const char* m = cirOpMnemonic(inst->op);
        switch (inst->op) {
            case CirOp::ConstInt:
                out << m << ' ' << inst->intConst;
                break;
            case CirOp::ConstFloat:
                out << m << ' ' << inst->floatConst;
                break;
            case CirOp::ConstBool:
                out << m << ' ' << (inst->boolConst ? "true" : "false");
                break;
            case CirOp::ConstStr:
                out << m << " \"" << inst->symbol << '"';
                break;
            case CirOp::ConstNull:
                out << m;
                break;

            case CirOp::ICmp:
            case CirOp::FCmp:
                // predicate carried in symbol: `icmp.slt %a, %b`
                out << m;
                if (!inst->symbol.empty()) out << '.' << inst->symbol;
                out << ' ' << refList(inst->operands);
                break;

            case CirOp::LoadField:
            case CirOp::FieldAddr:
                out << m << ' ' << vref(inst->operands.empty() ? nullptr : inst->operands[0])
                    << ", " << inst->symbol;
                break;
            case CirOp::StoreField:
                out << m << ' ' << vref(inst->operands.size() > 1 ? inst->operands[1] : nullptr)
                    << ", " << vref(inst->operands.empty() ? nullptr : inst->operands[0])
                    << ", " << inst->symbol;
                break;

            case CirOp::Call:
                out << m << ' ' << inst->symbol << '(' << refList(inst->operands) << ')';
                break;
            case CirOp::CallGeneric: {
                out << m << ' ' << inst->symbol;
                if (!inst->typeArgs.empty()) {
                    out << '<';
                    for (size_t i = 0; i < inst->typeArgs.size(); ++i) {
                        if (i) out << ", ";
                        out << inst->typeArgs[i];
                    }
                    out << '>';
                }
                out << '(' << refList(inst->operands) << ')';
                break;
            }
            case CirOp::CallIndirect:
                out << m << ' ' << vref(inst->operands.empty() ? nullptr : inst->operands[0])
                    << '(' << refList(inst->operands, 1) << ')';
                break;
            case CirOp::ApplyClosure:
                // apply.closure %cmp(%x, %y)
                out << m << ' ' << vref(inst->operands.empty() ? nullptr : inst->operands[0])
                    << '(' << refList(inst->operands, 1) << ')';
                break;
            case CirOp::CallMethod:
                out << m << ' ' << vref(inst->operands.empty() ? nullptr : inst->operands[0])
                    << ", " << inst->symbol << '(' << refList(inst->operands, 1) << ')';
                break;

            case CirOp::MakeClosure:
                // make.closure cmpNat, [%cap0, ...]
                out << m << ' ' << inst->symbol << ", [" << refList(inst->operands) << ']';
                break;

            case CirOp::Br:
                out << m << ' ';
                if (!inst->successors.empty()) printSuccessor(out, inst->successors[0]);
                break;
            case CirOp::CondBr:
                out << m << ' ' << vref(inst->operands.empty() ? nullptr : inst->operands[0]);
                for (auto& s : inst->successors) { out << ", "; printSuccessor(out, s); }
                break;
            case CirOp::Switch:
                out << m << ' ' << vref(inst->operands.empty() ? nullptr : inst->operands[0]);
                out << ", [";
                for (size_t i = 0; i < inst->successors.size(); ++i) {
                    if (i) out << ", ";
                    printSuccessor(out, inst->successors[i]);
                }
                out << ']';
                break;
            case CirOp::Return:
                out << m;
                if (!inst->operands.empty()) out << ' ' << vref(inst->operands[0]);
                break;
            case CirOp::Unreachable:
                out << m;
                break;

            default:
                // generic form: `<mnemonic> %a, %b`
                out << m;
                if (!inst->operands.empty()) out << ' ' << refList(inst->operands);
                break;
        }

        printResultType(out, inst);
        if (inst->op == CirOp::MakeClosure && inst->targetKnown) out << "   ; known";
        out << '\n';
    }

} // namespace

std::string CirPrinter::print(const CirFunction& fn) {
    std::ostringstream out;

    out << "fn " << fn.name;
    if (!fn.genericParams.empty()) {
        out << '<';
        for (size_t i = 0; i < fn.genericParams.size(); ++i) {
            if (i) out << ", ";
            out << fn.genericParams[i];
        }
        out << '>';
    }
    out << '(';
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out << ", ";
        printParamDecl(out, fn.params[i]);
    }
    out << ") -> ";
    if (fn.returnType.isVoid()) {
        out << "()";
    } else {
        out << fn.returnType.spelling;
        const char* own = cirOwnershipWord(fn.returnOwnership);
        if (own[0] != '\0') out << ' ' << own;
    }
    out << " {\n";

    for (auto& bb : fn.blocks) {
        out << bb->label;
        if (!bb->params.empty()) out << '(' << refList(bb->params) << ')';
        out << ":\n";
        for (auto& inst : bb->insts) printInst(out, inst);
        if (bb->terminator) printInst(out, bb->terminator);
    }

    out << "}\n";
    return out.str();
}

} // namespace ir
} // namespace cajeta
