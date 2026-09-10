// Cajeta IR (CIR) — core data structures: a typed SSA CFG between the checked
// AST and LLVM codegen. A program is a set of CirFunctions, each a CFG of
// CirBlocks, each a list of CirInsts ending in a terminator and producing values.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta {
    // Forward-declared only: analysis IR never dereferences a CajetaType.
    class CajetaType;
    using CajetaTypePtr = std::shared_ptr<CajetaType>;
}

namespace cajeta {
namespace ir {

    // By value, a #-owned heap reference owing a drop, or a scoped borrow.
    enum class CirOwnership { Value, Owned, Borrowed };

    // "" for value (printed as absence), "owned" / "borrowed" otherwise.
    const char* cirOwnershipWord(CirOwnership);

    // `resolved` is null until lowered; `spelling` is the round-tripped display.
    struct CirType {
        std::string spelling;
        CajetaTypePtr resolved;

        static CirType named(std::string s) { return CirType{std::move(s), nullptr}; }
        static CirType voidType() { return CirType{"()", nullptr}; }
        bool isVoid() const { return spelling.empty() || spelling == "()"; }
    };

    // An SSA value: an instruction result, a parameter, or a literal.
    struct CirValue {
        std::string name;                              // SSA name, printed with leading '%'
        CirType type;
        CirOwnership ownership = CirOwnership::Value;
    };
    using CirValuePtr = std::shared_ptr<CirValue>;

    CirValuePtr cirValue(std::string name, CirType type,
                         CirOwnership ownership = CirOwnership::Value);

    // The opcode families; CirPrinter.cpp is the single place that decodes their
    // operand payloads.
    enum class CirOp {
        ConstInt, ConstFloat, ConstBool, ConstStr, ConstNull,
        Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr,
        ICmp, FCmp,
        FieldAddr, LoadField, StoreField, ElemAddr, Extract, Insert,
        AllocStack, AllocHeap, Load, Store, Move, Drop,
        Call, CallIndirect, ApplyClosure, CallMethod, CallGeneric,
        MakeClosure,
        Br, CondBr, Switch, Return, Unreachable
    };

    const char* cirOpMnemonic(CirOp);

    bool cirIsTerminator(CirOp);

    struct CirInst;
    using CirInstPtr = std::shared_ptr<CirInst>;

    // An edge out of a terminator; `caseValue` is set only on a switch's cases.
    struct CirSuccessor {
        std::string label;
        std::vector<CirValuePtr> args;
        std::optional<long long> caseValue;
    };

    // One instruction. Operand/payload conventions by family:
    //   const.*        -> result; intConst/floatConst/boolConst/symbol payload
    //   add..shr, icmp/fcmp -> operands=[a, b]; the predicate in `symbol`
    //   field ops      -> operands[0]=object (+[1]=value for store); symbol=field
    //   elem ops       -> operands=[array, index] (+[2]=value for store)
    //   move/drop      -> operands[0]=subject
    //   call, call.generic -> symbol=callee; operands=args; typeArgs
    //   call.indirect / apply.closure -> operands[0]=fnptr|closure; rest=args
    //   call.method    -> operands[0]=receiver; symbol=selector; rest=args
    //   make.closure   -> symbol=target fn; operands=captures; targetKnown set
    //   br/cond_br/switch -> successors (cond_br: [0]=true,[1]=false; operands[0]=cond)
    //   return         -> operands[0]=value (optional)
    struct CirInst {
        CirOp op;
        std::vector<CirValuePtr> operands;
        CirValuePtr result;                 // null for void / store / drop / terminators

        std::string symbol;                 // callee / field / selector / predicate / closure target / const.str
        std::vector<std::string> typeArgs;  // call.generic type arguments
        long long intConst = 0;
        double floatConst = 0;
        bool boolConst = false;
        bool targetKnown = false;           // make.closure: §2.4.1 statically-known unique target

        std::vector<CirSuccessor> successors;
    };

    CirInstPtr cirInst(CirOp op);

    struct CirBlock {
        std::string label;
        std::vector<CirValuePtr> params;     // block params (SSA values passed on entry)
        std::vector<CirInstPtr> insts;       // non-terminator instructions
        CirInstPtr terminator;               // exactly one
    };
    using CirBlockPtr = std::shared_ptr<CirBlock>;

    struct CirFunction {
        std::string name;                        // "Sort.sort"
        std::vector<std::string> genericParams;  // ["T"] — symbolic, unbound until monomorphized
        std::vector<CirValuePtr> params;         // formal params (each with ownership)
        CirType returnType = CirType::voidType();
        CirOwnership returnOwnership = CirOwnership::Value;
        std::vector<CirBlockPtr> blocks;
    };
    using CirFunctionPtr = std::shared_ptr<CirFunction>;

} // namespace ir
} // namespace cajeta
