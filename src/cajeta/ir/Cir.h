//
// Cajeta IR (CIR) — core data structures.
//
// CIR is a typed SSA CFG sitting between the type-checked / borrow-checked
// AST and LLVM codegen (the MIR/SIL analog). See docs/specs/cajeta-ir-spec.md
// §2. Phase A is analysis-only: CIR is built for a closed slice (a generic
// function with a function-typed parameter, its callers, and the closures
// passed) and probed for specializable closure calls; it does not replace
// codegen. The shape here mirrors the sibling XpuMir containers: thin structs
// held by shared_ptr, a static printer, glob-built.
//
// A program in CIR is a set of CirFunctions; each function is a CFG of
// CirBlocks; each block is a list of CirInsts ending in a terminator;
// instructions produce CirValues; values carry a CIR type + an ownership kind.
//

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta {
    // Forward declaration only — CIR types ARE CajetaTypes (spec §2.2.1), but
    // Phase-A hand-built / analysis IR never dereferences the pointer, so we
    // avoid pulling in the type registry here. `CirType::spelling` is the
    // authoritative display form; `resolved` is populated once lowered from a
    // real AST node (Unit 2+).
    class CajetaType;
    using CajetaTypePtr = std::shared_ptr<CajetaType>;
}

namespace cajeta {
namespace ir {

    // ---- Types & ownership (spec §2.2) -------------------------------------

    // Storage/ownership kind of a value. `value` = value-type/primitive held by
    // value; `owned` = a #-owned heap reference responsible for its drop;
    // `borrowed` = a non-owning reference bounded by a scope. This is the
    // information drop-elision / move-opt need and LLVM lacks.
    enum class CirOwnership { Value, Owned, Borrowed };

    // "" for value (printed as absence), "owned" / "borrowed" otherwise.
    const char* cirOwnershipWord(CirOwnership);

    // A CIR type. `spelling` is the human-readable display form used by the
    // dumper and round-tripped by tests ("i32", "T[]", "(T,T)->i32", "()").
    // `resolved` is the authoritative CajetaType once lowered from the AST
    // (nullable: generic params `T` and hand-built IR carry only a spelling).
    struct CirType {
        std::string spelling;
        CajetaTypePtr resolved;

        static CirType named(std::string s) { return CirType{std::move(s), nullptr}; }
        static CirType voidType() { return CirType{"()", nullptr}; }
        bool isVoid() const { return spelling.empty() || spelling == "()"; }
    };

    // ---- Values (spec §2.1.3) ----------------------------------------------

    // An SSA value (defined once): an instruction result, a block parameter, a
    // function parameter, or a literal. Every value exposes its CIR type and
    // ownership kind for analyses.
    struct CirValue {
        std::string name;                              // SSA name, printed with leading '%'
        CirType type;
        CirOwnership ownership = CirOwnership::Value;
    };
    using CirValuePtr = std::shared_ptr<CirValue>;

    CirValuePtr cirValue(std::string name, CirType type,
                         CirOwnership ownership = CirOwnership::Value);

    // ---- Instructions (spec §2.3) ------------------------------------------

    // Opcode families. A Phase-A subset is actually lowered (Unit 2); the full
    // set is the target. Operand/payload conventions are documented per family
    // in CirPrinter.cpp (the single place that decodes them).
    enum class CirOp {
        // 2.3.1 constants
        ConstInt, ConstFloat, ConstBool, ConstStr, ConstNull,
        // 2.3.2 arithmetic / logic
        Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr,
        // 2.3.2 compare (predicate carried in `symbol`)
        ICmp, FCmp,
        // 2.3.3 aggregates
        FieldAddr, LoadField, StoreField, ElemAddr, Extract, Insert,
        // 2.3.4 memory & ownership
        AllocStack, AllocHeap, Load, Store, Move, Drop,
        // 2.3.5 calls
        Call, CallIndirect, ApplyClosure, CallMethod, CallGeneric,
        // 2.3.6 closures
        MakeClosure,
        // 2.3.7 terminators
        Br, CondBr, Switch, Return, Unreachable
    };

    // Mnemonic for the dumper / tests ("add", "apply.closure", "make.closure").
    const char* cirOpMnemonic(CirOp);

    // Is this opcode a block terminator?
    bool cirIsTerminator(CirOp);

    struct CirInst;
    using CirInstPtr = std::shared_ptr<CirInst>;

    // A control-flow edge out of a terminator: target block label, the SSA
    // values passed as that block's parameters, and (switch only) the case
    // value (nullopt = the default edge or a non-switch edge).
    struct CirSuccessor {
        std::string label;
        std::vector<CirValuePtr> args;
        std::optional<long long> caseValue;
    };

    // One instruction. Operand/payload conventions by family:
    //   const.*        -> result; intConst/floatConst/boolConst/symbol payload
    //   add..shr,icmp  -> operands = [a, b]; icmp/fcmp predicate in `symbol`
    //   field ops      -> operands[0]=object (+[1]=value for store); symbol=field
    //   elem ops       -> operands=[array, index] (+[2]=value for store)
    //   move/drop      -> operands[0]=subject
    //   call           -> symbol=callee; operands=args
    //   call.indirect  -> operands[0]=fnptr; rest=args
    //   apply.closure  -> operands[0]=closure; rest=args
    //   call.method    -> operands[0]=receiver; symbol=selector; rest=args
    //   call.generic   -> symbol=callee; typeArgs; operands=args
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

    // ---- Blocks (spec §2.1.2) ----------------------------------------------

    struct CirBlock {
        std::string label;
        std::vector<CirValuePtr> params;     // block params (SSA values passed on entry)
        std::vector<CirInstPtr> insts;       // non-terminator instructions
        CirInstPtr terminator;               // exactly one
    };
    using CirBlockPtr = std::shared_ptr<CirBlock>;

    // ---- Functions (spec §2.1.1) -------------------------------------------

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
