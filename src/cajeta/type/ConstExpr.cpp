#include "ConstExpr.h"

#include <cctype>
#include <set>

namespace cajeta {

namespace {

struct Parser {
    const std::string& s;
    const ConstLookup& lookup;
    size_t i = 0;
    ConstExprResult res;

    bool failed() const { return !res.error.empty(); }

    void fail(const std::string& why) {
        if (res.error.empty()) res.error = why;
    }

    void ws() {
        while (i < s.size() && std::isspace((unsigned char) s[i])) ++i;
    }

    bool eat(const char* tok) {
        ws();
        size_t n = std::char_traits<char>::length(tok);
        if (s.compare(i, n, tok) != 0) return false;
        // `<` must not eat the first half of `<<`, and so on.
        if (n == 1 && i + 1 < s.size() && (tok[0] == '<' || tok[0] == '>')
                && s[i + 1] == tok[0])
            return false;
        i += n;
        return true;
    }

    std::string ident() {
        ws();
        size_t b = i;
        if (i < s.size() && (std::isalpha((unsigned char) s[i]) || s[i] == '_')) {
            ++i;
            while (i < s.size() && (std::isalnum((unsigned char) s[i]) || s[i] == '_'))
                ++i;
        }
        return s.substr(b, i - b);
    }

    static bool isPrimitive(const std::string& n) {
        static const std::set<std::string> prims = {
            "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32",
            "uint64", "int", "long", "short", "byte"};
        return prims.count(n) > 0;
    }

    int64_t primary() {
        ws();
        if (i >= s.size()) { fail("the expression ends early"); return 0; }
        if (s[i] == '(') {
            // A primitive cast, `(int64) x`, changes no value the evaluator
            // can hold: skip it.
            size_t save = i;
            ++i;
            std::string n = ident();
            ws();
            if (isPrimitive(n) && i < s.size() && s[i] == ')') {
                ++i;
                return unary();
            }
            i = save + 1;
            int64_t v = expr();
            if (!eat(")")) fail("a '(' is not closed");
            return v;
        }
        if (std::isdigit((unsigned char) s[i])) {
            int base = 10;
            if (s[i] == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                base = 16;
                i += 2;
            }
            int64_t v = 0;
            bool any = false;
            while (i < s.size()) {
                char c = s[i];
                int d = -1;
                if (c == '_') { ++i; continue; }
                if (std::isdigit((unsigned char) c)) d = c - '0';
                else if (base == 16 && std::isxdigit((unsigned char) c))
                    d = 10 + (std::tolower((unsigned char) c) - 'a');
                if (d < 0 || d >= base) break;
                v = v * base + d;
                any = true;
                ++i;
            }
            if (!any) fail("a number has no digits");
            while (i < s.size() && (s[i] == 'L' || s[i] == 'l' || s[i] == 'u' || s[i] == 'U'))
                ++i;
            return v;
        }
        std::string n = ident();
        if (n.empty()) {
            fail(std::string("unexpected '") + s[i] + "'");
            return 0;
        }
        std::optional<int64_t> v = lookup ? lookup(n) : std::nullopt;
        if (!v) {
            if (res.unbound.empty()) res.unbound = n;
            fail("'" + n + "' is not a compile-time constant");
            return 0;
        }
        return *v;
    }

    int64_t unary() {
        if (eat("-")) return -unary();
        if (eat("+")) return unary();
        if (eat("~")) return ~unary();
        return primary();
    }

    int64_t mul() {
        int64_t v = unary();
        while (!failed()) {
            if (eat("*")) v = v * unary();
            else if (eat("/")) {
                int64_t r = unary();
                if (r == 0) { fail("division by zero"); return 0; }
                v = v / r;
            } else if (eat("%")) {
                int64_t r = unary();
                if (r == 0) { fail("division by zero"); return 0; }
                v = v % r;
            } else break;
        }
        return v;
    }

    int64_t add() {
        int64_t v = mul();
        while (!failed()) {
            if (eat("+")) v = v + mul();
            else if (eat("-")) v = v - mul();
            else break;
        }
        return v;
    }

    int64_t shift() {
        int64_t v = add();
        while (!failed()) {
            if (eat("<<")) v = v << add();
            else if (eat(">>")) v = v >> add();
            else break;
        }
        return v;
    }

    int64_t band() {
        int64_t v = shift();
        while (!failed() && eat("&")) v = v & shift();
        return v;
    }

    int64_t bxor() {
        int64_t v = band();
        while (!failed() && eat("^")) v = v ^ band();
        return v;
    }

    int64_t expr() {
        int64_t v = bxor();
        while (!failed() && eat("|")) v = v | bxor();
        return v;
    }
};

} // namespace

ConstExprResult evalConstExpr(const std::string& text, const ConstLookup& lookup) {
    Parser p{text, lookup};
    int64_t v = p.expr();
    p.ws();
    if (!p.failed() && p.i != text.size())
        p.fail("unexpected '" + text.substr(p.i, 1) + "'");
    if (p.failed()) {
        p.res.ok = false;
        return p.res;
    }
    p.res.ok = true;
    p.res.value = v;
    return p.res;
}

} // namespace cajeta
