// quatops - shared host/device quaternion intrinsics over the flat `<4 x T>` (w, x, y, z),
// w the scalar part, all float. dot/length/normalize reuse vecops (a quaternion is a 4-vector).

#pragma once

#include <functional>

#include "VectorOps.h"

namespace cajeta {
namespace quatops {

    // Per-backend transcendental emitter: (name, args) -> value; slerp asks for sin/acos.
    using TrigEmitter =
        std::function<llvm::Value*(const std::string&, llvm::ArrayRef<llvm::Value*>)>;

    inline llvm::Value* w(llvm::IRBuilderBase& b, llvm::Value* q) {
        return vecops::extractLane(b, q, 0u);
    }

    inline llvm::Value* vec3(llvm::IRBuilderBase& b, llvm::Value* q) {
        return b.CreateShuffleVector(q, std::vector<int>{1, 2, 3}, "quat.xyz");
    }

    inline llvm::Value* conjugate(llvm::IRBuilderBase& b, llvm::Value* q) {
        llvm::Type* elemTy =
            llvm::cast<llvm::FixedVectorType>(q->getType())->getElementType();
        llvm::Value* mask = llvm::ConstantVector::get({
            llvm::ConstantFP::get(elemTy, 1.0), llvm::ConstantFP::get(elemTy, -1.0),
            llvm::ConstantFP::get(elemTy, -1.0), llvm::ConstantFP::get(elemTy, -1.0)});
        return b.CreateFMul(q, mask, "quat.conj");
    }

    // Hamilton product a*b - quaternion composition, non-commutative, storage (w, x, y, z).
    inline llvm::Value* multiply(llvm::IRBuilderBase& b, llvm::Value* a,
                                 llvm::Value* c) {
        llvm::Value* aw = vecops::extractLane(b, a, 0u),
                   * ax = vecops::extractLane(b, a, 1u),
                   * ay = vecops::extractLane(b, a, 2u),
                   * az = vecops::extractLane(b, a, 3u);
        llvm::Value* cw = vecops::extractLane(b, c, 0u),
                   * cx = vecops::extractLane(b, c, 1u),
                   * cy = vecops::extractLane(b, c, 2u),
                   * cz = vecops::extractLane(b, c, 3u);
        auto mul = [&](llvm::Value* x, llvm::Value* y) { return b.CreateFMul(x, y); };
        auto add = [&](llvm::Value* x, llvm::Value* y) { return b.CreateFAdd(x, y); };
        auto sub = [&](llvm::Value* x, llvm::Value* y) { return b.CreateFSub(x, y); };
        llvm::Value* rw = sub(sub(sub(mul(aw, cw), mul(ax, cx)), mul(ay, cy)), mul(az, cz));
        llvm::Value* rx = sub(add(add(mul(aw, cx), mul(ax, cw)), mul(ay, cz)), mul(az, cy));
        llvm::Value* ry = add(add(sub(mul(aw, cy), mul(ax, cz)), mul(ay, cw)), mul(az, cx));
        llvm::Value* rz = add(sub(add(mul(aw, cz), mul(ax, cy)), mul(ay, cx)), mul(az, cw));
        llvm::Value* acc = llvm::UndefValue::get(a->getType());
        acc = vecops::insertLane(b, acc, rw, 0u);
        acc = vecops::insertLane(b, acc, rx, 1u);
        acc = vecops::insertLane(b, acc, ry, 2u);
        return vecops::insertLane(b, acc, rz, 3u);
    }

    // rotate(q, v): v rotated by unit q, as v' = v + w*t + qv x t with t = 2*(qv x v).
    inline llvm::Value* rotate(llvm::IRBuilderBase& b, llvm::Value* q,
                               llvm::Value* v) {
        llvm::Value* qv = vec3(b, q);
        llvm::Value* qw = w(b, q);
        llvm::Value* cross1 = vecops::cross(b, qv, v);
        llvm::Value* two = llvm::ConstantFP::get(qw->getType(), 2.0);
        llvm::Value* t = b.CreateFMul(vecops::splat(b, two, 3), cross1, "quat.t");
        llvm::Value* wt = b.CreateFMul(vecops::splat(b, qw, 3), t, "quat.wt");
        llvm::Value* ct = vecops::cross(b, qv, t);
        return b.CreateFAdd(b.CreateFAdd(v, wt), ct, "quat.rot");
    }

    // nlerp(a, b, t) -> normalize(lerp) along the shortest arc; branchless, device-capable.
    inline llvm::Value* nlerp(llvm::IRBuilderBase& b, llvm::Value* a,
                              llvm::Value* c, llvm::Value* t) {
        llvm::Value* d = vecops::dot(b, a, c, /*isFloat=*/true);
        llvm::Value* neg = b.CreateFCmpOLT(
            d, llvm::ConstantFP::get(d->getType(), 0.0), "quat.nlerp.neg");
        llvm::Value* cAdj = b.CreateSelect(neg, b.CreateFNeg(c), c, "quat.nlerp.adj");
        llvm::Value* diff = b.CreateFSub(cAdj, a);
        llvm::Value* scaled = b.CreateFMul(vecops::splat(b, t, 4), diff);
        return vecops::normalize(b, b.CreateFAdd(a, scaled, "quat.nlerp"));
    }

    // slerp(a, c, t) -> spherical linear interpolation (constant angular velocity) along
    // the shortest arc. Branchless: near-parallel inputs select nlerp instead of dividing.
    inline llvm::Value* slerp(llvm::IRBuilderBase& b, llvm::Value* a,
                              llvm::Value* c, llvm::Value* t,
                              const TrigEmitter& trig) {
        llvm::Type* ft =
            llvm::cast<llvm::FixedVectorType>(a->getType())->getElementType();
        llvm::Value* one = llvm::ConstantFP::get(ft, 1.0);
        unsigned lanes = vecops::laneCount(a);
        llvm::Value* d = vecops::dot(b, a, c, /*isFloat=*/true);
        llvm::Value* cAdj = b.CreateSelect(
            b.CreateFCmpOLT(d, llvm::ConstantFP::get(ft, 0.0)),
            b.CreateFNeg(c), c, "quat.slerp.adj");
        llvm::Value* ad = b.CreateBinaryIntrinsic(
            llvm::Intrinsic::minnum,
            b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, d), one);
        llvm::Value* theta = trig("acos", {ad});
        llvm::Value* safeSin = b.CreateBinaryIntrinsic(
            llvm::Intrinsic::maxnum, trig("sin", {theta}),
            llvm::ConstantFP::get(ft, 1e-6));
        llvm::Value* wa = b.CreateFDiv(
            trig("sin", {b.CreateFMul(b.CreateFSub(one, t), theta)}), safeSin);
        llvm::Value* wb = b.CreateFDiv(
            trig("sin", {b.CreateFMul(t, theta)}), safeSin);
        llvm::Value* slerpR = b.CreateFAdd(
            b.CreateFMul(vecops::splat(b, wa, lanes), a),
            b.CreateFMul(vecops::splat(b, wb, lanes), cAdj), "quat.slerp");
        llvm::Value* nlerpR = vecops::normalize(b, b.CreateFAdd(
            a, b.CreateFMul(vecops::splat(b, t, lanes), b.CreateFSub(cAdj, a))));
        return b.CreateSelect(
            b.CreateFCmpOGT(ad, llvm::ConstantFP::get(ft, 0.9995f)),
            nlerpR, slerpR, "quat.slerp.sel");
    }

} // namespace quatops
} // namespace cajeta
