# local-shadowing-field-member-lookup — defect

Found 2026-08-14 implementing `cajeta-llama` Unit 13 (the chat-template
interpreter, `TplEval.callMethod`).

## 1. Defect

When a local variable shadows a field of the same name **and a different
type**, member lookup on that name is validated against the **field's**
type. If the field's type lacks the member, the compiler rejects a valid
program:

```
TplEval.cajeta:1203:21: CAJETA_ERROR_MEMBER_NOT_FOUND:
    no member 'add' on 'dev.cajeta.llama.chat.TplBuf'
```

`TplBuf` is the type of the *field* `out`; the line in question operates
on a local `JsonArray out` declared three lines earlier. The diagnostic
names a type the programmer never wrote at that line.

Minimal repro (rejected; should compile):

```cajeta
final class Sink {
    int32 hits;
    Sink() { this.hits = 0; }
    void addStr(String s) { this.hits = this.hits + 1; }
}

public final class R {
    Sink out;                                   // field
    public R() { this.out = heap Sink(); return; }

    int32 probe() {
        ArrayList<String> out = heap ArrayList<String>();   // local
        out.add("x");        // <-- MEMBER_NOT_FOUND: no member 'add' on Sink
        return (int32) out.count();
    }

    public static int32 run() { R r = heap R(); return r.probe(); }
}
```

## 2. Characterization

Five repros isolate it. **Code generation is correct throughout — the
local always wins.** Only the diagnostic pass is wrong.

| local vs field type | use site | result |
|---|---|---|
| same type | same block | correct: local written, field untouched |
| same type | nested block | correct: local written, field untouched |
| different type, field LACKS the member | same block | **rejected** (this defect) |
| different type, field LACKS the member | nested block | **rejected** (this defect) |
| different type, field HAS the member | same block | correct: dispatches to the local |

Two consequences follow from the last row: there is **no wrong-object
dispatch and no silent corruption** — when both types carry the member,
the local is used. The bug is confined to a false-positive rejection.

Block nesting is NOT a factor; an earlier hypothesis that shadowing
failed to propagate into nested blocks is disproved by rows 2 and 5.

## 3. Expected behaviour

Java's rule (JLS §6.4.1), which cajeta's syntax invites: a local
variable declaration shadows a same-named field throughout the scope of
the declaration, and the field remains reachable as `this.name`. Member
lookup must therefore consult the LOCAL's type only.

- **3.1** When a local shadows a field of a different type, member access
  on the bare name resolves against the local's type.
- **3.2** When the local's type genuinely lacks the member, the error
  names the LOCAL's type — never the shadowed field's.
- **3.3** `this.name` continues to reach the field, with the field's type.
- **3.4** Existing behaviour for same-typed shadowing is unchanged.

## 4. Related, but separate: a shadowing warning

Worth having, worth NOT coupling to this fix, and worth not making an
error. Shadowing is legal and idiomatic — `this.x = x` in constructors
is the canonical case — so `javac` ships no such warning by default and
leaves it to Checkstyle (`HiddenField`) and IDE inspections. If cajeta
adds one it should be a flag (`-Wshadow` style), off or non-fatal by
default, or it fires on ordinary code. File separately; this spec is the
false positive only.

## 5. Workaround until fixed

Rename the local. Applied at `TplEval.callMethod` (`out` -> `pairsOut`).
