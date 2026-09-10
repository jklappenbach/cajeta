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

    // One captured annotation-argument value; an annotation with several arguments
    // holds several. The variant is open-coded rather than std::variant to keep this
    // header-only and the accessors inlinable.
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

        // `key == "value"` also matches the unnamed single-arg form, so call sites
        // can read `findArg("value")` whichever way the user wrote it.
        const AnnotationArg* findArg(const string& key) const {
            for (auto& a : args) {
                if (a.name == key) return &a;
                if (key == "value" && a.name.empty()) return &a;
            }
            return nullptr;
        }

        // Each falls back when the argument is missing OR of the wrong kind.
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
        // The captured type name without the `.class` suffix, or empty.
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
        // By-name set and ordered list, for call sites that only ask presence.
        set<QualifiedNamePtr> annotations;
        list<QualifiedNamePtr> annotationList;
        // Instances with their argument values, in lockstep with addAnnotation. An
        // argument-less annotation still appears, so findAnnotation works uniformly.
        vector<AnnotationInstancePtr> annotationInstances;
        // Rule ids from @SuppressLint, denormalized out of the instance so the
        // hot-path check is a scan of a tiny list, not an argument dispatch.
        vector<string> suppressedLints;
    public:
        Annotatable() { }

        Annotatable(set<QualifiedNamePtr>& src) {
            // Both containers must stay aligned: RTTI builds the LLVM type from the
            // list and its initializer from the set, and a size mismatch trips the
            // verifier with "Invalid size request on a scalable vector".
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

        // Also calls addAnnotation, so the by-name lookups stay consistent.
        void addAnnotationInstance(AnnotationInstancePtr inst) {
            if (inst && inst->getName()) {
                addAnnotation(inst->getName());
            }
            annotationInstances.push_back(std::move(inst));
        }

        const vector<AnnotationInstancePtr>& getAnnotationInstances() const {
            return annotationInstances;
        }

        // By SHORT type name, so callers need not know the annotation's package;
        // the first match, which is the only match while none are repeatable.
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
