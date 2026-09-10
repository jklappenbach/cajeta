#pragma once
// ONE answer to "what title does this value carry?" for every consumer position.

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

    enum class TitleAnswer : uint8_t {
        Borrow,      ///< a view of a value someone else owns and frees
        Owned,       ///< a fresh value, or a title moved out of its owner
        StackBound,  ///< a value that dies with the frame (`stack` / arena)
        Runtime,     ///< decided by a protocol at run time; `source` names it
        Scalar,      ///< no title at all: primitives, comparisons, void
    };

    enum class TitleSource : uint8_t {
        None,
        DropEntry,     ///< the named local's drop-entry active byte
        TransferWord,  ///< the enclosing function's transfer-word bit for a formal
        ReturnFlag,    ///< `__cajeta_return_flag_get()` right after the call
        ArmPhi,        ///< a conditional's / switch-expression's arm phi
        Slot,          ///< a field's own-bit or an element take's flag
    };

    enum class TitleFamily : uint8_t {
        Literal, LocalRead, ThisRead, FieldRead, ElementRead,
        Fresh, Concat, Move, CallResult, ClosureCall, Conditional, Closure,
        Scalar, Unsupported,
        Count
    };

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

    struct TitleVerdict {
        TitleAnswer answer;
        TitleSource source;
        const char* error;   ///< a CAJETA_ERROR_* code, or nullptr
        const char* label;
    };

    /// The static shape of `e`; total over ExprKind, never throws.
    TitleShape classify(const ExpressionPtr& e, const CajetaModulePtr& module);

    TitleShape moveFrom(const TitleShape& inner, bool sharpStore);

    TitleVerdict policy(const TitleShape& shape, ConsumerRole role);

    /// Deactivate a local's drop entry after a move — a runtime-conditional local
    /// only if the entry still describes its object, so nothing is orphaned.
    void deactivateLocalEntry(const CajetaModulePtr& module, const FieldPtr& field);
    void deactivateLocalEntry(const CajetaModulePtr& module, Field* field);

    /// The i64 title flag for `shape`. For a ReturnFlag source, call it
    /// IMMEDIATELY after the value's codegen: the next call clobbers the TLS.
    llvm::Value* titleFlag(const TitleShape& shape, const CajetaModulePtr& module);

    /// The title a STORE of `e` in `role` carries; null for none, and throws on a policy error.
    llvm::Value* storeTitleFlag(const ExpressionPtr& e, ConsumerRole role,
                                const CajetaModulePtr& module, const char* where);
    llvm::Value* storeTitleFlagOf(const TitleShape& shape, ConsumerRole role,
                                  const ExpressionPtr& e, const CajetaModulePtr& module,
                                  const char* where);

    llvm::Value* verdictFlag(const TitleShape& shape, const TitleVerdict& v,
                             const CajetaModulePtr& module);

    /// `verdictFlag` after codegen: an unresolved call was lowered by an intrinsic and stored no flag.
    llvm::Value* verdictFlagAfterCodegen(const TitleShape& shape, const TitleVerdict& v,
                                         const CajetaModulePtr& module);

    /// One argument classified AFTER its codegen; `flag` is read before anything deactivates the source.
    struct ArgTitle {
        TitleShape shape;
        llvm::Value* flag = nullptr;
    };
    ArgTitle classifyArgument(const ExpressionPtr& e, bool callerTransferred,
                              const CajetaModulePtr& module, const char* where);

    /// The `#T`-formal contract: every leaf arm must tender a title.
    void rejectOwnedFormalArgument(const ExpressionPtr& e, bool callerTransferred,
                                   const CajetaModulePtr& module,
                                   const std::string& callee, const std::string& formal,
                                   int line);

    void rejectEscape(const ExpressionPtr& e, ConsumerRole role,
                      const CajetaModulePtr& module, const char* where);

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

    void observeTitle(const ExpressionPtr& e, const CajetaModulePtr& module,
                      ConsumerRole role);

}  // namespace cajeta::ownership
