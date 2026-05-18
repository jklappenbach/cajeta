//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <set>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include "QualifiedName.h"

using namespace std;

namespace cajeta {

    // Captured value of a single annotation argument.
    //
    // Cajeta annotations carry typed literal arguments (`@Order(2)`,
    // `@Component(name = "disk")`, `@Profile("prod")`,
    // `@SuppressLint({"a", "b"})`). This struct holds one such value;
    // an annotation with multiple arguments has multiple entries on
    // its AnnotationInstance.
    //
    // `name` is the user-written parameter name, or empty for the
    // single-unnamed-arg form (`@Order(2)`). Consumers look up by
    // the conventional implicit name "value" — findArg treats an
    // empty-name arg as matching the key "value" so call sites stay
    // uniform regardless of which form the user wrote.
    //
    // The variant is open-coded (kind tag + per-shape storage) rather
    // than std::variant to keep header-only inclusion simple and the
    // accessors trivially inlinable. New kinds (nested annotation,
    // class literal, enum constant) can be added when A2+ needs them.
    enum class AnnotationArgKind {
        Int64,
        String,
        Bool,
        ClassRef,                    // `Foo.class` — strVal holds the type name (no `.class` suffix)
        Int64List,
        StringList,
        BoolList,
    };

    struct AnnotationArg {
        string name;                 // "" for the unnamed-arg form (`@Order(2)`)
        AnnotationArgKind kind = AnnotationArgKind::String;
        int64_t i64Val = 0;
        string strVal;               // also used by ClassRef (the type name)
        bool boolVal = false;
        vector<int64_t> i64List;
        vector<string> strList;
        vector<bool> boolList;
    };

    class AnnotationInstance {
        QualifiedNamePtr name;
        vector<AnnotationArg> args;
    public:
        explicit AnnotationInstance(QualifiedNamePtr name) : name(std::move(name)) { }

        const QualifiedNamePtr& getName() const { return name; }
        const vector<AnnotationArg>& getArgs() const { return args; }
        vector<AnnotationArg>& getMutableArgs() { return args; }

        void addArg(AnnotationArg arg) { args.push_back(std::move(arg)); }

        // Find an argument by name. `key == "value"` also matches an
        // unnamed single-arg (the spec-side "no name" form) — call
        // sites can read `findArg("value")` uniformly.
        const AnnotationArg* findArg(const string& key) const {
            for (auto& a : args) {
                if (a.name == key) return &a;
                if (key == "value" && a.name.empty()) return &a;
            }
            return nullptr;
        }

        // Typed accessors. Each returns a sentinel default when the
        // requested arg is missing OR has the wrong kind, so consumers
        // can call `getString("name")` without first checking findArg.
        // Tests / call sites that need the absence-vs-wrong-type
        // distinction should use findArg directly.
        string getString(const string& key = "value", const string& fallback = "") const {
            auto* a = findArg(key);
            return (a && a->kind == AnnotationArgKind::String) ? a->strVal : fallback;
        }
        int64_t getInt(const string& key = "value", int64_t fallback = 0) const {
            auto* a = findArg(key);
            return (a && a->kind == AnnotationArgKind::Int64) ? a->i64Val : fallback;
        }
        bool getBool(const string& key = "value", bool fallback = false) const {
            auto* a = findArg(key);
            return (a && a->kind == AnnotationArgKind::Bool) ? a->boolVal : fallback;
        }
        // Returns the captured type name (no `.class` suffix), or
        // empty string if the arg is missing or not a class literal.
        // Pointcut arguments — `@Before(Audited.class)` — capture as
        // ClassRef with strVal = "Audited".
        string getClassRef(const string& key = "value") const {
            auto* a = findArg(key);
            return (a && a->kind == AnnotationArgKind::ClassRef) ? a->strVal : string();
        }
        const vector<string>& getStringList(const string& key = "value") const {
            static const vector<string> empty;
            auto* a = findArg(key);
            return (a && a->kind == AnnotationArgKind::StringList) ? a->strList : empty;
        }
        const vector<int64_t>& getIntList(const string& key = "value") const {
            static const vector<int64_t> empty;
            auto* a = findArg(key);
            return (a && a->kind == AnnotationArgKind::Int64List) ? a->i64List : empty;
        }
    };
    typedef shared_ptr<AnnotationInstance> AnnotationInstancePtr;

    class Annotatable {
    protected:
        // By-name set + ordered-name list. Preserved for the call
        // sites (and tests) that only care whether an annotation is
        // present, not its argument values.
        set<QualifiedNamePtr> annotations;
        list<QualifiedNamePtr> annotationList;
        // Captured instances with their argument values. Populated by
        // the visitor in lockstep with addAnnotation when an
        // annotation has arguments worth keeping. An annotation
        // without arguments still appears here as an instance with an
        // empty args vector, so findAnnotation works uniformly.
        vector<AnnotationInstancePtr> annotationInstances;
        // Lint rule IDs suppressed at this declaration via
        // @SuppressLint(...). See cajeta-docs/LintRules.md. Now
        // derived from the captured AnnotationInstance for
        // @SuppressLint, kept as a denormalized vector so the
        // hot-path lint check (isLintSuppressed) is an O(N) scan
        // over a tiny list, not a per-rule annotation-arg dispatch.
        vector<string> suppressedLints;
    public:
        Annotatable() { }

        Annotatable(set<QualifiedNamePtr>& src) {
            // Mirror both containers: callers expect annotations and
            // annotationList to be aligned (RTTI's
            // createParameterType walks the list to build the LLVM
            // type while createParameterConstant walks the set to
            // build the initializer — a size mismatch trips the LLVM
            // verifier with "Invalid size request on a scalable
            // vector"). FormalParameter::fromContext takes this path.
            annotations.insert(src.begin(), src.end());
            for (auto& q : src) annotationList.push_back(q);
        }

        void addAnnotation(QualifiedNamePtr qName) {
            annotationList.push_back(qName);
            annotations.insert(qName);
        }

        set<QualifiedNamePtr>& getAnnotations() {
            return annotations;
        }

        list<QualifiedNamePtr>& getAnnotationList() { return annotationList; }

        // Per-instance accessors. addAnnotationInstance ALSO calls
        // addAnnotation so the by-name lookups stay consistent — call
        // sites pick whichever shape they need.
        void addAnnotationInstance(AnnotationInstancePtr inst) {
            if (inst && inst->getName()) {
                addAnnotation(inst->getName());
            }
            annotationInstances.push_back(std::move(inst));
        }

        const vector<AnnotationInstancePtr>& getAnnotationInstances() const {
            return annotationInstances;
        }

        // Look up an instance by short type name (e.g. "Component").
        // Returns the first match — annotations are not repeatable
        // in v1, so first-match equals only-match in practice.
        // Matches on the short typeName so callers don't need to
        // know what package a user-defined annotation lives in.
        AnnotationInstancePtr findAnnotation(const string& shortName) const {
            for (auto& a : annotationInstances) {
                if (a && a->getName() && a->getName()->getTypeName() == shortName) {
                    return a;
                }
            }
            return nullptr;
        }

        void addSuppressedLint(const string& ruleId) {
            suppressedLints.push_back(ruleId);
        }

        bool isLintSuppressed(const string& ruleId) const {
            for (const auto& id : suppressedLints) {
                if (id == ruleId) return true;
            }
            return false;
        }

        const vector<string>& getSuppressedLints() const { return suppressedLints; }

        string toCanonical() {
            string result;
            for (QualifiedNamePtr qName : annotationList) {
                result.append(string("@") + qName->toCanonical()).append(" ");
            }
            return result;
        }
    };
}
