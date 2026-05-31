//
// Minimal JSON value model + parser/serializer for the DAP server (CP4).
//
// DAP message bodies are small and flat, so a tiny hand-rolled implementation
// is the right call — no third-party dependency. The repo's only other JSON
// (src/cajeta/codec/JsonSynthesizer) is a *compiler* feature emitting Cajeta
// IR, not a host DOM, so it can't be reused here.
//
// Scope: objects, arrays, strings, numbers (stored as double + an integer
// accessor), booleans, null. Enough for the DAP request/response/event shapes.
//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cajeta::dap {

    class Json {
    public:
        enum class Type { Null, Bool, Number, String, Array, Object };

        Json() : type_(Type::Null) {}
        Json(std::nullptr_t) : type_(Type::Null) {}
        Json(bool b) : type_(Type::Bool), bool_(b) {}
        Json(int n) : type_(Type::Number), num_(n) {}
        Json(int64_t n) : type_(Type::Number), num_(static_cast<double>(n)) {}
        Json(double n) : type_(Type::Number), num_(n) {}
        Json(const char* s) : type_(Type::String), str_(s) {}
        Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

        static Json array() { Json j; j.type_ = Type::Array; return j; }
        static Json object() { Json j; j.type_ = Type::Object; return j; }

        Type type() const { return type_; }
        bool isNull() const { return type_ == Type::Null; }
        bool isObject() const { return type_ == Type::Object; }
        bool isArray() const { return type_ == Type::Array; }
        bool isString() const { return type_ == Type::String; }
        bool isNumber() const { return type_ == Type::Number; }
        bool isBool() const { return type_ == Type::Bool; }

        // Scalar accessors with defaults (no throw on type mismatch).
        bool asBool(bool dflt = false) const {
            return type_ == Type::Bool ? bool_ : dflt;
        }
        double asNumber(double dflt = 0) const {
            return type_ == Type::Number ? num_ : dflt;
        }
        int asInt(int dflt = 0) const {
            return type_ == Type::Number ? static_cast<int>(num_) : dflt;
        }
        long asLong(long dflt = 0) const {
            return type_ == Type::Number ? static_cast<long>(num_) : dflt;
        }
        const std::string& asString(const std::string& dflt = empty_) const {
            return type_ == Type::String ? str_ : dflt;
        }

        // Object access. operator[] inserts a Null on miss (so you can build
        // objects with j["a"] = 1); at()/has() are const lookups.
        Json& operator[](const std::string& key);
        bool has(const std::string& key) const;
        const Json& at(const std::string& key) const;  // returns Null sentinel on miss

        // Array access / building.
        void push_back(Json value);
        size_t size() const;
        const Json& operator[](size_t i) const;  // Null sentinel on out-of-range

        const std::map<std::string, Json>& items() const { return obj_; }
        const std::vector<Json>& elements() const { return arr_; }

        // Serialize to a compact JSON string.
        std::string dump() const;

        // Parse `text`. On success returns the value and sets *ok=true (if
        // provided). On a parse error returns Null and sets *ok=false.
        static Json parse(const std::string& text, bool* ok = nullptr);

    private:
        Type type_;
        bool bool_ = false;
        double num_ = 0;
        std::string str_;
        std::vector<Json> arr_;
        std::map<std::string, Json> obj_;

        static const std::string empty_;
        static const Json nullSentinel_;

        void dumpTo(std::string& out) const;
        static void dumpString(const std::string& s, std::string& out);
    };

} // namespace cajeta::dap
