// The kernel reference interpreter: see KernelInterpreter.h, and
// docs/specification/xpu/ReferenceInterpreter.md for the semantics written
// down. Nothing here calls into the lowering; the two meet only at the AST.

#include "KernelInterpreter.h"

#include "../core/KernelArgTrait.h"
#include "../core/XpuAttributes.h"
#include "../core/XpuKernelAttr.h"
#include "../../asn/AbstractSyntaxNode.h"
#include "../../asn/Block.h"
#include "../../asn/LocalVariableDeclaration.h"
#include "../../asn/Statement.h"
#include "../../asn/VariableDeclarator.h"
#include "../../asn/expression/BinaryOpExpression.h"
#include "../../asn/expression/CreatorRest.h"
#include "../../asn/expression/DotExpression.h"
#include "../../asn/expression/Expression.h"
#include "../../asn/expression/Identifier.h"
#include "../../asn/expression/LiteralExpression.h"
#include "../../asn/expression/MethodCallExpression.h"
#include "../../asn/expression/NewExpression.h"
#include "../../error/Exception.h"
#include "../../method/Method.h"
#include "../../ownership/TitleClassifier.h"
#include "../../type/CajetaArray.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaConstantType.h"
#include "../../type/CajetaType.h"
#include "../../type/CajetaVector.h"
#include "../../type/FormalParameter.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <typeinfo>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

namespace cajeta {
namespace xpu {
namespace reference {

namespace {

// ---- values ---------------------------------------------------------------

enum class Prim : uint8_t { Bool, I8, I16, I32, I64, F16, BF16, F32, F64 };

bool isFloat(Prim p) { return p >= Prim::F16; }

unsigned bitsOf(Prim p) {
    switch (p) {
        case Prim::Bool: return 1;
        case Prim::I8: return 8;
        case Prim::I16: case Prim::F16: case Prim::BF16: return 16;
        case Prim::I32: case Prim::F32: return 32;
        case Prim::I64: case Prim::F64: return 64;
    }
    return 32;
}

unsigned bytesOf(Prim p) { return p == Prim::Bool ? 1 : bitsOf(p) / 8; }

Prim intOfBits(unsigned bits) {
    return bits <= 8 ? Prim::I8 : bits <= 16 ? Prim::I16 : bits <= 32 ? Prim::I32 : Prim::I64;
}

struct Ty {
    Prim prim = Prim::I32;
    bool sgn = true;
    bool operator==(const Ty& o) const { return prim == o.prim && sgn == o.sgn; }
};

std::string tyName(const Ty& t) {
    switch (t.prim) {
        case Prim::Bool: return "boolean";
        case Prim::F16: return "float16";
        case Prim::BF16: return "bfloat16";
        case Prim::F32: return "float32";
        case Prim::F64: return "float64";
        default: return std::string(t.sgn ? "int" : "uint") + std::to_string(bitsOf(t.prim));
    }
}

// The scalar type of a primitive CajetaType, or nullopt.
std::optional<Ty> primOf(const CajetaTypePtr& t) {
    if (!t) return std::nullopt;
    CajetaTypeFlags f = t->getTypeFlags();
    if (!(f & PRIMITIVE_FLAG)) return std::nullopt;
    Ty out;
    out.sgn = (f & SIGNED_FLAG) != 0;
    if (f & FLOAT_FLAG) {
        out.sgn = true;
        if (f & BIT_64_FLAG) out.prim = Prim::F64;
        else if (f & BIT_16_FLAG) {
            constexpr CajetaTypeFlags kIdMask = 0x000000FF00000000ULL;
            out.prim = (f & kIdMask) == BFLOAT16_ID ? Prim::BF16 : Prim::F16;
        } else out.prim = Prim::F32;
        return out;
    }
    if (f & INT_FLAG) {
        if (f & BIT_64_FLAG) out.prim = Prim::I64;
        else if (f & BIT_32_FLAG) out.prim = Prim::I32;
        else if (f & BIT_16_FLAG) out.prim = Prim::I16;
        else if (f & BIT_8_FLAG) out.prim = Prim::I8;
        else { out.prim = Prim::Bool; out.sgn = false; }
        return out;
    }
    return std::nullopt;
}

// IEEE binary16 and bfloat16 from and to float32, round to nearest even.
float halfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t) (h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {
            int e = -1;
            do { ++e; man <<= 1; } while (!(man & 0x400));
            bits = sign | ((uint32_t) (127 - 15 - e) << 23) | ((man & 0x3FF) << 13);
        }
    } else if (exp == 0x1F) bits = sign | 0x7F800000 | (man << 13);
    else bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    uint32_t absx = x & 0x7FFFFFFF;
    if (absx >= 0x7F800000) return (uint16_t) (sign | 0x7C00 | (absx > 0x7F800000 ? 0x200 : 0));
    if (absx >= 0x477FF000) return (uint16_t) (sign | 0x7C00);   // rounds past the max
    if (absx < 0x38800000) {                                       // subnormal or zero
        if (absx < 0x33000000) return (uint16_t) sign;
        uint32_t man = (absx & 0x7FFFFF) | 0x800000;
        int shift = 113 - (int) (absx >> 23) + 13;
        uint32_t half = man >> shift;
        uint32_t rem = man & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1))) ++half;
        return (uint16_t) (sign | half);
    }
    uint32_t r = absx - 0x38000000;                                // rebias 127 -> 15
    uint32_t half = r >> 13, rem = r & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (half & 1))) ++half;
    return (uint16_t) (sign | half);
}

float bf16ToFloat(uint16_t b) {
    uint32_t x = (uint32_t) b << 16;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

uint16_t floatToBf16(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    if ((x & 0x7FFFFFFF) > 0x7F800000) return (uint16_t) ((x >> 16) | 0x40);
    x += 0x7FFF + ((x >> 16) & 1);
    return (uint16_t) (x >> 16);
}

// Round `d` to precision `p`, back as a double that holds it exactly.
double roundTo(Prim p, double d) {
    switch (p) {
        case Prim::F64: return d;
        case Prim::F32: return (double) (float) d;
        case Prim::F16: return (double) halfToFloat(floatToHalf((float) d));
        case Prim::BF16: return (double) bf16ToFloat(floatToBf16((float) d));
        default: return d;
    }
}

uint64_t maskOf(unsigned bits) { return bits >= 64 ? ~0ull : ((1ull << bits) - 1); }

int64_t sext(uint64_t v, unsigned bits) {
    if (bits >= 64) return (int64_t) v;
    uint64_t m = 1ull << (bits - 1);
    v &= maskOf(bits);
    return (int64_t) ((v ^ m) - m);
}

struct Mem;
struct Tile;

struct Val {
    enum Kind : uint8_t { None, Scalar, Vector, MemRef, TileRef } k = None;
    Ty t;
    bool lit = false;        // an unadorned integer literal: adapts to its sibling
    uint64_t i = 0;          // integer bits (masked to the width) and booleans
    double f = 0;            // floats, exactly representable in t.prim
    std::vector<Val> lanes;  // Vector
    Mem* mem = nullptr;
    std::shared_ptr<Tile> tile;

    int64_t s64() const { return sext(i, bitsOf(t.prim)); }
    uint64_t u64() const { return i & maskOf(bitsOf(t.prim)); }
    bool truthy() const { return isFloat(t.prim) ? f != 0 : i != 0; }
};

Val mkInt(Ty t, uint64_t raw) {
    Val v; v.k = Val::Scalar; v.t = t; v.i = raw & maskOf(bitsOf(t.prim));
    return v;
}
Val mkBool(bool b) { return mkInt({Prim::Bool, false}, b ? 1 : 0); }
Val mkFloat(Prim p, double d) {
    Val v; v.k = Val::Scalar; v.t = {p, true}; v.f = roundTo(p, d);
    return v;
}

std::string show(const Val& v) {
    if (v.k != Val::Scalar) return "<" + tyName(v.t) + " aggregate>";
    if (isFloat(v.t.prim)) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9g", v.f);
        return buf;
    }
    if (v.t.prim == Prim::Bool) return v.i ? "true" : "false";
    return v.t.sgn ? std::to_string(v.s64()) : std::to_string(v.u64());
}

// A buffer argument, or one workgroup's Shared array.
struct Mem {
    std::string name;
    uint8_t* data = nullptr;
    uint64_t count = 0;
    Ty elem;
    bool shared = false;
    std::vector<uint8_t> written;   // shared only: which elements a work-item stored
    std::vector<uint8_t> own;       // shared only: the storage
    // Bumped by every store, so a memoized tile load from this memory is
    // recomputed once the memory changes (stores may come from other threads).
    std::atomic<uint64_t> version{0};
};

// A tile's elements, row-major and immutable once made: floats as doubles
// that hold the element type's value exactly, integers as their masked bits.
// Tiles are wave-uniform values, so the lanes of a workgroup that compute the
// same tile share one of these (see Group::tileMemo).
struct TileData {
    std::vector<double> f;
    std::vector<uint64_t> i;
};

struct Tile {
    Ty elem;
    uint32_t rows = 0, cols = 0, use = 0;
    std::shared_ptr<const TileData> data;
};

[[noreturn]] void refuse(const std::string& what) {
    throw Exception(what, "XPU-REF01");
}
[[noreturn]] void undefined(const std::string& what) {
    throw Exception(what, "XPU-REF02");
}


// A source location, formatted only when a message is built: the operators
// take one on every evaluation, and only an error reads it.
struct Where {
    const AbstractSyntaxNode* n = nullptr;
    Where() = default;
    template <typename T>
    Where(const std::shared_ptr<T>& p) : n(p.get()) {}
    Where(const AbstractSyntaxNode* p) : n(p) {}
};

std::string at(const Where& w) {
    if (!w.n) return "";
    std::string s = "line " + std::to_string(w.n->getSourceLine());
    const std::string& text = w.n->getSourceText();
    if (!text.empty() && text.size() <= 80) s += ", near `" + text + "`";
    return s;
}
std::string operator+(const std::string& s, const Where& w) { return s + at(w); }
std::string operator+(const char* s, const Where& w) { return std::string(s) + at(w); }

Expression* child(AbstractSyntaxNode* n, size_t i) {
    if (!n || n->getChildren().size() <= i) return nullptr;
    return dynamic_cast<Expression*>(n->getChildren()[i].get());
}

ExpressionPtr child(const AbstractSyntaxNodePtr& n, size_t i) {
    if (!n || n->getChildren().size() <= i) return nullptr;
    return std::dynamic_pointer_cast<Expression>(n->getChildren()[i]);
}

// ---- conversions and operators -------------------------------------------

Val convert(const Val& v, Ty to) {
    if (v.k != Val::Scalar) undefined("a " + tyName(v.t) + " aggregate cannot convert to " + tyName(to));
    if (v.t == to && !v.lit) return v;
    Prim from = v.t.prim;
    if (isFloat(to.prim)) {
        if (isFloat(from)) return mkFloat(to.prim, v.f);
        if (from == Prim::Bool) return mkFloat(to.prim, v.i ? 1.0 : 0.0);
        bool sg = v.t.sgn;
        if (to.prim == Prim::F64)
            return mkFloat(to.prim, sg ? (double) v.s64() : (double) v.u64());
        float f = sg ? (float) v.s64() : (float) v.u64();
        return mkFloat(to.prim, (double) f);
    }
    if (to.prim == Prim::Bool) return mkBool(v.truthy());
    if (isFloat(from)) {
        double d = std::trunc(v.f);
        unsigned b = bitsOf(to.prim);
        bool inRange;
        if (std::isnan(v.f)) inRange = false;
        else if (to.sgn) inRange = d >= -std::ldexp(1.0, (int) b - 1) && d < std::ldexp(1.0, (int) b - 1);
        else inRange = d > -1.0 && d < std::ldexp(1.0, (int) b);
        if (!inRange)
            undefined("converting " + show(v) + " to " + tyName(to) + " is out of range");
        return mkInt(to, to.sgn ? (uint64_t) (int64_t) d : (uint64_t) d);
    }
    uint64_t wide = (v.t.sgn && from != Prim::Bool) ? (uint64_t) v.s64() : v.u64();
    return mkInt(to, wide);
}

Prim widerFloat(Prim a, Prim b) {
    if (!isFloat(a)) return b;
    if (!isFloat(b)) return a;
    if (a == Prim::F64 || b == Prim::F64) return Prim::F64;
    if (a == Prim::F32 || b == Prim::F32 || a != b) return Prim::F32;
    return a;
}

double floatOp(BinaryOp op, Prim p, double a, double b, bool& isCmp, bool& cmp) {
    isCmp = true;
    switch (op) {
        case BINARY_OP_LT: cmp = a < b; return 0;
        case BINARY_OP_LE: cmp = a <= b; return 0;
        case BINARY_OP_GT: cmp = a > b; return 0;
        case BINARY_OP_GE: cmp = a >= b; return 0;
        case BINARY_OP_EQ: cmp = a == b; return 0;
        case BINARY_OP_NE: cmp = !(a == b); return 0;   // unordered: the host's UNE
        default: break;
    }
    isCmp = false;
    if (p == Prim::F64) {
        switch (op) {
            case BINARY_OP_ADD: return a + b;
            case BINARY_OP_SUB: return a - b;
            case BINARY_OP_MUL: return a * b;
            case BINARY_OP_DIV: return a / b;
            case BINARY_OP_MOD: return std::fmod(a, b);
            default: break;
        }
    } else {
        // Float32 work, and float16/bfloat16 computed in float32 and narrowed.
        float x = (float) a, y = (float) b, r;
        switch (op) {
            case BINARY_OP_ADD: r = x + y; return r;
            case BINARY_OP_SUB: r = x - y; return r;
            case BINARY_OP_MUL: r = x * y; return r;
            case BINARY_OP_DIV: r = x / y; return r;
            case BINARY_OP_MOD: r = std::fmod(x, y); return r;
            default: break;
        }
    }
    refuse("operator on floating-point operands");
}

bool isCompare(BinaryOp op) {
    return op == BINARY_OP_LT || op == BINARY_OP_LE || op == BINARY_OP_GT
        || op == BINARY_OP_GE || op == BINARY_OP_EQ || op == BINARY_OP_NE;
}

const char* opText(BinaryOp op) {
    switch (op) {
        case BINARY_OP_ADD: return "+"; case BINARY_OP_SUB: return "-";
        case BINARY_OP_MUL: return "*"; case BINARY_OP_DIV: return "/";
        case BINARY_OP_MOD: return "%"; case BINARY_OP_BITAND: return "&";
        case BINARY_OP_BITOR: return "|"; case BINARY_OP_BITXOR: return "^";
        case BINARY_OP_SHIFTLEFT: return "<<"; case BINARY_OP_SHIFTRIGHT: return ">>";
        case BINARY_OP_USHIFTRIGHT: return ">>>";
        default: return "op";
    }
}

// One scalar binary operator, by the host compiler's rules (BinaryOpExpression):
// a bare literal adapts to its typed sibling; ints extend by their own
// signedness to the wider width; `/`, `%` and the bitwise operators are
// unsigned when either operand is; a comparison is signed when either is;
// `>>` follows the shifted operand alone.
Val scalarOp(BinaryOp op, Val a, Val b, const Where& where) {
    if (a.lit && !b.lit && b.k == Val::Scalar && b.t.prim != Prim::Bool) a = convert(a, b.t);
    if (b.lit && !a.lit && a.k == Val::Scalar && a.t.prim != Prim::Bool) b = convert(b, a.t);
    const bool bothLit = a.lit && b.lit;

    if (a.t.prim == Prim::Bool || b.t.prim == Prim::Bool) {
        if (a.t.prim != b.t.prim)
            refuse("`" + std::string(opText(op)) + "` between boolean and " +
                   tyName(a.t.prim == Prim::Bool ? b.t : a.t) + " (" + where + ")");
        switch (op) {
            case BINARY_OP_EQ: return mkBool(a.i == b.i);
            case BINARY_OP_NE: return mkBool(a.i != b.i);
            case BINARY_OP_BITAND: return mkBool(a.i & b.i);
            case BINARY_OP_BITOR: return mkBool(a.i | b.i);
            case BINARY_OP_BITXOR: return mkBool(a.i ^ b.i);
            default: refuse("operator on boolean operands (" + where + ")");
        }
    }

    if (isFloat(a.t.prim) || isFloat(b.t.prim)) {
        Prim p = widerFloat(a.t.prim, b.t.prim);
        Val x = convert(a, {p, true}), y = convert(b, {p, true});
        bool isCmp = false, cmp = false;
        double r = floatOp(op, p, x.f, y.f, isCmp, cmp);
        if (isCmp) return mkBool(cmp);
        return mkFloat(p, r);
    }

    unsigned w = std::max(bitsOf(a.t.prim), bitsOf(b.t.prim));
    Prim ip = intOfBits(w);
    uint64_t x = a.t.sgn ? (uint64_t) a.s64() : a.u64();
    uint64_t y = b.t.sgn ? (uint64_t) b.s64() : b.u64();
    x &= maskOf(w);
    y &= maskOf(w);
    const bool unsignedWins = !(a.t.sgn && b.t.sgn);
    Ty rt{ip, !unsignedWins};
    auto lit = [&](Val v) { v.lit = bothLit; return v; };

    if (isCompare(op)) {
        bool sg = a.t.sgn || b.t.sgn;
        int64_t sx = sext(x, w), sy = sext(y, w);
        switch (op) {
            case BINARY_OP_LT: return mkBool(sg ? sx < sy : x < y);
            case BINARY_OP_LE: return mkBool(sg ? sx <= sy : x <= y);
            case BINARY_OP_GT: return mkBool(sg ? sx > sy : x > y);
            case BINARY_OP_GE: return mkBool(sg ? sx >= sy : x >= y);
            case BINARY_OP_EQ: return mkBool(x == y);
            default: return mkBool(x != y);
        }
    }
    switch (op) {
        case BINARY_OP_ADD: return lit(mkInt(rt, x + y));
        case BINARY_OP_SUB: return lit(mkInt(rt, x - y));
        case BINARY_OP_MUL: return lit(mkInt(rt, x * y));
        case BINARY_OP_DIV:
        case BINARY_OP_MOD: {
            if (y == 0) undefined("integer `" + std::string(opText(op)) + "` by zero (" + where + ")");
            if (rt.sgn) {
                int64_t sx = sext(x, w), sy = sext(y, w);
                if (sy == -1 && sx == sext(1ull << (w - 1), w))
                    undefined("`" + std::string(opText(op)) + "` overflows " + tyName(rt) +
                              " (" + where + ")");
                return lit(mkInt(rt, (uint64_t) (op == BINARY_OP_DIV ? sx / sy : sx % sy)));
            }
            return lit(mkInt(rt, op == BINARY_OP_DIV ? x / y : x % y));
        }
        case BINARY_OP_BITAND: return lit(mkInt(rt, x & y));
        case BINARY_OP_BITOR: return lit(mkInt(rt, x | y));
        case BINARY_OP_BITXOR: return lit(mkInt(rt, x ^ y));
        case BINARY_OP_SHIFTLEFT:
        case BINARY_OP_SHIFTRIGHT:
        case BINARY_OP_USHIFTRIGHT: {
            Ty st{ip, a.t.sgn};
            uint64_t n = b.t.sgn && sext(y, w) < 0 ? ~0ull : y;
            if (n >= w)
                undefined("shift by " + show(b) + " on a " + std::to_string(w) +
                          "-bit value (" + where + ")");
            if (op == BINARY_OP_SHIFTLEFT) return lit(mkInt(st, x << n));
            if (op == BINARY_OP_SHIFTRIGHT && a.t.sgn)
                return lit(mkInt(st, (uint64_t) (sext(x, w) >> n)));
            return lit(mkInt(st, x >> n));
        }
        default: break;
    }
    refuse("binary operator (" + where + ")");
}

Val binaryOp(BinaryOp op, const Val& a, const Val& b, const Where& where) {
    if (a.k == Val::Vector || b.k == Val::Vector) {
        const Val& vec = a.k == Val::Vector ? a : b;
        Val out;
        out.k = Val::Vector;
        for (size_t l = 0; l < vec.lanes.size(); ++l) {
            Val x = a.k == Val::Vector ? a.lanes[l] : convert(a, vec.t);
            Val y = b.k == Val::Vector ? b.lanes.at(l) : convert(b, vec.t);
            out.lanes.push_back(scalarOp(op, x, y, where));
        }
        out.t = out.lanes.empty() ? vec.t : out.lanes[0].t;
        return out;
    }
    if (a.k != Val::Scalar || b.k != Val::Scalar)
        refuse("`" + std::string(opText(op)) + "` on a buffer, Shared array or tile (" + where + ")");
    return scalarOp(op, a, b, where);
}

BinaryOp compoundBase(BinaryOp op) {
    switch (op) {
        case BINARY_OP_ADD_EQUALS: return BINARY_OP_ADD;
        case BINARY_OP_SUB_EQUALS: return BINARY_OP_SUB;
        case BINARY_OP_MUL_EQUALS: return BINARY_OP_MUL;
        case BINARY_OP_DIV_EQUALS: return BINARY_OP_DIV;
        case BINARY_OP_MOD_EQUALS: return BINARY_OP_MOD;
        case BINARY_OP_BITAND_EQUALS: return BINARY_OP_BITAND;
        case BINARY_OP_BITOR_EQUALS: return BINARY_OP_BITOR;
        case BINARY_OP_BITXOR_EQUALS: return BINARY_OP_BITXOR;
        case BINARY_OP_SHIFTLEFT_EQUALS: return BINARY_OP_SHIFTLEFT;
        case BINARY_OP_SHIFTRIGHT_EQUALS: return BINARY_OP_SHIFTRIGHT;
        case BINARY_OP_USHIFTRIGHT_EQUALS: return BINARY_OP_USHIFTRIGHT;
        default: return op;
    }
}

// ---- memory ----------------------------------------------------------------

Val loadElem(Mem& m, uint64_t idx, const Where& where) {
    if (idx >= m.count)
        undefined("`" + m.name + "[" + std::to_string(idx) + "]` reads out of bounds (" +
                  std::to_string(m.count) + " elements; " + where + ")");
    if (m.shared && !m.written[idx])
        undefined("`" + m.name + "[" + std::to_string(idx) + "]` is read before any "
                  "work-item wrote it (" + where + ")");
    const uint8_t* p = m.data + idx * bytesOf(m.elem.prim);
    switch (m.elem.prim) {
        case Prim::F32: { float f; std::memcpy(&f, p, 4); return mkFloat(Prim::F32, f); }
        case Prim::F64: { double d; std::memcpy(&d, p, 8); return mkFloat(Prim::F64, d); }
        case Prim::F16: { uint16_t h; std::memcpy(&h, p, 2); return mkFloat(Prim::F16, halfToFloat(h)); }
        case Prim::BF16: { uint16_t h; std::memcpy(&h, p, 2); return mkFloat(Prim::BF16, bf16ToFloat(h)); }
        default: {
            uint64_t raw = 0;
            std::memcpy(&raw, p, bytesOf(m.elem.prim));   // little-endian host
            return mkInt(m.elem, raw);
        }
    }
}

void storeElem(Mem& m, uint64_t idx, const Val& v, const Where& where) {
    if (idx >= m.count)
        undefined("`" + m.name + "[" + std::to_string(idx) + "]` writes out of bounds (" +
                  std::to_string(m.count) + " elements; " + where + ")");
    Val c = convert(v, m.elem);
    uint8_t* p = m.data + idx * bytesOf(m.elem.prim);
    switch (m.elem.prim) {
        case Prim::F32: { float f = (float) c.f; std::memcpy(p, &f, 4); break; }
        case Prim::F64: std::memcpy(p, &c.f, 8); break;
        case Prim::F16: { uint16_t h = floatToHalf((float) c.f); std::memcpy(p, &h, 2); break; }
        case Prim::BF16: { uint16_t h = floatToBf16((float) c.f); std::memcpy(p, &h, 2); break; }
        default: std::memcpy(p, &c.i, bytesOf(m.elem.prim)); break;
    }
    if (m.shared) m.written[idx] = 1;
    m.version.fetch_add(1, std::memory_order_relaxed);
}

uint64_t indexOf(const Val& v, const Where& where) {
    if (v.k != Val::Scalar || isFloat(v.t.prim) || v.t.prim == Prim::Bool)
        refuse("an index that is not an integer (" + where + ")");
    if (v.t.sgn && v.s64() < 0) undefined("negative index " + show(v) + " (" + where + ")");
    return v.t.sgn ? (uint64_t) v.s64() : v.u64();
}

// ---- types the interpreter models -----------------------------------------

std::string canonical(const CajetaTypePtr& t) { return t ? t->toCanonical() : ""; }

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

CajetaTypePtr typeArg(const CajetaTypePtr& t, size_t i) {
    auto c = std::dynamic_pointer_cast<CajetaClass>(t);
    if (!c || c->getTypeArguments().size() <= i) return nullptr;
    return c->getTypeArguments()[i];
}

int64_t constArg(const CajetaTypePtr& t, size_t i) {
    auto c = std::dynamic_pointer_cast<CajetaConstantType>(typeArg(t, i));
    return c ? c->getValue() : -1;
}

bool isBuffer(const CajetaTypePtr& t) { return startsWith(canonical(t), "cajeta.xpu.KernelBuffer"); }
bool isShared(const CajetaTypePtr& t) { return startsWith(canonical(t), "cajeta.xpu.Shared"); }

std::string simpleName(const std::shared_ptr<CajetaClass>& c) {
    std::string q = c ? c->toCanonical() : "";
    return q.substr(q.find_last_of('.') + 1);
}

// `name(args)` or `Cls.name(args)` to a @Device method, by name and arity:
// unqualified and `Self.name` resolve in the calling class, `Other.name` in
// the class of that simple or canonical name. Null when nothing matches.
MethodPtr resolveDevice(const std::shared_ptr<CajetaClass>& self, const std::string& recv,
                        const std::string& name, size_t argc) {
    auto findIn = [&](const std::shared_ptr<CajetaClass>& c) -> MethodPtr {
        if (!c) return nullptr;
        for (auto& kv : c->getMethods()) {
            const MethodPtr& m = kv.second;
            if (m && m->getName() == name && isDevice(*m) && m->getParameters().size() == argc)
                return m;
        }
        return nullptr;
    };
    if (recv.empty() || recv == simpleName(self)) return findIn(self);
    for (auto& kv : CajetaType::getCanonicalMap()) {
        auto c = std::dynamic_pointer_cast<CajetaClass>(kv.second);
        if (!c) continue;
        if (simpleName(c) == recv || c->toCanonical() == recv)
            if (auto m = findIn(c)) return m;
    }
    return nullptr;
}

std::string qualified(const MethodPtr& m) {
    auto c = m->getParent();
    return (c ? c->toCanonical() + "." : std::string()) + m->getName();
}

// ---- the reference semantics of each built-in, by name ---------------------
//
// Every built-in the interpreter runs is listed here; anything else is
// refused before the kernel runs. The semantics are written down in
// docs/specification/xpu/ReferenceInterpreter.md.

const std::set<std::string>& staticBuiltins() {
    static const std::set<std::string> s = {
        "KernelThread.x", "KernelThread.y", "KernelThread.z",
        "KernelThread.globalIdX", "KernelThread.globalIdY", "KernelThread.globalIdZ",
        "Workgroup.x", "Workgroup.y", "Workgroup.z",
        "Workgroup.dimX", "Workgroup.dimY", "Workgroup.dimZ",
        "Barrier.workgroup", "Barrier.wave", "Barrier.workgroupMemory", "Barrier.deviceMemory",
        "Wave.width", "Wave.laneId", "Wave.isFirstLane",
        "Wave.shuffleSync", "Wave.ballotSync", "Wave.rotate",
        "Wave.reduceSum", "Wave.reduceMax", "Wave.reduceMin",
        "Wave.reduceAnd", "Wave.reduceOr", "Wave.reduceXor",
        "Wave.reduceSumF32", "Wave.reduceMaxF32",
        "Wave.reduceSumF32Segmented", "Wave.reduceMaxF32Segmented",
        "Wave.prefixSum", "Wave.prefixProduct",
        "Group.width", "Group.laneId", "Group.rowId", "Group.reduce", "Group.reduceSegmented",
        "Bits.reverse", "Bits.count", "Bits.rotateLeft", "Bits.rotateRight",
        "Cajeta.bitsToF32", "Cajeta.f32ToBits", "Cajeta.bitsToF64", "Cajeta.f64ToBits",
        "Math.min", "Math.max", "Math.abs", "Math.fma",
        "Math.sqrt", "Math.floor", "Math.ceil", "Math.trunc", "Math.round", "Math.rsqrt",
        "Math.sin", "Math.cos", "Math.tan", "Math.asin", "Math.acos", "Math.atan", "Math.atan2",
        "Math.exp", "Math.exp2", "Math.log", "Math.log2", "Math.log10", "Math.pow",
    };
    return s;
}

const std::set<std::string>& vectorMethods() {
    static const std::set<std::string> s = {
        "dotAccum", "dot", "dotSum", "asUnsigned", "asSigned", "asWords", "asBytes",
        "widenLo", "widenHi", "narrow", "toF32", "toF16", "toI32",
        "bitcastF32", "bitcastI32", "lut4",
    };
    return s;
}

const std::set<std::string>& atomics() {
    static const std::set<std::string> s = {
        "atomicAdd", "atomicSub", "atomicMin", "atomicMax", "atomicAnd", "atomicOr",
        "atomicXor", "atomicExchange", "atomicCompareExchange",
    };
    return s;
}

// The built-ins that are a rendezvous: every live member of the scope must
// arrive before any of them continues.
bool isCollective(const std::string& q) {
    if (q == "Barrier.workgroup" || q == "Barrier.wave" || q == "Group.reduce") return true;
    if (!startsWith(q, "Wave.")) return false;
    return q != "Wave.width" && q != "Wave.laneId" && q != "Wave.isFirstLane";
}

enum class LocalKind { Scalar, Vector, Buffer, Shared, Tile };

const std::map<LocalKind, std::set<std::string>>& memberBuiltins() {
    static const std::map<LocalKind, std::set<std::string>> m = {
        {LocalKind::Buffer, {"vload", "vstore", "atomicAdd", "atomicSub", "atomicMin",
                             "atomicMax", "atomicAnd", "atomicOr", "atomicXor",
                             "atomicExchange", "atomicCompareExchange"}},
        {LocalKind::Shared, {"vload", "vstore", "atomicAdd", "atomicSub", "atomicMin",
                             "atomicMax", "atomicAnd", "atomicOr", "atomicXor",
                             "atomicExchange", "atomicCompareExchange"}},
        {LocalKind::Vector, {"dotAccum", "dot", "dotSum", "asUnsigned", "asSigned", "asWords",
                             "asBytes", "widenLo", "widenHi", "narrow", "toF32", "toF16",
                             "toI32", "bitcastF32", "bitcastI32", "lut4"}},
        {LocalKind::Tile, {"splat", "load", "store", "mma"}},
        {LocalKind::Scalar, {}},
    };
    return m;
}

// ---- literals --------------------------------------------------------------

Val parseFloatLiteral(const FloatLiteralExpression* fl) {
    std::string text = fl->getRawValue();
    // The host's rule: an `f` suffix is float32, anything else float64.
    bool f32 = !text.empty() && (text.back() == 'f' || text.back() == 'F');
    if (!text.empty() && std::strchr("fFdD", text.back())) text.pop_back();
    return mkFloat(f32 ? Prim::F32 : Prim::F64,
                   f32 ? (double) std::strtof(text.c_str(), nullptr)
                       : std::strtod(text.c_str(), nullptr));
}

Val parseIntLiteral(const IntegerLiteralExpression* il) {
    unsigned radix = 10;
    size_t prefix = 0;
    switch (il->getIntegerLiteralType()) {
        case INTEGER_LITERAL_TYPE_BINARY: radix = 2; prefix = 2; break;
        case INTEGER_LITERAL_TYPE_OCT: radix = 8; break;
        case INTEGER_LITERAL_TYPE_HEX: radix = 16; prefix = 2; break;
        default: break;
    }
    std::string text = il->getRawValue();
    if (prefix && text.size() >= prefix) text.erase(0, prefix);
    bool isLong = !text.empty() && (text.back() == 'l' || text.back() == 'L');
    if (isLong) text.pop_back();
    text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
    uint64_t v = text.empty() ? 0 : std::stoull(text, nullptr, (int) radix);
    // Past int32, an unsuffixed literal is read as int64, so it adapts to a
    // uint32 or int64 sibling with its value intact.
    Val out = mkInt({isLong || v > 0x7FFFFFFFull ? Prim::I64 : Prim::I32, true}, v);
    out.lit = true;
    return out;
}

// ---- the refusal pass ------------------------------------------------------

// What the survey resolved, by node: the compiler's type registries are
// thread_local, so nothing that needs them may be looked up again from a
// work-item's thread.
struct Bindings {
    llvm::DenseMap<const Expression*, int32_t> enums;
    llvm::DenseMap<const Expression*, MethodPtr> helpers;
    // Each identifier the run reads: the frame slot of the local or parameter
    // it names, or the value of the template constant it names.
    struct Name { int slot = -1; bool isConst = false; Val c; };
    llvm::DenseMap<const Expression*, Name> names;
    llvm::DenseMap<const void*, int> declSlots;          // VariableDeclarator -> slot
    llvm::DenseMap<const void*, std::pair<int, int>> forSlots;   // for-each -> element, iterator
    llvm::DenseMap<const Method*, std::vector<int>> paramSlots;
    llvm::DenseMap<const Method*, int> frameSize;
    // Every literal, parsed once, so workgroups on other threads only read.
    llvm::DenseMap<const Expression*, Val> literals;
};

// A non-type parameter of `cls`'s class template instantiation, at its
// declared width and signedness, or nullopt.
std::optional<Val> templateConst(const std::shared_ptr<CajetaClass>& cls, const std::string& n) {
    if (!cls) return std::nullopt;
    const auto& params = cls->getTypeParameters();
    const auto& args = cls->getTypeArguments();
    for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
        if (!params[i].isNonType || params[i].name != n) continue;
        auto c = std::dynamic_pointer_cast<CajetaConstantType>(args[i]);
        if (!c) return std::nullopt;
        const std::string& prim = params[i].nonTypePrimitive;
        unsigned bits = 32;
        size_t d = prim.find_first_of("0123456789");
        if (d != std::string::npos) bits = (unsigned) std::stoul(prim.substr(d));
        return mkInt({intOfBits(bits), prim.empty() || prim[0] != 'u'}, (uint64_t) c->getValue());
    }
    return std::nullopt;
}

class Survey {
public:
    std::vector<std::string> refused;
    bool usesCollective = false;
    bool usesWave = false;
    bool usesAtomic = false;
    Bindings bindings;

    void kernel(const MethodPtr& m) { function(m, ""); }

private:
    std::map<std::string, LocalKind> kinds;
    std::shared_ptr<CajetaClass> cls;
    std::string prefix;              // "in <helper>: " inside a helper's body
    bool inHelper = false;
    std::set<const Method*> visited;
    // The function being surveyed: its lexical scopes, innermost last, each a
    // list of (name, slot), and the next free slot of its frame.
    std::vector<std::vector<std::pair<std::string, int>>> lex;
    int nextSlot = 0;

    int declare(const std::string& n) {
        lex.back().push_back({n, nextSlot});
        return nextSlot++;
    }
    int slotOf(const std::string& n) const {
        for (auto s = lex.rbegin(); s != lex.rend(); ++s)
            for (auto v = s->rbegin(); v != s->rend(); ++v)
                if (v->first == n) return v->second;
        return -1;
    }
    // Bind an identifier the run will read: a local or parameter, else a
    // template constant; a name that binds neither is refused.
    void bindName(const ExpressionPtr& e, const std::string& n) {
        if (int sl = slotOf(n); sl >= 0) {
            bindings.names[e.get()].slot = sl;
            return;
        }
        if (auto c = templateConst(cls, n)) {
            auto& b = bindings.names[e.get()];
            b.isConst = true;
            b.c = *c;
            return;
        }
        note("the name `" + n + "`, which binds no local, parameter or template value", e);
    }

    void function(const MethodPtr& m, const std::string& pre) {
        cls = m->getParent();
        prefix = pre;
        kinds.clear();
        lex.assign(1, {});
        nextSlot = 0;
        std::vector<int>& params = bindings.paramSlots[m.get()];
        params.clear();
        for (auto& p : m->getParameterList()) {
            if (!p || p->getName() == "this") continue;
            params.push_back(declare(p->getName()));
            CajetaTypePtr t = p->getType();
            if (isBuffer(t)) {
                if (!primOf(typeArg(t, 0)))
                    note("parameter `" + p->getName() + "` of type " + canonical(t),
                         m->getBlock());
                kinds[p->getName()] = LocalKind::Buffer;
            } else if (primOf(t)) {
                kinds[p->getName()] = LocalKind::Scalar;
            } else if (auto v = std::dynamic_pointer_cast<CajetaVector>(t);
                       v && inHelper && primOf(v->getElementType())) {
                kinds[p->getName()] = LocalKind::Vector;
            } else {
                note("parameter `" + p->getName() + "` of type " + canonical(t), m->getBlock());
            }
        }
        if (inHelper) {
            CajetaTypePtr rt = m->getReturnType();
            auto v = std::dynamic_pointer_cast<CajetaVector>(rt);
            if (rt && !primOf(rt) && canonical(rt) != "void"
                    && !(v && primOf(v->getElementType())))
                note("returning " + canonical(rt), m->getBlock());
        }
        stmt(m->getBlock());
        bindings.frameSize[m.get()] = nextSlot;
    }

    // Survey a helper's body once, in its own frame, its refusals prefixed
    // with its name.
    void helper(const MethodPtr& m) {
        if (!visited.insert(m.get()).second) return;
        auto savedKinds = kinds;
        auto savedCls = cls;
        auto savedPrefix = prefix;
        auto savedLex = lex;
        int savedNext = nextSlot;
        bool savedIn = inHelper;
        inHelper = true;
        function(m, "in " + qualified(m) + ": ");
        kinds = savedKinds;
        cls = savedCls;
        prefix = savedPrefix;
        lex = savedLex;
        nextSlot = savedNext;
        inHelper = savedIn;
    }

    void note(const std::string& what, const AbstractSyntaxNodePtr& n) {
        refused.push_back(prefix + what + " (" + at(n) + ")");
    }

    void stmt(const AbstractSyntaxNodePtr& n) {
        if (!n) return;
        if (auto b = std::dynamic_pointer_cast<Block>(n)) {
            lex.emplace_back();
            for (auto& s : b->getChildren()) stmt(s);
            lex.pop_back();
        } else if (auto ls = std::dynamic_pointer_cast<LabelStatement>(n)) {
            stmt(ls->getBlock());
        } else if (auto il = std::dynamic_pointer_cast<IdentifierLabel>(n)) {
            stmt(il->getBody());
        } else if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(n)) {
            decl(lvd);
        } else if (auto is = std::dynamic_pointer_cast<IfStatement>(n)) {
            expr(is->getCondition());
            stmt(is->getThenBranch());
            stmt(is->getElseBranch());
        } else if (auto fs = std::dynamic_pointer_cast<ForStatement>(n)) {
            lex.emplace_back();
            stmt(fs->getInit());
            expr(fs->getCondition());
            for (auto& u : fs->getUpdate()) expr(u);
            stmt(fs->getBody());
            lex.pop_back();
        } else if (auto ef = std::dynamic_pointer_cast<EnhancedForStatement>(n)) {
            auto mc = std::dynamic_pointer_cast<MethodCallExpression>(ef->getIterableExpr());
            auto recv = mc ? std::dynamic_pointer_cast<IdentifierExpression>(child(mc, 0)) : nullptr;
            bool stripe = recv && recv->getTextValue() == "Group" && mc->getMethodCallName() == "stripe";
            bool range = recv && mc->getMethodCallName() == "range"
                && kinds.count(recv->getTextValue())
                && kinds[recv->getTextValue()] == LocalKind::Buffer;
            if ((!stripe && !range) || mc->getParameters().size() != 1) {
                note("a for-each over something other than `Group.stripe(n)` or `buf.range(n)`", n);
                return;
            }
            if (stripe) usesWave = true;
            else bindName(recv, recv->getTextValue());
            expr(mc->getParameters()[0].expression);
            lex.emplace_back();
            kinds[ef->getElementName()] = LocalKind::Scalar;
            int el = declare(ef->getElementName()), it = -1;
            if (ef->getIteratorType()) {
                kinds[ef->getIteratorName()] = LocalKind::Scalar;
                it = declare(ef->getIteratorName());
            }
            bindings.forSlots[ef.get()] = {el, it};
            stmt(ef->getBody());
            lex.pop_back();
        } else if (auto ws = std::dynamic_pointer_cast<WhileStatement>(n)) {
            expr(ws->getCondition());
            stmt(ws->getBody());
        } else if (auto ds = std::dynamic_pointer_cast<DoStatement>(n)) {
            stmt(ds->getBody());
            expr(ds->getCondition());
        } else if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(n)) {
            expr(es->getExpression());
        } else if (auto rs = std::dynamic_pointer_cast<ReturnStatement>(n)) {
            if (rs->getExpression()) {
                if (!inHelper) note("a return with a value", n);
                else expr(rs->getExpression());
            }
        } else if (std::dynamic_pointer_cast<BreakStatement>(n)
                   || std::dynamic_pointer_cast<ContinueStatement>(n)) {
        } else if (auto e = std::dynamic_pointer_cast<Expression>(n)) {
            expr(e);
        } else {
            note("this statement form", n);
        }
    }

    void decl(const std::shared_ptr<LocalVariableDeclaration>& lvd) {
        CajetaTypePtr t = lvd->getType();
        LocalKind k;
        if (primOf(t)) k = LocalKind::Scalar;
        else if (auto v = std::dynamic_pointer_cast<CajetaVector>(t)) {
            k = LocalKind::Vector;
            if (!primOf(v->getElementType())) note("a vector of " + canonical(t), lvd);
        } else if (isCooperativeMatrixType(t)) {
            k = LocalKind::Tile;
            if (!primOf(typeArg(t, 0)) || constArg(t, 1) <= 0 || constArg(t, 2) <= 0)
                note("the tile type " + canonical(t), lvd);
        } else if (isShared(t) && primOf(typeArg(t, 0))) {
            k = LocalKind::Shared;
        } else {
            note("a local of type " + canonical(t), lvd);
            return;
        }
        for (auto& vd : lvd->getVariableDeclarators()) {
            if (!vd) continue;
            kinds[vd->getIdentifier()] = k;
            auto init = vd->getInitializer();
            auto e = init && !init->getChildren().empty()
                ? std::dynamic_pointer_cast<Expression>(init->getChildren()[0]) : nullptr;
            if (k == LocalKind::Shared) {
                auto ne = std::dynamic_pointer_cast<NewExpression>(e);
                auto acr = ne ? std::dynamic_pointer_cast<ArrayCreatorRest>(ne->getCreatorRest())
                              : nullptr;
                if (!ne || !ne->getSharedAlloc() || !acr || acr->getChildren().size() != 1)
                    note("a Shared<T> local not initialized by `shared T[n]`", lvd);
                else expr(std::dynamic_pointer_cast<Expression>(acr->getChildren()[0]));
            } else if (k == LocalKind::Tile) {
                if (e) note("an initialized tile local", lvd);
            } else if (e) {
                expr(e);
            }
            bindings.declSlots[vd.get()] = declare(vd->getIdentifier());
        }
    }

    void call(const std::shared_ptr<MethodCallExpression>& mc) {
        const std::string& name = mc->getMethodCallName();
        auto recvE = child(mc, 0);
        auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(recvE);
        for (auto& p : mc->getParameters()) expr(p.expression);
        const size_t argc = mc->getParameters().size();
        if (!recvE) {
            if (auto m = resolveDevice(cls, "", name, argc)) {
                bindings.helpers[mc.get()] = m;
                helper(m);
                return;
            }
            note("the call `" + name + "`, which names no @Device helper", mc);
            return;
        }
        if (!recvId) {
            expr(recvE);
            if (vectorMethods().count(name)) return;   // a vector-valued receiver
            note("the call `" + name + "` on a computed receiver", mc);
            return;
        }
        const std::string& r = recvId->getTextValue();
        if (auto k = kinds.find(r); k != kinds.end() && slotOf(r) >= 0) {
            bindName(recvId, r);
            const auto& ok = memberBuiltins().at(k->second);
            if (!ok.count(name)) note("`" + r + "." + name + "`", mc);
            if (startsWith(name, "atomic")) usesAtomic = true;
            return;
        }
        std::string q = r + "." + name;
        if (!staticBuiltins().count(q)) {
            if (auto m = resolveDevice(cls, r, name, argc)) {
                bindings.helpers[mc.get()] = m;
                helper(m);
                return;
            }
            note("`" + q + "`", mc);
            return;
        }
        if (isCollective(q)) usesCollective = true;
        if (startsWith(q, "Wave.") || startsWith(q, "Group.") || q == "Barrier.wave")
            usesWave = true;
    }

    void expr(const ExpressionPtr& e) {
        if (!e) return;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            bindName(e, id->getTextValue());
            return;
        }
        if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(e)) {
            bindings.literals[e.get()] = parseIntLiteral(il.get());
            return;
        }
        if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(e)) {
            bindings.literals[e.get()] = parseFloatLiteral(fl.get());
            return;
        }
        if (auto tl = std::dynamic_pointer_cast<TextLiteralExpression>(e)) {
            if (tl->getLiteralType() != LITERAL_TYPE_BOOL) note("a string literal", e);
            return;
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(e)) { call(mc); return; }
        if (auto ne = std::dynamic_pointer_cast<NewExpression>(e)) {
            auto ccr = std::dynamic_pointer_cast<ClassCreatorRest>(ne->getCreatorRest());
            const auto& ta = ne->getTypeArguments();
            if (ne->getTypeName() != "Vector" || ta.size() != 2 || !ccr || !primOf(ta[0]))
                note("`new " + ne->getTypeName() + "`", e);
            else for (auto& p : ccr->getParameters()) expr(p.expression);
            return;
        }
        if (auto cast = std::dynamic_pointer_cast<CastExpression>(e)) {
            CajetaTypePtr ct = cast->getResolvedType() ? cast->getResolvedType() : cast->getDestType();
            if (!primOf(ct)) note("a cast to " + canonical(ct), e);
            expr(child(cast, 0));
            return;
        }
        if (auto dot = std::dynamic_pointer_cast<DotExpression>(e)) {
            auto lhs = std::dynamic_pointer_cast<IdentifierExpression>(child(dot, 0));
            if (lhs && kinds.count(lhs->getTextValue())
                    && kinds[lhs->getTextValue()] == LocalKind::Vector
                    && slotOf(lhs->getTextValue()) >= 0) {
                bindName(lhs, lhs->getTextValue());
                return;
            }
            if (lhs)
                if (auto c = CajetaType::lookupEnumConstant(lhs->getTextValue(), dot->getIdentifier())) {
                    bindings.enums[e.get()] = *c;
                    return;
                }
            note("the field access `." + dot->getIdentifier() + "`", e);
            return;
        }
        if (std::dynamic_pointer_cast<BinaryOpExpression>(e)
                || std::dynamic_pointer_cast<PrefixExpression>(e)
                || std::dynamic_pointer_cast<PostfixExpression>(e)
                || std::dynamic_pointer_cast<ArrayIndexExpression>(e)
                || std::dynamic_pointer_cast<MoveExpression>(e)) {
            for (auto& c : e->getChildren()) expr(std::dynamic_pointer_cast<Expression>(c));
            return;
        }
        note("the expression " + std::string(ownership::toString(e->kind())), e);
    }
};

// ---- execution -------------------------------------------------------------

// Raised in a work-item thread to unwind it when another work-item failed.
struct Abort {};

struct Group;

// What a work-item is waiting at.
struct Arrival {
    std::string what;           // "Barrier.workgroup", "Wave.reduceSum", ...
    const void* site = nullptr; // the call node
    Where where;
    Val arg, arg2;
    Val result;
};

struct ItemSync {
    enum State { Ready, Waiting, Done } st = Ready;
    Arrival arrival;
};

struct Compiled;

struct Run {
    MethodPtr kernel;
    std::shared_ptr<CajetaClass> cls;
    Launch launch;
    uint32_t wave = 0;
    std::map<std::string, Mem> buffers;
    std::map<std::string, Val> scalars;
    Bindings bindings;
    const Compiled* compiled = nullptr;
    bool hasDeadline = false;
    std::chrono::steady_clock::time_point deadline;
};

// Work-items of a workgroup with a rendezvous run as fibers on the calling
// thread: each runs until it reaches a barrier or a wave collective, then
// switches back to the scheduler. A switch costs what a call does, where an
// OS-thread handoff cost microseconds, and everything stays on the thread
// that owns the compiler's thread_local registries.
class Fibers {
public:
    using Entry = void (*)(void*, uint32_t);

    Fibers() = default;
    Fibers(const Fibers&) = delete;
    Fibers& operator=(const Fibers&) = delete;
    ~Fibers() {
#ifdef _WIN32
        for (void* f : fibers_) if (f) DeleteFiber(f);
        if (converted_) ConvertFiberToThread();
#endif
    }

    // Prepare `n` fibers, each to run entry(arg, index) when first resumed.
    void start(uint32_t n, Entry entry, void* arg) {
        entry_ = entry;
        arg_ = arg;
#ifdef _WIN32
        if (!converted_ && !IsThreadAFiber()) {
            main_ = ConvertThreadToFiber(nullptr);
            converted_ = true;
        } else if (!main_) {
            main_ = GetCurrentFiber();
        }
        for (void* f : fibers_) if (f) DeleteFiber(f);
        fibers_.assign(n, nullptr);
        slots_.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            slots_[i] = {this, i};
            fibers_[i] = CreateFiber(kStack, &Fibers::trampolineWin, &slots_[i]);
        }
#else
        ctx_.resize(n);
        if (stacks_.size() < n) stacks_.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            // Left uninitialized: the pages are committed only as a stack grows.
            if (!stacks_[i]) stacks_[i].reset(new char[kStack]);
            getcontext(&ctx_[i]);
            ctx_[i].uc_stack.ss_sp = stacks_[i].get();
            ctx_[i].uc_stack.ss_size = kStack;
            ctx_[i].uc_link = &main_;
            uintptr_t self = (uintptr_t) this;
            makecontext(&ctx_[i], (void (*)()) &Fibers::trampolinePosix, 3,
                        (unsigned) (self & 0xFFFFFFFFu), (unsigned) (self >> 32), i);
        }
#endif
    }

    // From the scheduler: run fiber `i` until it yields or ends.
    void resume(uint32_t i) {
#ifdef _WIN32
        SwitchToFiber(fibers_[i]);
#else
        swapcontext(&main_, &ctx_[i]);
#endif
    }

    // From fiber `i`: hand control back to the scheduler.
    void yield(uint32_t i) {
#ifdef _WIN32
        (void) i;
        SwitchToFiber(main_);
#else
        swapcontext(&ctx_[i], &main_);
#endif
    }

private:
    static constexpr size_t kStack = 512 * 1024;
    Entry entry_ = nullptr;
    void* arg_ = nullptr;
#ifdef _WIN32
    struct Slot { Fibers* self; uint32_t i; };
    std::vector<Slot> slots_;
    std::vector<void*> fibers_;
    void* main_ = nullptr;
    bool converted_ = false;
    static void WINAPI trampolineWin(void* p) {
        Slot* s = (Slot*) p;
        s->self->entry_(s->self->arg_, s->i);
        SwitchToFiber(s->self->main_);
    }
#else
    ucontext_t main_;
    std::vector<ucontext_t> ctx_;
    std::vector<std::unique_ptr<char[]>> stacks_;
    static void trampolinePosix(unsigned lo, unsigned hi, unsigned i) {
        Fibers* self = (Fibers*) (((uintptr_t) hi << 32) | (uintptr_t) lo);
        self->entry_(self->arg_, i);
        // Returning ends the fiber; uc_link resumes the scheduler.
    }
#endif
};

struct Group {
    Run& run;
    uint32_t id[3];
    uint32_t size = 1;
    std::map<const void*, std::unique_ptr<Mem>> shared;
    Fibers* fibers = nullptr;
    bool abort = false;
    uint64_t steps = 0;           // statements run, for the budget check
    std::exception_ptr failure;
    std::vector<ItemSync> sync;
    // Tile operations memoized by their exact inputs: a lane whose splat,
    // load or mma matches one another lane already ran takes its result. The
    // entry keeps its inputs alive, so a freed tile's address cannot match.
    struct TileKey {
        int op;
        const void* a; const void* b; const void* c;
        uint64_t x, y, z, w;
        bool operator<(const TileKey& o) const {
            return std::tie(op, a, b, c, x, y, z, w) < std::tie(o.op, o.a, o.b, o.c, o.x, o.y, o.z, o.w);
        }
    };
    struct TileEntry {
        std::shared_ptr<const TileData> out;
        std::shared_ptr<const TileData> in[3];
    };
    std::map<TileKey, TileEntry> tileMemo;
    explicit Group(Run& r) : run(r) {}
};

enum class Flow { Next, Break, Continue, Return };

// ---- compiled kernels --------------------------------------------------------
//
// Before a launch runs, each kernel and helper body is compiled once into a
// tree of nodes over 8-byte scalars: types, literal adaptation, conversions
// and slots are settled at compile time, so a node does its operation and
// nothing else. A construct the compiler does not specialize (vectors, tiles,
// collectives, most calls) compiles to a fallback node that hands its subtree
// to the walker below, so the semantics live in one place.
// CAJETA_XPU_REF_WALK=1 runs everything through the walker instead.

class Item;

union SV {
    uint64_t i;
    double f;
};

struct CExpr {
    using Fn = SV (*)(const CExpr&, Item&);
    Fn fn = nullptr;
    Ty t;                       // the result's static type
    bool lit = false;           // a literal (or literals folded): adapts to its sibling
    const CExpr* a = nullptr;
    const CExpr* b = nullptr;
    int slot = -1;              // a local or a buffer's slot
    SV k{};                     // a constant
    BinaryOp op = BINARY_OP_ADD;
    int kind = 0;               // the operation's family, per fn
    Ty at, bt;                  // operand types, as the operation reads them
    unsigned w = 0;             // integer operation width
    bool sgn = false;           // signed operation / extension
    bool flag = false;          // per fn: postfix, compare-signed, ...
    Expression* src = nullptr;  // for messages and the fallback
};

struct CStmt {
    using Fn = Flow (*)(const CStmt&, Item&);
    Fn fn = nullptr;
    std::vector<const CStmt*> kids;
    const CStmt* s0 = nullptr;
    const CStmt* s1 = nullptr;
    const CStmt* s2 = nullptr;
    const CExpr* e = nullptr;
    std::vector<const CExpr*> upd;
    int slot = -1, slot2 = -1, slot3 = -1;
    bool flag = false;
    Ty t;
    AbstractSyntaxNode* src = nullptr;
};

struct Compiled {
    std::deque<CExpr> exprs;
    std::deque<CStmt> stmts;
    llvm::DenseMap<const Method*, const CStmt*> bodies;
};

class Item {
public:
    Item(Group& g, uint32_t linear, bool threaded)
        : G(g), R(g.run), me(linear), threaded(threaded), cls(g.run.cls) {
        const uint32_t* b = R.launch.block;
        tid[0] = linear % b[0];
        tid[1] = (linear / b[0]) % b[1];
        tid[2] = linear / (b[0] * b[1]);
    }

    void body() {
        frame.assign((size_t) R.bindings.frameSize.lookup(R.kernel.get()), Slot{});
        const std::vector<int>& ps = R.bindings.paramSlots.find(R.kernel.get())->second;
        size_t i = 0;
        for (auto& p : R.kernel->getParameterList()) {
            if (!p || p->getName() == "this") continue;
            Slot& sl = frame[(size_t) ps[i++]];
            if (auto b = R.buffers.find(p->getName()); b != R.buffers.end()) {
                sl.v.k = Val::MemRef;
                sl.v.mem = &b->second;
                sl.v.t = b->second.elem;
            } else {
                sl.v = R.scalars.at(p->getName());
            }
        }
        runBody(R.kernel.get(), R.kernel->getBlock());
    }

    // A function's body, compiled when it was compiled.
    void runBody(const Method* m, const BlockPtr& block) {
        if (R.compiled) {
            auto it = R.compiled->bodies.find(m);
            if (it != R.compiled->bodies.end()) {
                run(*it->second);
                return;
            }
        }
        exec(block);
    }

    // ---- compiled nodes: each the one operation it was compiled to ----

    Flow run(const CStmt& s) { return s.fn(s, *this); }
    SV ev(const CExpr& e) { return e.fn(e, *this); }

    static Val toVal(SV v, const Ty& t) {
        if (isFloat(t.prim)) return mkFloat(t.prim, v.f);
        return mkInt(t, v.i);
    }
    static SV toSV(const Val& v) {
        SV r;
        if (isFloat(v.t.prim)) r.f = v.f; else r.i = v.i;
        return r;
    }
    static SV cvt(SV v, const Ty& from, const Ty& to) {
        if (from == to) return v;
        return toSV(convert(toVal(v, from), to));
    }
    void tick() {
        if (R.hasDeadline && (++G.steps & 0xFFF) == 0
                && std::chrono::steady_clock::now() > R.deadline)
            throw Exception("the reference run of " + R.kernel->getName() +
                            " outlasted its budget", "XPU-REF03");
    }

    static SV xConst(const CExpr& n, Item&) { return n.k; }
    static SV xLocal(const CExpr& n, Item& it) { return toSV(it.frame[(size_t) n.slot].v); }
    static SV xFallback(const CExpr& n, Item& it) {
        Val v = it.eval(n.src);
        if (v.k != Val::Scalar) refuse("a value that is not a scalar where one was compiled (" + at(n.src) + ")");
        if (!(v.t == n.t)) v = convert(v, n.t);
        return toSV(v);
    }
    static SV xThread(const CExpr& n, Item& it) {
        const uint32_t* b = it.R.launch.block;
        SV r;
        switch (n.kind) {
            case 0: case 1: case 2: r.i = it.tid[n.kind]; break;
            case 3: case 4: case 5: r.i = (uint64_t) it.G.id[n.kind - 3] * b[n.kind - 3] + it.tid[n.kind - 3]; break;
            case 6: case 7: case 8: r.i = it.G.id[n.kind - 6]; break;
            case 9: case 10: case 11: r.i = b[n.kind - 9]; break;
            case 12: r.i = it.R.wave; break;
            default: r.i = it.R.wave ? it.me % it.R.wave : 0; break;
        }
        r.i &= maskOf(bitsOf(n.t.prim));
        return r;
    }
    static uint64_t index(const CExpr& ix, Item& it, const Where& w) {
        return indexOf(toVal(it.ev(ix), ix.t), w);
    }
    static SV xLoad(const CExpr& n, Item& it) {
        Mem& m = *it.frame[(size_t) n.slot].v.mem;
        return toSV(loadElem(m, index(*n.a, it, n.src), n.src));
    }
    static SV xCast(const CExpr& n, Item& it) { return cvt(it.ev(*n.a), n.a->t, n.t); }
    static SV xNeg(const CExpr& n, Item& it) {
        SV v = it.ev(*n.a);
        if (isFloat(n.t.prim)) { v.f = roundTo(n.t.prim, -v.f); return v; }
        v.i = (0 - v.i) & maskOf(bitsOf(n.t.prim));
        return v;
    }
    static SV xBitNot(const CExpr& n, Item& it) {
        SV v = it.ev(*n.a);
        v.i = ~v.i & maskOf(bitsOf(n.t.prim));
        return v;
    }
    static SV xNot(const CExpr& n, Item& it) {
        SV v = it.ev(*n.a);
        v.i = v.i ? 0 : 1;
        return v;
    }
    static SV xAnd(const CExpr& n, Item& it) {
        SV r;
        r.i = it.ev(*n.a).i && it.ev(*n.b).i ? 1 : 0;
        return r;
    }
    static SV xOr(const CExpr& n, Item& it) {
        SV r;
        r.i = it.ev(*n.a).i || it.ev(*n.b).i ? 1 : 0;
        return r;
    }
    // A binary operator over two scalars of settled types. kind 0: boolean,
    // 1: float in n.at's precision, 2: integer at width n.w.
    static SV xBinary(const CExpr& n, Item& it) {
        SV x = cvt(it.ev(*n.a), n.a->t, n.at);
        SV y = cvt(it.ev(*n.b), n.b->t, n.bt);
        return binop(n, x, y);
    }
    static SV binop(const CExpr& n, SV x, SV y) {
        SV r;
        if (n.kind == 0) {
            switch (n.op) {
                case BINARY_OP_EQ: r.i = x.i == y.i; break;
                case BINARY_OP_NE: r.i = x.i != y.i; break;
                case BINARY_OP_BITAND: r.i = x.i & y.i; break;
                case BINARY_OP_BITOR: r.i = x.i | y.i; break;
                default: r.i = x.i ^ y.i; break;
            }
            return r;
        }
        if (n.kind == 1) {
            bool isCmp = false, cmp = false;
            double d = floatOp(n.op, n.at.prim, x.f, y.f, isCmp, cmp);
            if (isCmp) r.i = cmp ? 1 : 0;
            else r.f = roundTo(n.at.prim, d);
            return r;
        }
        // Integers, by scalarOp's rules: each operand extended by its own
        // signedness to the wider width, unsigned when either operand is,
        // compared signed when either is, `>>` by the shifted operand.
        const unsigned w = n.w;
        const uint64_t mask = maskOf(w);
        uint64_t a = (n.at.sgn ? (uint64_t) sext(x.i, bitsOf(n.at.prim)) : x.i) & mask;
        uint64_t b = (n.bt.sgn ? (uint64_t) sext(y.i, bitsOf(n.bt.prim)) : y.i) & mask;
        switch (n.op) {
            case BINARY_OP_LT: r.i = n.flag ? sext(a, w) < sext(b, w) : a < b; return r;
            case BINARY_OP_LE: r.i = n.flag ? sext(a, w) <= sext(b, w) : a <= b; return r;
            case BINARY_OP_GT: r.i = n.flag ? sext(a, w) > sext(b, w) : a > b; return r;
            case BINARY_OP_GE: r.i = n.flag ? sext(a, w) >= sext(b, w) : a >= b; return r;
            case BINARY_OP_EQ: r.i = a == b; return r;
            case BINARY_OP_NE: r.i = a != b; return r;
            case BINARY_OP_ADD: r.i = (a + b) & mask; return r;
            case BINARY_OP_SUB: r.i = (a - b) & mask; return r;
            case BINARY_OP_MUL: r.i = (a * b) & mask; return r;
            case BINARY_OP_BITAND: r.i = a & b; return r;
            case BINARY_OP_BITOR: r.i = a | b; return r;
            case BINARY_OP_BITXOR: r.i = a ^ b; return r;
            default: break;
        }
        // Division, remainder and shifts, with their undefined cases.
        return toSV(scalarOp(n.op, toVal(x, n.at), toVal(y, n.bt), n.src));
    }
    static SV xAssignLocal(const CExpr& n, Item& it) {
        SV v = cvt(it.ev(*n.a), n.a->t, n.t);
        Val& dst = it.frame[(size_t) n.slot].v;
        dst = toVal(v, n.t);
        return v;
    }
    static SV xAssignElem(const CExpr& n, Item& it) {
        Mem& m = *it.frame[(size_t) n.slot].v.mem;
        uint64_t idx = index(*n.b, it, n.src);
        SV v = cvt(it.ev(*n.a), n.a->t, n.t);
        storeElem(m, idx, toVal(v, n.t), n.src);
        return v;
    }
    // ++ and -- on a local; n.flag: postfix, n.sgn: increment.
    static SV xIncDec(const CExpr& n, Item& it) {
        Val& cur = it.frame[(size_t) n.slot].v;
        Val old = cur;
        Val one = isFloat(old.t.prim) ? mkFloat(old.t.prim, 1) : mkInt(old.t, 1);
        cur = convert(binaryOp(n.sgn ? BINARY_OP_ADD : BINARY_OP_SUB, old, one, n.src), old.t);
        return toSV(n.flag ? old : cur);
    }

    static Flow sBlock(const CStmt& s, Item& it) {
        for (const CStmt* k : s.kids) {
            Flow f = it.run(*k);
            if (f != Flow::Next) return f;
        }
        return Flow::Next;
    }
    static Flow sExpr(const CStmt& s, Item& it) { it.ev(*s.e); return Flow::Next; }
    static Flow sFallback(const CStmt& s, Item& it) { return it.exec(s.src); }
    static Flow sDecl(const CStmt& s, Item& it) {
        it.frame[(size_t) s.slot].v = toVal(cvt(it.ev(*s.e), s.e->t, s.t), s.t);
        return Flow::Next;
    }
    static Flow sIf(const CStmt& s, Item& it) {
        if (it.ev(*s.e).i) return s.s1 ? it.run(*s.s1) : Flow::Next;
        return s.s2 ? it.run(*s.s2) : Flow::Next;
    }
    // A loop body's outcome: true to leave the loop, with `out` what to return.
    bool leave(Flow f, Flow& out) {
        if (f == Flow::Return) { out = f; return true; }
        if (f == Flow::Break) { if (!mine("")) out = f; return true; }
        if (f == Flow::Continue && !mine("")) { out = f; return true; }
        return false;
    }
    static Flow sWhile(const CStmt& s, Item& it) {
        Flow out = Flow::Next;
        while (it.ev(*s.e).i) {
            it.tick();
            if (s.s1 && it.leave(it.run(*s.s1), out)) break;
        }
        return out;
    }
    static Flow sDo(const CStmt& s, Item& it) {
        Flow out = Flow::Next;
        do {
            it.tick();
            if (s.s1 && it.leave(it.run(*s.s1), out)) break;
        } while (it.ev(*s.e).i);
        return out;
    }
    static Flow sFor(const CStmt& s, Item& it) {
        if (s.s0) it.run(*s.s0);
        Flow out = Flow::Next;
        while (!s.e || it.ev(*s.e).i) {
            it.tick();
            if (s.s1 && it.leave(it.run(*s.s1), out)) break;
            for (const CExpr* u : s.upd) it.ev(*u);
        }
        return out;
    }
    static Flow sBreak(const CStmt& s, Item& it) {
        it.flowLabel = static_cast<BreakStatement*>(s.src)->getLabel();
        return Flow::Break;
    }
    static Flow sContinue(const CStmt& s, Item& it) {
        it.flowLabel = static_cast<ContinueStatement*>(s.src)->getLabel();
        return Flow::Continue;
    }
    // Group.stripe(n) (s.flag) or buf.range(n): as the walker runs them.
    static Flow sForEach(const CStmt& s, Item& it) {
        const Ty& idxTy = s.t;
        uint64_t limit = cvt(it.ev(*s.e), s.e->t, idxTy).i & maskOf(bitsOf(idxTy.prim));
        uint64_t start, step;
        Mem* buf = nullptr;
        if (s.flag) {
            start = it.me % it.R.wave;
            step = it.R.wave;
        } else {
            start = (uint64_t) it.G.id[0] * it.R.launch.block[0] + it.tid[0];
            step = (uint64_t) it.R.launch.grid[0] * it.R.launch.block[0];
            buf = it.frame[(size_t) s.slot3].v.mem;
        }
        Flow out = Flow::Next;
        for (uint64_t i = start; i < limit; i += step) {
            it.tick();
            if (s.flag) {
                it.frame[(size_t) s.slot].v = mkInt(idxTy, i);
            } else {
                if (s.slot2 >= 0) it.frame[(size_t) s.slot2].v = mkInt(idxTy, i);
                it.frame[(size_t) s.slot].v = loadElem(*buf, i, s.src);
            }
            if (s.s1 && it.leave(it.run(*s.s1), out)) break;
        }
        return out;
    }

private:
    friend class KernelCompiler;
    Group& G;
    Run& R;
    uint32_t me;
    bool threaded;
    uint32_t tid[3];
    struct Slot { Val v; };
    // The running function's locals and parameters, by the slot the survey
    // gave each declaration.
    std::vector<Slot> frame;
    std::string flowLabel;
    std::shared_ptr<CajetaClass> cls;   // the class whose code is running
    Val retVal;                         // a helper's `return e`
    std::set<const Method*> active;     // helpers on the call stack

    // -- names --
    // The slot an identifier names, or null when it names a template constant.
    Slot* slotOf(Expression* id) {
        auto it = R.bindings.names.find(id);
        if (it == R.bindings.names.end() || it->second.isConst) return nullptr;
        return &frame[(size_t) it->second.slot];
    }

    Val name(const std::string& n, Expression* e) {
        auto it = R.bindings.names.find(e);
        if (it == R.bindings.names.end())
            refuse("the name `" + n + "` binds nothing the interpreter knows (" + at(e) + ")");
        if (it->second.isConst) return it->second.c;
        return frame[(size_t) it->second.slot].v;
    }

    // -- statements --
    Flow exec(const AbstractSyntaxNodePtr& n) { return exec(n.get()); }
    Flow exec(AbstractSyntaxNode* n) {
        if (!n) return Flow::Next;
        if (R.hasDeadline && (++G.steps & 0xFFF) == 0
                && std::chrono::steady_clock::now() > R.deadline)
            throw Exception("the reference run of " + R.kernel->getName() +
                            " outlasted its budget", "XPU-REF03");
        // The common statements by exact type first: a typeid compare where the
        // cast chain below pays a failed dynamic_cast per kind it passes.
        const std::type_info& ti = typeid(*n);
        if (ti == typeid(ExpressionStatement)) {
            eval(static_cast<ExpressionStatement*>(n)->getExpression().get());
            return Flow::Next;
        }
        if (ti == typeid(IfStatement)) {
            auto* is = static_cast<IfStatement*>(n);
            if (cond(is->getCondition().get())) return exec(is->getThenBranch().get());
            return exec(is->getElseBranch().get());
        }
        if (auto b = dynamic_cast<Block*>(n)) {
            Flow f = Flow::Next;
            for (auto& s : b->getChildren()) {
                f = exec(s);
                if (f != Flow::Next) break;
            }
            return f;
        }
        if (auto ls = dynamic_cast<LabelStatement*>(n)) return exec(ls->getBlock());
        if (auto il = dynamic_cast<IdentifierLabel*>(n)) {
            pendingLabel = il->getIdentifier();
            Flow f = exec(il->getBody());
            pendingLabel.clear();
            return f;
        }
        if (ti == typeid(LocalVariableDeclaration)) {
            decl(static_cast<LocalVariableDeclaration*>(n));
            return Flow::Next;
        }
        if (auto lvd = dynamic_cast<LocalVariableDeclaration*>(n)) {
            decl(lvd);
            return Flow::Next;
        }
        if (auto is = dynamic_cast<IfStatement*>(n)) {
            if (cond(is->getCondition())) return exec(is->getThenBranch());
            return exec(is->getElseBranch());
        }
        if (auto fs = dynamic_cast<ForStatement*>(n)) {
            std::string label = takeLabel();
            exec(fs->getInit());
            Flow out = Flow::Next;
            while (!fs->getCondition() || cond(fs->getCondition())) {
                Flow f = exec(fs->getBody());
                if (f == Flow::Return) { out = f; break; }
                if (f == Flow::Break) { if (mine(label)) break; out = f; break; }
                if (f == Flow::Continue && !mine(label)) { out = f; break; }
                for (auto& u : fs->getUpdate()) eval(u);
            }
            return out;
        }
        if (auto ef = dynamic_cast<EnhancedForStatement*>(n)) {
            std::string label = takeLabel();
            auto mc = dynamic_cast<MethodCallExpression*>(ef->getIterableExpr().get());
            const std::string recv =
                dynamic_cast<IdentifierExpression*>(child(mc, 0))->getTextValue();
            const bool stripe = recv == "Group";
            Val count = eval(mc->getParameters()[0].expression);
            // Group.stripe(n): this lane, then every wave-width lanes after it.
            // buf.range(n): this work-item's global x, then every grid-width after.
            CajetaTypePtr it = stripe ? ef->getElementType() : ef->getIteratorType();
            Ty idxTy = it && primOf(it) ? *primOf(it) : Ty{Prim::I32, true};
            uint64_t start = stripe ? me % R.wave
                                    : (uint64_t) G.id[0] * R.launch.block[0] + tid[0];
            uint64_t step = stripe ? R.wave : (uint64_t) R.launch.grid[0] * R.launch.block[0];
            uint64_t limit = convert(count, idxTy).u64() & maskOf(bitsOf(idxTy.prim));
            Mem* buf = nullptr;
            if (!stripe)
                if (Slot* bs = slotOf(child(mc, 0)); bs && bs->v.k == Val::MemRef) buf = bs->v.mem;
            auto fsl = R.bindings.forSlots.find(ef);
            const int elSlot = fsl->second.first, itSlot = fsl->second.second;
            if (!stripe && !buf) refuse("`" + recv + ".range` on something not a buffer (" + at(n) + ")");
            for (uint64_t i = start; i < limit; i += step) {
                if (stripe) {
                    frame[(size_t) elSlot].v = mkInt(idxTy, i);
                } else {
                    if (itSlot >= 0) frame[(size_t) itSlot].v = mkInt(idxTy, i);
                    frame[(size_t) elSlot].v = loadElem(*buf, i, n);
                }
                Flow f = exec(ef->getBody());
                if (f == Flow::Return) return f;
                if (f == Flow::Break) { if (mine(label)) break; return f; }
                if (f == Flow::Continue && !mine(label)) return f;
            }
            return Flow::Next;
        }
        if (auto ws = dynamic_cast<WhileStatement*>(n)) {
            std::string label = takeLabel();
            while (cond(ws->getCondition())) {
                Flow f = exec(ws->getBody());
                if (f == Flow::Return) return f;
                if (f == Flow::Break) { if (mine(label)) break; return f; }
                if (f == Flow::Continue && !mine(label)) return f;
            }
            return Flow::Next;
        }
        if (auto ds = dynamic_cast<DoStatement*>(n)) {
            std::string label = takeLabel();
            do {
                Flow f = exec(ds->getBody());
                if (f == Flow::Return) return f;
                if (f == Flow::Break) { if (mine(label)) break; return f; }
                if (f == Flow::Continue && !mine(label)) return f;
            } while (cond(ds->getCondition()));
            return Flow::Next;
        }
        if (auto bs = dynamic_cast<BreakStatement*>(n)) {
            flowLabel = bs->getLabel();
            return Flow::Break;
        }
        if (auto cs = dynamic_cast<ContinueStatement*>(n)) {
            flowLabel = cs->getLabel();
            return Flow::Continue;
        }
        if (auto rs = dynamic_cast<ReturnStatement*>(n)) {
            if (rs->getExpression()) retVal = eval(rs->getExpression());
            return Flow::Return;
        }
        if (auto es = dynamic_cast<ExpressionStatement*>(n)) {
            eval(es->getExpression());
            return Flow::Next;
        }
        if (auto e = dynamic_cast<Expression*>(n)) {
            eval(e);
            return Flow::Next;
        }
        refuse("this statement form (" + at(n) + ")");
    }

    std::string pendingLabel;
    std::string takeLabel() { std::string l = pendingLabel; pendingLabel.clear(); return l; }
    // A break or continue with no label is the innermost loop's; a labelled one
    // is the loop carrying that label.
    bool mine(const std::string& label) {
        if (flowLabel.empty() || flowLabel == label) { flowLabel.clear(); return true; }
        return false;
    }

    bool cond(const ExpressionPtr& e) { return cond(e.get()); }
    bool cond(Expression* e) {
        Val v = eval(e);
        if (v.k != Val::Scalar || v.t.prim != Prim::Bool)
            refuse("a condition that is not boolean (" + at(e) + ")");
        return v.i != 0;
    }

    void decl(LocalVariableDeclaration* lvd) {
        CajetaTypePtr t = lvd->getType();
        for (auto& vd : lvd->getVariableDeclarators()) {
            if (!vd) continue;
            const std::string& nm = vd->getIdentifier();
            auto init = vd->getInitializer();
            auto e = init && !init->getChildren().empty()
                ? dynamic_cast<Expression*>(init->getChildren()[0].get()) : nullptr;
            Slot s;
            if (auto p = primOf(t)) {
                s.v = e ? convert(eval(e), *p) : (isFloat(p->prim) ? mkFloat(p->prim, 0) : mkInt(*p, 0));
            } else if (auto vt = std::dynamic_pointer_cast<CajetaVector>(t)) {
                Ty et = *primOf(vt->getElementType());
                if (e) {
                    Val v = eval(e);
                    if (v.k != Val::Vector || v.lanes.size() != vt->getLanes())
                        refuse("initializing `" + nm + "` from a value that is not a " +
                               canonical(t) + " (" + at(e) + ")");
                    s.v = v;
                    for (auto& l : s.v.lanes) l = convert(l, et);
                } else {
                    s.v.k = Val::Vector;
                    s.v.lanes.assign(vt->getLanes(), isFloat(et.prim) ? mkFloat(et.prim, 0) : mkInt(et, 0));
                }
                s.v.t = et;
            } else if (isCooperativeMatrixType(t)) {
                auto tile = std::make_shared<Tile>();
                tile->elem = *primOf(typeArg(t, 0));
                tile->rows = (uint32_t) constArg(t, 1);
                tile->cols = (uint32_t) constArg(t, 2);
                tile->use = (uint32_t) constArg(t, 3);
                tile->data = zeroTile(*tile);
                s.v.k = Val::TileRef;
                s.v.t = tile->elem;
                s.v.tile = tile;
            } else if (isShared(t)) {
                s.v = sharedArray(nm, t, e, vd.get());
            } else {
                refuse("a local of type " + canonical(t) + " (" + at(lvd) + ")");
            }
            frame[(size_t) R.bindings.declSlots.lookup(vd.get())] = s;
        }
    }

    // One Shared array per declaration per workgroup: the first work-item to
    // reach the declaration makes it, and every other binds the same storage.
    Val sharedArray(const std::string& nm, const CajetaTypePtr& t, Expression* e,
                    const void* site) {
        auto ne = dynamic_cast<NewExpression*>(e);
        auto acr = dynamic_cast<ArrayCreatorRest*>(ne->getCreatorRest().get());
        Val n = eval(dynamic_cast<Expression*>(acr->getChildren()[0].get()));
        uint64_t count = indexOf(n, e);
        auto& slot = G.shared[site];
        if (!slot) {
            slot = std::make_unique<Mem>();
            slot->name = nm;
            slot->elem = *primOf(typeArg(t, 0));
            slot->shared = true;
            slot->count = count;
            slot->own.assign(count * bytesOf(slot->elem.prim), 0);
            slot->data = slot->own.data();
            slot->written.assign(count, 0);
        } else if (slot->count != count) {
            undefined("work-items of one workgroup size Shared array `" + nm + "` differently (" +
                      std::to_string(slot->count) + " and " + std::to_string(count) + ")");
        }
        Val v;
        v.k = Val::MemRef;
        v.mem = slot.get();
        v.t = slot->elem;
        return v;
    }

    // -- expressions --
    Val eval(const ExpressionPtr& e) { return eval(e.get()); }
    Val eval(Expression* e) {
        switch (e->kind()) {
            case ExprKind::Identifier:
                return name(static_cast<IdentifierExpression*>(e)->getTextValue(), e);
            case ExprKind::IntegerLiteral:
            case ExprKind::FloatLiteral: {
                auto c = R.bindings.literals.find(e);
                if (c != R.bindings.literals.end()) return c->second;
                return e->kind() == ExprKind::IntegerLiteral
                    ? parseIntLiteral(static_cast<IntegerLiteralExpression*>(e))
                    : parseFloatLiteral(static_cast<FloatLiteralExpression*>(e));
            }
            case ExprKind::BinaryOp:
                return binary(static_cast<BinaryOpExpression*>(e));
            case ExprKind::MethodCall:
                return call(static_cast<MethodCallExpression*>(e));
            default:
                break;
        }
        if (auto id = dynamic_cast<IdentifierExpression*>(e))
            return name(id->getTextValue(), e);
        if (auto il = dynamic_cast<IntegerLiteralExpression*>(e)) return parseIntLiteral(il);
        if (auto fl = dynamic_cast<FloatLiteralExpression*>(e)) return parseFloatLiteral(fl);
        if (false) {
            std::string text;
            // The host's rule: an `f` suffix is float32, anything else float64.
            bool f32 = !text.empty() && (text.back() == 'f' || text.back() == 'F');
            if (!text.empty() && std::strchr("fFdD", text.back())) text.pop_back();
            return mkFloat(f32 ? Prim::F32 : Prim::F64,
                           f32 ? (double) std::strtof(text.c_str(), nullptr)
                               : std::strtod(text.c_str(), nullptr));
        }
        if (auto tl = dynamic_cast<TextLiteralExpression*>(e))
            return mkBool(tl->getRawValue() == "true");
        if (auto bin = dynamic_cast<BinaryOpExpression*>(e)) return binary(bin);
        if (auto pre = dynamic_cast<PrefixExpression*>(e)) return prefix(pre);
        if (auto post = dynamic_cast<PostfixExpression*>(e)) {
            Val old = eval(child(post, 0));
            Val one = old.k == Val::Scalar && isFloat(old.t.prim) ? mkFloat(old.t.prim, 1) : mkInt(old.t, 1);
            assign(child(post, 0), binaryOp(post->getOp() == POSTFIX_OP_INC ? BINARY_OP_ADD
                                                                          : BINARY_OP_SUB,
                                            old, one, e));
            return old;
        }
        if (auto cast = dynamic_cast<CastExpression*>(e)) {
            CajetaTypePtr ct = cast->getResolvedType() ? cast->getResolvedType() : cast->getDestType();
            Val v = eval(child(cast, 0));
            v.lit = false;
            return convert(v, *primOf(ct));
        }
        if (auto ai = dynamic_cast<ArrayIndexExpression*>(e)) {
            Val base = eval(child(ai, 0));
            Val idx = eval(child(ai, 1));
            if (base.k == Val::MemRef) return loadElem(*base.mem, indexOf(idx, e), e);
            if (base.k == Val::Vector) {
                uint64_t l = indexOf(idx, e);
                if (l >= base.lanes.size())
                    undefined("lane " + std::to_string(l) + " of a " +
                              std::to_string(base.lanes.size()) + "-lane vector (" + at(e) + ")");
                return base.lanes[l];
            }
            refuse("indexing a value that is not a buffer, Shared array or vector (" + at(e) + ")");
        }
        if (auto dot = dynamic_cast<DotExpression*>(e)) {
            auto lhs = dynamic_cast<IdentifierExpression*>(child(dot, 0));
            if (lhs) {
                if (Slot* s = slotOf(lhs); s && s->v.k == Val::Vector) {
                    static const std::string comps = "xyzw";
                    size_t l = comps.find(dot->getIdentifier());
                    if (dot->getIdentifier().size() != 1 || l == std::string::npos
                            || l >= s->v.lanes.size())
                        refuse("the vector component `." + dot->getIdentifier() + "` (" + at(e) + ")");
                    return s->v.lanes[l];
                }
                if (auto c = R.bindings.enums.find(e); c != R.bindings.enums.end())
                    return mkInt({Prim::I32, true}, (uint64_t) (int64_t) c->second);
            }
            refuse("the field access `." + dot->getIdentifier() + "` (" + at(e) + ")");
        }
        if (auto mc = dynamic_cast<MethodCallExpression*>(e)) return call(mc);
        if (auto ne = dynamic_cast<NewExpression*>(e)) {
            const auto& ta = ne->getTypeArguments();
            Ty et = *primOf(ta[0]);
            auto ccr = dynamic_cast<ClassCreatorRest*>(ne->getCreatorRest().get());
            auto cN = std::dynamic_pointer_cast<CajetaConstantType>(ta[1]);
            Val v;
            v.k = Val::Vector;
            v.t = et;
            for (auto& p : ccr->getParameters()) v.lanes.push_back(convert(eval(p.expression), et));
            if (!cN || v.lanes.size() != (size_t) cN->getValue())
                refuse("a Vector built from the wrong number of lanes (" + at(e) + ")");
            return v;
        }
        if (dynamic_cast<MoveExpression*>(e)) return eval(child(e, 0));
        refuse("the expression " + std::string(ownership::toString(e->kind())) + " (" + at(e) + ")");
    }

    Val prefix(PrefixExpression* pre) {
        Expression* operand = child(pre, 0);
        switch (pre->getOp()) {
            case PREFIX_OP_POSITIVE: return eval(operand);
            case PREFIX_OP_NEGATIVE: {
                Val v = eval(operand);
                if (v.k == Val::Scalar && isFloat(v.t.prim)) return mkFloat(v.t.prim, -v.f);
                Val zero = mkInt(v.t, 0);
                zero.lit = v.lit;
                Val r = binaryOp(BINARY_OP_SUB, zero, v, pre);
                r.lit = v.lit;
                return r;
            }
            case PREFIX_OP_BITNOT: {
                Val v = eval(operand);
                Val r = mkInt(v.t, ~v.i);
                r.lit = v.lit;
                return r;
            }
            case PREFIX_OP_LOGNOT: return mkBool(!cond(operand));
            case PREFIX_OP_INC:
            case PREFIX_OP_DEC: {
                Val old = eval(operand);
                Val one = isFloat(old.t.prim) ? mkFloat(old.t.prim, 1) : mkInt(old.t, 1);
                Val nv = binaryOp(pre->getOp() == PREFIX_OP_INC ? BINARY_OP_ADD : BINARY_OP_SUB,
                                  old, one, pre);
                assign(operand, nv);
                return eval(operand);
            }
        }
        refuse("prefix operator (" + at(pre) + ")");
    }

    Val binary(BinaryOpExpression* bin) {
        BinaryOp op = bin->getBinaryOp();
        Expression* l = child(bin, 0); Expression* r = child(bin, 1);
        if (bin->isAssignment()) {
            Val rv = eval(r);
            if (op != BINARY_OP_ASSIGN) rv = binaryOp(compoundBase(op), eval(l), rv, bin);
            return assign(l, rv);
        }
        if (op == BINARY_OP_LOGAND) return mkBool(cond(l) && cond(r));
        if (op == BINARY_OP_LOGOR) return mkBool(cond(l) || cond(r));
        return binaryOp(op, eval(l), eval(r), bin);
    }

    // Store `v` through the l-value `e`, converted to its declared type.
    // Store `v` through the l-value `e`, converted to its declared type, and
    // return the value as stored.
    Val assign(Expression* e, const Val& v) {
        if (auto id = dynamic_cast<IdentifierExpression*>(e)) {
            Slot* s = slotOf(id);
            if (!s) refuse("assigning `" + id->getTextValue() + "`, which is not a local (" + at(e) + ")");
            if (s->v.k == Val::Scalar) { s->v = convert(v, s->v.t); return s->v; }
            if (s->v.k == Val::Vector && v.k == Val::Vector && v.lanes.size() == s->v.lanes.size()) {
                for (size_t l = 0; l < v.lanes.size(); ++l) s->v.lanes[l] = convert(v.lanes[l], s->v.t);
                return s->v;
            }
            refuse("assigning `" + id->getTextValue() + "` (" + at(e) + ")");
        }
        if (auto ai = dynamic_cast<ArrayIndexExpression*>(e)) {
            Val idx = eval(child(ai, 1));
            if (auto vid = dynamic_cast<IdentifierExpression*>(child(ai, 0))) {
                if (Slot* s = slotOf(vid); s && s->v.k == Val::Vector) {
                    uint64_t l = indexOf(idx, e);
                    if (l >= s->v.lanes.size()) undefined("lane " + std::to_string(l) + " (" + at(e) + ")");
                    s->v.lanes[l] = convert(v, s->v.t);
                    return s->v.lanes[l];
                }
            }
            Val base = eval(child(ai, 0));
            if (base.k != Val::MemRef) refuse("storing through `" + at(e) + "`");
            storeElem(*base.mem, indexOf(idx, e), v, e);
            return convert(v, base.mem->elem);
        }
        refuse("an assignment target that is not a local or an element (" + at(e) + ")");
    }

    std::vector<Val> args(MethodCallExpression* mc) {
        std::vector<Val> out;
        for (auto& p : mc->getParameters()) out.push_back(eval(p.expression));
        return out;
    }

    Val u32(uint64_t v) { return mkInt({Prim::I32, false}, v); }

    Val call(MethodCallExpression* mc) {
        const std::string& nm = mc->getMethodCallName();
        auto recvE = child(mc, 0);
        auto recvId = dynamic_cast<IdentifierExpression*>(recvE);
        Where where{mc};
        auto bound = R.bindings.helpers.find(mc);
        if (!recvE) {
            if (bound != R.bindings.helpers.end()) return deviceCall(bound->second, mc);
            refuse("the call `" + nm + "`, which names no @Device helper (" + where + ")");
        }
        if (recvId && !slotOf(recvId)) {
            std::string q = recvId->getTextValue() + "." + nm;
            if (staticBuiltins().count(q)) return staticCall(q, mc);
            if (bound != R.bindings.helpers.end()) return deviceCall(bound->second, mc);
            refuse("`" + q + "` (" + where + ")");
        }
        Val self = eval(recvE);
        std::vector<Val> a = args(mc);
        if (self.k == Val::MemRef) return bufferCall(self, nm, mc, a);
        if (self.k == Val::Vector && nm == "dotAccum") return dotAccum(self, a, where);
        if (self.k == Val::Vector && vectorMethods().count(nm)) return vectorCall(self, nm, a, where);
        if (self.k == Val::TileRef) return tileCall(*self.tile, nm, a, where);
        refuse("the call `." + nm + "` (" + where + ")");
    }

    // A @Device helper: its own frame, the arguments converted to its
    // parameter types, its result to its return type. Buffers pass by
    // reference, everything else by value.
    Val deviceCall(const MethodPtr& m, MethodCallExpression* mc) {
        Where where{mc};
        if (active.count(m.get())) refuse("a recursive call to " + qualified(m) + " (" + where + ")");
        std::vector<Val> a = args(mc);
        std::vector<Slot> callee((size_t) R.bindings.frameSize.lookup(m.get()));
        const std::vector<int>& ps = R.bindings.paramSlots.find(m.get())->second;
        size_t i = 0;
        for (auto& p : m->getParameterList()) {
            if (!p || p->getName() == "this") continue;
            CajetaTypePtr t = p->getType();
            Val v = a.at(i++);
            if (auto pt = primOf(t)) {
                v = convert(v, *pt);
            } else if (auto vt = std::dynamic_pointer_cast<CajetaVector>(t)) {
                Ty et = *primOf(vt->getElementType());
                if (v.k != Val::Vector || v.lanes.size() != vt->getLanes())
                    refuse("passing a value that is not a " + canonical(t) + " to " + qualified(m) +
                           " (" + where + ")");
                for (auto& l : v.lanes) l = convert(l, et);
                v.t = et;
            } else if (v.k != Val::MemRef) {
                refuse("passing `" + p->getName() + "` to " + qualified(m) + " (" + where + ")");
            }
            callee[(size_t) ps[i - 1]].v = std::move(v);
        }
        struct Restore {
            Item& it;
            std::vector<Slot> frame;
            std::shared_ptr<CajetaClass> cls;
            const Method* m;
            ~Restore() { it.frame = std::move(frame); it.cls = cls; it.active.erase(m); }
        } restore{*this, std::move(frame), cls, m.get()};
        frame = std::move(callee);
        if (m->getParent()) cls = m->getParent();
        active.insert(m.get());
        retVal = Val();
        runBody(m.get(), m->getBlock());
        Val r = retVal;
        retVal = Val();
        CajetaTypePtr rt = m->getReturnType();
        if (auto pt = primOf(rt)) {
            if (r.k == Val::None) undefined(qualified(m) + " ends without returning a value");
            r.lit = false;
            return convert(r, *pt);
        }
        if (auto vt = std::dynamic_pointer_cast<CajetaVector>(rt)) {
            Ty et = *primOf(vt->getElementType());
            for (auto& l : r.lanes) l = convert(l, et);
            r.t = et;
        }
        return r;
    }

    Val bufferCall(const Val& self, const std::string& nm,
                   MethodCallExpression* mc, const std::vector<Val>& a) {
        Where where{mc};
        Mem& m = *self.mem;
        if (nm == "vload") {
            const auto& ta = mc->getExplicitMethodTypeArgs();
            auto cN = ta.size() == 1 ? std::dynamic_pointer_cast<CajetaConstantType>(ta[0]) : nullptr;
            if (!cN || a.size() != 1) refuse("`vload` without a constant lane count (" + where + ")");
            uint64_t base = indexOf(a[0], where);
            Val v;
            v.k = Val::Vector;
            v.t = m.elem;
            for (int64_t l = 0; l < cN->getValue(); ++l) v.lanes.push_back(loadElem(m, base + l, where));
            return v;
        }
        if (nm == "vstore") {
            if (a.size() != 2 || a[1].k != Val::Vector) refuse("`vstore` takes (index, Vector) (" + where + ")");
            uint64_t base = indexOf(a[0], where);
            for (size_t l = 0; l < a[1].lanes.size(); ++l) storeElem(m, base + l, a[1].lanes[l], where);
            return Val();
        }
        if (atomics().count(nm)) return atomic(m, nm, a, where);
        refuse("`" + m.name + "." + nm + "` (" + where + ")");
    }

    // One work-item at a time, so an atomic is a load, an operation and a
    // store, in work-item order; it returns the element's old value.
    Val atomic(Mem& m, const std::string& nm, const std::vector<Val>& a, const Where& where) {
        size_t need = nm == "atomicCompareExchange" ? 3 : 2;
        if (a.size() < need) refuse("`" + nm + "` arity (" + where + ")");
        uint64_t idx = indexOf(a[0], where);
        Val old = loadElem(m, idx, where);
        Val v = convert(a[1], m.elem);
        Val nv;
        if (nm == "atomicExchange") nv = v;
        else if (nm == "atomicCompareExchange") {
            Val desired = convert(a[2], m.elem);
            bool eq = isFloat(m.elem.prim) ? old.f == v.f : old.i == v.i;
            nv = eq ? desired : old;
        } else if (nm == "atomicMin" || nm == "atomicMax") {
            bool lt = scalarOp(BINARY_OP_LT, v, old, where).i != 0;
            if (isFloat(m.elem.prim) && (std::isnan(v.f) || std::isnan(old.f)))
                nv = std::isnan(old.f) ? v : old;          // maxnum / minnum
            else nv = (nm == "atomicMin") == lt ? v : old;
        } else {
            BinaryOp op = nm == "atomicAdd" ? BINARY_OP_ADD : nm == "atomicSub" ? BINARY_OP_SUB
                        : nm == "atomicAnd" ? BINARY_OP_BITAND : nm == "atomicOr" ? BINARY_OP_BITOR
                        : BINARY_OP_BITXOR;
            nv = scalarOp(op, old, v, where);
        }
        storeElem(m, idx, nv, where);
        return old;
    }

    Val vec(Ty et, std::vector<Val> lanes) {
        Val v;
        v.k = Val::Vector;
        v.t = et;
        v.lanes = std::move(lanes);
        return v;
    }

    // The Vector methods, by the host compiler's definitions
    // (MethodCallExpression's Vector branch, VectorOps.h).
    Val vectorCall(const Val& self, const std::string& nm, const std::vector<Val>& a,
                   const Where& where) {
        const Ty et = self.t;
        const size_t n = self.lanes.size();
        const unsigned w = bitsOf(et.prim);
        auto lane = [&](const Val& v, size_t i) -> int64_t {
            const Val& l = v.lanes[i];
            return l.t.sgn ? l.s64() : (int64_t) l.u64();
        };
        auto bad = [&](const std::string& why) -> Val {
            refuse("`." + nm + "` " + why + " (" + where + ")");
        };
        if (nm == "asUnsigned" || nm == "asSigned") {
            if (isFloat(et.prim)) return bad("on a float vector");
            Val out = self;
            out.t.sgn = nm == "asSigned";
            for (auto& l : out.lanes) l.t.sgn = out.t.sgn;
            return out;
        }
        if (nm == "asWords") {
            if (w != 8 || n % 4) return bad("needs 8-bit lanes, a multiple of 4");
            std::vector<Val> out;
            for (size_t j = 0; j < n / 4; ++j) {
                uint64_t word = 0;
                for (size_t k = 0; k < 4; ++k) word |= (self.lanes[j * 4 + k].i & 0xFF) << (8 * k);
                out.push_back(mkInt({Prim::I32, true}, word));
            }
            return vec({Prim::I32, true}, out);
        }
        if (nm == "asBytes") {
            if (w != 32 || isFloat(et.prim)) return bad("needs 32-bit integer lanes");
            std::vector<Val> out;
            for (auto& l : self.lanes)
                for (size_t k = 0; k < 4; ++k) out.push_back(mkInt({Prim::I8, true}, l.i >> (8 * k)));
            return vec({Prim::I8, true}, out);
        }
        if (nm == "widenLo" || nm == "widenHi") {
            if (isFloat(et.prim) || w >= 64 || n < 2) return bad("on these lanes");
            Ty wt{intOfBits(w * 2), et.sgn};
            std::vector<Val> out;
            size_t base = nm == "widenLo" ? 0 : n / 2;
            for (size_t i = 0; i < n / 2; ++i) out.push_back(convert(self.lanes[base + i], wt));
            return vec(wt, out);
        }
        if (nm == "narrow") {
            if (a.size() != 1 || a[0].k != Val::Vector || a[0].lanes.size() != n || w <= 8)
                return bad("takes a vector of the receiver's type");
            Ty nt{intOfBits(w / 2), et.sgn};
            std::vector<Val> out;
            for (auto& l : self.lanes) out.push_back(mkInt(nt, l.i));
            for (auto& l : a[0].lanes) out.push_back(mkInt(nt, l.i));
            return vec(nt, out);
        }
        if (nm == "toF32" || nm == "toF16") {
            Prim p = nm == "toF32" ? Prim::F32 : Prim::F16;
            if (nm == "toF16" && !isFloat(et.prim)) return bad("needs float lanes");
            std::vector<Val> out;
            for (auto& l : self.lanes) out.push_back(convert(l, {p, true}));
            return vec({p, true}, out);
        }
        if (nm == "toI32") {
            if (!isFloat(et.prim)) return bad("needs float lanes");
            std::vector<Val> out;
            for (auto& l : self.lanes) out.push_back(convert(l, {Prim::I32, true}));
            return vec({Prim::I32, true}, out);
        }
        if (nm == "bitcastF32" || nm == "bitcastI32") {
            bool toF = nm == "bitcastF32";
            if (toF == isFloat(et.prim) || w != 32) return bad("on these lanes");
            std::vector<Val> out;
            for (auto& l : self.lanes) {
                if (toF) {
                    uint32_t b = (uint32_t) l.i;
                    float f;
                    std::memcpy(&f, &b, 4);
                    out.push_back(mkFloat(Prim::F32, f));
                } else {
                    float f = (float) l.f;
                    uint32_t b;
                    std::memcpy(&b, &f, 4);
                    out.push_back(mkInt({Prim::I32, true}, b));
                }
            }
            return toF ? vec({Prim::F32, true}, out) : vec({Prim::I32, true}, out);
        }
        if (nm == "lut4") {
            if (w != 8 || a.size() != 1 || a[0].k != Val::Vector || a[0].lanes.size() != 16
                    || bitsOf(a[0].t.prim) != 8)
                return bad("takes a 16-lane 8-bit table");
            std::vector<Val> out;
            for (auto& l : self.lanes) out.push_back(mkInt(et, a[0].lanes[l.i & 15].i));
            return vec(et, out);
        }
        if (nm == "dotSum") {
            if (w != 8 || n % 4 || a.size() != 2 || a[0].k != Val::Vector
                    || a[0].lanes.size() != n)
                return bad("takes (8-bit vector, int32)");
            uint64_t sum = convert(a[1], {Prim::I32, true}).i;
            for (size_t i = 0; i < n; ++i)
                sum += (uint64_t) (lane(self, i) * sext(a[0].lanes[i].i, 8));
            return mkInt({Prim::I32, true}, sum);
        }
        if (nm == "dot") {
            if (a.empty() || a[0].k != Val::Vector || a[0].lanes.size() != n)
                return bad("takes a vector of the same length");
            if (isFloat(et.prim)) {
                Prim p = et.prim == Prim::F64 ? Prim::F64 : Prim::F32;
                Val acc = scalarOp(BINARY_OP_MUL, convert(self.lanes[0], {p, true}),
                                   convert(a[0].lanes[0], {p, true}), where);
                for (size_t i = 1; i < n; ++i)
                    acc = scalarOp(BINARY_OP_ADD, acc,
                                   scalarOp(BINARY_OP_MUL, convert(self.lanes[i], {p, true}),
                                            convert(a[0].lanes[i], {p, true}), where), where);
                return mkFloat(et.prim, acc.f);
            }
            if (w != 8 || n != 4) return bad("on integers needs 4 lanes of 8 bits");
            // Symmetric: both operands take the receiver's signedness.
            uint64_t sum = a.size() == 2 ? convert(a[1], {Prim::I32, true}).i : 0;
            for (size_t i = 0; i < n; ++i) {
                int64_t x = et.sgn ? sext(self.lanes[i].i, 8) : (int64_t) (self.lanes[i].i & 0xFF);
                int64_t y = et.sgn ? sext(a[0].lanes[i].i, 8) : (int64_t) (a[0].lanes[i].i & 0xFF);
                sum += (uint64_t) (x * y);
            }
            return mkInt({Prim::I32, true}, sum);
        }
        return bad("has no reference semantics");
    }

    // dotAccum: unsigned-or-signed weights (the receiver's own signedness)
    // times SIGNED activations, four lanes into each int32 accumulator lane.
    Val dotAccum(const Val& w, const std::vector<Val>& a, const Where& where) {
        if (a.size() != 2 || a[0].k != Val::Vector || a[1].k != Val::Vector)
            refuse("`dotAccum` takes (Vector, Vector) (" + where + ")");
        const Val& act = a[0];
        const Val& acc = a[1];
        if (bitsOf(w.t.prim) != 8 || bitsOf(act.t.prim) != 8 || w.lanes.size() != act.lanes.size()
                || acc.lanes.size() * 4 != w.lanes.size() || bitsOf(acc.t.prim) != 32)
            refuse("`dotAccum` on these lane shapes (" + where + ")");
        Val out = acc;
        for (size_t j = 0; j < acc.lanes.size(); ++j) {
            uint64_t sum = acc.lanes[j].i;
            for (size_t k = 0; k < 4; ++k) {
                const Val& wl = w.lanes[j * 4 + k];
                int64_t wv = w.t.sgn ? wl.s64() : (int64_t) wl.u64();
                int64_t av = sext(act.lanes[j * 4 + k].i, 8);
                sum += (uint64_t) (wv * av);
            }
            out.lanes[j] = mkInt(acc.lanes[j].t, sum);
        }
        return out;
    }

    // Tiles are wave-uniform values; each work-item holds its own copy and every
    // copy is computed identically from the same uniform arguments.
    static uint64_t tyCode(const Ty& t) { return ((uint64_t) t.prim << 1) | (t.sgn ? 1 : 0); }

    // The memoized result of a tile operation, or the one `make` computes.
    std::shared_ptr<const TileData> memo(const Group::TileKey& key,
                                         std::initializer_list<std::shared_ptr<const TileData>> in,
                                         const std::function<std::shared_ptr<const TileData>()>& make) {
        auto it = G.tileMemo.find(key);
        if (it != G.tileMemo.end()) return it->second.out;
        Group::TileEntry e;
        e.out = make();
        size_t n = 0;
        for (auto& d : in) if (n < 3) e.in[n++] = d;
        G.tileMemo.emplace(key, e);
        return e.out;
    }

    std::shared_ptr<const TileData> zeroTile(const Tile& t) {
        Group::TileKey key{0, nullptr, nullptr, nullptr, tyCode(t.elem), t.rows, t.cols, 0};
        return memo(key, {}, [&] {
            auto d = std::make_shared<TileData>();
            if (isFloat(t.elem.prim)) d->f.assign((size_t) t.rows * t.cols, 0.0);
            else d->i.assign((size_t) t.rows * t.cols, 0);
            return std::shared_ptr<const TileData>(d);
        });
    }

    Val tileCall(Tile& t, const std::string& nm, const std::vector<Val>& a, const Where& where) {
        const size_t n = (size_t) t.rows * t.cols;
        const bool fl = isFloat(t.elem.prim);
        if (nm == "splat") {
            if (a.size() != 1) refuse("`splat` takes one value (" + where + ")");
            Val v = convert(a[0], t.elem);
            uint64_t bits;
            if (fl) std::memcpy(&bits, &v.f, 8); else bits = v.i;
            Group::TileKey key{1, nullptr, nullptr, nullptr, tyCode(t.elem), t.rows, t.cols, bits};
            t.data = memo(key, {}, [&] {
                auto d = std::make_shared<TileData>();
                if (fl) d->f.assign(n, v.f); else d->i.assign(n, v.i);
                return std::shared_ptr<const TileData>(d);
            });
            return Val();
        }
        if (nm == "load" || nm == "store") {
            if (a.size() != 4 || a[0].k != Val::MemRef)
                refuse("`" + nm + "` takes (buffer, offset, layout, stride) (" + where + ")");
            Mem& m = *a[0].mem;
            if (!(m.elem == t.elem))
                refuse("a " + tyName(t.elem) + " tile " + nm + " through a " + tyName(m.elem) +
                       " buffer (" + where + ")");
            uint64_t off = indexOf(a[1], where), layout = indexOf(a[2], where),
                     stride = indexOf(a[3], where);
            if (layout > 1) undefined("tile layout " + std::to_string(layout) + " (" + where + ")");
            auto at = [&](uint32_t r, uint32_t c) {
                return off + (layout == 0 ? (uint64_t) r * stride + c : (uint64_t) c * stride + r);
            };
            if (nm == "store") {
                const TileData& d = *t.data;
                for (uint32_t r = 0; r < t.rows; ++r)
                    for (uint32_t c = 0; c < t.cols; ++c) {
                        size_t k = (size_t) r * t.cols + c;
                        storeElem(m, at(r, c), fl ? mkFloat(t.elem.prim, d.f[k]) : mkInt(t.elem, d.i[k]),
                                  where);
                    }
                return Val();
            }
            Group::TileKey key{2, &m, nullptr, nullptr,
                               m.version.load(std::memory_order_relaxed), off,
                               (layout << 40) ^ stride, ((uint64_t) t.rows << 32) | t.cols};
            t.data = memo(key, {}, [&] {
                auto d = std::make_shared<TileData>();
                if (fl) d->f.resize(n); else d->i.resize(n);
                for (uint32_t r = 0; r < t.rows; ++r)
                    for (uint32_t c = 0; c < t.cols; ++c) {
                        Val x = loadElem(m, at(r, c), where);
                        size_t k = (size_t) r * t.cols + c;
                        if (fl) d->f[k] = x.f; else d->i[k] = x.i;
                    }
                return std::shared_ptr<const TileData>(d);
            });
            return Val();
        }
        if (nm == "mma") {
            if (a.size() != 2 || a[0].k != Val::TileRef || a[1].k != Val::TileRef)
                refuse("`mma` takes (A, B) tiles (" + where + ")");
            const Tile& A = *a[0].tile;
            const Tile& B = *a[1].tile;
            if (A.rows != t.rows || B.cols != t.cols || A.cols != B.rows)
                refuse("`mma` on tiles whose shapes do not chain (" + where + ")");
            if (isFloat(A.elem.prim) != fl || isFloat(B.elem.prim) != fl)
                refuse("`mma` mixing float and integer tiles (" + where + ")");
            Group::TileKey key{3, t.data.get(), A.data.get(), B.data.get(), tyCode(t.elem),
                               tyCode(A.elem), tyCode(B.elem), 0};
            t.data = memo(key, {t.data, A.data, B.data}, [&] {
                auto d = std::make_shared<TileData>(*t.data);
                const TileData& ad = *A.data;
                const TileData& bd = *B.data;
                const uint32_t K = A.cols;
                for (uint32_t r = 0; r < t.rows; ++r)
                    for (uint32_t c = 0; c < t.cols; ++c) {
                        size_t k0 = (size_t) r * t.cols + c;
                        if (fl && t.elem.prim == Prim::F64) {
                            double s = d->f[k0];
                            for (uint32_t k = 0; k < K; ++k) {
                                double prod = ad.f[(size_t) r * K + k] * bd.f[(size_t) k * B.cols + c];
                                s = s + prod;
                            }
                            d->f[k0] = s;
                        } else if (fl) {
                            // Accumulated in float32 in k order, one rounding per
                            // product and one per sum.
                            float s = (float) d->f[k0];
                            for (uint32_t k = 0; k < K; ++k) {
                                float prod = (float) ad.f[(size_t) r * K + k] *
                                             (float) bd.f[(size_t) k * B.cols + c];
                                s = s + prod;
                            }
                            d->f[k0] = roundTo(t.elem.prim, (double) s);
                        } else {
                            const unsigned aw = bitsOf(A.elem.prim), bw = bitsOf(B.elem.prim);
                            uint64_t s = d->i[k0];
                            for (uint32_t k = 0; k < K; ++k) {
                                uint64_t x = ad.i[(size_t) r * K + k], y = bd.i[(size_t) k * B.cols + c];
                                int64_t xv = A.elem.sgn ? sext(x, aw) : (int64_t) x;
                                int64_t yv = B.elem.sgn ? sext(y, bw) : (int64_t) y;
                                s += (uint64_t) (xv * yv);
                            }
                            d->i[k0] = s & maskOf(bitsOf(t.elem.prim));
                        }
                    }
                return std::shared_ptr<const TileData>(d);
            });
            return Val();
        }
        refuse("the tile operation `" + nm + "` (" + where + ")");
    }

    Val staticCall(const std::string& q, MethodCallExpression* mc) {
        Where where{mc};
        const uint32_t* b = R.launch.block;
        if (q == "KernelThread.x") return u32(tid[0]);
        if (q == "KernelThread.y") return u32(tid[1]);
        if (q == "KernelThread.z") return u32(tid[2]);
        if (q == "KernelThread.globalIdX") return u32(G.id[0] * b[0] + tid[0]);
        if (q == "KernelThread.globalIdY") return u32(G.id[1] * b[1] + tid[1]);
        if (q == "KernelThread.globalIdZ") return u32(G.id[2] * b[2] + tid[2]);
        if (q == "Workgroup.x") return u32(G.id[0]);
        if (q == "Workgroup.y") return u32(G.id[1]);
        if (q == "Workgroup.z") return u32(G.id[2]);
        if (q == "Workgroup.dimX") return u32(b[0]);
        if (q == "Workgroup.dimY") return u32(b[1]);
        if (q == "Workgroup.dimZ") return u32(b[2]);
        // Memory fences order nothing here: every access is already sequentially
        // consistent, one work-item at a time.
        if (q == "Barrier.workgroupMemory" || q == "Barrier.deviceMemory") return Val();
        if (q == "Wave.width") return u32(R.wave);
        if (q == "Wave.laneId") return u32(me % R.wave);
        if (q == "Wave.isFirstLane") return mkBool(me % R.wave == 0);
        if (q == "Group.width") return mkInt({Prim::I32, true}, R.wave);
        if (q == "Group.laneId") return mkInt({Prim::I32, true}, me % R.wave);
        if (q == "Group.rowId") return mkInt({Prim::I32, true}, G.id[0]);
        if (startsWith(q, "Cajeta.")) return bitCast(q, args(mc), where);
        if (startsWith(q, "Bits.")) {
            std::vector<Val> a = args(mc);
            if (a.empty()) refuse("`" + q + "` without its value (" + where + ")");
            uint32_t v = (uint32_t) convert(a[0], {Prim::I32, false}).i;
            uint32_t k = a.size() > 1 ? (uint32_t) convert(a[1], {Prim::I32, false}).i & 31 : 0;
            uint32_t r = 0;
            if (q == "Bits.count") { for (; v; v &= v - 1) ++r; }
            else if (q == "Bits.reverse") { for (int b = 0; b < 32; ++b) if (v >> b & 1) r |= 1u << (31 - b); }
            else if (q == "Bits.rotateLeft") r = k ? (v << k) | (v >> (32 - k)) : v;
            else r = k ? (v >> k) | (v << (32 - k)) : v;
            return u32(r);
        }
        if (startsWith(q, "Math.")) return math(q, args(mc), where);
        std::vector<Val> a = args(mc);
        Arrival arr;
        arr.what = q;
        arr.site = mc;
        arr.where = where;
        if (q == "Group.reduce") {
            if (a.size() != 2) refuse("`Group.reduce` takes (GroupOp, value) (" + where + ")");
            arr.what = a[0].i == 0 ? "Wave.reduceSumF32" : "Wave.reduceMaxF32";
            arr.arg = convert(a[1], {Prim::F32, true});
        } else if (q == "Group.reduceSegmented") {
            if (a.size() != 3)
                refuse("`Group.reduceSegmented` takes (segment, GroupOp, value) (" + where + ")");
            arr.what = a[1].i == 0 ? "Wave.reduceSumF32Segmented" : "Wave.reduceMaxF32Segmented";
            arr.arg = convert(a[2], {Prim::F32, true});
            arr.arg2 = convert(a[0], {Prim::I32, false});
        } else if (q.rfind("Wave.", 0) == 0) {
            // Each argument at its declared parameter type.
            bool f32 = q == "Wave.reduceSumF32" || q == "Wave.reduceMaxF32"
                    || q == "Wave.reduceSumF32Segmented" || q == "Wave.reduceMaxF32Segmented";
            Ty pt = f32 ? Ty{Prim::F32, true} : Ty{Prim::I32, false};
            if (q == "Wave.ballotSync") pt = {Prim::Bool, false};
            if (a.empty()) refuse("`" + q + "` without its value (" + where + ")");
            arr.arg = (q == "Wave.shuffleSync" || q == "Wave.rotate") ? a[0] : convert(a[0], pt);
            if (a.size() > 1) arr.arg2 = convert(a[1], {Prim::I32, false});
        }
        return rendezvous(arr);
    }

    Val bitCast(const std::string& q, const std::vector<Val>& a, const Where& where) {
        if (a.size() != 1) refuse("`" + q + "` takes one value (" + where + ")");
        if (q == "Cajeta.bitsToF32") {
            uint32_t b = (uint32_t) convert(a[0], {Prim::I32, true}).i;
            float f;
            std::memcpy(&f, &b, 4);
            return mkFloat(Prim::F32, f);
        }
        if (q == "Cajeta.f32ToBits") {
            float f = (float) convert(a[0], {Prim::F32, true}).f;
            uint32_t b;
            std::memcpy(&b, &f, 4);
            return mkInt({Prim::I32, true}, b);
        }
        if (q == "Cajeta.bitsToF64") {
            uint64_t b = convert(a[0], {Prim::I64, true}).i;
            double d;
            std::memcpy(&d, &b, 8);
            return mkFloat(Prim::F64, d);
        }
        double d = convert(a[0], {Prim::F64, true}).f;
        uint64_t b;
        std::memcpy(&b, &d, 8);
        return mkInt({Prim::I64, true}, b);
    }

    // The Math intrinsics, in the argument's float type, element-wise over a
    // vector. sqrt, floor, ceil, trunc, round and fma are exact or correctly
    // rounded; the transcendentals are computed in float64 and rounded to the
    // argument's precision, so a backend's device library is compared within
    // a stated bound.
    Val math(const std::string& q, const std::vector<Val>& a, const Where& where) {
        if (!a.empty() && a[0].k == Val::Vector) {
            std::vector<Val> out;
            for (size_t i = 0; i < a[0].lanes.size(); ++i) {
                std::vector<Val> la;
                for (auto& x : a) la.push_back(x.k == Val::Vector ? x.lanes.at(i) : x);
                out.push_back(math(q, la, where));
            }
            return vec(out.empty() ? a[0].t : out[0].t, out);
        }
        static const std::map<std::string, double (*)(double)> unary = {
            {"Math.sin", [](double x) { return std::sin(x); }},
            {"Math.cos", [](double x) { return std::cos(x); }},
            {"Math.tan", [](double x) { return std::tan(x); }},
            {"Math.asin", [](double x) { return std::asin(x); }},
            {"Math.acos", [](double x) { return std::acos(x); }},
            {"Math.atan", [](double x) { return std::atan(x); }},
            {"Math.exp", [](double x) { return std::exp(x); }},
            {"Math.exp2", [](double x) { return std::exp2(x); }},
            {"Math.log", [](double x) { return std::log(x); }},
            {"Math.log2", [](double x) { return std::log2(x); }},
            {"Math.log10", [](double x) { return std::log10(x); }},
            {"Math.rsqrt", [](double x) { return 1.0 / std::sqrt(x); }},
            {"Math.floor", [](double x) { return std::floor(x); }},
            {"Math.ceil", [](double x) { return std::ceil(x); }},
            {"Math.trunc", [](double x) { return std::trunc(x); }},
            {"Math.round", [](double x) { return std::round(x); }},
        };
        auto fp = [&](const Val& v) {
            if (isFloat(v.t.prim)) return v;
            return convert(v, {Prim::F32, true});
        };
        if (auto u = unary.find(q); u != unary.end()) {
            if (a.size() != 1) refuse("`" + q + "` takes one value (" + where + ")");
            Val x = fp(a[0]);
            return mkFloat(x.t.prim, u->second(x.f));
        }
        if (q == "Math.sqrt") {
            if (a.size() != 1) refuse("`Math.sqrt` takes one value (" + where + ")");
            Val x = fp(a[0]);
            if (x.t.prim == Prim::F64) return mkFloat(Prim::F64, std::sqrt(x.f));
            return mkFloat(x.t.prim, (double) std::sqrt((float) x.f));
        }
        if (q == "Math.fma") {
            if (a.size() != 3) refuse("`Math.fma` takes three values (" + where + ")");
            Prim p = widerFloat(widerFloat(fp(a[0]).t.prim, fp(a[1]).t.prim), fp(a[2]).t.prim);
            double x = convert(a[0], {p, true}).f, y = convert(a[1], {p, true}).f,
                   z = convert(a[2], {p, true}).f;
            if (p == Prim::F64) return mkFloat(p, std::fma(x, y, z));
            return mkFloat(p, (double) std::fma((float) x, (float) y, (float) z));
        }
        if (q == "Math.atan2" || q == "Math.pow") {
            if (a.size() != 2) refuse("`" + q + "` takes two values (" + where + ")");
            Prim p = widerFloat(fp(a[0]).t.prim, fp(a[1]).t.prim);
            double x = convert(a[0], {p, true}).f, y = convert(a[1], {p, true}).f;
            return mkFloat(p, q == "Math.pow" ? std::pow(x, y) : std::atan2(x, y));
        }
        if (q == "Math.abs") {
            if (a.size() != 1) refuse("`Math.abs` arity (" + where + ")");
            const Val& v = a[0];
            if (isFloat(v.t.prim)) return mkFloat(v.t.prim, std::fabs(v.f));
            if (!v.t.sgn || v.s64() >= 0) return v;
            return binaryOp(BINARY_OP_SUB, mkInt(v.t, 0), v, where);
        }
        if (a.size() != 2) refuse("`" + q + "` arity (" + where + ")");
        bool lt = binaryOp(BINARY_OP_LT, a[0], a[1], where).i != 0;
        Val x = a[0], y = a[1];
        if (x.lit && !y.lit) x = convert(x, y.t);
        if (y.lit && !x.lit) y = convert(y, x.t);
        if (q == "Math.min") return lt ? x : y;
        return lt ? y : x;
    }

    Val rendezvous(Arrival& arr) {
        if (!threaded)
            refuse("`" + arr.what + "` outside a fibered run (" + arr.where + ")");
        ItemSync& s = G.sync[me];
        s.arrival = arr;
        s.st = ItemSync::Waiting;
        G.fibers->yield(me);
        if (G.abort) throw Abort{};
        return s.arrival.result;
    }
};

// ---- the compiler ------------------------------------------------------------

class KernelCompiler {
public:
    KernelCompiler(Compiled& c, const Bindings& b) : C(c), B(b) {}

    // Compile `m`'s body once. A helper is compiled when a call to it is, so
    // the caller's slot types are kept aside meanwhile; a recursive helper is
    // left to the walker, which refuses it.
    void function(const MethodPtr& m) {
        if (C.bodies.count(m.get()) || !inProgress.insert(m.get()).second) return;
        std::vector<STy> outer = std::move(slots);
        struct Restore {
            KernelCompiler& kc;
            std::vector<STy>& outer;
            const Method* m;
            ~Restore() { kc.slots = std::move(outer); kc.inProgress.erase(m); }
        } restore{*this, outer, m.get()};
        slots.assign((size_t) B.frameSize.lookup(m.get()), STy{});
        auto ps = B.paramSlots.find(m.get());
        if (ps == B.paramSlots.end()) return;
        size_t i = 0;
        for (auto& p : m->getParameterList()) {
            if (!p || p->getName() == "this") continue;
            int sl = ps->second[i++];
            CajetaTypePtr t = p->getType();
            if (isBuffer(t)) {
                if (auto e = primOf(typeArg(t, 0))) slots[(size_t) sl] = {2, *e};
            } else if (auto e = primOf(t)) {
                slots[(size_t) sl] = {1, *e};
            }
        }
        C.bodies[m.get()] = stmt(m->getBlock().get());
    }

private:
    struct STy { int k = 0; Ty t; };   // k: 0 not compiled, 1 scalar, 2 memory
    Compiled& C;
    const Bindings& B;
    std::vector<STy> slots;
    std::set<const Method*> inProgress;

    CExpr* node(CExpr::Fn fn, Ty t, Expression* src) {
        C.exprs.emplace_back();
        CExpr* n = &C.exprs.back();
        n->fn = fn;
        n->t = t;
        n->src = src;
        return n;
    }
    CStmt* snode(CStmt::Fn fn, AbstractSyntaxNode* src) {
        C.stmts.emplace_back();
        CStmt* n = &C.stmts.back();
        n->fn = fn;
        n->src = src;
        return n;
    }
    const CExpr* constant(const Val& v, Expression* src) {
        CExpr* n = node(&Item::xConst, v.t, src);
        n->k = Item::toSV(v);
        n->lit = v.lit;
        return n;
    }
    static Val valueOf(const CExpr& c) {
        Val v = Item::toVal(c.k, c.t);
        v.lit = c.lit;
        return v;
    }
    static bool isConst(const CExpr* c) { return c && c->fn == &Item::xConst; }
    static bool isBool(const CExpr* c) { return c && c->t.prim == Prim::Bool; }
    const Bindings::Name* nameOf(Expression* e) const {
        auto it = B.names.find(e);
        return it == B.names.end() ? nullptr : &it->second;
    }
    const STy* slotTy(int sl) const {
        return sl >= 0 && (size_t) sl < slots.size() ? &slots[(size_t) sl] : nullptr;
    }

    // ---- statements ----

    // CAJETA_XPU_REF_COMPILE_LOG=1 names each statement left to the walker.
    const CStmt* fallback(AbstractSyntaxNode* n) {
        static const bool log = [] {
            const char* e = std::getenv("CAJETA_XPU_REF_COMPILE_LOG");
            return e && *e && *e != '0';
        }();
        if (log) std::fprintf(stderr, "[ref-compile] walker: %s\n", at(n).c_str());
        return snode(&Item::sFallback, n);
    }

    const CStmt* stmt(AbstractSyntaxNode* n) {
        if (!n) return nullptr;
        if (auto b = dynamic_cast<Block*>(n)) {
            CStmt* s = snode(&Item::sBlock, n);
            for (auto& k : b->getChildren())
                if (const CStmt* c = stmt(k.get())) s->kids.push_back(c);
            return s;
        }
        if (auto ls = dynamic_cast<LabelStatement*>(n)) return stmt(ls->getBlock().get());
        if (auto es = dynamic_cast<ExpressionStatement*>(n)) {
            if (const CExpr* e = expr(es->getExpression().get())) {
                CStmt* s = snode(&Item::sExpr, n);
                s->e = e;
                return s;
            }
            return fallback(n);
        }
        if (auto lvd = dynamic_cast<LocalVariableDeclaration*>(n)) return decl(lvd);
        if (auto is = dynamic_cast<IfStatement*>(n)) {
            const CExpr* c = expr(is->getCondition().get());
            if (!isBool(c)) return fallback(n);
            CStmt* s = snode(&Item::sIf, n);
            s->e = c;
            s->s1 = stmt(is->getThenBranch().get());
            s->s2 = stmt(is->getElseBranch().get());
            return s;
        }
        if (auto ws = dynamic_cast<WhileStatement*>(n)) {
            const CExpr* c = expr(ws->getCondition().get());
            if (!isBool(c)) return fallback(n);
            CStmt* s = snode(&Item::sWhile, n);
            s->e = c;
            s->s1 = stmt(ws->getBody().get());
            return s;
        }
        if (auto ds = dynamic_cast<DoStatement*>(n)) {
            const CStmt* body = stmt(ds->getBody().get());
            const CExpr* c = expr(ds->getCondition().get());
            if (!isBool(c)) return fallback(n);
            CStmt* s = snode(&Item::sDo, n);
            s->e = c;
            s->s1 = body;
            return s;
        }
        if (auto fs = dynamic_cast<ForStatement*>(n)) {
            const CStmt* init = stmt(fs->getInit().get());
            const CExpr* c = fs->getCondition() ? expr(fs->getCondition().get()) : nullptr;
            if (fs->getCondition() && !isBool(c)) return fallback(n);
            std::vector<const CExpr*> upd;
            for (auto& u : fs->getUpdate()) {
                const CExpr* ce = expr(u.get());
                if (!ce) return fallback(n);
                upd.push_back(ce);
            }
            CStmt* s = snode(&Item::sFor, n);
            s->s0 = init;
            s->e = c;
            s->upd = std::move(upd);
            s->s1 = stmt(fs->getBody().get());
            return s;
        }
        if (auto ef = dynamic_cast<EnhancedForStatement*>(n)) return forEach(ef);
        if (dynamic_cast<BreakStatement*>(n)) return snode(&Item::sBreak, n);
        if (dynamic_cast<ContinueStatement*>(n)) return snode(&Item::sContinue, n);
        // A bare expression in statement position (an assignment, a call).
        if (auto e = dynamic_cast<Expression*>(n)) {
            if (const CExpr* ce = expr(e)) {
                CStmt* s = snode(&Item::sExpr, n);
                s->e = ce;
                return s;
            }
        }
        return fallback(n);
    }

    const CStmt* decl(LocalVariableDeclaration* lvd) {
        CajetaTypePtr t = lvd->getType();
        auto p = primOf(t);
        if (!p) {
            // Memory locals compile their element reads and writes; anything
            // else (vectors, tiles) stays with the walker.
            if (isShared(t))
                if (auto e = primOf(typeArg(t, 0)))
                    for (auto& vd : lvd->getVariableDeclarators())
                        if (vd) slots[(size_t) B.declSlots.lookup(vd.get())] = {2, *e};
            return fallback(lvd);
        }
        CStmt* blk = snode(&Item::sBlock, lvd);
        for (auto& vd : lvd->getVariableDeclarators()) {
            if (!vd) continue;
            int sl = B.declSlots.lookup(vd.get());
            auto init = vd->getInitializer();
            Expression* ie = init && !init->getChildren().empty()
                ? dynamic_cast<Expression*>(init->getChildren()[0].get()) : nullptr;
            const CExpr* e = ie ? expr(ie) : constant(isFloat(p->prim) ? mkFloat(p->prim, 0)
                                                                       : mkInt(*p, 0), nullptr);
            if (!e || e->t.prim == Prim::Bool) {
                if (!e || p->prim != Prim::Bool) return fallback(lvd);
            }
            slots[(size_t) sl] = {1, *p};
            CStmt* d = snode(&Item::sDecl, lvd);
            d->slot = sl;
            d->t = *p;
            d->e = e;
            blk->kids.push_back(d);
        }
        return blk;
    }

    const CStmt* forEach(EnhancedForStatement* ef) {
        auto mc = dynamic_cast<MethodCallExpression*>(ef->getIterableExpr().get());
        auto recv = mc ? dynamic_cast<IdentifierExpression*>(child(mc, 0)) : nullptr;
        auto fs = B.forSlots.find(ef);
        if (!recv || fs == B.forSlots.end() || mc->getParameters().size() != 1) return fallback(ef);
        const bool stripe = recv->getTextValue() == "Group" && mc->getMethodCallName() == "stripe";
        int bufSlot = -1;
        if (!stripe) {
            const Bindings::Name* nm = nameOf(recv);
            const STy* st = nm && !nm->isConst ? slotTy(nm->slot) : nullptr;
            if (mc->getMethodCallName() != "range" || !st || st->k != 2) return fallback(ef);
            bufSlot = nm->slot;
        }
        const CExpr* count = expr(mc->getParameters()[0].expression.get());
        if (!count || isFloat(count->t.prim) || count->t.prim == Prim::Bool) return fallback(ef);
        CajetaTypePtr it = stripe ? ef->getElementType() : ef->getIteratorType();
        Ty idxTy = it && primOf(it) ? *primOf(it) : Ty{Prim::I32, true};
        slots[(size_t) fs->second.first] = stripe ? STy{1, idxTy} : STy{1, slots[(size_t) bufSlot].t};
        if (fs->second.second >= 0) slots[(size_t) fs->second.second] = {1, idxTy};
        CStmt* s = snode(&Item::sForEach, ef);
        s->flag = stripe;
        s->e = count;
        s->t = idxTy;
        s->slot = fs->second.first;
        s->slot2 = fs->second.second;
        s->slot3 = bufSlot;
        s->s1 = stmt(ef->getBody().get());
        return s;
    }

    // ---- expressions: null when the walker must evaluate it ----

    const CExpr* expr(Expression* e) {
        if (!e) return nullptr;
        switch (e->kind()) {
            case ExprKind::Identifier: {
                const Bindings::Name* nm = nameOf(e);
                if (!nm) return nullptr;
                if (nm->isConst) return constant(nm->c, e);
                const STy* st = slotTy(nm->slot);
                if (!st || st->k != 1) return nullptr;
                CExpr* n = node(&Item::xLocal, st->t, e);
                n->slot = nm->slot;
                return n;
            }
            case ExprKind::IntegerLiteral:
            case ExprKind::FloatLiteral: {
                auto it = B.literals.find(e);
                return it == B.literals.end() ? nullptr : constant(it->second, e);
            }
            case ExprKind::TextLiteral: {
                auto* tl = static_cast<TextLiteralExpression*>(e);
                if (tl->getLiteralType() != LITERAL_TYPE_BOOL) return nullptr;
                return constant(mkBool(tl->getRawValue() == "true"), e);
            }
            case ExprKind::Cast: {
                auto* cast = static_cast<CastExpression*>(e);
                CajetaTypePtr ct = cast->getResolvedType() ? cast->getResolvedType() : cast->getDestType();
                auto to = primOf(ct);
                const CExpr* a = expr(child(cast, 0));
                if (!to || !a) return nullptr;
                if (isConst(a)) {
                    Val v = valueOf(*a);
                    v.lit = false;
                    return constant(convert(v, *to), e);
                }
                CExpr* n = node(&Item::xCast, *to, e);
                n->a = a;
                return n;
            }
            case ExprKind::Move:
                return expr(child(e, 0));
            case ExprKind::Prefix: return prefix(static_cast<PrefixExpression*>(e));
            case ExprKind::Postfix: {
                auto* post = static_cast<PostfixExpression*>(e);
                return incDec(child(post, 0), post->getOp() == POSTFIX_OP_INC, true, e);
            }
            case ExprKind::ArrayIndex: return load(static_cast<ArrayIndexExpression*>(e));
            case ExprKind::MethodCall: return call(static_cast<MethodCallExpression*>(e));
            case ExprKind::BinaryOp: return binary(static_cast<BinaryOpExpression*>(e));
            default: return nullptr;
        }
    }

    const CExpr* incDec(Expression* operand, bool inc, bool post, Expression* src) {
        const Bindings::Name* nm = nameOf(operand);
        const STy* st = nm && !nm->isConst ? slotTy(nm->slot) : nullptr;
        if (!st || st->k != 1 || st->t.prim == Prim::Bool) return nullptr;
        CExpr* n = node(&Item::xIncDec, st->t, src);
        n->slot = nm->slot;
        n->sgn = inc;
        n->flag = post;
        return n;
    }

    const CExpr* prefix(PrefixExpression* pre) {
        Expression* operand = child(pre, 0);
        switch (pre->getOp()) {
            case PREFIX_OP_POSITIVE: return expr(operand);
            case PREFIX_OP_INC: return incDec(operand, true, false, pre);
            case PREFIX_OP_DEC: return incDec(operand, false, false, pre);
            default: break;
        }
        const CExpr* a = expr(operand);
        if (!a) return nullptr;
        if (pre->getOp() == PREFIX_OP_LOGNOT) {
            if (!isBool(a)) return nullptr;
            CExpr* n = node(&Item::xNot, a->t, pre);
            n->a = a;
            return n;
        }
        if (a->t.prim == Prim::Bool) return nullptr;
        if (pre->getOp() == PREFIX_OP_BITNOT && isFloat(a->t.prim)) return nullptr;
        if (isConst(a)) {
            Val v = valueOf(*a);
            Val r;
            if (pre->getOp() == PREFIX_OP_BITNOT) r = mkInt(v.t, ~v.i);
            else if (isFloat(v.t.prim)) r = mkFloat(v.t.prim, -v.f);
            else {
                Val zero = mkInt(v.t, 0);
                zero.lit = v.lit;
                r = binaryOp(BINARY_OP_SUB, zero, v, Where{});
            }
            r.lit = v.lit && !isFloat(v.t.prim);
            return constant(r, pre);
        }
        CExpr* n = node(pre->getOp() == PREFIX_OP_BITNOT ? &Item::xBitNot : &Item::xNeg, a->t, pre);
        n->a = a;
        n->lit = a->lit && !isFloat(a->t.prim);
        return n;
    }

    // A memory local's slot, if `base` names one.
    int memSlot(Expression* base, Ty& elem) {
        const Bindings::Name* nm = nameOf(base);
        const STy* st = nm && !nm->isConst ? slotTy(nm->slot) : nullptr;
        if (!st || st->k != 2) return -1;
        elem = st->t;
        return nm->slot;
    }

    const CExpr* load(ArrayIndexExpression* ai) {
        Ty elem;
        int sl = memSlot(child(ai, 0), elem);
        if (sl < 0) return nullptr;
        const CExpr* ix = expr(child(ai, 1));
        if (!ix) return nullptr;
        CExpr* n = node(&Item::xLoad, elem, ai);
        n->slot = sl;
        n->a = ix;
        return n;
    }

    // The scalar return type of a call the compiler leaves to the walker but
    // whose result type is fixed, so its parent can still be compiled.
    std::optional<Ty> fixedReturn(const std::string& q) {
        static const std::map<std::string, Ty> table = {
            {"Wave.reduceSum", {Prim::I32, false}}, {"Wave.reduceMax", {Prim::I32, false}},
            {"Wave.reduceMin", {Prim::I32, false}}, {"Wave.reduceAnd", {Prim::I32, false}},
            {"Wave.reduceOr", {Prim::I32, false}}, {"Wave.reduceXor", {Prim::I32, false}},
            {"Wave.prefixSum", {Prim::I32, false}}, {"Wave.prefixProduct", {Prim::I32, false}},
            {"Wave.ballotSync", {Prim::I64, false}}, {"Wave.isFirstLane", {Prim::Bool, false}},
            {"Wave.reduceSumF32", {Prim::F32, true}}, {"Wave.reduceMaxF32", {Prim::F32, true}},
            {"Wave.reduceSumF32Segmented", {Prim::F32, true}},
            {"Wave.reduceMaxF32Segmented", {Prim::F32, true}},
            {"Group.reduce", {Prim::F32, true}}, {"Group.reduceSegmented", {Prim::F32, true}},
            {"Bits.count", {Prim::I32, false}}, {"Bits.reverse", {Prim::I32, false}},
            {"Bits.rotateLeft", {Prim::I32, false}}, {"Bits.rotateRight", {Prim::I32, false}},
            {"Cajeta.bitsToF32", {Prim::F32, true}}, {"Cajeta.f32ToBits", {Prim::I32, true}},
            {"Cajeta.bitsToF64", {Prim::F64, true}}, {"Cajeta.f64ToBits", {Prim::I64, true}},
        };
        auto it = table.find(q);
        if (it == table.end()) return std::nullopt;
        return it->second;
    }

    const CExpr* call(MethodCallExpression* mc) {
        Expression* recvE = child(mc, 0);
        auto* recvId = dynamic_cast<IdentifierExpression*>(recvE);
        const std::string& nm = mc->getMethodCallName();
        auto bound = B.helpers.find(mc);
        if (bound != B.helpers.end()) {
            function(bound->second);
            auto rt = primOf(bound->second->getReturnType());
            if (!rt) return nullptr;
            return node(&Item::xFallback, *rt, mc);
        }
        if (!recvId || nameOf(recvId)) return nullptr;   // a call on a local
        const std::string q = recvId->getTextValue() + "." + nm;
        static const std::map<std::string, int> ids = {
            {"KernelThread.x", 0}, {"KernelThread.y", 1}, {"KernelThread.z", 2},
            {"KernelThread.globalIdX", 3}, {"KernelThread.globalIdY", 4},
            {"KernelThread.globalIdZ", 5}, {"Workgroup.x", 6}, {"Workgroup.y", 7},
            {"Workgroup.z", 8}, {"Workgroup.dimX", 9}, {"Workgroup.dimY", 10},
            {"Workgroup.dimZ", 11}, {"Wave.width", 12}, {"Wave.laneId", 13},
            {"Group.width", 12}, {"Group.laneId", 13},
        };
        if (auto it = ids.find(q); it != ids.end() && mc->getParameters().empty()) {
            Ty t = recvId->getTextValue() == "Group" ? Ty{Prim::I32, true} : Ty{Prim::I32, false};
            CExpr* n = node(&Item::xThread, t, mc);
            n->kind = it->second;
            return n;
        }
        if (auto rt = fixedReturn(q)) return node(&Item::xFallback, *rt, mc);
        return nullptr;
    }

    const CExpr* binary(BinaryOpExpression* bin) {
        BinaryOp op = bin->getBinaryOp();
        Expression* l = child(bin, 0);
        Expression* r = child(bin, 1);
        if (bin->isAssignment()) return assignment(bin, op, l, r);
        if (op == BINARY_OP_LOGAND || op == BINARY_OP_LOGOR) {
            const CExpr* a = expr(l);
            const CExpr* b = expr(r);
            if (!isBool(a) || !isBool(b)) return nullptr;
            CExpr* n = node(op == BINARY_OP_LOGAND ? &Item::xAnd : &Item::xOr, a->t, bin);
            n->a = a;
            n->b = b;
            return n;
        }
        return binop(op, expr(l), expr(r), bin);
    }

    // A binary operator, its types settled as scalarOp settles them at run time.
    const CExpr* binop(BinaryOp op, const CExpr* a, const CExpr* b, Expression* src) {
        if (!a || !b) return nullptr;
        if (isConst(a) && isConst(b)) {
            try {
                return constant(scalarOp(op, valueOf(*a), valueOf(*b), src), src);
            } catch (Exception&) {
                return nullptr;   // the walker reports it, at its line, when it runs
            }
        }
        if (a->lit && !b->lit && b->t.prim != Prim::Bool) {
            if (!isConst(a)) return nullptr;
            a = constant(convert(valueOf(*a), b->t), src);
        } else if (b->lit && !a->lit && a->t.prim != Prim::Bool) {
            if (!isConst(b)) return nullptr;
            b = constant(convert(valueOf(*b), a->t), src);
        }
        if (a->t.prim == Prim::Bool || b->t.prim == Prim::Bool) {
            if (a->t.prim != b->t.prim) return nullptr;
            if (op != BINARY_OP_EQ && op != BINARY_OP_NE && op != BINARY_OP_BITAND
                    && op != BINARY_OP_BITOR && op != BINARY_OP_BITXOR) return nullptr;
            CExpr* n = node(&Item::xBinary, a->t, src);
            n->kind = 0;
            n->op = op;
            n->a = a; n->b = b;
            n->at = a->t; n->bt = b->t;
            return n;
        }
        if (isFloat(a->t.prim) || isFloat(b->t.prim)) {
            Prim p = widerFloat(a->t.prim, b->t.prim);
            bool cmp = isCompare(op);
            if (!cmp && op != BINARY_OP_ADD && op != BINARY_OP_SUB && op != BINARY_OP_MUL
                    && op != BINARY_OP_DIV && op != BINARY_OP_MOD) return nullptr;
            CExpr* n = node(&Item::xBinary, cmp ? Ty{Prim::Bool, false} : Ty{p, true}, src);
            n->kind = 1;
            n->op = op;
            n->a = a; n->b = b;
            n->at = n->bt = Ty{p, true};
            return n;
        }
        unsigned w = std::max(bitsOf(a->t.prim), bitsOf(b->t.prim));
        Prim ip = intOfBits(w);
        bool unsignedWins = !(a->t.sgn && b->t.sgn);
        Ty rt{ip, !unsignedWins};
        if (op == BINARY_OP_SHIFTLEFT || op == BINARY_OP_SHIFTRIGHT || op == BINARY_OP_USHIFTRIGHT)
            rt = Ty{ip, a->t.sgn};
        CExpr* n = node(&Item::xBinary, isCompare(op) ? Ty{Prim::Bool, false} : rt, src);
        n->kind = 2;
        n->op = op;
        n->a = a; n->b = b;
        n->at = a->t; n->bt = b->t;
        n->w = w;
        n->flag = a->t.sgn || b->t.sgn;   // a comparison is signed when either is
        return n;
    }

    const CExpr* assignment(BinaryOpExpression* bin, BinaryOp op, Expression* l, Expression* r) {
        const CExpr* rv = expr(r);
        if (!rv) return nullptr;
        if (dynamic_cast<IdentifierExpression*>(l)) {
            const Bindings::Name* nm = nameOf(l);
            const STy* st = nm && !nm->isConst ? slotTy(nm->slot) : nullptr;
            if (!st || st->k != 1) return nullptr;
            const CExpr* v = rv;
            if (op != BINARY_OP_ASSIGN) v = binop(compoundBase(op), expr(l), rv, bin);
            if (!v) return nullptr;
            CExpr* n = node(&Item::xAssignLocal, st->t, bin);
            n->slot = nm->slot;
            n->a = v;
            return n;
        }
        if (auto ai = dynamic_cast<ArrayIndexExpression*>(l)) {
            Ty elem;
            int sl = memSlot(child(ai, 0), elem);
            const CExpr* ix = sl >= 0 ? expr(child(ai, 1)) : nullptr;
            if (!ix) return nullptr;
            const CExpr* v = rv;
            if (op != BINARY_OP_ASSIGN) v = binop(compoundBase(op), load(ai), rv, bin);
            if (!v) return nullptr;
            CExpr* n = node(&Item::xAssignElem, elem, bin);
            n->slot = sl;
            n->a = v;
            n->b = ix;
            return n;
        }
        return nullptr;
    }
};

// ---- collectives -----------------------------------------------------------

void resolveWave(Group& G, const std::vector<uint32_t>& lanes, uint32_t W) {
    ItemSync& first = G.sync[lanes[0]];
    const std::string& q = first.arrival.what;
    const Where& where = first.arrival.where;
    for (uint32_t m : lanes)
        if (G.sync[m].arrival.site != first.arrival.site)
            undefined("lanes of one wave reach different wave operations: `" + q + "` (" +
                      where + ") and `" + G.sync[m].arrival.what + "` (" +
                      G.sync[m].arrival.where + ")");
    auto argOf = [&](uint32_t m) -> const Val& { return G.sync[m].arrival.arg; };
    auto laneOf = [&](uint32_t m) { return m % W; };
    std::map<uint32_t, uint32_t> byLane;
    for (uint32_t m : lanes) byLane[laneOf(m)] = m;
    const Ty u32{Prim::I32, false};

    if (q == "Barrier.wave") {
        for (uint32_t m : lanes) G.sync[m].arrival.result = Val();
        return;
    }
    if (q == "Wave.shuffleSync" || q == "Wave.rotate") {
        for (uint32_t m : lanes) {
            uint64_t src = G.sync[m].arrival.arg2.u64();
            if (q == "Wave.rotate") src = (laneOf(m) + src) % W;
            auto it = byLane.find((uint32_t) src);
            if (src >= W || it == byLane.end())
                undefined("`" + q + "` reads lane " + std::to_string(src) +
                          ", which is not active (" + where + ")");
            G.sync[m].arrival.result = argOf(it->second);
        }
        return;
    }
    if (q == "Wave.ballotSync") {
        uint64_t mask = 0;
        for (uint32_t m : lanes) if (argOf(m).i) mask |= 1ull << laneOf(m);
        for (uint32_t m : lanes) G.sync[m].arrival.result = mkInt({Prim::I64, false}, mask);
        return;
    }
    if (q == "Wave.prefixSum" || q == "Wave.prefixProduct") {
        bool sum = q == "Wave.prefixSum";
        uint64_t run = sum ? 0 : 1;
        for (auto& [l, m] : byLane) {
            G.sync[m].arrival.result = mkInt(u32, run);
            run = sum ? run + argOf(m).u64() : run * argOf(m).u64();
        }
        return;
    }
    if (q == "Wave.reduceSumF32" || q == "Wave.reduceMaxF32"
            || q == "Wave.reduceSumF32Segmented" || q == "Wave.reduceMaxF32Segmented") {
        // A butterfly over the full width, partners at distance 1, 2, 4 ...
        // W/2 in that order, as the shared lowering emits it; inactive lanes
        // contribute the identity. Every lane computes the same association.
        // The segmented form stops at the segment, so each aligned span of
        // `seg` lanes reduces on its own and every lane of a span holds the
        // span's result. The segment is uniform, a power of two, and clamped
        // to the width.
        bool sum = q == "Wave.reduceSumF32" || q == "Wave.reduceSumF32Segmented";
        uint32_t seg = W;
        if (q.size() > 9 && q.compare(q.size() - 9, 9, "Segmented") == 0) {
            seg = (uint32_t) std::min<uint64_t>(G.sync[lanes[0]].arrival.arg2.u64(), W);
            for (uint32_t m : lanes)
                if (G.sync[m].arrival.arg2.u64() != G.sync[lanes[0]].arrival.arg2.u64())
                    undefined("`" + q + "` with a segment that differs across the wave (" +
                              where + ")");
            if (seg == 0 || (seg & (seg - 1)) != 0)
                undefined("`" + q + "` with a segment of " + std::to_string(seg) +
                          ", not a power of two (" + where + ")");
        }
        std::vector<float> v(W, sum ? 0.0f : -std::numeric_limits<float>::infinity());
        for (auto& [l, m] : byLane) v[l] = (float) argOf(m).f;
        for (uint32_t off = 1; off < seg; off *= 2) {
            std::vector<float> nv(W);
            for (uint32_t l = 0; l < W; ++l)
                nv[l] = sum ? v[l] + v[l ^ off] : std::fmax(v[l], v[l ^ off]);
            v = nv;
        }
        for (uint32_t m : lanes) G.sync[m].arrival.result = mkFloat(Prim::F32, v[laneOf(m)]);
        return;
    }
    uint64_t acc = 0;
    bool firstLane = true;
    for (auto& [l, m] : byLane) {
        uint64_t x = argOf(m).u64();
        if (firstLane) { acc = x; firstLane = false; continue; }
        if (q == "Wave.reduceSum") acc += x;
        else if (q == "Wave.reduceMax") acc = std::max(acc, x);
        else if (q == "Wave.reduceMin") acc = std::min(acc, x);
        else if (q == "Wave.reduceAnd") acc &= x;
        else if (q == "Wave.reduceOr") acc |= x;
        else if (q == "Wave.reduceXor") acc ^= x;
        else refuse("`" + q + "` (" + where + ")");
    }
    for (uint32_t m : lanes) G.sync[m].arrival.result = mkInt(u32, acc);
}

// Settle what can be settled; false when nothing could.
bool resolve(Group& G) {
    const uint32_t n = G.size;
    bool any = false;
    std::vector<uint32_t> waiting, done;
    for (uint32_t m = 0; m < n; ++m) {
        if (G.sync[m].st == ItemSync::Waiting) waiting.push_back(m);
        if (G.sync[m].st == ItemSync::Done) done.push_back(m);
    }
    if (waiting.empty()) return false;

    // Wave collectives: a wave whose every live lane waits at a wave operation.
    const uint32_t W = G.run.wave;
    if (W) {
        for (uint32_t w0 = 0; w0 < n; w0 += W) {
            std::vector<uint32_t> lanes;
            bool ready = true, anyLive = false;
            for (uint32_t m = w0; m < std::min(n, w0 + W); ++m) {
                if (G.sync[m].st == ItemSync::Done) continue;
                anyLive = true;
                if (G.sync[m].st != ItemSync::Waiting
                        || G.sync[m].arrival.what == "Barrier.workgroup") { ready = false; break; }
                lanes.push_back(m);
            }
            if (!anyLive || !ready || lanes.empty()) continue;
            resolveWave(G, lanes, W);
            for (uint32_t m : lanes) G.sync[m].st = ItemSync::Ready;
            any = true;
        }
        if (any) return true;
    }

    // The workgroup barrier: every work-item waits at the same one.
    bool allAtBarrier = true;
    for (uint32_t m : waiting)
        if (G.sync[m].arrival.what != "Barrier.workgroup") allAtBarrier = false;
    if (allAtBarrier) {
        const Arrival& a = G.sync[waiting[0]].arrival;
        if (!done.empty())
            undefined("`Barrier.workgroup` (" + a.where + ") is never reached by " +
                      std::to_string(done.size()) + " of the workgroup's " + std::to_string(n) +
                      " work-items, which returned first");
        for (uint32_t m : waiting)
            if (G.sync[m].arrival.site != a.site)
                undefined("work-items of one workgroup wait at different barriers (" + a.where +
                          " and " + G.sync[m].arrival.where + ")");
        for (uint32_t m : waiting) G.sync[m].st = ItemSync::Ready;
        return true;
    }
    return false;
}

// A fiber's body: run work-item `m` to its end, recording a failure for the
// scheduler, and unwinding quietly when the group is being abandoned.
void runItem(void* g, uint32_t m) {
    Group& G = *(Group*) g;
    try {
        Item(G, m, true).body();
    } catch (Abort&) {
    } catch (...) {
        if (!G.failure) G.failure = std::current_exception();
    }
    G.sync[m].st = ItemSync::Done;
}

void runGroup(Group& G, bool threaded, Fibers& fibers) {
    const uint32_t n = G.size;
    G.sync.assign(n, ItemSync());
    if (!threaded) {
        for (uint32_t m = 0; m < n; ++m) Item(G, m, false).body();
        return;
    }
    G.fibers = &fibers;
    fibers.start(n, &runItem, &G);
    // Resume every fiber still parked, with `abort` set, so each unwinds its
    // own frames before the group is torn down.
    auto abandon = [&] {
        G.abort = true;
        for (uint32_t m = 0; m < n; ++m)
            if (G.sync[m].st == ItemSync::Waiting) fibers.resume(m);
    };
    auto pastBudget = [&] {
        return G.run.hasDeadline && std::chrono::steady_clock::now() > G.run.deadline;
    };
    try {
        for (;;) {
            bool ranAny = false;
            for (uint32_t m = 0; m < n && !G.failure; ++m) {
                if (G.sync[m].st != ItemSync::Ready) continue;
                fibers.resume(m);
                ranAny = true;
            }
            if (G.failure) break;
            bool allDone = true;
            for (auto& st : G.sync) if (st.st != ItemSync::Done) allDone = false;
            if (allDone) break;
            if (pastBudget())
                throw Exception("the reference run of " + G.run.kernel->getName() +
                                " outlasted its budget", "XPU-REF03");
            if (!resolve(G) && !ranAny) {
                std::string what;
                for (uint32_t m = 0; m < n; ++m)
                    if (G.sync[m].st == ItemSync::Waiting) {
                        what = "`" + G.sync[m].arrival.what + "` (" + G.sync[m].arrival.where + ")";
                        break;
                    }
                undefined("the workgroup cannot proceed: work-items wait at " + what +
                          " while others wait elsewhere or have returned");
            }
        }
    } catch (...) {
        abandon();
        throw;
    }
    if (G.failure) {
        abandon();
        std::rethrow_exception(G.failure);
    }
}

} // namespace

Arg Arg::f32(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return scalar(b);
}

Arg Arg::f64(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return scalar(b);
}

std::vector<std::string> refusals(const MethodPtr& kernel) {
    Survey s;
    s.kernel(kernel);
    return s.refused;
}

void run(const MethodPtr& kernel, const std::vector<Arg>& args, const Launch& launch) {
    const std::string kname = kernel->getName();
    Survey survey;
    survey.kernel(kernel);
    if (!survey.refused.empty()) {
        std::string msg = "the reference interpreter cannot run kernel " + kname + ": " +
                          survey.refused[0];
        if (survey.refused.size() > 1)
            msg += "; and " + std::to_string(survey.refused.size() - 1) + " more";
        refuse(msg);
    }

    Run R;
    R.bindings = survey.bindings;
    R.kernel = kernel;
    Compiled compiled;
    if (const char* walk = std::getenv("CAJETA_XPU_REF_WALK"); !walk || !*walk || *walk == '0') {
        KernelCompiler kc(compiled, R.bindings);
        kc.function(kernel);
        R.compiled = &compiled;
    }
    R.cls = kernel->getParent();
    R.launch = launch;
    R.wave = launch.waveWidth;
    if (launch.budgetSeconds > 0) {
        R.hasDeadline = true;
        R.deadline = std::chrono::steady_clock::now() +
            std::chrono::microseconds((int64_t) (launch.budgetSeconds * 1e6));
    }
    if (!R.wave)
        if (auto attr = XpuKernelAttr::from(*kernel); attr && attr->waveWidth())
            R.wave = (uint32_t) *attr->waveWidth();
    if (survey.usesWave && !R.wave)
        refuse("the reference interpreter cannot run kernel " + kname + ": it uses a wave "
               "operation, and neither the launch nor an @Wave(width = N) gives the width");

    size_t ai = 0;
    for (auto& p : kernel->getParameterList()) {
        if (!p || p->getName() == "this") continue;
        if (ai >= args.size())
            refuse("kernel " + kname + " takes more arguments than the " +
                   std::to_string(args.size()) + " given");
        const Arg& a = args[ai++];
        CajetaTypePtr t = p->getType();
        if (isBuffer(t)) {
            if (!a.isBuffer) refuse("argument `" + p->getName() + "` of " + kname + " is a buffer");
            Mem& m = R.buffers[p->getName()];
            m.name = p->getName();
            m.elem = *primOf(typeArg(t, 0));
            m.data = (uint8_t*) a.data;
            m.count = a.byteCount ? a.byteCount / bytesOf(m.elem.prim) : a.count;
        } else {
            Ty st = *primOf(t);
            Val v;
            if (st.prim == Prim::F32) {
                float f;
                uint32_t b = (uint32_t) a.bits;
                std::memcpy(&f, &b, 4);
                v = mkFloat(Prim::F32, f);
            } else if (st.prim == Prim::F64) {
                double d;
                std::memcpy(&d, &a.bits, 8);
                v = mkFloat(Prim::F64, d);
            } else if (isFloat(st.prim)) {
                v = mkFloat(st.prim, st.prim == Prim::F16 ? halfToFloat((uint16_t) a.bits)
                                                          : bf16ToFloat((uint16_t) a.bits));
            } else {
                v = mkInt(st, a.bits);
            }
            R.scalars[p->getName()] = v;
        }
    }
    if (ai != args.size())
        refuse("kernel " + kname + " takes " + std::to_string(ai) + " arguments, not " +
               std::to_string(args.size()));

    // Workgroups cannot synchronize with each other, so they run on a pool of
    // threads, each taking the next group in order. A kernel with atomics runs
    // its groups in order on one thread: atomics would need real atomicity
    // across threads, and float atomics answer by the order they ran in.
    // CAJETA_XPU_REF_THREADS caps the pool. A failure stops new groups from
    // starting, and the lowest-numbered failing group's error is the one
    // reported, so the report does not depend on scheduling.
    const uint32_t* g = launch.grid;
    const uint32_t* b = launch.block;
    const uint64_t groups = (uint64_t) g[0] * g[1] * g[2];
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    if (const char* t = std::getenv("CAJETA_XPU_REF_THREADS"); t && *t)
        threads = std::max(1, std::atoi(t));
    if (survey.usesAtomic) threads = 1;
    threads = (unsigned) std::min<uint64_t>(threads, groups);

    std::atomic<uint64_t> next{0};
    std::atomic<uint64_t> firstFailure{UINT64_MAX};
    std::mutex failureLock;
    std::exception_ptr failure;
    auto worker = [&] {
        Fibers fibers;
        for (;;) {
            uint64_t i = next.fetch_add(1);
            if (i >= groups || i > firstFailure.load()) return;
            Group G(R);
            G.id[0] = (uint32_t) (i % g[0]);
            G.id[1] = (uint32_t) ((i / g[0]) % g[1]);
            G.id[2] = (uint32_t) (i / ((uint64_t) g[0] * g[1]));
            G.size = b[0] * b[1] * b[2];
            try {
                if (R.hasDeadline && std::chrono::steady_clock::now() > R.deadline)
                    throw Exception("the reference run of " + kname + " outlasted its budget",
                                    "XPU-REF03");
                runGroup(G, survey.usesCollective, fibers);
            } catch (...) {
                std::lock_guard<std::mutex> lk(failureLock);
                if (i < firstFailure.load()) {
                    firstFailure.store(i);
                    failure = std::current_exception();
                }
            }
        }
    };
    if (threads <= 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }
    if (failure) std::rethrow_exception(failure);
}

std::vector<ParamShape> paramShapes(const MethodPtr& kernel) {
    std::vector<ParamShape> out;
    for (auto& p : kernel->getParameterList()) {
        if (!p || p->getName() == "this") continue;
        ParamShape s;
        s.name = p->getName();
        CajetaTypePtr t = p->getType();
        if (isBuffer(t)) {
            s.isBuffer = true;
            if (auto e = primOf(typeArg(t, 0))) {
                s.isFloat = isFloat(e->prim);
                s.elementBytes = bytesOf(e->prim);
            }
        } else if (auto e = primOf(t)) {
            s.isFloat = isFloat(e->prim);
        }
        out.push_back(s);
    }
    return out;
}

uint64_t ulpDistance(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b) ? 0 : UINT64_MAX;
    auto key = [](float f) -> int64_t {
        int32_t i;
        std::memcpy(&i, &f, 4);
        return i < 0 ? (int64_t) INT32_MIN - (int64_t) i : (int64_t) i;
    };
    int64_t d = key(a) - key(b);
    return (uint64_t) (d < 0 ? -d : d);
}

uint64_t ulpDistance(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b) ? 0 : UINT64_MAX;
    auto key = [](double f) -> __int128 {
        int64_t i;
        std::memcpy(&i, &f, 8);
        return i < 0 ? (__int128) INT64_MIN - (__int128) i : (__int128) i;
    };
    __int128 d = key(a) - key(b);
    return (uint64_t) (d < 0 ? -d : d);
}

} // namespace reference
} // namespace xpu
} // namespace cajeta
