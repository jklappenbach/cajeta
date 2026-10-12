// KernelCost - a symbolic count over the kernel body, per group or per work item.
#include "KernelCost.h"
#include "XpuAttributes.h"

#include "cajeta/asn/AbstractSyntaxNode.h"
#include "cajeta/asn/Block.h"
#include "cajeta/asn/Statement.h"
#include "cajeta/asn/LocalVariableDeclaration.h"
#include "cajeta/asn/VariableDeclarator.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/asn/expression/MethodCallExpression.h"
#include "cajeta/asn/expression/BinaryOpExpression.h"
#include "cajeta/asn/expression/LiteralExpression.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaConstantType.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaVector.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/type/StructureProperty.h"

#include <cctype>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <set>

namespace cajeta {
namespace xpu {

namespace {

    const char* kWidth = "$W";

    struct Rat {
        int64_t n = 0;
        int64_t d = 1;
        static Rat of(int64_t n, int64_t d = 1) {
            if (d < 0) { n = -n; d = -d; }
            int64_t g = std::gcd(n < 0 ? -n : n, d);
            if (g > 1) { n /= g; d /= g; }
            return {n, d};
        }
        Rat operator+(const Rat& o) const { return of(n * o.d + o.n * d, d * o.d); }
        Rat operator*(const Rat& o) const { return of(n * o.n, d * o.d); }
        bool zero() const { return n == 0; }
    };

    // A polynomial over parameter names with rational coefficients; a monomial is a sorted name list.
    struct Poly {
        std::map<std::vector<std::string>, Rat> t;

        static Poly k(int64_t v, int64_t d = 1) {
            Poly p;
            if (v) p.t[{}] = Rat::of(v, d);
            return p;
        }
        static Poly sym(const std::string& s) {
            Poly p;
            p.t[{s}] = Rat::of(1);
            return p;
        }
        Poly operator+(const Poly& o) const {
            Poly r = *this;
            for (auto& [m, c] : o.t) {
                Rat s = r.t.count(m) ? r.t[m] + c : c;
                if (s.zero()) r.t.erase(m); else r.t[m] = s;
            }
            return r;
        }
        Poly scaled(const Rat& c) const {
            Poly r;
            if (c.zero()) return r;
            for (auto& [m, v] : t) r.t[m] = v * c;
            return r;
        }
        Poly operator-(const Poly& o) const { return *this + o.scaled(Rat::of(-1)); }
        Poly operator*(const Poly& o) const {
            Poly r;
            for (auto& [ma, ca] : t)
                for (auto& [mb, cb] : o.t) {
                    std::vector<std::string> m = ma;
                    m.insert(m.end(), mb.begin(), mb.end());
                    std::sort(m.begin(), m.end());
                    r = r + Poly{{{m, ca * cb}}};
                }
            return r;
        }
        std::optional<int64_t> constant() const {
            if (t.empty()) return 0;
            if (t.size() == 1 && t.begin()->first.empty() && t.begin()->second.d == 1)
                return t.begin()->second.n;
            return std::nullopt;
        }
        // Divides out one group width from every term: a lane count summed to a group count.
        std::optional<Poly> perGroup() const {
            Poly r;
            for (auto& [m, c] : t) {
                auto it = std::find(m.begin(), m.end(), kWidth);
                if (it == m.end()) return std::nullopt;
                std::vector<std::string> mm = m;
                mm.erase(mm.begin() + (it - m.begin()));
                r.t[mm] = c;
            }
            return r;
        }
        bool mentions(const std::string& s) const {
            for (auto& [m, c] : t)
                if (std::find(m.begin(), m.end(), s) != m.end()) return true;
            return false;
        }
        Poly withWidth(unsigned w) const {
            Poly r;
            for (auto& [m, c] : t) {
                std::vector<std::string> mm;
                int64_t f = 1;
                for (auto& s : m) { if (s == kWidth) f *= w; else mm.push_back(s); }
                r = r + Poly{{{mm, c * Rat::of(f)}}};
            }
            return r;
        }
        std::string str() const {
            if (t.empty()) return "0";
            std::string out;
            auto emit = [&](const std::vector<std::string>& m, const Rat& c) {
                int64_t n = c.n;
                if (n < 0) { out += "-"; n = -n; }
                else if (!out.empty()) out += "+";
                std::string body;
                if (n != 1 || m.empty()) body = std::to_string(n);
                for (auto& s : m) body += (body.empty() ? "" : "*") + s;
                if (c.d != 1) body += "/" + std::to_string(c.d);
                out += body;
            };
            for (auto& [m, c] : t) if (!m.empty()) emit(m, c);
            if (auto it = t.find({}); it != t.end()) emit(it->first, it->second);
            return out;
        }
    };

    struct Count {
        Poly p;
        bool measured = false;
        void add(const Poly& amount, bool unknown) {
            if (unknown) measured = true; else p = p + amount;
        }
    };

    // A multiplier: how many times the current statement runs, per group or per item.
    struct Mult {
        Poly p;
        bool measured = false;
        Mult times(const std::optional<Poly>& q) const {
            if (measured || !q) return {Poly(), true};
            return {p * *q, false};
        }
        Mult perGroup() const {
            if (measured) return *this;
            auto g = p.perGroup();
            if (!g) return {Poly(), true};
            return {*g, false};
        }
    };

    // A value's type as far as counting needs it.
    struct Ty {
        enum Kind { Unknown, Int, Float, Bool, Buffer, Tile, Other } kind = Unknown;
        unsigned bits = 0;
        unsigned lanes = 1;
        unsigned rows = 0, cols = 0;
        unsigned elemBits = 0;    // a buffer's or tile's element
        bool elemFloat = false;
    };

    unsigned primitiveBits(const std::string& n, bool* isFloat) {
        static const std::map<std::string, std::pair<unsigned, bool>> table = {
            {"int8", {8, false}}, {"uint8", {8, false}}, {"int16", {16, false}},
            {"uint16", {16, false}}, {"int32", {32, false}}, {"uint32", {32, false}},
            {"int64", {64, false}}, {"uint64", {64, false}}, {"boolean", {8, false}},
            {"float16", {16, true}}, {"bfloat16", {16, true}}, {"float32", {32, true}},
            {"float64", {64, true}}};
        auto it = table.find(n);
        if (it == table.end()) return 0;
        if (isFloat) *isFloat = it->second.second;
        return it->second.first;
    }

    std::vector<std::string> templateArgs(const std::string& canon) {
        std::vector<std::string> out;
        size_t lt = canon.find('<');
        if (lt == std::string::npos || canon.back() != '>') return out;
        int depth = 0;
        std::string cur;
        for (size_t i = lt + 1; i + 1 < canon.size(); ++i) {
            char c = canon[i];
            if (c == '<') depth++;
            if (c == '>') depth--;
            if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
            if (c != ' ') cur += c;
        }
        out.push_back(cur);
        return out;
    }

    std::string simpleName(const std::string& qualified) {
        size_t dot = qualified.find_last_of('.');
        return dot == std::string::npos ? qualified : qualified.substr(dot + 1);
    }

    Ty typeOf(const CajetaTypePtr& t) {
        Ty r;
        if (!t) return r;
        if (auto v = std::dynamic_pointer_cast<CajetaVector>(t)) {
            bool f = false;
            unsigned b = primitiveBits(v->getElementType() ? v->getElementType()->toCanonical() : "", &f);
            r.kind = f ? Ty::Float : Ty::Int;
            r.bits = b;
            r.lanes = v->getLanes();
            return r;
        }
        std::string canon = t->toCanonical();
        bool f = false;
        if (unsigned b = primitiveBits(canon, &f)) {
            r.kind = canon == "boolean" ? Ty::Bool : (f ? Ty::Float : Ty::Int);
            r.bits = b;
            return r;
        }
        std::string base = simpleName(canon.substr(0, canon.find('<')));
        auto args = templateArgs(canon);
        if (base == "KernelBuffer" && args.size() == 1) {
            r.kind = Ty::Buffer;
            r.elemBits = primitiveBits(simpleName(args[0]), &r.elemFloat);
            return r;
        }
        if ((base == "Tile" || base == "CooperativeMatrix") && args.size() >= 3) {
            r.kind = Ty::Tile;
            r.elemBits = primitiveBits(simpleName(args[0]), &r.elemFloat);
            r.rows = (unsigned) std::strtoul(args[1].c_str(), nullptr, 10);
            r.cols = (unsigned) std::strtoul(args[2].c_str(), nullptr, 10);
            return r;
        }
        r.kind = Ty::Other;
        return r;
    }

    std::optional<int64_t> integerLiteral(const std::shared_ptr<IntegerLiteralExpression>& il) {
        std::string s;
        for (char c : il->getRawValue()) if (c != '_' && c != 'L' && c != 'l') s += c;
        int base = 10;
        switch (il->getIntegerLiteralType()) {
            case INTEGER_LITERAL_TYPE_BINARY: base = 2; s = s.substr(2); break;
            case INTEGER_LITERAL_TYPE_HEX: base = 16; s = s.substr(2); break;
            case INTEGER_LITERAL_TYPE_OCT: base = 8; break;
            default: break;
        }
        if (s.empty()) return std::nullopt;
        char* end = nullptr;
        unsigned long long v = std::strtoull(s.c_str(), &end, base);
        if (!end || *end) return std::nullopt;
        return (int64_t) v;
    }

    ExpressionPtr child(const AbstractSyntaxNodePtr& n, size_t i) {
        if (!n || n->getChildren().size() <= i) return nullptr;
        return std::dynamic_pointer_cast<Expression>(n->getChildren()[i]);
    }

    ExpressionPtr stripCasts(ExpressionPtr e) {
        for (;;) {
            if (auto c = std::dynamic_pointer_cast<CastExpression>(e)) e = child(c, 0);
            else if (auto mv = std::dynamic_pointer_cast<MoveExpression>(e)) e = child(mv, 0);
            else return e;
        }
    }

    std::string receiverOf(const std::shared_ptr<MethodCallExpression>& mc) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(child(mc, 0)))
            return id->getTextValue();
        return {};
    }

    bool isCall(const ExpressionPtr& e, const char* recv, const char* name) {
        auto mc = std::dynamic_pointer_cast<MethodCallExpression>(stripCasts(e));
        return mc && receiverOf(mc) == recv && mc->getMethodCallName() == name;
    }

    ExpressionPtr initializerOf(const VariableDeclaratorPtr& vd) {
        if (!vd || !vd->getInitializer() || vd->getInitializer()->getChildren().empty())
            return nullptr;
        return std::dynamic_pointer_cast<Expression>(vd->getInitializer()->getChildren()[0]);
    }

    // Visits every node under `n`, statements and expressions alike.
    void forEachNode(const AbstractSyntaxNodePtr& n,
                     const std::function<void(const AbstractSyntaxNodePtr&)>& f) {
        if (!n) return;
        f(n);
        if (auto il = std::dynamic_pointer_cast<IdentifierLabel>(n)) {
            forEachNode(il->getBody(), f);
            return;
        }
        if (auto vd = std::dynamic_pointer_cast<VariableDeclarator>(n)) {
            forEachNode(initializerOf(vd), f);
            return;
        }
        n->forEachSubNode([&](const AbstractSyntaxNodePtr& c) { forEachNode(c, f); });
    }

    bool exitsEarly(const AbstractSyntaxNodePtr& body) {
        bool found = false;
        forEachNode(body, [&](const AbstractSyntaxNodePtr& n) {
            if (std::dynamic_pointer_cast<BreakStatement>(n)
                    || std::dynamic_pointer_cast<ReturnStatement>(n))
                found = true;
        });
        return found;
    }

    // Where a value is used: an index, address or control position, or the value of local `owner`.
    struct Use {
        enum Kind { Value, Index, Init } kind = Value;
        std::string owner;
    };

    struct Shared {
        MethodPtr kernel;
        bool group = false;
        Count matrix, vector, intOps;
        std::map<std::string, Count> read, written;   // by kernel buffer parameter
        bool failed = false;
        int depth = 0;
        std::map<std::string, MethodPtr> callees;
    };

    class Counter {
    public:
        Counter(Shared& s, const MethodPtr& method, std::map<std::string, std::string> alias)
            : s(s), method(method), alias(std::move(alias)) {
            for (auto& p : method->getParameterList()) {
                if (!p || p->getName() == "this") continue;
                Ty t = typeOf(p->getType());
                params[p->getName()] = t;
                if (t.kind == Ty::Buffer && method == s.kernel) this->alias[p->getName()] = p->getName();
            }
            collectLocals();
            classifyIndexLocals();
        }

        void run(const Mult& m) { stmt(method->getBlock(), m); }

        // The guard comparing a grid-derived index with a scalar parameter names the work axis.
        std::optional<std::string> workAxis() {
            std::optional<std::string> found;
            forEachNode(method->getBlock(), [&](const AbstractSyntaxNodePtr& n) {
                if (found) return;
                auto ifs = std::dynamic_pointer_cast<IfStatement>(n);
                if (!ifs) return;
                found = axisOf(ifs->getCondition());
            });
            return found;
        }

    private:
        Shared& s;
        MethodPtr method;
        std::map<std::string, std::string> alias;   // buffer parameter -> kernel buffer parameter
        std::map<std::string, Ty> params;
        struct Local { Ty ty; ExpressionPtr init; int decls = 0; int assigns = 0; bool indexOnly = true; };
        std::map<std::string, Local> locals;

        bool isScalarParam(const std::string& n) const {
            auto it = params.find(n);
            return it != params.end() && it->second.kind == Ty::Int && method == s.kernel;
        }

        std::optional<std::string> axisOf(const ExpressionPtr& cond) {
            auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(cond);
            if (!bin) return std::nullopt;
            BinaryOp op = bin->getBinaryOp();
            if (op == BINARY_OP_LOGAND) {
                if (auto a = axisOf(child(bin, 0))) return a;
                return axisOf(child(bin, 1));
            }
            if (op != BINARY_OP_LT && op != BINARY_OP_LE && op != BINARY_OP_GT
                    && op != BINARY_OP_GE)
                return std::nullopt;
            ExpressionPtr l = stripCasts(child(bin, 0)), r = stripCasts(child(bin, 1));
            auto paramName = [&](const ExpressionPtr& e) -> std::optional<std::string> {
                auto id = std::dynamic_pointer_cast<IdentifierExpression>(e);
                if (id && isScalarParam(id->getTextValue())) return id->getTextValue();
                return std::nullopt;
            };
            if (gridDerived(l, 0)) if (auto p = paramName(r)) return p;
            if (gridDerived(r, 0)) if (auto p = paramName(l)) return p;
            return std::nullopt;
        }

        bool gridDerived(const ExpressionPtr& e, int depth) {
            if (!e || depth > 8) return false;
            bool found = false;
            forEachNode(e, [&](const AbstractSyntaxNodePtr& n) {
                if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(n)) {
                    std::string r = receiverOf(mc);
                    const std::string& nm = mc->getMethodCallName();
                    if ((r == "KernelThread" && nm.rfind("globalId", 0) == 0) || (r == "Group" && nm == "rowId")
                            || (r == "Workgroup" && nm.rfind("id", 0) == 0))
                        found = true;
                }
                if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(n)) {
                    auto it = locals.find(id->getTextValue());
                    if (it != locals.end() && it->second.init && fixed(it->second)
                            && gridDerived(it->second.init, depth + 1))
                        found = true;
                }
            });
            return found;
        }

        static bool fixed(const Local& l) { return l.decls == 1 && l.assigns == 0; }

        void collectLocals() {
            forEachNode(method->getBlock(), [&](const AbstractSyntaxNodePtr& n) {
                if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(n)) {
                    for (auto& vd : lvd->getVariableDeclarators()) {
                        Local& l = locals[vd->getIdentifier()];
                        l.ty = typeOf(lvd->getType());
                        l.init = initializerOf(vd);
                        l.decls++;
                        if (!l.init) l.assigns++;
                    }
                } else if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(n)) {
                    Local& l = locals[efs->getElementName()];
                    l.ty = typeOf(efs->getElementType());
                    l.decls++;
                    l.assigns++;
                } else if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(n)) {
                    if (!bin->isAssignment()) return;
                    if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(child(bin, 0)))
                        locals[id->getTextValue()].assigns++;
                } else if (std::dynamic_pointer_cast<PostfixExpression>(n)
                        || std::dynamic_pointer_cast<PrefixExpression>(n)) {
                    if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(child(n, 0)))
                        locals[id->getTextValue()].assigns++;
                }
            });
        }

        // An integer local whose every use is an index or feeds another index-only local.
        void classifyIndexLocals() {
            std::map<std::string, std::vector<Use>> uses;
            std::function<void(const AbstractSyntaxNodePtr&, Use)> scan =
                    [&](const AbstractSyntaxNodePtr& n, Use u) {
                if (!n) return;
                if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(n)) {
                    uses[id->getTextValue()].push_back(u);
                    return;
                }
                Use idx{Use::Index, {}};
                if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(n)) {
                    for (auto& vd : lvd->getVariableDeclarators())
                        scan(initializerOf(vd), {Use::Init, vd->getIdentifier()});
                    return;
                }
                if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(n)) {
                    if (bin->isAssignment()) {
                        auto lhs = child(bin, 0);
                        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(lhs)) {
                            scan(child(bin, 1), {Use::Init, id->getTextValue()});
                            return;
                        }
                        scan(lhs, idx);
                        scan(child(bin, 1), {Use::Value, {}});
                        return;
                    }
                    if (bin->getBinaryOp() >= BINARY_OP_LT && bin->getBinaryOp() <= BINARY_OP_LOGOR) {
                        scan(child(bin, 0), idx);
                        scan(child(bin, 1), idx);
                        return;
                    }
                }
                if (std::dynamic_pointer_cast<ArrayIndexExpression>(n)) {
                    scan(n->getChildren().empty() ? nullptr : n->getChildren()[0], u);
                    for (size_t i = 1; i < n->getChildren().size(); ++i) scan(n->getChildren()[i], idx);
                    return;
                }
                if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(n)) {
                    for (auto& c : mc->getChildren()) scan(c, u);
                    bool addressing = isAddressingCall(mc);
                    for (size_t i = 0; i < mc->getParameters().size(); ++i)
                        scan(mc->getParameters()[i].expression,
                             addressing && addressArg(mc, i) ? idx : Use{Use::Value, {}});
                    return;
                }
                if (auto ifs = std::dynamic_pointer_cast<IfStatement>(n)) {
                    scan(ifs->getCondition(), idx);
                    scan(ifs->getThenBranch(), u);
                    scan(ifs->getElseBranch(), u);
                    return;
                }
                if (auto fs = std::dynamic_pointer_cast<ForStatement>(n)) {
                    scan(fs->getInit(), idx);
                    scan(fs->getCondition(), idx);
                    for (auto& up : fs->getUpdate()) scan(up, idx);
                    scan(fs->getBody(), u);
                    return;
                }
                if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(n)) {
                    scan(efs->getIterableExpr(), idx);
                    scan(efs->getBody(), u);
                    return;
                }
                if (auto ws = std::dynamic_pointer_cast<WhileStatement>(n)) {
                    scan(ws->getCondition(), idx);
                    scan(ws->getBody(), u);
                    return;
                }
                if (auto ds = std::dynamic_pointer_cast<DoStatement>(n)) {
                    scan(ds->getBody(), u);
                    scan(ds->getCondition(), idx);
                    return;
                }
                if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(n)) {
                    scan(es->getExpression(), {Use::Value, {}});
                    return;
                }
                if (auto rs = std::dynamic_pointer_cast<ReturnStatement>(n)) {
                    scan(rs->getExpression(), {Use::Value, {}});
                    return;
                }
                if (auto ls = std::dynamic_pointer_cast<LabelStatement>(n)) { scan(ls->getBlock(), u); return; }
                if (auto il = std::dynamic_pointer_cast<IdentifierLabel>(n)) { scan(il->getBody(), u); return; }
                n->forEachSubNode([&](const AbstractSyntaxNodePtr& c) { scan(c, u); });
            };
            scan(method->getBlock(), {Use::Value, {}});
            for (auto& [name, l] : locals) l.indexOnly = l.ty.kind == Ty::Int && l.ty.lanes == 1;
            for (bool changed = true; changed;) {
                changed = false;
                for (auto& [name, l] : locals) {
                    if (!l.indexOnly) continue;
                    for (const Use& u : uses[name]) {
                        bool ok = u.kind == Use::Index
                            || (u.kind == Use::Init && (u.owner == name
                                || (locals.count(u.owner) && locals[u.owner].indexOnly)));
                        if (!ok) { l.indexOnly = false; changed = true; break; }
                    }
                }
            }
        }

        bool isAddressingCall(const std::shared_ptr<MethodCallExpression>& mc) {
            const std::string& nm = mc->getMethodCallName();
            return nm == "vload" || nm == "vstore" || nm.rfind("atomic", 0) == 0
                || nm == "load" || nm == "store";
        }

        // Whether argument `i` of an addressing call is an offset, stride or layout.
        bool addressArg(const std::shared_ptr<MethodCallExpression>& mc, size_t i) {
            const std::string& nm = mc->getMethodCallName();
            if (nm == "load" || nm == "store") return i > 0;
            return i == 0;
        }

        // ---- symbolic values --------------------------------------------------

        std::optional<Poly> value(const ExpressionPtr& e0, int depth = 0) {
            ExpressionPtr e = stripCasts(e0);
            if (!e || depth > 16) return std::nullopt;
            if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(e)) {
                if (auto v = integerLiteral(il)) return Poly::k(*v);
                return std::nullopt;
            }
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
                const std::string& n = id->getTextValue();
                if (auto it = locals.find(n); it != locals.end()) {
                    if (fixed(it->second) && it->second.init) return value(it->second.init, depth + 1);
                    return std::nullopt;
                }
                if (isScalarParam(n)) return Poly::sym(n);
                if (auto c = templateConstant(n)) return Poly::k(*c);
                if (auto init = staticConstant(n)) return value(init, depth + 1);
                return std::nullopt;
            }
            if (isCall(e, "Group", "width")) return Poly::sym(kWidth);
            if (auto pre = std::dynamic_pointer_cast<PrefixExpression>(e)) {
                auto v = value(child(pre, 0), depth + 1);
                if (!v) return std::nullopt;
                if (pre->getOp() == PREFIX_OP_NEGATIVE) return v->scaled(Rat::of(-1));
                if (pre->getOp() == PREFIX_OP_POSITIVE) return v;
                return std::nullopt;
            }
            if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(e)) {
                auto a = value(child(bin, 0), depth + 1);
                auto b = value(child(bin, 1), depth + 1);
                if (!a || !b) return std::nullopt;
                switch (bin->getBinaryOp()) {
                    case BINARY_OP_ADD: return *a + *b;
                    case BINARY_OP_SUB: return *a - *b;
                    case BINARY_OP_MUL: return *a * *b;
                    case BINARY_OP_DIV:
                        if (auto c = b->constant(); c && *c > 0) return a->scaled(Rat::of(1, *c));
                        return std::nullopt;
                    default: return std::nullopt;
                }
            }
            return std::nullopt;
        }

        // A non-type parameter of the class template instantiation (`TM` in `Tile<4>`).
        std::optional<int64_t> templateConstant(const std::string& nm) {
            CajetaClassPtr cls = method->getParent();
            if (!cls) return std::nullopt;
            const auto& tps = cls->getTypeParameters();
            const auto& args = cls->getTypeArguments();
            for (size_t i = 0; i < tps.size() && i < args.size(); ++i) {
                if (!tps[i].isNonType || tps[i].name != nm) continue;
                if (auto c = std::dynamic_pointer_cast<CajetaConstantType>(args[i])) return c->getValue();
            }
            return std::nullopt;
        }

        // The initializer of a `static final` property of the method's class.
        ExpressionPtr staticConstant(const std::string& nm) {
            CajetaClassPtr cls = method->getParent();
            if (!cls) return nullptr;
            for (auto& prop : cls->getPropertyList()) {
                if (!prop || prop->getName() != nm || !prop->isStatic()) continue;
                if (!prop->getModifiers().count(FINAL) || !prop->getInitializer()
                        || prop->getInitializer()->getChildren().empty())
                    continue;
                return std::dynamic_pointer_cast<Expression>(prop->getInitializer()->getChildren()[0]);
            }
            return nullptr;
        }

        // ---- types ------------------------------------------------------------

        Ty tyOf(const ExpressionPtr& e, int depth = 0) {
            Ty unknown;
            if (!e || depth > 16) return unknown;
            if (auto c = std::dynamic_pointer_cast<CastExpression>(e)) return typeOf(c->getDestType());
            if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(e)) {
                Ty t; t.kind = Ty::Int;
                const std::string& raw = il->getRawValue();
                t.bits = (!raw.empty() && (raw.back() == 'L' || raw.back() == 'l')) ? 64 : 32;
                return t;
            }
            if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(e)) {
                Ty t; t.kind = Ty::Float;
                const std::string& raw = fl->getRawValue();
                t.bits = (!raw.empty() && (raw.back() == 'f' || raw.back() == 'F')) ? 32 : 64;
                return t;
            }
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
                if (auto it = locals.find(id->getTextValue()); it != locals.end()) return it->second.ty;
                if (auto it = params.find(id->getTextValue()); it != params.end()) return it->second;
                if (templateConstant(id->getTextValue())) { Ty t; t.kind = Ty::Int; t.bits = 32; return t; }
                if (auto init = staticConstant(id->getTextValue())) return tyOf(init, depth + 1);
                return unknown;
            }
            if (std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
                Ty base = tyOf(child(e, 0), depth + 1);
                Ty t;
                if (base.kind == Ty::Buffer) {
                    t.kind = base.elemFloat ? Ty::Float : Ty::Int;
                    t.bits = base.elemBits;
                    return t;
                }
                if ((base.kind == Ty::Int || base.kind == Ty::Float) && base.lanes > 1) {
                    t = base;
                    t.lanes = 1;
                    return t;
                }
                return unknown;
            }
            if (auto pre = std::dynamic_pointer_cast<PrefixExpression>(e)) {
                if (pre->getOp() == PREFIX_OP_LOGNOT) { Ty t; t.kind = Ty::Bool; return t; }
                return tyOf(child(pre, 0), depth + 1);
            }
            if (auto post = std::dynamic_pointer_cast<PostfixExpression>(e)) return tyOf(child(post, 0), depth + 1);
            if (auto bs = std::dynamic_pointer_cast<BooleanSwitchExpression>(e)) return tyOf(child(bs, 1), depth + 1);
            if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(e)) {
                BinaryOp op = bin->getBinaryOp();
                if (op >= BINARY_OP_LT && op <= BINARY_OP_LOGOR) { Ty t; t.kind = Ty::Bool; return t; }
                Ty a = tyOf(child(bin, 0), depth + 1);
                if (bin->isAssignment()) return a;
                Ty b = tyOf(child(bin, 1), depth + 1);
                if (a.kind == Ty::Unknown || b.kind == Ty::Unknown) {
                    Ty known = a.kind == Ty::Unknown ? b : a;
                    if (known.kind == Ty::Float) return known;
                    return unknown;
                }
                Ty r = (a.kind == Ty::Float || b.kind != Ty::Float) ? a : b;
                r.lanes = std::max(a.lanes, b.lanes);
                return r;
            }
            if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(e)) {
                std::string recv = receiverOf(mc);
                const std::string& nm = mc->getMethodCallName();
                if (nm == "vload") {
                    Ty base = tyOf(child(mc, 0), depth + 1);
                    const auto& targs = mc->getExplicitMethodTypeArgs();
                    auto cN = targs.size() == 1 ? std::dynamic_pointer_cast<CajetaConstantType>(targs[0]) : nullptr;
                    if (base.kind != Ty::Buffer || !cN) return unknown;
                    Ty t;
                    t.kind = base.elemFloat ? Ty::Float : Ty::Int;
                    t.bits = base.elemBits;
                    t.lanes = (unsigned) cN->getValue();
                    return t;
                }
                if (recv == "KernelThread" || (recv == "Group" && (nm == "width" || nm == "laneId" || nm == "rowId"))) {
                    Ty t; t.kind = Ty::Int; t.bits = 32; return t;
                }
                if (recv == "Group" && (nm == "mac" || nm == "reduce" || nm == "reduceSegmented")) {
                    const auto& ps = mc->getParameters();
                    if (!ps.empty()) return tyOf(ps[nm == "mac" ? 0 : ps.size() - 1].expression, depth + 1);
                }
                if (!recv.empty() && (locals.count(recv) || params.count(recv)))
                    return tyOf(child(mc, 0), depth + 1);
                if (auto chained = child(mc, 0); chained && recv.empty()) return tyOf(chained, depth + 1);
                if (MethodPtr callee = calleeOf(mc)) return typeOf(callee->getReturnType());
            }
            return unknown;
        }

        MethodPtr calleeOf(const std::shared_ptr<MethodCallExpression>& mc) {
            std::string recv = receiverOf(mc);
            if (!recv.empty() && (locals.count(recv) || params.count(recv))) return nullptr;
            if (!child(mc, 0) && !mc->getChildren().empty()) return nullptr;
            const std::string& nm = mc->getMethodCallName();
            size_t argc = mc->getParameters().size();
            CajetaClassPtr own = method->getParent();
            std::string key = recv + "." + nm + "/" + std::to_string(argc) + "@"
                + (own ? own->toCanonical() : std::string());
            if (auto it = s.callees.find(key); it != s.callees.end()) return it->second;
            auto search = [&](const CajetaClassPtr& c) -> MethodPtr {
                if (!c) return nullptr;
                for (auto& kv : c->getMethods())
                    if (kv.second && kv.second->getName() == nm
                            && kv.second->getParameters().size() == argc)
                        return kv.second;
                return nullptr;
            };
            MethodPtr found;
            if (recv.empty() || (own && simpleName(own->toCanonical()) == recv)) {
                found = search(own);
            } else {
                for (auto& kv : CajetaType::getCanonicalMap()) {
                    auto c = std::dynamic_pointer_cast<CajetaClass>(kv.second);
                    if (!c || simpleName(c->toCanonical()) != recv) continue;
                    if ((found = search(c))) break;
                }
            }
            s.callees[key] = found;
            return found;
        }

        // ---- counting ---------------------------------------------------------

        std::string bufferParam(const ExpressionPtr& e) {
            auto id = std::dynamic_pointer_cast<IdentifierExpression>(stripCasts(e));
            if (!id) return {};
            auto it = alias.find(id->getTextValue());
            return it == alias.end() ? std::string() : it->second;
        }

        unsigned elemBytes(const std::string& kernelParam) {
            for (auto& p : s.kernel->getParameterList())
                if (p && p->getName() == kernelParam) return typeOf(p->getType()).elemBits / 8;
            return 0;
        }

        void moved(std::map<std::string, Count>& side, const std::string& buf, const Mult& m, int64_t bytes) {
            if (buf.empty()) return;
            side[buf].add(m.p * Poly::k(bytes), m.measured);
        }

        void stmt(const AbstractSyntaxNodePtr& n, const Mult& m) {
            if (!n) return;
            if (auto e = std::dynamic_pointer_cast<Expression>(n)) { expr(e, m, false); return; }
            if (auto blk = std::dynamic_pointer_cast<Block>(n)) {
                for (auto& c : blk->getChildren()) stmt(c, m);
                return;
            }
            if (auto ls = std::dynamic_pointer_cast<LabelStatement>(n)) { stmt(ls->getBlock(), m); return; }
            if (auto il = std::dynamic_pointer_cast<IdentifierLabel>(n)) { stmt(il->getBody(), m); return; }
            if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(n)) {
                for (auto& vd : lvd->getVariableDeclarators())
                    expr(initializerOf(vd), m, useForLocal(vd->getIdentifier()));
                return;
            }
            if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(n)) {
                expr(es->getExpression(), m, false);
                return;
            }
            if (auto rs = std::dynamic_pointer_cast<ReturnStatement>(n)) {
                expr(rs->getExpression(), m, false);
                return;
            }
            if (auto ifs = std::dynamic_pointer_cast<IfStatement>(n)) {
                expr(ifs->getCondition(), m, true);
                Mult unknown{Poly(), true};
                Mult taken = laneZero(ifs->getCondition()) ? m.perGroup()
                    : boundGuard(ifs->getCondition()) ? m : unknown;
                stmt(ifs->getThenBranch(), taken);
                stmt(ifs->getElseBranch(), unknown);
                return;
            }
            if (auto fs = std::dynamic_pointer_cast<ForStatement>(n)) {
                stmt(fs->getInit(), m);
                Mult body = exitsEarly(fs->getBody()) ? Mult{Poly(), true} : m.times(tripCount(fs));
                expr(fs->getCondition(), body, true);
                for (auto& u : fs->getUpdate()) expr(u, body, true);
                stmt(fs->getBody(), body);
                return;
            }
            if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(n)) {
                auto mc = std::dynamic_pointer_cast<MethodCallExpression>(efs->getIterableExpr());
                Mult body{Poly(), true};
                if (mc && receiverOf(mc) == "Group" && mc->getMethodCallName() == "stripe"
                        && mc->getParameters().size() == 1 && !exitsEarly(efs->getBody())) {
                    expr(mc->getParameters()[0].expression, m, true);
                    body = m.perGroup().times(value(mc->getParameters()[0].expression));
                } else {
                    expr(efs->getIterableExpr(), m, true);
                }
                stmt(efs->getBody(), body);
                return;
            }
            if (auto ws = std::dynamic_pointer_cast<WhileStatement>(n)) {
                Mult body = exitsEarly(ws->getBody()) ? Mult{Poly(), true} : m.times(whileTripCount(ws));
                expr(ws->getCondition(), body, true);
                stmt(ws->getBody(), body);
                return;
            }
            if (auto ds = std::dynamic_pointer_cast<DoStatement>(n)) {
                Mult body{Poly(), true};
                stmt(ds->getBody(), body);
                expr(ds->getCondition(), body, true);
                return;
            }
            if (std::dynamic_pointer_cast<BreakStatement>(n) || std::dynamic_pointer_cast<ContinueStatement>(n))
                return;
            n->forEachSubNode([&](const AbstractSyntaxNodePtr& c) { stmt(c, m); });
        }

        bool useForLocal(const std::string& name) {
            auto it = locals.find(name);
            return it != locals.end() && it->second.indexOnly;
        }

        // Every conjunct compares a grid-derived index with a scalar parameter: in range, the body runs.
        bool boundGuard(const ExpressionPtr& cond) {
            auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(cond);
            if (bin && bin->getBinaryOp() == BINARY_OP_LOGAND)
                return boundGuard(child(bin, 0)) && boundGuard(child(bin, 1));
            return axisOf(cond).has_value();
        }

        bool laneZero(const ExpressionPtr& cond) {
            auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(cond);
            if (!bin || bin->getBinaryOp() != BINARY_OP_EQ) return false;
            auto zero = [&](const ExpressionPtr& e) {
                auto v = value(e);
                return v && v->t.empty();
            };
            return (isCall(child(bin, 0), "Group", "laneId") && zero(child(bin, 1)))
                || (isCall(child(bin, 1), "Group", "laneId") && zero(child(bin, 0)));
        }

        bool isCounter(const ExpressionPtr& e, const std::string& nm) {
            auto id = std::dynamic_pointer_cast<IdentifierExpression>(stripCasts(e));
            return id && id->getTextValue() == nm;
        }

        // The constant step an update adds to counter `nm`, or none.
        std::optional<int64_t> stepOf(const ExpressionPtr& u, const std::string& nm) {
            if (auto post = std::dynamic_pointer_cast<PostfixExpression>(u)) {
                if (isCounter(child(post, 0), nm)) return post->getOp() == POSTFIX_OP_INC ? 1 : -1;
                return std::nullopt;
            }
            if (auto pre = std::dynamic_pointer_cast<PrefixExpression>(u)) {
                if (isCounter(child(pre, 0), nm) && (pre->getOp() == PREFIX_OP_INC || pre->getOp() == PREFIX_OP_DEC))
                    return pre->getOp() == PREFIX_OP_INC ? 1 : -1;
                return std::nullopt;
            }
            auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(u);
            if (!bin || !bin->isAssignment() || !isCounter(child(bin, 0), nm)) return std::nullopt;
            ExpressionPtr rhs = child(bin, 1);
            BinaryOp op = bin->getBinaryOp();
            auto constOf = [&](const ExpressionPtr& e) -> std::optional<int64_t> {
                auto v = e ? value(e) : std::nullopt;
                return v ? v->constant() : std::nullopt;
            };
            if (op == BINARY_OP_ADD_EQUALS || op == BINARY_OP_SUB_EQUALS) {
                auto c = constOf(rhs);
                if (!c) return std::nullopt;
                return op == BINARY_OP_ADD_EQUALS ? *c : -*c;
            }
            if (op != BINARY_OP_ASSIGN) return std::nullopt;
            auto r = std::dynamic_pointer_cast<BinaryOpExpression>(stripCasts(rhs));
            if (!r || (r->getBinaryOp() != BINARY_OP_ADD && r->getBinaryOp() != BINARY_OP_SUB))
                return std::nullopt;
            ExpressionPtr other;
            if (isCounter(child(r, 0), nm)) other = child(r, 1);
            else if (r->getBinaryOp() == BINARY_OP_ADD && isCounter(child(r, 1), nm)) other = child(r, 0);
            auto c = constOf(other);
            if (!c) return std::nullopt;
            return r->getBinaryOp() == BINARY_OP_ADD ? *c : -*c;
        }

        // Trips of a loop over counter `nm` from `start` by `step` while `cond` holds.
        std::optional<Poly> tripOf(const Poly& start, int64_t step, const ExpressionPtr& condition,
                                   const std::string& nm) {
            auto cond = std::dynamic_pointer_cast<BinaryOpExpression>(condition);
            if (!cond || step == 0) return std::nullopt;
            BinaryOp op = cond->getBinaryOp();
            ExpressionPtr bound;
            if (isCounter(child(cond, 0), nm)) bound = child(cond, 1);
            else if (isCounter(child(cond, 1), nm)) {
                bound = child(cond, 0);
                if (op == BINARY_OP_LT) op = BINARY_OP_GT;
                else if (op == BINARY_OP_GT) op = BINARY_OP_LT;
                else if (op == BINARY_OP_LE) op = BINARY_OP_GE;
                else if (op == BINARY_OP_GE) op = BINARY_OP_LE;
            }
            auto end = bound ? value(bound) : std::nullopt;
            if (!end) return std::nullopt;
            if (step > 0 && (op == BINARY_OP_LT || op == BINARY_OP_NE))
                return (*end - start).scaled(Rat::of(1, step));
            if (step > 0 && op == BINARY_OP_LE)
                return (*end - start + Poly::k(1)).scaled(Rat::of(1, step));
            if (step < 0 && (op == BINARY_OP_GT || op == BINARY_OP_NE))
                return (start - *end).scaled(Rat::of(1, -step));
            if (step < 0 && op == BINARY_OP_GE)
                return (start - *end + Poly::k(1)).scaled(Rat::of(1, -step));
            return std::nullopt;
        }

        std::optional<Poly> tripCount(const std::shared_ptr<ForStatement>& fs) {
            auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(fs->getInit());
            if (!lvd || lvd->getVariableDeclarators().size() != 1 || fs->getUpdate().size() != 1)
                return std::nullopt;
            auto vd = lvd->getVariableDeclarators().front();
            auto start = value(initializerOf(vd));
            auto step = stepOf(fs->getUpdate().front(), vd->getIdentifier());
            if (!start || !step) return std::nullopt;
            return tripOf(*start, *step, fs->getCondition(), vd->getIdentifier());
        }

        // A counted while: a counter declared once with a start, stepped once at the body's top level.
        std::optional<Poly> whileTripCount(const std::shared_ptr<WhileStatement>& ws) {
            auto cond = std::dynamic_pointer_cast<BinaryOpExpression>(ws->getCondition());
            if (!cond) return std::nullopt;
            for (size_t side = 0; side < 2; ++side) {
                auto id = std::dynamic_pointer_cast<IdentifierExpression>(stripCasts(child(cond, side)));
                if (!id) continue;
                const std::string nm = id->getTextValue();
                auto it = locals.find(nm);
                if (it == locals.end() || it->second.decls != 1 || it->second.assigns != 1 || !it->second.init)
                    continue;
                AbstractSyntaxNodePtr body = ws->getBody();
                if (auto ls = std::dynamic_pointer_cast<LabelStatement>(body)) body = ls->getBlock();
                auto blk = std::dynamic_pointer_cast<Block>(body);
                if (!blk) continue;
                std::optional<int64_t> step;
                for (auto& st : blk->getChildren())
                    if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(st))
                        if (auto s1 = stepOf(es->getExpression(), nm)) step = s1;
                auto start = value(it->second.init);
                if (!step || !start) continue;
                return tripOf(*start, *step, ws->getCondition(), nm);
            }
            return std::nullopt;
        }

        void multiply(const ExpressionPtr& e, const Mult& m, bool indexing) {
            Ty t = tyOf(e);
            Poly lanes = Poly::k(t.lanes);
            if (t.kind == Ty::Float) { s.vector.add(m.p * lanes, m.measured); return; }
            if (t.kind == Ty::Int) {
                if (!indexing) s.intOps.add(m.p * lanes, m.measured);
                return;
            }
            if (!indexing) {
                s.vector.measured = true;
                s.intOps.measured = true;
            }
        }

        void expr(const ExpressionPtr& e, const Mult& m, bool indexing) {
            if (!e) return;
            if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(e)) {
                BinaryOp op = bin->getBinaryOp();
                ExpressionPtr lhs = child(bin, 0), rhs = child(bin, 1);
                if (bin->isAssignment()) {
                    bool rhsIndexing = false;
                    if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(lhs)) {
                        rhsIndexing = useForLocal(id->getTextValue());
                    } else if (std::dynamic_pointer_cast<ArrayIndexExpression>(lhs)) {
                        std::string buf = bufferParam(child(lhs, 0));
                        int64_t b = buf.empty() ? 0 : elemBytes(buf);
                        moved(s.written, buf, m, b);
                        if (op != BINARY_OP_ASSIGN) moved(s.read, buf, m, b);
                        for (size_t i = 1; i < lhs->getChildren().size(); ++i) expr(child(lhs, i), m, true);
                    } else {
                        expr(lhs, m, true);
                    }
                    if (op == BINARY_OP_MUL_EQUALS) multiply(lhs, m, rhsIndexing);
                    expr(rhs, m, rhsIndexing);
                    return;
                }
                if (op == BINARY_OP_MUL) multiply(e, m, indexing);
                bool control = op >= BINARY_OP_LT && op <= BINARY_OP_LOGOR;
                expr(lhs, m, indexing || control);
                expr(rhs, m, indexing || control);
                return;
            }
            if (std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
                std::string buf = bufferParam(child(e, 0));
                if (!buf.empty()) moved(s.read, buf, m, elemBytes(buf));
                else expr(child(e, 0), m, indexing);
                for (size_t i = 1; i < e->getChildren().size(); ++i) expr(child(e, i), m, true);
                return;
            }
            if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(e)) {
                call(mc, m, indexing);
                return;
            }
            if (auto bs = std::dynamic_pointer_cast<BooleanSwitchExpression>(e)) {
                expr(child(bs, 0), m, true);
                Mult unknown{Poly(), true};
                expr(child(bs, 1), unknown, indexing);
                expr(child(bs, 2), unknown, indexing);
                return;
            }
            e->forEachSubNode([&](const AbstractSyntaxNodePtr& c) {
                expr(std::dynamic_pointer_cast<Expression>(c), m, indexing);
            });
        }

        void call(const std::shared_ptr<MethodCallExpression>& mc, const Mult& m, bool indexing) {
            const std::string recv = receiverOf(mc);
            const std::string& nm = mc->getMethodCallName();
            const auto& ps = mc->getParameters();
            std::string buf = bufferParam(child(mc, 0));
            if (!buf.empty() && (nm == "vload" || nm == "vstore" || nm.rfind("atomic", 0) == 0)) {
                int64_t eb = elemBytes(buf);
                if (nm == "vload") {
                    moved(s.read, buf, m, eb * (int64_t) tyOf(mc).lanes);
                } else if (nm == "vstore") {
                    unsigned lanes = ps.size() == 2 ? tyOf(ps[1].expression).lanes : 1;
                    moved(s.written, buf, m, eb * (int64_t) lanes);
                } else {
                    moved(s.read, buf, m, eb);
                    moved(s.written, buf, m, eb);
                }
                for (size_t i = 0; i < ps.size(); ++i) expr(ps[i].expression, m, i == 0);
                return;
            }
            Ty recvTy = recv.empty() ? Ty() : tyOf(child(mc, 0));
            if (recvTy.kind == Ty::Tile && (nm == "load" || nm == "store") && !ps.empty()) {
                Mult g = m.perGroup();
                std::string target = bufferParam(ps[0].expression);
                int64_t bytes = (int64_t) recvTy.rows * recvTy.cols * (target.empty() ? 0 : elemBytes(target));
                moved(nm == "load" ? s.read : s.written, target, g, bytes);
                for (size_t i = 1; i < ps.size(); ++i) expr(ps[i].expression, m, true);
                return;
            }
            bool groupMac = recv == "Group" && nm == "mac" && ps.size() == 3;
            bool tileMma = recvTy.kind == Ty::Tile && nm == "mma" && ps.size() == 2;
            if (groupMac || tileMma) {
                Ty a = tyOf(ps[groupMac ? 1 : 0].expression);
                Ty b = tyOf(ps[groupMac ? 2 : 1].expression);
                if (a.kind == Ty::Tile && b.kind == Ty::Tile) {
                    Mult g = m.perGroup();
                    s.matrix.add(g.p * Poly::k((int64_t) a.rows * a.cols * b.cols), g.measured);
                    return;
                }
                if (a.kind == Ty::Int) s.intOps.add(m.p * Poly::k(a.lanes), m.measured);
                else if (a.kind == Ty::Float) s.vector.add(m.p * Poly::k(a.lanes), m.measured);
                else { s.intOps.measured = true; s.vector.measured = true; }
                for (auto& p : ps) expr(p.expression, m, false);
                return;
            }
            if (MethodPtr callee = calleeOf(mc); callee && callee->getBlock()
                    && callee != s.kernel && s.depth < 4) {
                std::map<std::string, std::string> calleeAlias;
                auto formals = callee->getParameterList();
                size_t fi = 0;
                for (auto& f : formals) {
                    if (!f || f->getName() == "this") continue;
                    if (fi < ps.size()) {
                        std::string b = bufferParam(ps[fi].expression);
                        if (!b.empty()) calleeAlias[f->getName()] = b;
                    }
                    ++fi;
                }
                s.depth++;
                Counter(s, callee, std::move(calleeAlias)).run(m);
                s.depth--;
            }
            for (auto& c : mc->getChildren()) expr(std::dynamic_pointer_cast<Expression>(c), m, indexing);
            for (auto& p : ps) expr(p.expression, m, indexing);
        }
    };

    bool usesGroup(const MethodPtr& kernel) {
        bool found = false;
        forEachNode(kernel->getBlock(), [&](const AbstractSyntaxNodePtr& n) {
            if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(n))
                if (receiverOf(mc) == "Group") found = true;
            if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(n))
                if (typeOf(lvd->getType()).kind == Ty::Tile) found = true;
        });
        return found;
    }

    std::string render(const Count& c, std::optional<unsigned> width) {
        if (c.measured) return "measured";
        if (!c.p.mentions(kWidth)) return c.p.str();
        if (!width) return "measured";
        return c.p.withWidth(*width).str();
    }

    // ---- evaluation -----------------------------------------------------------

    struct Eval {
        const std::string& s;
        const std::map<std::string, double>& args;
        size_t i = 0;
        bool ok = true;
        void ws() { while (i < s.size() && std::isspace((unsigned char) s[i])) ++i; }
        double primary() {
            ws();
            if (i >= s.size()) { ok = false; return 0; }
            if (s[i] == '(') {
                ++i;
                double v = sum();
                ws();
                if (i >= s.size() || s[i] != ')') ok = false; else ++i;
                return v;
            }
            if (s[i] == '-') { ++i; return -primary(); }
            if (std::isdigit((unsigned char) s[i])) {
                size_t j = i;
                while (j < s.size() && std::isdigit((unsigned char) s[j])) ++j;
                double v = std::strtod(s.substr(i, j - i).c_str(), nullptr);
                i = j;
                return v;
            }
            if (std::isalpha((unsigned char) s[i]) || s[i] == '_') {
                size_t j = i;
                while (j < s.size() && (std::isalnum((unsigned char) s[j]) || s[j] == '_')) ++j;
                auto it = args.find(s.substr(i, j - i));
                i = j;
                if (it == args.end()) { ok = false; return 0; }
                return it->second;
            }
            ok = false;
            return 0;
        }
        double product() {
            double v = primary();
            for (;;) {
                ws();
                if (i < s.size() && s[i] == '*') { ++i; v *= primary(); }
                else if (i < s.size() && s[i] == '/') {
                    ++i;
                    double d = primary();
                    if (d == 0) ok = false; else v /= d;
                } else return v;
            }
        }
        double sum() {
            double v = product();
            for (;;) {
                ws();
                if (i < s.size() && s[i] == '+') { ++i; v += product(); }
                else if (i < s.size() && s[i] == '-') { ++i; v -= product(); }
                else return v;
            }
        }
    };

} // namespace

KernelCost deriveKernelCost(const MethodPtr& kernel, std::optional<unsigned> groupWidth) {
    KernelCost out;
    if (!kernel) return out;
    Shared s;
    s.kernel = kernel;
    try {
        s.group = usesGroup(kernel);
        Counter top(s, kernel, {});
        top.run(s.group ? Mult{Poly::sym(kWidth), false} : Mult{Poly::k(1), false});
        out.workAxisParam = top.workAxis();
    } catch (...) {
        s.failed = true;
    }
    out.costUnit = s.group ? "group" : "item";
    auto field = [&](const Count& c) { return s.failed ? std::string("measured") : render(c, groupWidth); };
    out.matrixMacs = field(s.matrix);
    out.vectorMacs = field(s.vector);
    out.intOps = field(s.intOps);
    for (auto& p : kernel->getParameterList()) {
        if (!p || p->getName() == "this") continue;
        Ty t = typeOf(p->getType());
        if (t.kind != Ty::Buffer) continue;
        KernelBufferCost b;
        b.param = p->getName();
        b.storedBits = t.elemBits;
        if (auto ann = p->findAnnotation(XpuAttr::Stored))
            if (int64_t bits = ann->getInt("bits", 0); bits > 0) b.storedBits = (unsigned) bits;
        b.bytesRead = field(s.read[b.param]);
        b.bytesWritten = field(s.written[b.param]);
        out.buffers.push_back(std::move(b));
    }
    return out;
}

double evaluateCost(const std::string& expr, const std::map<std::string, double>& args, bool* ok) {
    Eval e{expr, args};
    double v = e.sum();
    e.ws();
    if (e.i != expr.size()) e.ok = false;
    if (ok) *ok = e.ok;
    return e.ok ? v : 0;
}

std::string storedAnnotationError(const MethodPtr& kernel) {
    if (!kernel) return {};
    for (auto& p : kernel->getParameterList()) {
        if (!p || p->getName() == "this") continue;
        auto ann = p->findAnnotation(XpuAttr::Stored);
        if (!ann) continue;
        Ty t = typeOf(p->getType());
        if (t.kind != Ty::Buffer)
            return "`@Stored` on parameter `" + p->getName() + "`: only a KernelBuffer parameter "
                   "has a stored width";
        int64_t bits = ann->getInt("bits", 0);
        if (bits < 1 || (t.elemBits && bits > (int64_t) t.elemBits))
            return "`@Stored(bits = " + std::to_string(bits) + ")` on parameter `" + p->getName()
                   + "`: the stored width is between 1 and the element's " + std::to_string(t.elemBits)
                   + " bits";
    }
    return {};
}

} // namespace xpu
} // namespace cajeta
