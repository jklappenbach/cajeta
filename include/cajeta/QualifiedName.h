//
// Created by James Klappenbach on 10/1/22.
//

#pragma once
#include <string>
#include <CajetaParser.h>
#include <map>

using namespace std;

namespace cajeta {

    class QualifiedName {
    private:
        static map<string, map<string, QualifiedName*>> global;
        string packageName;
        string typeName;

        QualifiedName(string name, string packageName) {
            this->typeName = name;
            this->packageName = packageName;
        }
        QualifiedName(const QualifiedName& src) {
            packageName = src.packageName;
            typeName = src.typeName;
        }
        static QualifiedName* toQualifiedName(std::vector<CajetaParser::IdentifierContext*> identifiers);
    public:
        const string& getPackageName() const {
            return packageName;
        }

        const string& getTypeName() const {
            return typeName;
        }

        bool operator <(const QualifiedName& rhs) const {
            return typeName < rhs.typeName && packageName < rhs.packageName;
        }
        static map<string, map<string, QualifiedName*>> getGlobal();
        static QualifiedName* toQualifiedName(string typeName);
        static QualifiedName* toQualifiedName(string typeName, string packageName);
        static QualifiedName* toQualifiedName(CajetaParser::QualifiedNameContext* ctxQName);
        static QualifiedName* toQualifiedName(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext);
        static void free();
    };
}
