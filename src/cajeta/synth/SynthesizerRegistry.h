// Source-synthesis facility (nucleo Layer-1a): the trigger registry, the handler
// interfaces, and the synthesis context. One registry maps a trigger (an annotation
// or a generic/method-template instantiation) to exactly one synthesizer.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cajeta {
    class CajetaClass;  using CajetaClassPtr  = std::shared_ptr<CajetaClass>;
    class CajetaType;   using CajetaTypePtr   = std::shared_ptr<CajetaType>;
    class CajetaModule; using CajetaModulePtr = std::shared_ptr<CajetaModule>;
    class Method;       using MethodPtr       = std::shared_ptr<Method>;
}

namespace cajeta::synth {

    struct SynthesisContext {
        CajetaClassPtr parent;                  // declaring / enclosing class
        std::string methodName;                 // body synthesis: the method
        std::vector<CajetaTypePtr> typeArgs;    // monomorphized type arguments
        std::vector<CajetaTypePtr> paramTypes;  // body synthesis: params (no `this`)
        CajetaModulePtr module;
        // Declaration-time body dispatch only; null on the method-template path.
        MethodPtr method;
    };

    // Provides the body of a declared-but-bodyless method, or declines (nullopt).
    // With ctx.method set (declaration-time) return a `{ ... }` BODY BLOCK; with it
    // null (method-template instantiation) return FULL method source.
    using BodySynthesizer =
        std::function<std::optional<std::string>(const SynthesisContext&)>;

    // A `{ ... }` class-body fragment to inject, plus short-name imports it needs
    // (each injected only-when-unbound). An invalid trigger throws, injecting nothing.
    struct MemberSynthesisResult {
        std::string classBodyFragment;
        std::vector<std::pair<std::string, std::string>> imports;  // (short, package)
    };

    using MemberSynthesizer =
        std::function<std::optional<MemberSynthesisResult>(const SynthesisContext&)>;

    class SynthesizerRegistry {
    public:
        SynthesizerRegistry() = default;

        static SynthesizerRegistry& instance();

        void registerBody(std::string label, BodySynthesizer fn);

        void registerMember(std::string label, MemberSynthesizer fn);

        // COMPANION-CLASS synthesis: emits a whole sibling class, registered under
        // `className` in the trigger's user-visible package so source can name it.
        struct CompanionSynthesisResult {
            std::string className;    // short name, e.g. "TickCols"
            std::string packageName;  // registration package (the record's)
            std::string classSource;  // full `public class X { ... }` source
            std::vector<std::pair<std::string, std::string>> imports;
        };
        using CompanionSynthesizer = std::function<
            std::optional<CompanionSynthesisResult>(const SynthesisContext&)>;
        void registerCompanion(std::string label, CompanionSynthesizer fn);
        std::vector<std::pair<std::string, CompanionSynthesisResult>>
            collectCompanions(const SynthesisContext& ctx) const;
        std::size_t companionCount() const { return companionSynths.size(); }

        // Fragments of every member synthesizer that claims the target - several may
        // compose into one declaration. A validate-first rejection propagates.
        std::vector<std::pair<std::string, MemberSynthesisResult>>
            collectMembers(const SynthesisContext& ctx) const;

        std::size_t memberCount() const { return memberSynths.size(); }

        // Dispatch a body trigger. AT MOST ONE synthesizer may claim it; two matches
        // throw naming both, zero matches returns nullopt (the failsafe).
        std::optional<std::string> dispatchBody(const SynthesisContext& ctx) const;

        std::size_t bodyCount() const { return bodySynths.size(); }

    private:
        std::vector<std::pair<std::string, BodySynthesizer>> bodySynths;
        std::vector<std::pair<std::string, MemberSynthesizer>> memberSynths;
        std::vector<std::pair<std::string, CompanionSynthesizer>> companionSynths;
    };

    // Register the compiler's built-in body synthesizers into instance(). Idempotent.
    void registerBuiltinSynthesizers();

}
