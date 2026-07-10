//
// Source-synthesis facility (núcleo Layer-1a) — the trigger registry + handler
// interface + synthesis context. One registry maps a trigger (an annotation on
// a declaration, or a generic/method-template instantiation) to exactly one
// synthesizer, replacing the scattered `findAnnotation` checks and the
// if-else chain in MethodTemplateInstantiator. Spec §2 / §1.5; plan §2.
//
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

    // What a synthesizer sees: the resolved declaration plus its trigger
    // arguments. (Unit 3+ adds a reflection view and a diagnostics sink.)
    struct SynthesisContext {
        CajetaClassPtr parent;                  // declaring / enclosing class
        std::string methodName;                 // body synthesis: the method
        std::vector<CajetaTypePtr> typeArgs;    // monomorphized type arguments
        std::vector<CajetaTypePtr> paramTypes;  // body synthesis: params (no `this`)
        CajetaModulePtr module;
        // Declaration-time body dispatch only (Unit 6): the resolved bodyless
        // Method — carries the annotations, named formals, and return type a
        // signature-validating synthesizer (@Einsum) reads. Null on the
        // method-template instantiation path.
        MethodPtr method;
    };

    // A body synthesizer provides the body of a declared-but-bodyless method,
    // or declines (nullopt) so the caller keeps the captured/declared source.
    // Contract by dispatch site: with ctx.method set (declaration-time, the
    // caller splices into the original declaration) return a `{ ... }` BODY
    // BLOCK; with ctx.method null (method-template instantiation) return FULL
    // method source (the codec model). A synthesizer claims only its own site.
    using BodySynthesizer =
        std::function<std::optional<std::string>(const SynthesisContext&)>;

    // What a member synthesizer returns: a `{ ... }` class-body fragment to
    // inject into the target, plus any short-name imports the fragment needs
    // (each injected only-when-unbound). A synthesizer that validates its
    // trigger and finds it invalid throws a cajeta::Exception (validate-first,
    // spec §6) — the caller surfaces it as a user-attributed compile error and
    // injects nothing.
    struct MemberSynthesisResult {
        std::string classBodyFragment;
        std::vector<std::pair<std::string, std::string>> imports;  // (short, package)
    };

    // A member synthesizer inspects the target (via the context's `parent`) and
    // either declines (nullopt) or returns a fragment to inject.
    using MemberSynthesizer =
        std::function<std::optional<MemberSynthesisResult>(const SynthesisContext&)>;

    class SynthesizerRegistry {
    public:
        SynthesizerRegistry() = default;

        // The compiler-wide registry (the built-in synthesizers register here).
        static SynthesizerRegistry& instance();

        // Register a body synthesizer under a stable label (used in diagnostics).
        void registerBody(std::string label, BodySynthesizer fn);

        // Register a member synthesizer under a stable label.
        void registerMember(std::string label, MemberSynthesizer fn);

        // Collect the fragments of every member synthesizer that claims the
        // target (composition — several may inject into one declaration). A
        // synthesizer that throws (validate-first rejection) propagates. The
        // caller injects the returned fragments and detects name collisions.
        std::vector<std::pair<std::string, MemberSynthesisResult>>
            collectMembers(const SynthesisContext& ctx) const;

        std::size_t memberCount() const { return memberSynths.size(); }

        // Dispatch a body trigger: try every registered body synthesizer. AT
        // MOST ONE may claim the declaration — two matches throw a loud
        // cajeta::Exception naming both (no silent precedence; spec §2.3 /
        // §1.5 [S2]). Zero matches returns nullopt (the failsafe; spec §2.4).
        std::optional<std::string> dispatchBody(const SynthesisContext& ctx) const;

        std::size_t bodyCount() const { return bodySynths.size(); }

    private:
        std::vector<std::pair<std::string, BodySynthesizer>> bodySynths;
        std::vector<std::pair<std::string, MemberSynthesizer>> memberSynths;
    };

    // Register the compiler's built-in body synthesizers (the codecs) into the
    // process-wide instance(). Idempotent — safe to call at every dispatch site.
    void registerBuiltinSynthesizers();

}
