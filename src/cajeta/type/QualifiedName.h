//
// Created by James Klappenbach on 10/1/22.
//

#pragma once

#include <string>
#include "CajetaParser.h"
#include <map>
#include <llvm/ADT/Twine.h>

using namespace std;

namespace cajeta {
    class QualifiedName;
    typedef shared_ptr<QualifiedName> QualifiedNamePtr;

    class QualifiedName {
    private:
        static map <string, map<string, QualifiedNamePtr>> cache;
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
            return typeName < rhs->typeName && packageName < rhs->packageName;
        }

        bool operator==(const QualifiedNamePtr& source) const {
            return typeName == source->typeName && packageName == source->packageName;
        }

        static map <string, map<string, QualifiedNamePtr>>& getCache();

        static QualifiedNamePtr getOrCreate(string typeName);

        static QualifiedNamePtr getOrInsert(string typeName, string packageName);

        static QualifiedNamePtr fromContext(std::vector<CajetaParser::IdentifierContext*> identifiers);

        static QualifiedNamePtr fromContext(CajetaParser::QualifiedNameContext* ctxQName);

        static QualifiedNamePtr fromContext(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext);

        static void free();
    };

    typedef shared_ptr<QualifiedName> QualifiedNamePtr;
}
