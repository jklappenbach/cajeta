// cajetadoc — declaration model (plan §2.2.1).
//
// Pure structure produced by source ingestion: packages -> types ->
// members, each carrying its signature, modifiers, visibility, type
// parameters, source location, and the raw text of its attached `/** */`
// block. No comment parsing, no HTML — those live in DocComment / Render.
#ifndef CAJETADOC_MODEL_H
#define CAJETADOC_MODEL_H

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cajetadoc {

struct DocComment; // defined in DocComment.h; attached after parsing (§3).

// A location in a source file, relative to the ingest root.
struct SourceRef {
    std::string file; // path relative to the source root
    int line = 0;     // 1-based
    int col = 0;      // 0-based (antlr convention)
};

enum class TypeKind { Class, Interface, Enum, View, Annotation, Unknown };
enum class MemberKind { Field, Constructor, Destructor, Method, EnumConstant };

// Java/cajeta visibility. `Package` is the default (no explicit modifier).
enum class Visibility { Public, Protected, Private, Package };

const char* toString(TypeKind k);
const char* toString(MemberKind k);
const char* toString(Visibility v);

// A generic type parameter, e.g. `<T extends Comparable>`.
struct TypeParam {
    std::string name;
    std::string bound; // text after `extends`, or empty
};

// A formal parameter of a method/constructor.
struct Param {
    std::string name;
    std::string type;             // rendered type text (preserves spacing)
    bool ownershipTransfer = false; // leading `#` (REFERENCE) ownership marker
    bool variadic = false;          // trailing `...`
};

struct Member {
    MemberKind kind = MemberKind::Method;
    std::string name;
    std::string returnType;          // method return / field type ("" for ctor)
    bool returnTransfer = false;     // `#`-return (ownership to caller)
    std::vector<TypeParam> typeParams; // method-level templates
    std::vector<Param> params;
    std::vector<std::string> throws;   // declared exception types
    Visibility visibility = Visibility::Package;
    std::set<std::string> modifiers;   // static, final, abstract, async, native…
    std::vector<std::string> annotations; // raw annotation text, e.g. @Native("…")
    SourceRef loc;
    std::string rawDoc;                // raw `/** */` text, or empty
    std::shared_ptr<DocComment> doc;   // parsed form (filled by §3), or null

    // A readable one-line signature for display/snapshotting.
    std::string signature() const;
};

struct Type {
    TypeKind kind = TypeKind::Unknown;
    std::string name;
    std::string packageName;             // dotted, e.g. "cajeta.hash"
    std::vector<TypeParam> typeParams;
    std::vector<std::string> extends;    // superclass(es) / superinterfaces
    std::vector<std::string> implements; // implemented interfaces
    Visibility visibility = Visibility::Package;
    std::set<std::string> modifiers;
    std::vector<std::string> annotations;
    std::vector<Member> members;
    std::vector<Type> nested;            // nested/inner types
    SourceRef loc;
    std::string rawDoc;
    std::shared_ptr<DocComment> doc;

    std::string qualifiedName() const; // packageName + "." + name (handles empty pkg)
};

struct Package {
    std::string name;          // dotted; "" for the default package
    std::vector<Type> types;
    std::string rawDoc;        // from a package.cajeta doc comment, if any
    std::shared_ptr<DocComment> doc;
};

// The whole ingested corpus.
struct Model {
    std::vector<Package> packages;

    Package* findPackage(const std::string& name);
    Package& ensurePackage(const std::string& name); // get-or-create
    int typeCount() const;
};

} // namespace cajetadoc

#endif // CAJETADOC_MODEL_H
