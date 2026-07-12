#ifndef CAJETA_XREF_INDEX_H
#define CAJETA_XREF_INDEX_H

// The compiler's resolved view of a source root, source-mapped, as a
// machine-readable index for external tools (ide-symbol-index spec §2).
//
// Why this exists: the IntelliJ plugin parses Cajeta but does not understand it.
// The alternative — reimplementing Cajeta's name resolution, overload selection
// and override matching in Kotlin — has no oracle to be pinned against
// (`cajeta doc --emit-model-json` PARSES but does not RESOLVE; its
// extends/implements are raw declared names), and a resolver that silently
// disagrees with the compiler is worse than none: it sends you to the wrong
// declaration with total confidence, and renames the wrong thing. So the compiler
// exports what it already computed, and the IDE presents it.
//
// Contract: specs/schemas/cajeta-xref-v1.schema.json.
//
// Determinism is required (spec §2.0.7): the writer sorts every relation, so the
// same input yields byte-identical output.

#include <cstdint>
#include <string>
#include <vector>

namespace cajeta::xref {

    // Schema version. Bump MAJOR on any breaking change — consumers are required
    // to REFUSE an unknown major rather than guess at the contents.
    constexpr int kSchemaMajor = 1;
    constexpr int kSchemaMinor = 0;

    // A source span. Line is 1-based, column 0-based — the ANTLR convention the
    // compiler already carries on every AbstractSyntaxNode.
    struct SourceRef {
        std::string file;   // relative to the source root
        int line = 0;
        int col = 0;

        bool valid() const { return !file.empty() && line > 0; }
    };

    struct Declaration {
        std::string fqn;
        std::string kind;         // class | interface | method | field | ...
        std::string owner;        // declaring type, for members
        std::string signature;    // methods/constructors
        // Identifies ONE overload: the compiler's own canonical unlabeled
        // signature. Two same-name, same-arity overloads MUST differ here — if
        // they collide, "who calls f(int32)" returns f(String)'s callers, and
        // renaming one overload rewrites the other's call sites.
        std::string overloadKey;
        std::vector<std::string> modifiers;
        SourceRef at;
    };

    struct InheritanceEdge {
        std::string child;    // FQN
        std::string parent;   // RESOLVED FQN — never the raw declared name
        std::string kind;     // extends | implements
        SourceRef at;
    };

    struct Reference {      // Unit 2
        std::string target;
        std::string kind;
        std::string from;
        SourceRef at;
    };

    struct OverrideEdge {   // Unit 2
        std::string method;      // overloadKey of the overrider
        std::string overrides;   // overloadKey of the overridden
        SourceRef at;
    };

    struct Call {           // Unit 2
        std::string callee;      // overloadKey of the STATIC target
        std::string caller;      // overloadKey of the enclosing method
        bool isVirtual = false;
        SourceRef at;
    };

    class XrefIndex {
    public:
        void setSourceRoot(const std::string& root) { sourceRoot_ = root; }

        void addDeclaration(Declaration d) { declarations_.push_back(std::move(d)); }
        void addInheritance(InheritanceEdge e) { inheritance_.push_back(std::move(e)); }
        void addReference(Reference r) { references_.push_back(std::move(r)); }
        void addOverride(OverrideEdge o) { overrides_.push_back(std::move(o)); }
        void addCall(Call c) { calls_.push_back(std::move(c)); }

        // Sort + de-duplicate every relation, then render. Deterministic by
        // construction: the same input yields byte-identical output.
        std::string toJson() const;

        // Render and write to `path`. Returns false (and leaves no partial file)
        // if the file cannot be opened.
        bool writeToFile(const std::string& path) const;

        bool empty() const {
            return declarations_.empty() && inheritance_.empty()
                && references_.empty() && overrides_.empty() && calls_.empty();
        }

    private:
        std::string sourceRoot_;
        std::vector<Declaration> declarations_;
        std::vector<InheritanceEdge> inheritance_;
        std::vector<Reference> references_;
        std::vector<OverrideEdge> overrides_;
        std::vector<Call> calls_;
    };

    // Build the index for everything the compiler currently holds resolved:
    // walks the canonical type registry, emitting declarations and inheritance
    // edges (Unit 1). `sourceRoot` is stripped from recorded paths.
    void collectDeclarationsAndInheritance(XrefIndex& index,
                                           const std::string& sourceRoot);

} // namespace cajeta::xref

#endif // CAJETA_XREF_INDEX_H
