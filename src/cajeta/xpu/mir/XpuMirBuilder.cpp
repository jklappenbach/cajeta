//
// XpuMirBuilder — see header for shape and rationale.
//

#include "XpuMirBuilder.h"

#include "../core/XpuAttributes.h"
#include "../core/AddressSpace.h"

#include "../../method/Method.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaType.h"
#include "../../type/FormalParameter.h"
#include "../../compile/CajetaModule.h"

#include "../../asn/AbstractSyntaxNode.h"
#include "../../asn/Block.h"
#include "../../asn/Statement.h"                     // Return/If/Expression/…Statement
#include "../../asn/LocalVariableDeclaration.h"
#include "../../asn/VariableDeclarator.h"
#include "../../asn/expression/Expression.h"        // ArrayLiteralExpression
#include "../../asn/expression/Identifier.h"        // IdentifierExpression
#include "../../asn/expression/MethodCallExpression.h"
#include "../../asn/expression/CallExpression.h"

#include <functional>

namespace cajeta {
namespace xpu {
namespace mir {

namespace {

// Address-space qualifier for a parameter type. Recognized when the
// type's canonical name maps to one of cajeta.xpu.{Global,
// Shared, Constant, Private, Generic}. Anything else defaults to
// Generic — including primitives and Buffer<T>, which carry their
// own backend lowering at codegen time.
AddressSpace addressSpaceForType(const CajetaTypePtr& type) {
    if (!type) return AddressSpace::Generic;
    AddressSpace as;
    if (isAddressSpaceCanonical(type->toCanonical(), as)) return as;
    return AddressSpace::Generic;
}

// Build the parameter list, skipping `this` for instance methods
// (defensive — @Kernel methods are required to be static, but the
// validator that enforces that lands later; v1 just drops `this`
// if it slipped in).
std::vector<XpuKernelParam>
buildParams(const MethodPtr& method) {
    std::vector<XpuKernelParam> out;
    out.reserve(method->getParameterList().size());
    for (auto& fp : method->getParameterList()) {
        if (!fp) continue;
        if (fp->getName() == "this") continue;
        XpuKernelParam p;
        p.name = fp->getName();
        p.type = XpuMirType(fp->getType(), addressSpaceForType(fp->getType()));
        out.push_back(std::move(p));
    }
    return out;
}

// The receiver of `Recv.method(...)` is children[0] of the
// MethodCallExpression (the DOT branch attaches the lhs there). When
// it's a bare identifier we can match it syntactically — the same
// shape the Math.* intrinsic block uses (MethodCallExpression.cpp).
std::string receiverIdentifier(MethodCallExpression* mc) {
    const auto& kids = mc->getChildren();
    if (kids.empty()) return {};
    if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(kids[0])) {
        return id->getTextValue();
    }
    return {};
}

// Map a recognized leaf builtin call (`Thread.x()`, `Workgroup.dimY()`,
// `Barrier.workgroup()`, …) to an XpuMirOp and append it. Unrecognized
// receiver/name pairs are ignored. globalIdX/Y/Z record the LEAF
// ThreadId axis only — the device pass composes ctaid*ntid+tid.
void emitBuiltinOp(const std::string& recv, const std::string& name,
                   std::vector<XpuMirOpPtr>& out) {
    auto axisOf = [](char c) {
        return c == 'X' || c == 'x' ? Axis::X
             : c == 'Y' || c == 'y' ? Axis::Y
             : Axis::Z;
    };
    auto push = [&](OpKind k, Axis a) {
        auto op = std::make_shared<XpuMirOp>(k);
        op->axis = a;
        out.push_back(std::move(op));
    };

    if (recv == "KernelThread") {
        if (name == "x" || name == "y" || name == "z") {
            push(OpKind::ThreadId, axisOf(name[0]));
        } else if (name == "globalIdX") {
            push(OpKind::ThreadId, Axis::X);
        } else if (name == "globalIdY") {
            push(OpKind::ThreadId, Axis::Y);
        } else if (name == "globalIdZ") {
            push(OpKind::ThreadId, Axis::Z);
        }
    } else if (recv == "Workgroup") {
        if (name == "x" || name == "y" || name == "z") {
            push(OpKind::WorkgroupId, axisOf(name[0]));
        } else if (name == "dimX") {
            push(OpKind::WorkgroupDim, Axis::X);
        } else if (name == "dimY") {
            push(OpKind::WorkgroupDim, Axis::Y);
        } else if (name == "dimZ") {
            push(OpKind::WorkgroupDim, Axis::Z);
        }
    } else if (recv == "Barrier") {
        if (name == "workgroup")  push(OpKind::BarrierWorkgroup, Axis::X);
        else if (name == "wave")  push(OpKind::BarrierWave, Axis::X);
    }
}

// Visit every node reachable from `node`, calling `fn` once per node.
void forEachNode(const AbstractSyntaxNodePtr& node,
                 const std::function<void(const AbstractSyntaxNodePtr&)>& fn) {
    if (!node) return;
    fn(node);

    // `forEachSubNode` is the canonical analysis-descent primitive: it visits
    // the payloads statements/calls hide in private fields (if/loop/try/switch
    // bodies, return/throw expressions, ctor and method-call arguments) AND the
    // codegen children. A plain getChildren() walk stops dead at any loop/try/
    // switch body, so the launch-site and builtin scans must use this.
    // Two forms hold a sub-node in a private field with no forEachSubNode
    // override, so the primitive can't reach them: a CallExpression's args, and
    // a VariableDeclarator's initializer (`uint32 i = KernelThread.x()` — the
    // builtin read lives here). Visit those explicitly.
    if (auto ce = std::dynamic_pointer_cast<CallExpression>(node)) {
        for (auto& a : ce->getArgs()) forEachNode(a.expression, fn);
    } else if (auto vd = std::dynamic_pointer_cast<VariableDeclarator>(node)) {
        forEachNode(vd->getInitializer(), fn);
    }
    node->forEachSubNode([&](const AbstractSyntaxNodePtr& sub) { forEachNode(sub, fn); });
}

// `"grid:"` → `"grid"`. Parameter labels keep their trailing colon
// (matching MethodCallExpression); strip it before comparing.
std::string stripLabelColon(const std::string& label) {
    if (!label.empty() && label.back() == ':') {
        return label.substr(0, label.size() - 1);
    }
    return label;
}

// Flatten a launch dimension argument into its element expressions.
// The common form is an array literal (`[256]`, `[(n+255)/256]`); a
// bare scalar is treated as a single-element 1-D dimension.
std::vector<ExpressionPtr> dimElements(const ExpressionPtr& dimExpr) {
    std::vector<ExpressionPtr> out;
    if (auto arr = std::dynamic_pointer_cast<ArrayLiteralExpression>(dimExpr)) {
        for (auto& e : arr->getElements()) out.push_back(e);
    } else if (dimExpr) {
        out.push_back(dimExpr);
    }
    return out;
}

// Build a launch-site record from a `kernel.launch(cfg)(args)`
// CallExpression. Returns nullptr if the callee isn't a `launch`
// method call. `kernels` is the already-built kernel list, used to
// resolve the receiver's simple name to a kernel canonical.
XpuMirLaunchSitePtr buildLaunchSite(
        const std::shared_ptr<CallExpression>& call,
        const std::vector<XpuMirKernelPtr>& kernels) {
    if (!call) return nullptr;
    auto callee = std::dynamic_pointer_cast<MethodCallExpression>(call->getCallee());
    if (!callee || callee->getMethodCallName() != "launch") return nullptr;

    auto site = std::make_shared<XpuMirLaunchSite>();

    // Kernel name: the receiver of `<kernel>.launch(...)`.
    std::string recv = receiverIdentifier(callee.get());
    site->kernelCanonicalName = recv;   // fallback: bare name
    for (auto& k : kernels) {
        if (!k) continue;
        const std::string& cn = k->canonicalName;
        if (cn == recv ||
            (cn.size() > recv.size() &&
             cn.compare(cn.size() - recv.size() - 1, recv.size() + 1,
                        "." + recv) == 0)) {
            site->kernelCanonicalName = cn;
            break;
        }
    }

    // launch() config: first positional arg is the stream; labeled
    // `grid:` / `block:` carry the dimension array literals.
    for (auto& p : callee->getParameters()) {
        std::string label = stripLabelColon(p.label);
        if (label == "grid") {
            site->grid = dimElements(p.expression);
        } else if (label == "block") {
            site->block = dimElements(p.expression);
        } else if (label.empty() && !site->stream) {
            site->stream = p.expression;
        }
    }

    // Trailing (args): the kernel arguments.
    for (auto& a : call->getArgs()) site->kernelArgs.push_back(a.expression);

    return site;
}

} // namespace

void XpuMirBuilder::collectBodyOps(const MethodPtr& method,
                                   std::vector<XpuMirOpPtr>& out) {
    if (!method) return;
    forEachNode(method->getBlock(), [&](const AbstractSyntaxNodePtr& n) {
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(n)) {
            emitBuiltinOp(receiverIdentifier(mc.get()),
                          mc->getMethodCallName(), out);
        }
    });
}

XpuMirKernelPtr XpuMirBuilder::buildKernelForMethod(const MethodPtr& method) {
    if (!method) return nullptr;
    if (!isKernel(*method)) return nullptr;

    auto k = std::make_shared<XpuMirKernel>();
    k->method = method;
    k->params = buildParams(method);

    // Compose the canonical name as `pkg.Class.method`. The Method
    // doesn't directly carry the full canonical (it has a separate
    // signature-mangled form), so we synthesize it from the parent
    // class's canonical plus the method short name.
    std::string parentCanonical;
    if (method->getParent()) {
        parentCanonical = method->getParent()->toCanonical();
    }
    k->canonicalName = parentCanonical.empty()
        ? method->getName()
        : (parentCanonical + "." + method->getName());

    // Pull @Wave / @Backend values via the typed view from step 1.
    if (auto attr = XpuKernelAttr::from(*method)) {
        k->waveWidth = attr->waveWidth();
        k->backends  = attr->backends();
    }

    // Leaf-builtin walk: populate bodyOps with the Thread / Workgroup /
    // Barrier reads the device lowering must resolve to nvvm intrinsics.
    collectBodyOps(method, k->bodyOps);
    return k;
}

XpuMirModulePtr XpuMirBuilder::buildForModule(const CajetaModulePtr& module) {
    auto m = std::make_shared<XpuMirModule>();
    if (!module) return m;
    for (auto& method : module->getAllMethods()) {
        if (auto k = buildKernelForMethod(method)) {
            m->kernels.push_back(std::move(k));
        }
    }
    // Second pass — now that every kernel canonical is known, scan all
    // method bodies for host-side launch sites and resolve their target
    // kernels against the kernel list.
    for (auto& method : module->getAllMethods()) {
        if (!method) continue;
        forEachNode(method->getBlock(), [&](const AbstractSyntaxNodePtr& n) {
            if (auto ce = std::dynamic_pointer_cast<CallExpression>(n)) {
                if (auto site = buildLaunchSite(ce, m->kernels)) {
                    m->launchSites.push_back(site);
                }
            }
        });
    }
    return m;
}

} // namespace mir
} // namespace xpu
} // namespace cajeta
