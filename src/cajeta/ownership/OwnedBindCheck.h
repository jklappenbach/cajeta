#pragma once

// stdlib-ownership-convention 8.2.7 (spec §4.6) —
// `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`.
//
// A `#T` result must be received with `#=`. Binding it with plain `=` is an
// error naming the transfer.
//
// This is the CALLER half of §2.8's return-side redesign, and it is the half
// that carries the volume. 8.2.6 put the callee under contract — every return
// in a `#`-declared method must establish a title — and measured ZERO existing
// violations, because a callee-side rule is checked where the promise is made
// and almost nothing violates it. This rule lands on every USE, and uses
// concentrate: 148 sites measured across three libraries, 38 of them
// `String.toBytes` alone.
//
// What it buys is legibility, which §2.8 treats as the point rather than a
// bonus: the reader sees where title moves without opening the callee. `int8[]
// w = s.toBytes()` and `int8[] w = s.trimView()` are the same line to a reader
// and opposite facts about who frees `w`; `#=` on the first is what tells them
// apart.
//
// It lands as a WARNING under spec §5.5 (`CAJETA_OWNED_BIND=warn`) because 148
// is not a number an error can ship against — that is 3.3.3's lesson, paid for
// once already by Unit 3's gate.

#include <string>

namespace cajeta::ownership {

    // `calleeKey` names the `#`-returning method, `lvalue` the local being
    // declared, `inMethod` the receiving method (for the enumeration record).
    // Throws in the default mode; reports and returns in warn mode.
    void rejectPlainOwnedBind(const std::string& calleeKey,
                              const std::string& lvalue,
                              const std::string& file, int line,
                              const std::string& inMethod);

    // Test control over the §5.5 switch, mirroring Scope's for
    // CAPTURED_BORROW. Off (error) by default.
    bool ownedBindWarns();
    void setOwnedBindWarns(bool on);
    void clearOwnedBindWarnsOverride();

}  // namespace cajeta::ownership
