//
// CIR core — enum<->text helpers and node factories. See Cir.h.
//

#include "Cir.h"

namespace cajeta {
namespace ir {

    const char* cirOwnershipWord(CirOwnership o) {
        switch (o) {
            case CirOwnership::Owned:    return "owned";
            case CirOwnership::Borrowed: return "borrowed";
            case CirOwnership::Value:    return "";
        }
        return "";
    }

    const char* cirOpMnemonic(CirOp op) {
        switch (op) {
            case CirOp::ConstInt:     return "const.int";
            case CirOp::ConstFloat:   return "const.float";
            case CirOp::ConstBool:    return "const.bool";
            case CirOp::ConstStr:     return "const.str";
            case CirOp::ConstNull:    return "const.null";
            case CirOp::Add:          return "add";
            case CirOp::Sub:          return "sub";
            case CirOp::Mul:          return "mul";
            case CirOp::Div:          return "div";
            case CirOp::Rem:          return "rem";
            case CirOp::And:          return "and";
            case CirOp::Or:           return "or";
            case CirOp::Xor:          return "xor";
            case CirOp::Shl:          return "shl";
            case CirOp::Shr:          return "shr";
            case CirOp::ICmp:         return "icmp";
            case CirOp::FCmp:         return "fcmp";
            case CirOp::FieldAddr:    return "field.addr";
            case CirOp::LoadField:    return "load.field";
            case CirOp::StoreField:   return "store.field";
            case CirOp::ElemAddr:     return "elem.addr";
            case CirOp::Extract:      return "extract";
            case CirOp::Insert:       return "insert";
            case CirOp::AllocStack:   return "alloc.stack";
            case CirOp::AllocHeap:    return "alloc.heap";
            case CirOp::Load:         return "load";
            case CirOp::Store:        return "store";
            case CirOp::Move:         return "move";
            case CirOp::Drop:         return "drop";
            case CirOp::Call:         return "call";
            case CirOp::CallIndirect: return "call.indirect";
            case CirOp::ApplyClosure: return "apply.closure";
            case CirOp::CallMethod:   return "call.method";
            case CirOp::CallGeneric:  return "call.generic";
            case CirOp::MakeClosure:  return "make.closure";
            case CirOp::Br:           return "br";
            case CirOp::CondBr:       return "cond_br";
            case CirOp::Switch:       return "switch";
            case CirOp::Return:       return "return";
            case CirOp::Unreachable:  return "unreachable";
        }
        return "<?>";
    }

    bool cirIsTerminator(CirOp op) {
        switch (op) {
            case CirOp::Br:
            case CirOp::CondBr:
            case CirOp::Switch:
            case CirOp::Return:
            case CirOp::Unreachable:
                return true;
            default:
                return false;
        }
    }

    CirValuePtr cirValue(std::string name, CirType type, CirOwnership ownership) {
        auto v = std::make_shared<CirValue>();
        v->name = std::move(name);
        v->type = std::move(type);
        v->ownership = ownership;
        return v;
    }

    CirInstPtr cirInst(CirOp op) {
        auto i = std::make_shared<CirInst>();
        i->op = op;
        return i;
    }

} // namespace ir
} // namespace cajeta
