// The kernel reference interpreter: see KernelInterpreter.h, and
// docs/specification/xpu/ReferenceInterpreter.md for the semantics written
// down. Nothing here calls into the lowering; the two meet only at the AST.

#include "KernelInterpreter.h"

#include "../core/KernelArgTrait.h"
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

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

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
};

struct Tile {
    Ty elem;
    uint32_t rows = 0, cols = 0, use = 0;
    std::vector<Val> e;   // row-major scalars
};

[[noreturn]] void refuse(const std::string& what) {
    throw Exception(what, "XPU-REF01");
}
[[noreturn]] void undefined(const std::string& what) {
    throw Exception(what, "XPU-REF02");
}

// Where a node is, for a message: "line L, near `text`".
std::string at(const AbstractSyntaxNodePtr& n) {
    if (!n) return "";
    std::string s = "line " + std::to_string(n->getSourceLine());
    const std::string& text = n->getSourceText();
    if (!text.empty() && text.size() <= 80) s += ", near `" + text + "`";
    return s;
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
Val scalarOp(BinaryOp op, Val a, Val b, const std::string& where) {
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

Val binaryOp(BinaryOp op, const Val& a, const Val& b, const std::string& where) {
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

Val loadElem(Mem& m, uint64_t idx, const std::string& where) {
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

void storeElem(Mem& m, uint64_t idx, const Val& v, const std::string& where) {
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
}

uint64_t indexOf(const Val& v, const std::string& where) {
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
        "Wave.prefixSum", "Wave.prefixProduct",
        "Math.min", "Math.max", "Math.abs",
    };
    return s;
}

// The built-ins that are a rendezvous: every live member of the scope must
// arrive before any of them continues.
bool isCollective(const std::string& q) {
    if (q == "Barrier.workgroup" || q == "Barrier.wave") return true;
    if (!startsWith(q, "Wave.")) return false;
    return q != "Wave.width" && q != "Wave.laneId" && q != "Wave.isFirstLane";
}

enum class LocalKind { Scalar, Vector, Buffer, Shared, Tile };

const std::map<LocalKind, std::set<std::string>>& memberBuiltins() {
    static const std::map<LocalKind, std::set<std::string>> m = {
        {LocalKind::Buffer, {"vload", "vstore"}},
        {LocalKind::Shared, {}},
        {LocalKind::Vector, {"dotAccum"}},
        {LocalKind::Tile, {"splat", "load", "store", "mma"}},
        {LocalKind::Scalar, {}},
    };
    return m;
}

// ---- the refusal pass ------------------------------------------------------

class Survey {
public:
    std::vector<std::string> refused;
    bool usesCollective = false;
    bool usesWave = false;

    void kernel(const MethodPtr& m) {
        for (auto& p : m->getParameterList()) {
            if (!p || p->getName() == "this") continue;
            CajetaTypePtr t = p->getType();
            if (isBuffer(t)) {
                if (!primOf(typeArg(t, 0)))
                    note("parameter `" + p->getName() + "` of type " + canonical(t),
                         m->getBlock());
                kinds[p->getName()] = LocalKind::Buffer;
            } else if (primOf(t)) {
                kinds[p->getName()] = LocalKind::Scalar;
            } else {
                note("parameter `" + p->getName() + "` of type " + canonical(t), m->getBlock());
            }
        }
        stmt(m->getBlock());
    }

private:
    std::map<std::string, LocalKind> kinds;

    void note(const std::string& what, const AbstractSyntaxNodePtr& n) {
        refused.push_back(what + " (" + at(n) + ")");
    }

    void stmt(const AbstractSyntaxNodePtr& n) {
        if (!n) return;
        if (auto b = std::dynamic_pointer_cast<Block>(n)) {
            for (auto& s : b->getChildren()) stmt(s);
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
            stmt(fs->getInit());
            expr(fs->getCondition());
            for (auto& u : fs->getUpdate()) expr(u);
            stmt(fs->getBody());
        } else if (auto ws = std::dynamic_pointer_cast<WhileStatement>(n)) {
            expr(ws->getCondition());
            stmt(ws->getBody());
        } else if (auto ds = std::dynamic_pointer_cast<DoStatement>(n)) {
            stmt(ds->getBody());
            expr(ds->getCondition());
        } else if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(n)) {
            expr(es->getExpression());
        } else if (auto rs = std::dynamic_pointer_cast<ReturnStatement>(n)) {
            if (rs->getExpression()) note("a return with a value", n);
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
                continue;
            }
            if (k == LocalKind::Tile) {
                if (e) note("an initialized tile local", lvd);
                continue;
            }
            if (e) expr(e);
        }
    }

    void call(const std::shared_ptr<MethodCallExpression>& mc) {
        const std::string& name = mc->getMethodCallName();
        auto recvE = child(mc, 0);
        auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(recvE);
        for (auto& p : mc->getParameters()) expr(p.expression);
        if (!recvId) {
            if (recvE) expr(recvE);
            if (name == "dotAccum" && recvE) return;   // a vector-valued receiver
            note("the call `" + name + "` on a computed receiver", mc);
            return;
        }
        const std::string& r = recvId->getTextValue();
        if (auto k = kinds.find(r); k != kinds.end()) {
            const auto& ok = memberBuiltins().at(k->second);
            if (!ok.count(name)) note("`" + r + "." + name + "`", mc);
            return;
        }
        std::string q = r + "." + name;
        if (!staticBuiltins().count(q)) { note("`" + q + "`", mc); return; }
        if (isCollective(q)) usesCollective = true;
        if (startsWith(q, "Wave.") || q == "Barrier.wave") usesWave = true;
    }

    void expr(const ExpressionPtr& e) {
        if (!e) return;
        if (std::dynamic_pointer_cast<IdentifierExpression>(e)
                || std::dynamic_pointer_cast<IntegerLiteralExpression>(e)
                || std::dynamic_pointer_cast<FloatLiteralExpression>(e)) return;
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
                    && kinds[lhs->getTextValue()] == LocalKind::Vector) return;
            if (lhs && CajetaType::lookupEnumConstant(lhs->getTextValue(), dot->getIdentifier()))
                return;
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
    std::string where;
    Val arg, arg2;
    Val result;
};

struct ItemSync {
    enum State { Ready, Waiting, Done } st = Ready;
    Arrival arrival;
};

struct Run {
    MethodPtr kernel;
    std::shared_ptr<CajetaClass> cls;
    Launch launch;
    uint32_t wave = 0;
    std::map<std::string, Mem> buffers;
    std::map<std::string, Val> scalars;
};

struct Group {
    Run& run;
    uint32_t id[3];
    uint32_t size = 1;
    std::map<const void*, std::unique_ptr<Mem>> shared;
    // The baton: only the work-item named by `turn` runs; -1 hands it back to
    // the scheduler.
    std::mutex m;
    std::condition_variable cv;
    int turn = -1;
    bool abort = false;
    std::vector<ItemSync> sync;
    explicit Group(Run& r) : run(r) {}
};

enum class Flow { Next, Break, Continue, Return };

class Item {
public:
    Item(Group& g, uint32_t linear, bool threaded) : G(g), R(g.run), me(linear), threaded(threaded) {
        const uint32_t* b = R.launch.block;
        tid[0] = linear % b[0];
        tid[1] = (linear / b[0]) % b[1];
        tid[2] = linear / (b[0] * b[1]);
    }

    void body() {
        scopes.emplace_back();
        exec(R.kernel->getBlock());
    }

private:
    Group& G;
    Run& R;
    uint32_t me;
    bool threaded;
    uint32_t tid[3];
    struct Slot { Val v; };
    std::vector<std::map<std::string, Slot>> scopes;
    std::string flowLabel;

    // -- names --
    Slot* lookup(const std::string& n) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (auto f = it->find(n); f != it->end()) return &f->second;
        return nullptr;
    }

    Val name(const std::string& n, const ExpressionPtr& e) {
        if (Slot* s = lookup(n)) return s->v;
        if (auto b = R.buffers.find(n); b != R.buffers.end()) {
            Val v; v.k = Val::MemRef; v.mem = &b->second; v.t = b->second.elem; return v;
        }
        if (auto s = R.scalars.find(n); s != R.scalars.end()) return s->second;
        if (R.cls) {
            const auto& params = R.cls->getTypeParameters();
            const auto& args = R.cls->getTypeArguments();
            for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
                if (!params[i].isNonType || params[i].name != n) continue;
                auto c = std::dynamic_pointer_cast<CajetaConstantType>(args[i]);
                if (!c) break;
                const std::string& prim = params[i].nonTypePrimitive;
                unsigned bits = 32;
                size_t d = prim.find_first_of("0123456789");
                if (d != std::string::npos) bits = (unsigned) std::stoul(prim.substr(d));
                return mkInt({intOfBits(bits), prim.empty() || prim[0] != 'u'},
                             (uint64_t) c->getValue());
            }
        }
        refuse("the name `" + n + "` binds nothing the interpreter knows (" + at(e) + ")");
    }

    // -- statements --
    Flow exec(const AbstractSyntaxNodePtr& n) {
        if (!n) return Flow::Next;
        if (auto b = std::dynamic_pointer_cast<Block>(n)) {
            scopes.emplace_back();
            Flow f = Flow::Next;
            for (auto& s : b->getChildren()) {
                f = exec(s);
                if (f != Flow::Next) break;
            }
            scopes.pop_back();
            return f;
        }
        if (auto ls = std::dynamic_pointer_cast<LabelStatement>(n)) return exec(ls->getBlock());
        if (auto il = std::dynamic_pointer_cast<IdentifierLabel>(n)) {
            pendingLabel = il->getIdentifier();
            Flow f = exec(il->getBody());
            pendingLabel.clear();
            return f;
        }
        if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(n)) {
            decl(lvd);
            return Flow::Next;
        }
        if (auto is = std::dynamic_pointer_cast<IfStatement>(n)) {
            if (cond(is->getCondition())) return exec(is->getThenBranch());
            return exec(is->getElseBranch());
        }
        if (auto fs = std::dynamic_pointer_cast<ForStatement>(n)) {
            std::string label = takeLabel();
            scopes.emplace_back();
            exec(fs->getInit());
            Flow out = Flow::Next;
            while (!fs->getCondition() || cond(fs->getCondition())) {
                Flow f = exec(fs->getBody());
                if (f == Flow::Return) { out = f; break; }
                if (f == Flow::Break) { if (mine(label)) break; out = f; break; }
                if (f == Flow::Continue && !mine(label)) { out = f; break; }
                for (auto& u : fs->getUpdate()) eval(u);
            }
            scopes.pop_back();
            return out;
        }
        if (auto ws = std::dynamic_pointer_cast<WhileStatement>(n)) {
            std::string label = takeLabel();
            while (cond(ws->getCondition())) {
                Flow f = exec(ws->getBody());
                if (f == Flow::Return) return f;
                if (f == Flow::Break) { if (mine(label)) break; return f; }
                if (f == Flow::Continue && !mine(label)) return f;
            }
            return Flow::Next;
        }
        if (auto ds = std::dynamic_pointer_cast<DoStatement>(n)) {
            std::string label = takeLabel();
            do {
                Flow f = exec(ds->getBody());
                if (f == Flow::Return) return f;
                if (f == Flow::Break) { if (mine(label)) break; return f; }
                if (f == Flow::Continue && !mine(label)) return f;
            } while (cond(ds->getCondition()));
            return Flow::Next;
        }
        if (auto bs = std::dynamic_pointer_cast<BreakStatement>(n)) {
            flowLabel = bs->getLabel();
            return Flow::Break;
        }
        if (auto cs = std::dynamic_pointer_cast<ContinueStatement>(n)) {
            flowLabel = cs->getLabel();
            return Flow::Continue;
        }
        if (std::dynamic_pointer_cast<ReturnStatement>(n)) return Flow::Return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(n)) {
            eval(es->getExpression());
            return Flow::Next;
        }
        if (auto e = std::dynamic_pointer_cast<Expression>(n)) {
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

    bool cond(const ExpressionPtr& e) {
        Val v = eval(e);
        if (v.k != Val::Scalar || v.t.prim != Prim::Bool)
            refuse("a condition that is not boolean (" + at(e) + ")");
        return v.i != 0;
    }

    void decl(const std::shared_ptr<LocalVariableDeclaration>& lvd) {
        CajetaTypePtr t = lvd->getType();
        for (auto& vd : lvd->getVariableDeclarators()) {
            if (!vd) continue;
            const std::string& nm = vd->getIdentifier();
            auto init = vd->getInitializer();
            auto e = init && !init->getChildren().empty()
                ? std::dynamic_pointer_cast<Expression>(init->getChildren()[0]) : nullptr;
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
                tile->e.assign((size_t) tile->rows * tile->cols,
                               isFloat(tile->elem.prim) ? mkFloat(tile->elem.prim, 0)
                                                        : mkInt(tile->elem, 0));
                s.v.k = Val::TileRef;
                s.v.t = tile->elem;
                s.v.tile = tile;
            } else if (isShared(t)) {
                s.v = sharedArray(nm, t, e, vd.get());
            } else {
                refuse("a local of type " + canonical(t) + " (" + at(lvd) + ")");
            }
            scopes.back()[nm] = s;
        }
    }

    // One Shared array per declaration per workgroup: the first work-item to
    // reach the declaration makes it, and every other binds the same storage.
    Val sharedArray(const std::string& nm, const CajetaTypePtr& t, const ExpressionPtr& e,
                    const void* site) {
        auto ne = std::dynamic_pointer_cast<NewExpression>(e);
        auto acr = std::dynamic_pointer_cast<ArrayCreatorRest>(ne->getCreatorRest());
        Val n = eval(std::dynamic_pointer_cast<Expression>(acr->getChildren()[0]));
        uint64_t count = indexOf(n, at(e));
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
    Val eval(const ExpressionPtr& e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e))
            return name(id->getTextValue(), e);
        if (auto il = std::dynamic_pointer_cast<IntegerLiteralExpression>(e)) return intLiteral(il);
        if (auto fl = std::dynamic_pointer_cast<FloatLiteralExpression>(e)) {
            std::string text = fl->getRawValue();
            // The host's rule: an `f` suffix is float32, anything else float64.
            bool f32 = !text.empty() && (text.back() == 'f' || text.back() == 'F');
            if (!text.empty() && std::strchr("fFdD", text.back())) text.pop_back();
            return mkFloat(f32 ? Prim::F32 : Prim::F64,
                           f32 ? (double) std::strtof(text.c_str(), nullptr)
                               : std::strtod(text.c_str(), nullptr));
        }
        if (auto tl = std::dynamic_pointer_cast<TextLiteralExpression>(e))
            return mkBool(tl->getRawValue() == "true");
        if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(e)) return binary(bin);
        if (auto pre = std::dynamic_pointer_cast<PrefixExpression>(e)) return prefix(pre);
        if (auto post = std::dynamic_pointer_cast<PostfixExpression>(e)) {
            Val old = eval(child(post, 0));
            Val one = old.k == Val::Scalar && isFloat(old.t.prim) ? mkFloat(old.t.prim, 1) : mkInt(old.t, 1);
            assign(child(post, 0), binaryOp(post->getOp() == POSTFIX_OP_INC ? BINARY_OP_ADD
                                                                          : BINARY_OP_SUB,
                                            old, one, at(e)));
            return old;
        }
        if (auto cast = std::dynamic_pointer_cast<CastExpression>(e)) {
            CajetaTypePtr ct = cast->getResolvedType() ? cast->getResolvedType() : cast->getDestType();
            Val v = eval(child(cast, 0));
            v.lit = false;
            return convert(v, *primOf(ct));
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            Val base = eval(child(ai, 0));
            Val idx = eval(child(ai, 1));
            if (base.k == Val::MemRef) return loadElem(*base.mem, indexOf(idx, at(e)), at(e));
            if (base.k == Val::Vector) {
                uint64_t l = indexOf(idx, at(e));
                if (l >= base.lanes.size())
                    undefined("lane " + std::to_string(l) + " of a " +
                              std::to_string(base.lanes.size()) + "-lane vector (" + at(e) + ")");
                return base.lanes[l];
            }
            refuse("indexing a value that is not a buffer, Shared array or vector (" + at(e) + ")");
        }
        if (auto dot = std::dynamic_pointer_cast<DotExpression>(e)) {
            auto lhs = std::dynamic_pointer_cast<IdentifierExpression>(child(dot, 0));
            if (lhs) {
                if (Slot* s = lookup(lhs->getTextValue()); s && s->v.k == Val::Vector) {
                    static const std::string comps = "xyzw";
                    size_t l = comps.find(dot->getIdentifier());
                    if (dot->getIdentifier().size() != 1 || l == std::string::npos
                            || l >= s->v.lanes.size())
                        refuse("the vector component `." + dot->getIdentifier() + "` (" + at(e) + ")");
                    return s->v.lanes[l];
                }
                if (auto c = CajetaType::lookupEnumConstant(lhs->getTextValue(), dot->getIdentifier()))
                    return mkInt({Prim::I32, true}, (uint64_t) (int64_t) *c);
            }
            refuse("the field access `." + dot->getIdentifier() + "` (" + at(e) + ")");
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(e)) return call(mc);
        if (auto ne = std::dynamic_pointer_cast<NewExpression>(e)) {
            const auto& ta = ne->getTypeArguments();
            Ty et = *primOf(ta[0]);
            auto ccr = std::dynamic_pointer_cast<ClassCreatorRest>(ne->getCreatorRest());
            auto cN = std::dynamic_pointer_cast<CajetaConstantType>(ta[1]);
            Val v;
            v.k = Val::Vector;
            v.t = et;
            for (auto& p : ccr->getParameters()) v.lanes.push_back(convert(eval(p.expression), et));
            if (!cN || v.lanes.size() != (size_t) cN->getValue())
                refuse("a Vector built from the wrong number of lanes (" + at(e) + ")");
            return v;
        }
        if (std::dynamic_pointer_cast<MoveExpression>(e)) return eval(child(e, 0));
        refuse("the expression " + std::string(ownership::toString(e->kind())) + " (" + at(e) + ")");
    }

    Val intLiteral(const std::shared_ptr<IntegerLiteralExpression>& il) {
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

    Val prefix(const std::shared_ptr<PrefixExpression>& pre) {
        ExpressionPtr operand = child(pre, 0);
        switch (pre->getOp()) {
            case PREFIX_OP_POSITIVE: return eval(operand);
            case PREFIX_OP_NEGATIVE: {
                Val v = eval(operand);
                if (v.k == Val::Scalar && isFloat(v.t.prim)) return mkFloat(v.t.prim, -v.f);
                Val zero = mkInt(v.t, 0);
                zero.lit = v.lit;
                Val r = binaryOp(BINARY_OP_SUB, zero, v, at(pre));
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
                                  old, one, at(pre));
                assign(operand, nv);
                return eval(operand);
            }
        }
        refuse("prefix operator (" + at(pre) + ")");
    }

    Val binary(const std::shared_ptr<BinaryOpExpression>& bin) {
        BinaryOp op = bin->getBinaryOp();
        ExpressionPtr l = child(bin, 0), r = child(bin, 1);
        if (bin->isAssignment()) {
            Val rv = eval(r);
            if (op != BINARY_OP_ASSIGN) rv = binaryOp(compoundBase(op), eval(l), rv, at(bin));
            assign(l, rv);
            return eval(l);
        }
        if (op == BINARY_OP_LOGAND) return mkBool(cond(l) && cond(r));
        if (op == BINARY_OP_LOGOR) return mkBool(cond(l) || cond(r));
        return binaryOp(op, eval(l), eval(r), at(bin));
    }

    // Store `v` through the l-value `e`, converted to its declared type.
    void assign(const ExpressionPtr& e, const Val& v) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
            Slot* s = lookup(id->getTextValue());
            if (!s) refuse("assigning `" + id->getTextValue() + "`, which is not a local (" + at(e) + ")");
            if (s->v.k == Val::Scalar) { s->v = convert(v, s->v.t); return; }
            if (s->v.k == Val::Vector && v.k == Val::Vector && v.lanes.size() == s->v.lanes.size()) {
                for (size_t l = 0; l < v.lanes.size(); ++l) s->v.lanes[l] = convert(v.lanes[l], s->v.t);
                return;
            }
            refuse("assigning `" + id->getTextValue() + "` (" + at(e) + ")");
        }
        if (auto ai = std::dynamic_pointer_cast<ArrayIndexExpression>(e)) {
            Val idx = eval(child(ai, 1));
            if (auto vid = std::dynamic_pointer_cast<IdentifierExpression>(child(ai, 0))) {
                if (Slot* s = lookup(vid->getTextValue()); s && s->v.k == Val::Vector) {
                    uint64_t l = indexOf(idx, at(e));
                    if (l >= s->v.lanes.size()) undefined("lane " + std::to_string(l) + " (" + at(e) + ")");
                    s->v.lanes[l] = convert(v, s->v.t);
                    return;
                }
            }
            Val base = eval(child(ai, 0));
            if (base.k != Val::MemRef) refuse("storing through `" + at(e) + "`");
            storeElem(*base.mem, indexOf(idx, at(e)), v, at(e));
            return;
        }
        refuse("an assignment target that is not a local or an element (" + at(e) + ")");
    }

    std::vector<Val> args(const std::shared_ptr<MethodCallExpression>& mc) {
        std::vector<Val> out;
        for (auto& p : mc->getParameters()) out.push_back(eval(p.expression));
        return out;
    }

    Val u32(uint64_t v) { return mkInt({Prim::I32, false}, v); }

    Val call(const std::shared_ptr<MethodCallExpression>& mc) {
        const std::string& nm = mc->getMethodCallName();
        auto recvE = child(mc, 0);
        auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(recvE);
        std::string where = at(mc);
        if (recvId && !lookup(recvId->getTextValue())
                && !R.buffers.count(recvId->getTextValue()))
            return staticCall(recvId->getTextValue() + "." + nm, mc);
        Val self = eval(recvE);
        std::vector<Val> a = args(mc);
        if (self.k == Val::MemRef) return bufferCall(self, nm, mc, a);
        if (self.k == Val::Vector && nm == "dotAccum") return dotAccum(self, a, where);
        if (self.k == Val::TileRef) return tileCall(*self.tile, nm, a, where);
        refuse("the call `." + nm + "` (" + where + ")");
    }

    Val bufferCall(const Val& self, const std::string& nm,
                   const std::shared_ptr<MethodCallExpression>& mc, const std::vector<Val>& a) {
        std::string where = at(mc);
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
        refuse("`" + m.name + "." + nm + "` (" + where + ")");
    }

    // dotAccum: unsigned-or-signed weights (the receiver's own signedness)
    // times SIGNED activations, four lanes into each int32 accumulator lane.
    Val dotAccum(const Val& w, const std::vector<Val>& a, const std::string& where) {
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
    Val tileCall(Tile& t, const std::string& nm, const std::vector<Val>& a, const std::string& where) {
        if (nm == "splat") {
            if (a.size() != 1) refuse("`splat` takes one value (" + where + ")");
            Val v = convert(a[0], t.elem);
            for (auto& x : t.e) x = v;
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
            for (uint32_t r = 0; r < t.rows; ++r)
                for (uint32_t c = 0; c < t.cols; ++c) {
                    uint64_t at = off + (layout == 0 ? r * stride + c : c * stride + r);
                    Val& x = t.e[(size_t) r * t.cols + c];
                    if (nm == "load") x = loadElem(m, at, where);
                    else storeElem(m, at, x, where);
                }
            return Val();
        }
        if (nm == "mma") {
            if (a.size() != 2 || a[0].k != Val::TileRef || a[1].k != Val::TileRef)
                refuse("`mma` takes (A, B) tiles (" + where + ")");
            const Tile& A = *a[0].tile;
            const Tile& B = *a[1].tile;
            if (A.rows != t.rows || B.cols != t.cols || A.cols != B.rows)
                refuse("`mma` on tiles whose shapes do not chain (" + where + ")");
            for (uint32_t r = 0; r < t.rows; ++r)
                for (uint32_t c = 0; c < t.cols; ++c) {
                    Val& acc = t.e[(size_t) r * t.cols + c];
                    if (isFloat(t.elem.prim)) {
                        // Accumulated in float32 in k order, one rounding per product
                        // and one per sum; float64 tiles in float64.
                        Prim p = t.elem.prim == Prim::F64 ? Prim::F64 : Prim::F32;
                        double s = acc.f;
                        for (uint32_t k = 0; k < A.cols; ++k) {
                            bool ic, cmp;
                            double prod = floatOp(BINARY_OP_MUL, p, convert(A.e[r * A.cols + k], {p, true}).f,
                                                  convert(B.e[k * B.cols + c], {p, true}).f, ic, cmp);
                            s = floatOp(BINARY_OP_ADD, p, s, prod, ic, cmp);
                        }
                        acc = mkFloat(t.elem.prim, s);
                    } else {
                        uint64_t s = acc.i;
                        for (uint32_t k = 0; k < A.cols; ++k) {
                            const Val& x = A.e[r * A.cols + k];
                            const Val& y = B.e[k * B.cols + c];
                            int64_t xv = x.t.sgn ? x.s64() : (int64_t) x.u64();
                            int64_t yv = y.t.sgn ? y.s64() : (int64_t) y.u64();
                            s += (uint64_t) (xv * yv);
                        }
                        acc = mkInt(t.elem, s);
                    }
                }
            return Val();
        }
        refuse("the tile operation `" + nm + "` (" + where + ")");
    }

    Val staticCall(const std::string& q, const std::shared_ptr<MethodCallExpression>& mc) {
        std::string where = at(mc);
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
        if (q == "Math.min" || q == "Math.max" || q == "Math.abs") return math(q, args(mc), where);
        std::vector<Val> a = args(mc);
        Arrival arr;
        arr.what = q;
        arr.site = mc.get();
        arr.where = where;
        if (q.rfind("Wave.", 0) == 0) {
            // Each argument at its declared parameter type.
            bool f32 = q == "Wave.reduceSumF32" || q == "Wave.reduceMaxF32";
            Ty pt = f32 ? Ty{Prim::F32, true} : Ty{Prim::I32, false};
            if (q == "Wave.ballotSync") pt = {Prim::Bool, false};
            if (a.empty()) refuse("`" + q + "` without its value (" + where + ")");
            arr.arg = (q == "Wave.shuffleSync" || q == "Wave.rotate") ? a[0] : convert(a[0], pt);
            if (a.size() > 1) arr.arg2 = convert(a[1], {Prim::I32, false});
        }
        return rendezvous(arr);
    }

    Val math(const std::string& q, const std::vector<Val>& a, const std::string& where) {
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
            refuse("`" + arr.what + "` outside a threaded run (" + arr.where + ")");
        std::unique_lock<std::mutex> lk(G.m);
        ItemSync& s = G.sync[me];
        s.arrival = arr;
        s.st = ItemSync::Waiting;
        G.turn = -1;
        G.cv.notify_all();
        G.cv.wait(lk, [&] { return G.turn == (int) me || G.abort; });
        if (G.abort) throw Abort{};
        return s.arrival.result;
    }
};

// ---- collectives -----------------------------------------------------------

void resolveWave(Group& G, const std::vector<uint32_t>& lanes, uint32_t W) {
    ItemSync& first = G.sync[lanes[0]];
    const std::string& q = first.arrival.what;
    const std::string& where = first.arrival.where;
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
    if (q == "Wave.reduceSumF32" || q == "Wave.reduceMaxF32") {
        // A butterfly over the full width, inactive lanes contributing the
        // identity: the association every lane computes identically.
        bool sum = q == "Wave.reduceSumF32";
        std::vector<float> v(W, sum ? 0.0f : -std::numeric_limits<float>::infinity());
        for (auto& [l, m] : byLane) v[l] = (float) argOf(m).f;
        for (uint32_t off = W / 2; off >= 1; off /= 2) {
            std::vector<float> nv(W);
            for (uint32_t l = 0; l < W; ++l)
                nv[l] = sum ? v[l] + v[l ^ off] : std::fmax(v[l], v[l ^ off]);
            v = nv;
        }
        for (uint32_t m : lanes) G.sync[m].arrival.result = mkFloat(Prim::F32, v[0]);
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

void runGroup(Group& G, bool threaded) {
    const uint32_t n = G.size;
    G.sync.assign(n, ItemSync());
    if (!threaded) {
        for (uint32_t m = 0; m < n; ++m) Item(G, m, false).body();
        return;
    }
    std::exception_ptr failure;
    std::vector<std::thread> workers;
    workers.reserve(n);
    for (uint32_t m = 0; m < n; ++m) {
        workers.emplace_back([&G, m, &failure] {
            {
                std::unique_lock<std::mutex> lk(G.m);
                G.cv.wait(lk, [&] { return G.turn == (int) m || G.abort; });
                if (G.abort) return;
            }
            std::exception_ptr ep;
            try {
                Item(G, m, true).body();
            } catch (Abort&) {
                return;
            } catch (...) {
                ep = std::current_exception();
            }
            std::lock_guard<std::mutex> lk(G.m);
            if (ep && !failure) failure = ep;
            G.sync[m].st = ItemSync::Done;
            G.turn = -1;
            G.cv.notify_all();
        });
    }
    auto stopAll = [&] {
        {
            std::lock_guard<std::mutex> lk(G.m);
            G.abort = true;
            G.cv.notify_all();
        }
        for (auto& t : workers) t.join();
    };
    try {
        for (;;) {
            bool ranAny = false;
            for (uint32_t m = 0; m < n; ++m) {
                std::unique_lock<std::mutex> lk(G.m);
                if (G.sync[m].st != ItemSync::Ready) continue;
                G.turn = (int) m;
                G.cv.notify_all();
                G.cv.wait(lk, [&] { return G.turn == -1; });
                ranAny = true;
                if (failure) break;
            }
            if (failure) break;
            bool allDone = true;
            for (auto& s : G.sync) if (s.st != ItemSync::Done) allDone = false;
            if (allDone) break;
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
        stopAll();
        throw;
    }
    stopAll();
    if (failure) std::rethrow_exception(failure);
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
    R.kernel = kernel;
    R.cls = kernel->getParent();
    R.launch = launch;
    R.wave = launch.waveWidth;
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
            Mem m;
            m.name = p->getName();
            m.elem = *primOf(typeArg(t, 0));
            m.data = (uint8_t*) a.data;
            m.count = a.count;
            R.buffers[p->getName()] = m;
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

    const uint32_t* g = launch.grid;
    const uint32_t* b = launch.block;
    for (uint32_t z = 0; z < g[2]; ++z)
        for (uint32_t y = 0; y < g[1]; ++y)
            for (uint32_t x = 0; x < g[0]; ++x) {
                Group G(R);
                G.id[0] = x; G.id[1] = y; G.id[2] = z;
                G.size = b[0] * b[1] * b[2];
                runGroup(G, survey.usesCollective);
            }
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
