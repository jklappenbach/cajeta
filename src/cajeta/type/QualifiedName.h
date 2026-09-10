// QualifiedName - an interned package + type name and its canonical string form.
#pragma once

#include <string>
#include "CajetaParser.h"
#include <map>

using namespace std;

namespace cajeta {
    class QualifiedName;
    typedef shared_ptr<QualifiedName> QualifiedNamePtr;

    class QualifiedName {
    private:
        static thread_local map <string, map<string, QualifiedNamePtr>> cache;  // per-thread (threadsafe U4)
        string packageName;
        string typeName;
        string canonical;
    public:

        QualifiedName(string typeName) {
            this->typeName = typeName;
            this->packageName = "";
            canonical = typeName;
        }

        QualifiedName(string typeName, string packageName) {
            this->typeName = typeName;
            this->packageName = packageName;
            if (!packageName.empty()) {
                canonical.append(packageName);
                canonical.append(".");
            }
            canonical.append(typeName);
        }

        QualifiedName(const QualifiedName& src) {
            packageName = src.packageName;
            typeName = src.typeName;
            if (!packageName.empty()) {
                canonical.append(packageName);
                canonical.append(".");
            }
            canonical.append(typeName);
        }

    public:
        QualifiedNamePtr toArrayType();

        const string& getPackageName() const {
            return packageName;
        }

        const string& getTypeName() const {
            return typeName;
        }

        const string toString() {
            return packageName + "." + typeName;
        }

        const string& toCanonical() { return canonical; }

        bool operator<(const QualifiedNamePtr& rhs) const {
            // Must stay a strict weak ordering: std::set/std::sort depend on it.
            if (typeName != rhs->typeName) return typeName < rhs->typeName;
            return packageName < rhs->packageName;
        }

        bool operator==(const QualifiedNamePtr& source) const {
            return typeName == source->typeName && packageName == source->packageName;
        }

        static map <string, map<string, QualifiedNamePtr>>& getCache();

        // Interns a DOTTED name: everything before the last dot is the package, and a
        // name with no dot interns under the empty one. `typeName` is taken by value
        // because it is tokenized in place.
        static QualifiedNamePtr getOrCreate(string typeName);

        // Interns an already-split package/type pair, keyed cache[package][typeName] as
        // getOrCreate keys it, so both entry points return the same instance.
        static QualifiedNamePtr getOrInsert(string typeName, string packageName);

        // Interns the name a parsed identifier chain spells: the last identifier is the
        // type, the ones before it the package. A lone identifier lands in "code".
        static QualifiedNamePtr fromContext(std::vector<CajetaParser::IdentifierContext*> identifiers);

        static QualifiedNamePtr fromContext(CajetaParser::QualifiedNameContext* ctxQName);

        static QualifiedNamePtr fromContext(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext);

        static void free();
    };

    typedef shared_ptr<QualifiedName> QualifiedNamePtr;
}
