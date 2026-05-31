#include "cajeta/dbg/DebugVars.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace cajeta::dbg {

// Stateless frame-chain accessors, defined in the native runtime copy
// (runtime/native/cajeta_runtime.c) linked into this binary. See DebugVars.h.
extern "C" {
    int __cajeta_dbg_frame_depth(void* top);
    void* __cajeta_dbg_frame_prev(void* frame);
    const char* __cajeta_dbg_frame_func(void* frame);
    int32_t __cajeta_dbg_frame_loc(void* frame);
    int __cajeta_dbg_frame_nlocals(void* frame);
    const char* __cajeta_dbg_local_name(void* frame, int i);
    const char* __cajeta_dbg_local_type(void* frame, int i);
    void* __cajeta_dbg_local_addr(void* frame, int i);
}

std::vector<DbgFrameInfo> walkFrames(void* top) {
    std::vector<DbgFrameInfo> out;
    for (void* f = top; f != nullptr; f = __cajeta_dbg_frame_prev(f)) {
        DbgFrameInfo info;
        const char* fn = __cajeta_dbg_frame_func(f);
        info.func = fn ? fn : "";
        info.locId = __cajeta_dbg_frame_loc(f);
        int n = __cajeta_dbg_frame_nlocals(f);
        for (int i = 0; i < n; ++i) {
            DbgVar v;
            const char* nm = __cajeta_dbg_local_name(f, i);
            const char* ty = __cajeta_dbg_local_type(f, i);
            v.name = nm ? nm : "";
            v.type = ty ? ty : "";
            v.addr = __cajeta_dbg_local_addr(f, i);
            info.locals.push_back(std::move(v));
        }
        out.push_back(std::move(info));
    }
    return out;
}

namespace {
    // Primitive value categories keyed by cajeta canonical type name. The
    // matrix mirrors CajetaType::init's NATIVE_TYPE_ENTRY registrations.
    enum class Prim { None, Bool, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64 };

    Prim classify(const std::string& type) {
        if (type == "boolean") return Prim::Bool;
        if (type == "int8") return Prim::I8;
        if (type == "uint8" || type == "uchar") return Prim::U8;
        if (type == "int16") return Prim::I16;
        if (type == "uint16") return Prim::U16;
        // `char` is a 32-bit Unicode codepoint (see CajetaType::init).
        if (type == "int32" || type == "char") return Prim::I32;
        if (type == "uint32") return Prim::U32;
        if (type == "int64") return Prim::I64;
        if (type == "uint64") return Prim::U64;
        if (type == "float32") return Prim::F32;
        if (type == "float64") return Prim::F64;
        return Prim::None;
    }

    // Read a primitive into either the integer or the float domain. Returns
    // false for non-primitives. boolean/char fold into the integer domain.
    bool readNumber(Prim p, void* addr, bool& isFloat,
                    long long& ival, double& dval) {
        isFloat = false;
        switch (p) {
            case Prim::Bool: ival = (*reinterpret_cast<uint8_t*>(addr) & 1); return true;
            case Prim::I8:  ival = *reinterpret_cast<int8_t*>(addr);   return true;
            case Prim::U8:  ival = *reinterpret_cast<uint8_t*>(addr);  return true;
            case Prim::I16: ival = *reinterpret_cast<int16_t*>(addr);  return true;
            case Prim::U16: ival = *reinterpret_cast<uint16_t*>(addr); return true;
            case Prim::I32: ival = *reinterpret_cast<int32_t*>(addr);  return true;
            case Prim::U32: ival = *reinterpret_cast<uint32_t*>(addr); return true;
            case Prim::I64: ival = *reinterpret_cast<int64_t*>(addr);  return true;
            case Prim::U64: ival = static_cast<long long>(
                                *reinterpret_cast<uint64_t*>(addr));  return true;
            case Prim::F32: dval = *reinterpret_cast<float*>(addr);
                            isFloat = true; return true;
            case Prim::F64: dval = *reinterpret_cast<double*>(addr);
                            isFloat = true; return true;
            case Prim::None: return false;
        }
        return false;
    }

    template <typename T>
    bool applyOp(const std::string& op, T l, T r, bool& okOp) {
        okOp = true;
        if (op == "==") return l == r;
        if (op == "!=") return l != r;
        if (op == "<")  return l <  r;
        if (op == "<=") return l <= r;
        if (op == ">")  return l >  r;
        if (op == ">=") return l >= r;
        okOp = false;
        return false;
    }

    std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }
}

std::string formatValue(const std::string& type, void* addr) {
    if (!addr) return "<null>";
    switch (classify(type)) {
        case Prim::Bool: {
            uint8_t v = *reinterpret_cast<uint8_t*>(addr);
            return (v & 1) ? "true" : "false";
        }
        case Prim::I8:  return std::to_string(*reinterpret_cast<int8_t*>(addr));
        case Prim::U8:  return std::to_string(*reinterpret_cast<uint8_t*>(addr));
        case Prim::I16: return std::to_string(*reinterpret_cast<int16_t*>(addr));
        case Prim::U16: return std::to_string(*reinterpret_cast<uint16_t*>(addr));
        case Prim::I32: return std::to_string(*reinterpret_cast<int32_t*>(addr));
        case Prim::U32: return std::to_string(*reinterpret_cast<uint32_t*>(addr));
        case Prim::I64: return std::to_string(*reinterpret_cast<int64_t*>(addr));
        case Prim::U64: return std::to_string(*reinterpret_cast<uint64_t*>(addr));
        case Prim::F32: {
            std::ostringstream os; os << *reinterpret_cast<float*>(addr);
            return os.str();
        }
        case Prim::F64: {
            std::ostringstream os; os << *reinterpret_cast<double*>(addr);
            return os.str();
        }
        case Prim::None: break;
    }
    // Opaque object: the slot holds the heap pointer. Render <type@0xADDR>.
    void* obj = *reinterpret_cast<void**>(addr);
    std::ostringstream os;
    os << "<" << type << "@" << obj << ">";
    return os.str();
}

bool writeValue(const std::string& type, void* addr,
                const std::string& text, std::string* err) {
    if (!addr) {
        if (err) *err = "no address for variable";
        return false;
    }
    Prim p = classify(type);
    if (p == Prim::None) {
        if (err) *err = "cannot set non-primitive value of type " + type;
        return false;
    }
    try {
        switch (p) {
            case Prim::Bool: {
                bool v = (text == "true" || text == "1");
                if (!v && text != "false" && text != "0") {
                    if (err) *err = "expected true/false";
                    return false;
                }
                *reinterpret_cast<uint8_t*>(addr) = v ? 1 : 0;
                return true;
            }
            case Prim::I8:  *reinterpret_cast<int8_t*>(addr)   = (int8_t) std::stoll(text);  return true;
            case Prim::U8:  *reinterpret_cast<uint8_t*>(addr)  = (uint8_t) std::stoull(text); return true;
            case Prim::I16: *reinterpret_cast<int16_t*>(addr)  = (int16_t) std::stoll(text);  return true;
            case Prim::U16: *reinterpret_cast<uint16_t*>(addr) = (uint16_t) std::stoull(text);return true;
            case Prim::I32: *reinterpret_cast<int32_t*>(addr)  = (int32_t) std::stoll(text);  return true;
            case Prim::U32: *reinterpret_cast<uint32_t*>(addr) = (uint32_t) std::stoull(text);return true;
            case Prim::I64: *reinterpret_cast<int64_t*>(addr)  = (int64_t) std::stoll(text);  return true;
            case Prim::U64: *reinterpret_cast<uint64_t*>(addr) = (uint64_t) std::stoull(text);return true;
            case Prim::F32: *reinterpret_cast<float*>(addr)    = std::stof(text);  return true;
            case Prim::F64: *reinterpret_cast<double*>(addr)   = std::stod(text);  return true;
            case Prim::None: break;
        }
    } catch (const std::exception&) {
        if (err) *err = "could not parse '" + text + "' as " + type;
        return false;
    }
    if (err) *err = "unsupported type " + type;
    return false;
}

bool evaluateCondition(const std::string& expr,
                       const std::vector<DbgVar>& locals,
                       std::string* err) {
    // A malformed/unevaluable condition stops (returns true) with *err set.
    auto fail = [&](const std::string& m) -> bool {
        if (err) *err = m;
        return true;
    };

    // Locate the comparison operator: first of = < > !, widened to two chars
    // when followed by '='. This yields ==, !=, <, <=, >, >= (a lone '=' or
    // '!' is rejected below).
    size_t pos = expr.find_first_of("=<>!");
    if (pos == std::string::npos)
        return fail("no comparison operator in condition: " + expr);
    std::string op = (pos + 1 < expr.size() && expr[pos + 1] == '=')
                         ? expr.substr(pos, 2)
                         : expr.substr(pos, 1);
    if (op != "==" && op != "!=" && op != "<" && op != "<=" &&
        op != ">" && op != ">=")
        return fail("unsupported operator '" + op + "' in condition");

    std::string lhs = trim(expr.substr(0, pos));
    std::string rhs = trim(expr.substr(pos + op.size()));

    const DbgVar* var = nullptr;
    for (const auto& v : locals) {
        if (v.name == lhs) { var = &v; break; }
    }
    if (!var) return fail("unknown variable in condition: '" + lhs + "'");

    Prim p = classify(var->type);
    if (p == Prim::None)
        return fail("cannot evaluate condition on non-primitive '" + lhs + "'");

    bool isFloat = false;
    long long li = 0;
    double ld = 0;
    if (!var->addr || !readNumber(p, var->addr, isFloat, li, ld))
        return fail("cannot read variable '" + lhs + "'");

    try {
        bool okOp = false;
        bool res;
        if (isFloat) {
            res = applyOp<double>(op, ld, std::stod(rhs), okOp);
        } else {
            long long r;
            if (rhs == "true") r = 1;
            else if (rhs == "false") r = 0;
            else r = std::stoll(rhs);
            res = applyOp<long long>(op, li, r, okOp);
        }
        if (!okOp) return fail("unsupported operator '" + op + "'");
        if (err) err->clear();
        return res;
    } catch (const std::exception&) {
        return fail("could not parse literal '" + rhs + "' in condition");
    }
}

} // namespace cajeta::dbg
