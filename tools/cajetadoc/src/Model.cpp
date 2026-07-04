#include "cajetadoc/Model.h"

#include <sstream>

namespace cajetadoc {

const char* toString(TypeKind k) {
    switch (k) {
        case TypeKind::Class: return "class";
        case TypeKind::Interface: return "interface";
        case TypeKind::Enum: return "enum";
        case TypeKind::View: return "view";
        case TypeKind::Record: return "record";
        case TypeKind::Annotation: return "annotation";
        case TypeKind::Unknown: default: return "unknown";
    }
}

const char* toString(MemberKind k) {
    switch (k) {
        case MemberKind::Field: return "field";
        case MemberKind::Constructor: return "constructor";
        case MemberKind::Destructor: return "destructor";
        case MemberKind::Method: return "method";
        case MemberKind::EnumConstant: return "enumConstant";
        default: return "member";
    }
}

const char* toString(Visibility v) {
    switch (v) {
        case Visibility::Public: return "public";
        case Visibility::Protected: return "protected";
        case Visibility::Private: return "private";
        case Visibility::Package: default: return "package";
    }
}

std::string Member::signature() const {
    std::ostringstream os;
    // modifiers in a stable, conventional order
    static const char* order[] = {"public", "protected", "private", "static",
                                  "abstract", "final", "const", "async",
                                  "native", "volatile", "transient"};
    for (const char* m : order) {
        if (modifiers.count(m)) os << m << ' ';
    }
    if (!typeParams.empty()) {
        os << '<';
        for (size_t i = 0; i < typeParams.size(); ++i) {
            if (i) os << ", ";
            os << typeParams[i].name;
            if (!typeParams[i].bound.empty()) os << " extends " << typeParams[i].bound;
        }
        os << "> ";
    }
    if (kind == MemberKind::Method) {
        if (returnTransfer) os << '#';
        os << (returnType.empty() ? "void" : returnType) << ' ';
    } else if (kind == MemberKind::Field || kind == MemberKind::EnumConstant) {
        if (!returnType.empty()) os << returnType << ' ';
    }
    os << name;
    if (kind == MemberKind::Method || kind == MemberKind::Constructor ||
        kind == MemberKind::Destructor) {
        os << '(';
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) os << ", ";
            if (params[i].ownershipTransfer) os << '#';
            os << params[i].type << ' ' << params[i].name;
            if (params[i].variadic) os << "...";
        }
        os << ')';
    }
    if (!throws.empty()) {
        os << " throws ";
        for (size_t i = 0; i < throws.size(); ++i) {
            if (i) os << ", ";
            os << throws[i];
        }
    }
    return os.str();
}

std::string Type::qualifiedName() const {
    if (packageName.empty()) return name;
    return packageName + "." + name;
}

Package* Model::findPackage(const std::string& name) {
    for (auto& p : packages) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

Package& Model::ensurePackage(const std::string& name) {
    if (Package* p = findPackage(name)) return *p;
    packages.push_back(Package{});
    packages.back().name = name;
    return packages.back();
}

int Model::typeCount() const {
    int n = 0;
    for (const auto& p : packages) {
        for (const auto& t : p.types) {
            n += 1 + static_cast<int>(t.nested.size());
        }
    }
    return n;
}

} // namespace cajetadoc
