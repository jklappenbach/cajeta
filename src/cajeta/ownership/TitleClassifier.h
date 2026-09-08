#pragma once
//
// ownership-title-classifier — ONE answer to "what title does this value
// carry?" for every consumer position (specs/ownership-title-classifier-spec.md).
//
// Six consumer sites used to answer that question with their own if/else
// chain over AST node types, and disagreed on a dozen shapes (spec §3). This
// header is the single classifier they migrate onto, unit by unit:
//
//   classify(expr, module)  — the STATIC shape: provenance family, answer,
//                             flag source, diagnostic label. Usable before any
//                             IR exists. TOTAL over ExprKind (spec §2.1): the
//                             switch in TitleClassifier.cpp has no default and
//                             builds with -Werror=switch, so a new Expression
//                             subclass fails the compiler's build until named.
//   policy(shape, role)     — what the answer means for a consumer ROLE: the
//                             answer to act on, or the diagnostic code the role
//                             rejects this shape with (spec §2.3).
//   titleFlag(expr, ...)    — the RUNTIME flag for a Runtime answer, as an i64
//                             read once and cached on the node; constants for
//                             static answers so a consumer's branch folds
//                             (spec §2.2).
//
// Optimality (spec §1.4): the kind dispatch is one switch on a one-byte tag
// (Expression::kind()), the tables are constexpr, nothing here allocates or
// takes a std::function, and the runtime reads are emitted as loads and bit
// operations, not runtime calls, wherever the read is a few instructions.
// classify()'s body is out of line on purpose: a 200-line switch inlined at
// each consumer would be code growth, not speed — the call is once per node.
//

#include <cstdint>
#include <string>
#include <vector>

#include "cajeta/asn/expression/Expression.h"

namespace llvm {
    class Value;
}

namespace cajeta {
    class CajetaModule;
    class Field;
    class Method;
}

namespace cajeta::ownership {

    /// The static answer (spec §2.1).
    enum class TitleAnswer : uint8_t {
        Borrow,      ///< a view of a value someone else owns and frees
        Owned,       ///< a fresh value, or a title moved out of its owner
        StackBound,  ///< a value that dies with the frame (`stack` / arena)
        Runtime,     ///< decided by a protocol at run time; `source` names it
        Scalar,      ///< no title at all: primitives, comparisons, void
    };

    /// Where a Runtime answer's flag comes from (spec §2.2).
    enum class TitleSource : uint8_t {
        None,
        DropEntry,     ///< the named local's drop-entry active byte
        TransferWord,  ///< the enclosing function's transfer-word bit for a formal
        ReturnFlag,    ///< `__cajeta_return_flag_get()` right after the call
        ArmPhi,        ///< a conditional's / switch-expression's arm phi
        Slot,          ///< a field's own-bit or an element take's flag
    };

    /// Provenance family — what the expression IS, before the answer.
    enum class TitleFamily : uint8_t {
        Literal, LocalRead, ThisRead, FieldRead, ElementRead,
        Fresh, Concat, Move, CallResult, ClosureCall, Conditional, Closure,
        Scalar, Unsupported,
        Count
    };

    /// The consumer positions (spec §2.3).
    enum class ConsumerRole : uint8_t {
        Bind,         ///< `T x = e`
        StoreString,  ///< `#=` / `=` into a String field or slot
        StoreSlot,    ///< `#=` / `=` into a class field, tail slot, element
        Reassign,     ///< `=` into a binding that has a drop entry
        ReturnOwned,  ///< `return e` under a `#T` declaration
        ReturnPlain,  ///< `return e` under a plain `T` declaration
        ArgPlain,     ///< argument to a plain formal
        ArgOwned,     ///< argument to a `#T` formal
        Arm,          ///< an arm of a conditional / switch expression
        Count
    };

    struct TitleShape {
        TitleFamily family = TitleFamily::Unsupported;
        TitleAnswer answer = TitleAnswer::Scalar;
        TitleSource source = TitleSource::None;
        uint32_t flags = 0;
        int8_t paramIndex = -1;      ///< index among non-`this` formals (TransferWord)
        const char* label = "";       ///< for diagnostics: "a field read", …
        ExpressionPtr leaf;          ///< the expression the answer is about (casts peeled)
        Field* field = nullptr;      ///< the named local / formal (LocalRead)
        Method* callee = nullptr;    ///< the resolved callee (CallResult), when known

        static constexpr uint32_t kArena = 1 << 0;
        static constexpr uint32_t kStack = 1 << 1;
        static constexpr uint32_t kShared = 1 << 2;
        static constexpr uint32_t kHasEntry = 1 << 3;
        static constexpr uint32_t kIsParam = 1 << 4;
        static constexpr uint32_t kTransferredParam = 1 << 5;
        static constexpr uint32_t kBorrowOrigin = 1 << 6;
        static constexpr uint32_t kString = 1 << 7;
        static constexpr uint32_t kValue = 1 << 8;
        static constexpr uint32_t kInterface = 1 << 9;
        static constexpr uint32_t kArray = 1 << 10;
        static constexpr uint32_t kView = 1 << 11;
        static constexpr uint32_t kSharpStore = 1 << 12;
        static constexpr uint32_t kOwnedDecl = 1 << 13;   ///< the callee is declared `#R`
        static constexpr uint32_t kStaticTitle = 1 << 14; ///< the scope says the local holds a static title
        static constexpr uint32_t kRuntimeOwner = 1 << 15; ///< the entry's active byte was armed at run time (a callee's flag, a formal's word bit)
        static constexpr uint32_t kFunction = 1 << 16;    ///< a function-typed value (a closure: its own drop protocol)

        bool has(uint32_t f) const { return (flags & f) != 0; }
    };

    /// What a consumer role does with a shape: the answer to act on, or the
    /// diagnostic code that rejects it (`error` non-null).
    struct TitleVerdict {
        TitleAnswer answer;
        TitleSource source;
        const char* error;   ///< a CAJETA_ERROR_* code, or nullptr
        const char* label;
    };

    /// The static shape of `e` in the current codegen context of `module`
    /// (scope for names, the current method for formals and arena locals).
    /// Total over ExprKind; never throws; never allocates beyond the returned
    /// record.
    TitleShape classify(const ExpressionPtr& e, const CajetaModulePtr& module);

    /// The shape of a `#` move / `#=` store whose SOURCE has shape `inner`
    /// (spec §2.1, Move): the inner's provenance decides the flag source —
    /// a local's entry, a formal's word bit, a slot's own-bit, a call's
    /// return flag, a conditional's arm phi — or a static answer. The leaf,
    /// field and flags are the inner's, so titleFlag() reads the right slot.
    TitleShape moveFrom(const TitleShape& inner, bool sharpStore);

    /// The policy table (spec §2.3), a pure function of (shape, role).
    TitleVerdict policy(const TitleShape& shape, ConsumerRole role);

    /// The i64 title flag for `shape` — a constant for static answers, the
    /// named runtime read for Runtime ones — emitted at the builder's current
    /// point and cached on the leaf node for the rest of that function's
    /// codegen. Call it IMMEDIATELY after the value's codegen when the source
    /// is ReturnFlag: the next call clobbers the TLS. Returns nullptr only for
    /// TitleSource::Slot on a node that has not generated yet (the move site
    /// keeps its own take protocol until it migrates).
    llvm::Value* titleFlag(const TitleShape& shape, const CajetaModulePtr& module);

    /// The title a STORE of `e` in `role` carries into its slot, or null when
    /// it carries none (a borrow, a scalar, a stack value): the constant 1 for
    /// an Owned answer, the runtime flag for a Runtime one. One call per store
    /// site replaces the per-node-type chains (spec §2.3; Unit 5). A policy
    /// error (spec 5.11: a `stack` value moved into a retaining slot) is
    /// thrown here as the CAJETA_ERROR the verdict names, with `where` in the
    /// message.
    llvm::Value* storeTitleFlag(const ExpressionPtr& e, ConsumerRole role,
                                const CajetaModulePtr& module, const char* where);
    /// The same, from a shape the caller already computed (one classify).
    llvm::Value* storeTitleFlagOf(const TitleShape& shape, ConsumerRole role,
                                  const ExpressionPtr& e, const CajetaModulePtr& module,
                                  const char* where);

    /// Throw the CAJETA_ERROR a policy verdict names for `e` in `role`
    /// (spec 5.10 ARRAY_SLOT_BORROWS_LOCAL, 5.11 STACK_TRANSFER, …); no-op
    /// when the verdict carries no error. The escaping positions that have
    /// not migrated yet (a `#` return, a `#T` argument) call this directly.
    /// The flag a consumer stores for a policy VERDICT: the constant 1 for
    /// an Owned answer (a row may promote), the runtime read for a Runtime
    /// answer (the verdict's source over the shape's own — a LocalRead's
    /// entry, a call's TLS), the constant 0 for a Borrow or StackBound,
    /// nullptr for a Scalar or an error.
    llvm::Value* verdictFlag(const TitleShape& shape, const TitleVerdict& v,
                             const CajetaModulePtr& module);

    void rejectEscape(const ExpressionPtr& e, ConsumerRole role,
                      const CajetaModulePtr& module, const char* where);

    /// Constexpr label per family — one table, every diagnostic reads it.
    constexpr const char* labelOfFamily(TitleFamily f) noexcept {
        switch (f) {
            case TitleFamily::Literal:     return "a literal";
            case TitleFamily::LocalRead:   return "a bare local or formal (no `#`)";
            case TitleFamily::ThisRead:    return "`this`";
            case TitleFamily::FieldRead:   return "a field read";
            case TitleFamily::ElementRead: return "an element read";
            case TitleFamily::Fresh:       return "a fresh value";
            case TitleFamily::Concat:      return "a String concatenation";
            case TitleFamily::Move:        return "a `#` move";
            case TitleFamily::CallResult:  return "a call result";
            case TitleFamily::ClosureCall: return "a closure call result";
            case TitleFamily::Conditional: return "a conditional";
            case TitleFamily::Closure:     return "a closure";
            case TitleFamily::Scalar:      return "a scalar";
            case TitleFamily::Unsupported: return "an unsupported construct";
            case TitleFamily::Count:       return "";
        }
        return "";
    }

    const char* toString(TitleAnswer a) noexcept;
    const char* toString(TitleSource s) noexcept;
    const char* toString(TitleFamily f) noexcept;
    const char* toString(ConsumerRole r) noexcept;
    const char* toString(ExprKind k) noexcept;

    /// One classification, as the audit records it (tests read these; the
    /// switch costs one static-bool test when off, as ReturnTitleAudit does).
    struct TitleShapeRecord {
        std::string file;
        std::string holder;   ///< `pkg.Class.method` the site is in
        int line = 0;
        ExprKind kind = ExprKind::Unsupported;
        TitleFamily family = TitleFamily::Unsupported;
        TitleAnswer answer = TitleAnswer::Scalar;
        TitleSource source = TitleSource::None;
        ConsumerRole role = ConsumerRole::Bind;
        uint32_t flags = 0;
    };

    class TitleShapeAudit {
    public:
        static bool enabled();
        static void setEnabled(bool on);
        static void record(TitleShapeRecord rec);
        static const std::vector<TitleShapeRecord>& records();
        static void clear();
    };

    /// Audit-only observation of a classification at a consumer site: no
    /// behaviour, one branch on the audit switch. Sites call this until they
    /// migrate onto policy(); tests measure every §2.1 row through it.
    void observeTitle(const ExpressionPtr& e, const CajetaModulePtr& module,
                      ConsumerRole role);

}  // namespace cajeta::ownership
