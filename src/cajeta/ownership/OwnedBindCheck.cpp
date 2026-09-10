#include "OwnedBindCheck.h"

#include "MigrationSwitch.h"
#include "cajeta/error/Exception.h"

namespace cajeta::ownership {

    namespace {
        MigrationSwitch g_ownedBind("CAJETA_OWNED_BIND");
    }

    bool ownedBindWarns() { return g_ownedBind.warns(); }
    void setOwnedBindWarns(bool on) { g_ownedBind.setWarns(on); }
    void clearOwnedBindWarnsOverride() { g_ownedBind.clearOverride(); }

    void rejectPlainOwnedBind(const std::string& calleeKey,
                              const std::string& lvalue,
                              const std::string& file, int line,
                              const std::string& inMethod,
                              bool classpathOrigin) {
        std::string message =
            "`" + calleeKey + "` returns an OWNED result (`#`), and `" + lvalue
            + "` receives it with a plain `=`: the title moves here, but "
              "nothing at this line says so. A reader cannot tell this apart "
              "from a borrow-returning call without opening the callee, which "
              "is the ambiguity the `#` return exists to remove. Fix: spell "
              "the binding `" + lvalue + " #= " + "…`, which records the "
              "acquisition where it happens.";

        if (!g_ownedBind.warns() && !classpathOrigin) {
            throw Exception(message,
                            "CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER");
        }

        // `file=` goes in the NOTE as well as the diagnostic: the note drives the
        // migration, and a class plus a line is ambiguous inside a generic body.
        g_ownedBind.report(
            "[owned-bind] " + inMethod + ":" + std::to_string(line)
                + " lvalue=" + lvalue + " callee=" + calleeKey
                + " file=" + (file.empty() ? std::string("?") : file),
            "CAJETA_WARN_OWNED_RESULT_NEEDS_TRANSFER", message, file, line);
    }

}  // namespace cajeta::ownership
