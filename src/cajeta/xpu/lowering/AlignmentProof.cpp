#include "AlignmentProof.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <vector>

namespace cajeta::xpu {

namespace {

/// The number of low bits of an address that are provably zero, given that the
/// buffer base `base` is aligned to `1 << baseBits`.
///
/// LLVM's computeKnownBits answers the same question and gives up six
/// operators deep. The offset of a quantized row is deeper than that by
/// construction (`zext(row * (blocks * 144) + block * 144 + 32 * g) + 16` is
/// nine), so the hot loads were exactly the ones it could not see. This solves
/// the same algebra over the whole expression, without the limit: a product
/// adds its operands' zeros, a sum keeps the smaller, a loop-carried value is
/// the smaller of where it starts and what it steps by. It is a descending
/// fixpoint (every value starts at "all zeros" and is only ever lowered), so a
/// value reached through a loop is never credited with zeros its own phi
/// does not end up having. Anything outside the algebra is a leaf answered by
/// computeKnownBits.
class TrailingZeroProof {
public:
    TrailingZeroProof(const llvm::Argument* base, unsigned baseBits,
                      const llvm::DataLayout& dl)
        : base(base), baseBits(baseBits), dl(dl) {}

    unsigned of(llvm::Value* v) {
        collect(v);
        bool changed = true;
        while (changed) {
            changed = false;
            for (llvm::Value* n : order) {
                const unsigned now = transfer(n);
                unsigned& cur = val[n];
                if (now < cur) { cur = now; changed = true; }
            }
        }
        return get(v);
    }

private:
    static constexpr unsigned kAll = 64;
    const llvm::Argument* base;
    unsigned baseBits;
    const llvm::DataLayout& dl;
    llvm::DenseMap<llvm::Value*, unsigned> val;   // interior nodes and leaves
    std::vector<llvm::Value*> order;              // interior nodes only

    static bool interior(llvm::Value* v) {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
            switch (bo->getOpcode()) {
                case llvm::Instruction::Add: case llvm::Instruction::Sub:
                case llvm::Instruction::Or:  case llvm::Instruction::Xor:
                case llvm::Instruction::And: case llvm::Instruction::Mul:
                case llvm::Instruction::Shl:
                    return true;
                default:
                    return false;
            }
        }
        if (auto* ci = llvm::dyn_cast<llvm::CastInst>(v)) {
            return ci->getOpcode() == llvm::Instruction::ZExt
                || ci->getOpcode() == llvm::Instruction::SExt
                || ci->getOpcode() == llvm::Instruction::Trunc;
        }
        if (llvm::isa<llvm::SelectInst>(v) || llvm::isa<llvm::PHINode>(v)) return true;
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
            for (auto gti = llvm::gep_type_begin(gep), e = llvm::gep_type_end(gep);
                 gti != e; ++gti)
                if (gti.isStruct()) return false;
            return true;
        }
        if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(v)) {
            switch (ii->getIntrinsicID()) {
                case llvm::Intrinsic::umin: case llvm::Intrinsic::umax:
                case llvm::Intrinsic::smin: case llvm::Intrinsic::smax:
                    return true;
                default:
                    return false;
            }
        }
        return false;
    }

    void collect(llvm::Value* root) {
        std::vector<llvm::Value*> work{root};
        while (!work.empty()) {
            llvm::Value* v = work.back();
            work.pop_back();
            if (llvm::isa<llvm::ConstantInt>(v) || v == base || val.count(v)) continue;
            if (!interior(v)) {
                val[v] = v->getType()->isIntOrPtrTy()
                    ? std::min<unsigned>(kAll,
                          llvm::computeKnownBits(v, dl).countMinTrailingZeros())
                    : 0;
                continue;
            }
            val[v] = kAll;
            order.push_back(v);
            auto* u = llvm::cast<llvm::User>(v);
            if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v)) {
                work.push_back(sel->getTrueValue());
                work.push_back(sel->getFalseValue());
            } else if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(v)) {
                work.push_back(ii->getArgOperand(0));
                work.push_back(ii->getArgOperand(1));
            } else {
                for (llvm::Value* op : u->operands()) work.push_back(op);
            }
        }
    }

    unsigned get(llvm::Value* v) const {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(v))
            return c->isZero() ? kAll
                               : std::min<unsigned>(kAll, c->getValue().countr_zero());
        if (v == base) return baseBits;
        auto it = val.find(v);
        return it == val.end() ? 0 : it->second;
    }

    unsigned transfer(llvm::Value* v) const {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
            const unsigned l = get(bo->getOperand(0));
            const unsigned r = get(bo->getOperand(1));
            switch (bo->getOpcode()) {
                case llvm::Instruction::And: return std::max(l, r);
                case llvm::Instruction::Mul: return std::min(kAll, l + r);
                case llvm::Instruction::Shl:
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(bo->getOperand(1)))
                        return (unsigned) std::min<uint64_t>(
                            kAll, l + c->getLimitedValue(kAll));
                    return l;
                default: return std::min(l, r);           // add, sub, or, xor
            }
        }
        if (auto* ci = llvm::dyn_cast<llvm::CastInst>(v)) return get(ci->getOperand(0));
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v))
            return std::min(get(sel->getTrueValue()), get(sel->getFalseValue()));
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
            unsigned tz = kAll;
            for (llvm::Value* in : phi->incoming_values()) tz = std::min(tz, get(in));
            return tz;
        }
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
            unsigned tz = get(gep->getPointerOperand());
            for (auto gti = llvm::gep_type_begin(gep), e = llvm::gep_type_end(gep);
                 gti != e; ++gti) {
                const uint64_t scale =
                    gti.getSequentialElementStride(dl).getFixedValue();
                const unsigned scaleTz =
                    scale ? (unsigned) llvm::countr_zero(scale) : kAll;
                tz = std::min(tz, std::min(kAll, get(gti.getOperand()) + scaleTz));
            }
            return tz;
        }
        if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(v))
            return std::min(get(ii->getArgOperand(0)), get(ii->getArgOperand(1)));
        return 0;
    }
};

} // namespace

uint64_t provenAlignment(llvm::Value* addr, const llvm::Argument* base,
                         uint64_t baseAlign, const llvm::DataLayout& dl) {
    const unsigned baseBits =
        baseAlign ? (unsigned) llvm::countr_zero(baseAlign) : 0;
    const unsigned tz = TrailingZeroProof(base, baseBits, dl).of(addr);
    const unsigned cap = (unsigned) llvm::countr_zero(kMaxProvenAlignment);
    return uint64_t(1) << std::min(tz, cap);
}

} // namespace cajeta::xpu
