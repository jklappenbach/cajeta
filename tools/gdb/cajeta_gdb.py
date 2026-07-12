# Cajeta gdb bridge — debug an AOT cajeta binary in Cajeta's terms.
#
# Cajeta emits NO DWARF (specs/external-debug-spec.md §1.3). gdb's native verbs
# therefore see nothing: `break File:42` cannot resolve, `info locals` answers
# "No symbol table info available". This script reads Cajeta's own encoding
# instead, through retained C accessors in the runtime:
#
#   the location table   loc_id <-> (file, line, col, function)
#   the shadow stack     the live Cajeta frames, with files and lines
#   the debug frames     each frame's locals: name, type, address, ownership
#   RTTI                 an object's DYNAMIC type, its fields, their byte offsets
#
# Commands: cjbreak, cjstack, cjlocals, cjstep, cjnext, cjlist.
#
# Requires a binary built with --debug-info=full (the `debug` flavor's default).
# A `line` build carries the shadow stack only: cjstack works, the rest do not.
#
# Load:  gdb -x cajeta_gdb.py ./your-binary
# (Installed alongside the toolchain; gdb auto-loads it for a cajeta binary.)

import gdb
import re

# --- typeFlags: the TYPE_ID word, verbatim from src/cajeta/type/CajetaType.h ---
# Keep these in step with that header. The width lives in the same word, so the
# flags alone fully determine how to read a primitive out of memory.
PRIMITIVE_FLAG = 1 << 0
NUMBER_FLAG    = 1 << 1
INT_FLAG       = 1 << 2
FLOAT_FLAG     = 1 << 3
SIGNED_FLAG    = 1 << 4
BIT_8_FLAG     = 1 << 11
BIT_16_FLAG    = 1 << 12
BIT_32_FLAG    = 1 << 13
BIT_64_FLAG    = 1 << 14
BIT_128_FLAG   = 1 << 15

MAX_DEPTH = 3          # object graph render depth (spec §4.1.5)
MAX_FIELDS = 64        # per object, so a pathological class can't wedge gdb


class NoDebugInfo(gdb.GdbError):
    """The binary carries no debug records (spec §5.1.3)."""
    def __init__(self, what):
        super().__init__(
            "%s needs a binary built with --debug-info=full; this one carries "
            "no debug records. Rebuild with `cajeta build --flavor=debug` (or "
            "pass --debug-info=full) and try again." % what)


def _call(sig, name, *args):
    """Call a retained runtime accessor. Casting is mandatory: with no DWARF gdb
    has no prototype for any of them. The leading `*` matters — casting a symbol
    to a function POINTER yields a pointer value, which gdb's Python cannot call;
    dereferencing it yields the function itself (TYPE_CODE_FUNC), which it can."""
    try:
        fn = gdb.parse_and_eval("*(%s)%s" % (sig, name))
    except gdb.error:
        raise NoDebugInfo(name)
    return fn(*args)


def _running():
    try:
        return gdb.selected_inferior().pid != 0
    except gdb.error:
        return False


def _first_int_arg_register():
    """Where __cajeta_dbg_safepoint's loc_id argument sits at function entry.
    With no DWARF gdb cannot name the parameter, so the breakpoint condition has
    to read the ABI's first integer-argument register directly."""
    arch = gdb.selected_frame().architecture().name()
    if "aarch64" in arch or "arm64" in arch:
        return "$w0"
    if "i386:x86-64" in arch or "x86-64" in arch:
        return "$edi"
    raise gdb.GdbError(
        "cjbreak: unsupported architecture %r — it reads the ABI's first "
        "integer-argument register, and only x86-64 and aarch64 are mapped."
        % arch)


def _require_running(what):
    if not _running():
        raise gdb.GdbError("%s needs a running program — `run` first." % what)


def _cstr(val):
    if val is None or int(val) == 0:
        return ""
    return val.string()


# --- the location table -------------------------------------------------

def loc_count():
    return int(_call("int(*)(void)", "__cajeta_dbg_loc_count"))


def loc_file(i):
    return _cstr(_call("char*(*)(int)", "__cajeta_dbg_loc_file", i))


def loc_line(i):
    return int(_call("int(*)(int)", "__cajeta_dbg_loc_line", i))


def loc_func(i):
    return _cstr(_call("char*(*)(int)", "__cajeta_dbg_loc_func", i))


def ids_for_line(path, line):
    """The loc_ids to arm for `file:line`. The out-array must live in the
    INFERIOR — a gdb-side value's address means nothing to the callee."""
    n = int(_call("int(*)(char*,int,int*,int)", "__cajeta_dbg_ids_for_line",
                  path, line, 0, 0))
    if n <= 0:
        return []
    buf = _call("void*(*)(unsigned long)", "malloc", 4 * n)
    if int(buf) == 0:
        raise gdb.GdbError("could not allocate in the inferior")
    _call("int(*)(char*,int,int*,int)", "__cajeta_dbg_ids_for_line",
          path, line, buf, n)
    ptr = buf.cast(gdb.lookup_type("int").pointer())
    ids = [int(ptr[i]) for i in range(n)]
    _call("void(*)(void*)", "free", buf)
    return ids


# --- the shadow stack (live Cajeta frames) ------------------------------

def stack_depth():
    return int(_call("int(*)(void)", "__cajeta_stack_depth"))


def stack_frame(i):
    return (_cstr(_call("char*(*)(int)", "__cajeta_stack_type", i)),
            _cstr(_call("char*(*)(int)", "__cajeta_stack_method", i)),
            _cstr(_call("char*(*)(int)", "__cajeta_stack_file", i)),
            int(_call("int(*)(int)", "__cajeta_stack_line", i)))


# --- the debug frame chain (locals) -------------------------------------

def frame_top():
    return _call("void*(*)(void)", "__cajeta_dbg_frame_top")


def frame_nlocals(f):
    return int(_call("int(*)(void*)", "__cajeta_dbg_frame_nlocals", f))


def local_at(f, i):
    return {
        "name":  _cstr(_call("char*(*)(void*,int)", "__cajeta_dbg_local_name", f, i)),
        "type":  _cstr(_call("char*(*)(void*,int)", "__cajeta_dbg_local_type", f, i)),
        "addr":  _call("void*(*)(void*,int)", "__cajeta_dbg_local_addr", f, i),
        "alloc": int(_call("unsigned char(*)(void*,int)",
                           "__cajeta_dbg_local_alloc", f, i)),
        "own":   int(_call("unsigned char(*)(void*,int)",
                           "__cajeta_dbg_local_ownership", f, i)),
        # 1 = active owner (scheduled to drop), 0 = deactivated (moved out at
        # runtime), -1 = no drop entry (a borrow or a plain value).
        "drop":  int(_call("signed char(*)(void*,int)",
                           "__cajeta_dbg_local_drop_active", f, i)),
    }


# LifetimeState — mirrors deriveLifetime() in src/cajeta/dbg/MemoryFacets.cpp.
# Keep the two in step: the DAP and this bridge must not disagree about whether a
# value is live.
LIVE, MOVED_OUT, ABOUT_TO_DROP = "live", "moved-out", "about-to-drop"


def derive_lifetime(own, drop_active):
    # The static `#`-transferred-out role dominates...
    if own == OWNERSHIP_TRANSFERRED_OUT:
        return MOVED_OUT
    # ...but an owner's ROLE stays Owner even after `#` moves the value out. The
    # runtime drop entry is the only thing that knows: the transfer DEACTIVATED
    # it. Without this an owner that has been moved from renders as a live value.
    if own == OWNERSHIP_OWNER and drop_active >= 0:
        return ABOUT_TO_DROP if drop_active == 1 else MOVED_OUT
    return LIVE


# MemoryFacets — keep in step with src/cajeta/dbg/MemoryFacets.h. 0 = Unknown is
# a first-class state (the not-statically-determinable case), never silently
# rendered as Stack/Owner.
#   AllocClass:    Unknown=0, Stack=1, Heap=2, Shared=3
#   OwnershipRole: Unknown=0, Owner=1, Borrow=2, TransferredOut=3
ALLOC = {0: "?", 1: "stack", 2: "heap", 3: "shared"}
OWNERSHIP = {0: "?", 1: "owner", 2: "borrow", 3: "moved"}
OWNERSHIP_OWNER = 1
OWNERSHIP_TRANSFERRED_OUT = 3    # `#` moved it out; the binding is consumed


# --- RTTI ---------------------------------------------------------------

def rtti_of(obj):
    return _call("void*(*)(void*)", "__cajeta_rtti_of", obj)


def rtti_for_name(name):
    return _call("void*(*)(char*)", "__cajeta_rtti_for_name", name)


def rtti_name(r):
    return _cstr(_call("char*(*)(void*)", "__cajeta_rtti_type_name", r))


def rtti_fields(r):
    """(name, byteOffset, typeFlags) for this class's OWN fields."""
    n = int(_call("int(*)(void*)", "__cajeta_rtti_field_count", r))
    out = []
    for i in range(min(n, MAX_FIELDS)):
        out.append((
            _cstr(_call("char*(*)(void*,int)", "__cajeta_rtti_field_name", r, i)),
            int(_call("int(*)(void*,int)", "__cajeta_rtti_field_offset", r, i)),
            int(_call("long(*)(void*,int)", "__cajeta_rtti_field_type_flags", r, i)),
        ))
    return out


def rtti_parents(r):
    n = int(_call("int(*)(void*)", "__cajeta_rtti_parent_count", r))
    return [_cstr(_call("char*(*)(void*,int)", "__cajeta_rtti_parent_name", r, i))
            for i in range(n)]


def _ctype_for(flags):
    """The C type to read a primitive with, from its typeFlags. Both the kind
    (int / float) and the WIDTH are in the flag word, so this is exact — no
    guessing from the declared name."""
    if flags & FLOAT_FLAG:
        if flags & BIT_32_FLAG:
            return "float"
        if flags & BIT_64_FLAG:
            return "double"
        return None            # float16 / bf16 / fp128: no faithful C type here
    if flags & INT_FLAG:
        signed = bool(flags & SIGNED_FLAG)
        if flags & BIT_8_FLAG:
            return "signed char" if signed else "unsigned char"
        if flags & BIT_16_FLAG:
            return "short" if signed else "unsigned short"
        if flags & BIT_32_FLAG:
            return "int" if signed else "unsigned int"
        if flags & BIT_64_FLAG:
            return "long" if signed else "unsigned long"
        # boolean carries INT_FLAG with no width bit.
        return "signed char"
    return None


def _read_primitive(addr, flags):
    ctype = _ctype_for(flags)
    if ctype is None:
        return None
    try:
        return gdb.Value(addr).cast(
            gdb.lookup_type(ctype).pointer()).dereference()
    except gdb.error:
        return None


MAX_STRING = 200       # bytes of a String we print before eliding


def _string_contents(obj):
    """A String's text (spec §4.1.6 — contents, not an address). The layout is
    tagged (inline for <= 12 bytes, windowed root beyond), which gdb cannot
    decode without DWARF, so the runtime hands us the length and the bytes."""
    n = int(_call("int(*)(void*)", "__cajeta_string_byte_len", obj))
    if n <= 0:
        return '""'
    p = _call("char*(*)(void*)", "__cajeta_string_bytes", obj)
    if int(p) == 0:
        return '""'
    try:
        raw = p.string(length=min(n, MAX_STRING))
    except (gdb.error, UnicodeDecodeError):
        return "<%d bytes>" % n
    return '"%s"%s' % (raw, "..." if n > MAX_STRING else "")


def _raw_word(addr):
    try:
        w = gdb.Value(addr).cast(
            gdb.lookup_type("unsigned long").pointer()).dereference()
        return "0x%x" % int(w)
    except gdb.error:
        return "?"


def _array_count(obj):
    """A cajeta array is { i64 count, [count x T] } — the count leads."""
    try:
        return int(gdb.Value(int(obj)).cast(
            gdb.lookup_type("long").pointer()).dereference())
    except gdb.error:
        return None


def render_object(obj, depth, seen, indent, declared=""):
    """Render an object from its DYNAMIC type (spec §4.2.2), walking the parent
    chain for inherited fields — a class's RTTI carries only its own. Cycles
    terminate via `seen` (spec §4.2.3); depth is capped."""
    if int(obj) == 0:
        return ["%snull" % indent]
    key = int(obj)
    if key in seen:
        return ["%s<cycle -> 0x%x>" % (indent, key)]
    if depth <= 0:
        return ["%s<...>" % indent]
    seen.add(key)

    # An array has no class RTTI — it is a raw { count, elements } block, so it
    # must be recognized from the DECLARED type, not asked for its type.
    if declared.endswith("[]"):
        n = _array_count(obj)
        return ["%s<%s, %s element(s)>" % (indent, declared,
                                           n if n is not None else "?")]

    r = rtti_of(obj)
    if int(r) == 0:
        return ["%s0x%x  <no RTTI — rebuild with --debug-info=full>"
                % (indent, key)]

    if rtti_name(r) == "cajeta.lang.String":
        return ["%s%s" % (indent, _string_contents(obj))]

    out = []
    for (rtti, owner) in [(r, None)] + [(rtti_for_name(p), p)
                                        for p in rtti_parents(r)]:
        if int(rtti) == 0:
            continue
        fields = rtti_fields(rtti)
        if not fields:
            continue          # don't print an empty "inherited from Object:"
        if owner:
            out.append("%sinherited from %s:" % (indent, owner))
        for (fname, off, flags) in fields:
            if off < 0:
                # byteOffset -1 = a static field: it lives in a global, not in
                # the instance. Say so rather than decode a bogus offset
                # (spec §4.1.8).
                out.append("%s  .%s = <static — not supported>" % (indent, fname))
                continue
            faddr = int(obj) + off
            if flags & PRIMITIVE_FLAG:
                v = _read_primitive(faddr, flags)
                if v is None:
                    # PRIMITIVE_FLAG with no int/float width: an array, Vector or
                    # Matrix — they all ride PRIMITIVE_FLAG for by-value
                    # marshalling (CajetaType.h). Show the raw word rather than
                    # a bare "?", which says nothing.
                    v = "%s  <aggregate>" % _raw_word(faddr)
                out.append("%s  .%s @+%d = %s" % (indent, fname, off, v))
            else:
                inner = gdb.Value(faddr).cast(
                    gdb.lookup_type("void").pointer().pointer()).dereference()
                out.append("%s  .%s @+%d ->" % (indent, fname, off))
                out.extend(render_object(inner, depth - 1, seen,
                                         indent + "    "))
    return out


# --- commands -----------------------------------------------------------

class CjStack(gdb.Command):
    """cjstack — the live Cajeta stack: Type.method(File.cajeta:NN)."""

    def __init__(self):
        super(CjStack, self).__init__("cjstack", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        _require_running("cjstack")
        n = stack_depth()
        if n == 0:
            gdb.write("cjstack: no Cajeta frames on the stack. Either the "
                      "program is not in Cajeta code, or it was built with "
                      "--debug-info=off (which drops the shadow stack).\n")
            return
        for i in range(n):
            (t, m, f, line) = stack_frame(i)
            gdb.write("  #%-2d %s.%s(%s:%d)\n" % (i, t, m, f or "?", line))


class CjBreak(gdb.Command):
    """cjbreak File.cajeta:NN — break at a Cajeta source line.

    Resolves the line to its loc_ids through the embedded table and arms a
    conditional breakpoint on __cajeta_dbg_safepoint. That lands AFTER the
    prologue's frame_enter, so the current frame is already on the stack at the
    stop (spec §5.1.4) — breaking on the function symbol would show the callers
    but not the frame you stopped in.
    """

    def __init__(self):
        super(CjBreak, self).__init__("cjbreak", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, arg, from_tty):
        m = re.match(r"^\s*(\S+):(\d+)\s*$", arg or "")
        if not m:
            raise gdb.GdbError("usage: cjbreak <File.cajeta>:<line>")
        path, line = m.group(1), int(m.group(2))
        _require_running(
            "cjbreak (it reads the embedded location table from the inferior)")

        ids = ids_for_line(path, line)
        if not ids:
            if loc_count() == 0:
                raise NoDebugInfo("cjbreak")
            raise gdb.GdbError(
                "cjbreak: no statement at %s:%d. (The line may be blank, a "
                "comment, or a declaration that emits no code.)" % (path, line))

        # One conditional breakpoint covering every id on the line: two
        # statements on one line get two ids, and both must stop.
        reg = _first_int_arg_register()
        cond = " || ".join("%s == %d" % (reg, i) for i in ids)
        bp = gdb.Breakpoint("__cajeta_dbg_safepoint")
        bp.condition = cond
        bp.silent = True
        gdb.write("cjbreak %s:%d -> breakpoint %d on %d statement(s)\n"
                  % (path, line, bp.number, len(ids)))


class CjLocals(gdb.Command):
    """cjlocals [depth] — the current frame's locals, with ownership.

    Each local shows its name, DECLARED type, and — for a reference — the
    DYNAMIC type of what it actually holds, plus its fields decoded from the
    RTTI's byte offsets, and its allocation kind and ownership. That last part
    is the information DWARF cannot express, and the reason for this encoding.
    """

    def __init__(self):
        super(CjLocals, self).__init__("cjlocals", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        _require_running("cjlocals")
        depth = int(arg) if (arg or "").strip().isdigit() else MAX_DEPTH

        f = frame_top()
        if int(f) == 0:
            if loc_count() == 0:
                raise NoDebugInfo("cjlocals")
            gdb.write("cjlocals: no Cajeta debug frame here — the program is "
                      "not inside a Cajeta method.\n")
            return

        n = frame_nlocals(f)
        if n == 0:
            gdb.write("cjlocals: this frame declares no locals.\n")
            return

        for i in range(n):
            lv = local_at(f, i)
            life = derive_lifetime(lv["own"], lv["drop"])
            facets = "%s, %s, %s" % (ALLOC.get(lv["alloc"], "?"),
                                     OWNERSHIP.get(lv["own"], "?"), life)
            # A moved-from local has no live value to show: ownership was
            # transferred out and the binding is consumed (spec §4.2.4). Printing
            # the stale pointer as though it were live is exactly the bug a
            # borrow-checked language must not ship in its own debugger.
            if life == MOVED_OUT:
                gdb.write("  %s : %s = <moved-from — ownership transferred out>"
                          "  [%s]\n" % (lv["name"], lv["type"], facets))
                continue

            if _is_primitive_type(lv["type"]):
                gdb.write("  %s : %s = %s  [%s]\n"
                          % (lv["name"], lv["type"],
                             _read_declared(lv["addr"], lv["type"]), facets))
                continue

            obj = gdb.Value(lv["addr"]).cast(
                gdb.lookup_type("void").pointer().pointer()).dereference()
            dyn = ""
            if int(obj) != 0 and not lv["type"].endswith("[]"):
                r = rtti_of(obj)
                if int(r) != 0:
                    name = rtti_name(r)
                    if name and name != lv["type"]:
                        dyn = "  (dynamic: %s)" % name
            gdb.write("  %s : %s%s  [%s]\n"
                      % (lv["name"], lv["type"], dyn, facets))
            for row in render_object(obj, depth, set(), "      ",
                                     declared=lv["type"]):
                gdb.write("%s\n" % row)


PRIMITIVES = {
    "int8": "signed char", "int16": "short", "int32": "int", "int64": "long",
    "uint8": "unsigned char", "uint16": "unsigned short",
    "uint32": "unsigned int", "uint64": "unsigned long",
    "float32": "float", "float64": "double", "boolean": "signed char",
}


def _is_primitive_type(t):
    return t in PRIMITIVES


def _read_declared(addr, t):
    try:
        ctype = gdb.lookup_type(PRIMITIVES[t])
        return gdb.Value(addr).cast(ctype.pointer()).dereference()
    except (gdb.error, KeyError):
        return "?"


class CjStep(gdb.Command):
    """cjstep — run to the next Cajeta statement (not the next instruction)."""

    def __init__(self, name="cjstep"):
        super(CjStep, self).__init__(name, gdb.COMMAND_RUNNING)

    def invoke(self, arg, from_tty):
        _require_running("cjstep")
        # Every statement boundary calls __cajeta_dbg_safepoint, so "the next
        # statement" is just "the next safepoint". A temporary, silent breakpoint
        # gets us there in one hop. NOT internal=True — an internal breakpoint
        # does not stop the inferior, so the program just runs to exit.
        bp = gdb.Breakpoint("__cajeta_dbg_safepoint", temporary=True)
        bp.silent = True
        gdb.execute("continue", to_string=True)
        if not _running():
            gdb.write("cjstep: the program exited — no further statements.\n")
            return
        _where()


class CjNext(CjStep):
    """cjnext — the next statement in THIS frame (steps over calls)."""

    def __init__(self):
        super(CjNext, self).__init__("cjnext")

    def invoke(self, arg, from_tty):
        _require_running("cjnext")
        here = frame_top()
        while True:
            bp = gdb.Breakpoint("__cajeta_dbg_safepoint", temporary=True)
            bp.silent = True
            gdb.execute("continue", to_string=True)
            if not _running():
                return
            if int(frame_top()) == int(here):
                break
        _where()


def _where():
    if not _running():
        return
    n = stack_depth()
    if n:
        (t, m, f, line) = stack_frame(0)
        gdb.write("%s.%s(%s:%d)\n" % (t, m, f or "?", line))


class CjList(gdb.Command):
    """cjlist [n] — source around the current line, from the table's path."""

    def __init__(self):
        super(CjList, self).__init__("cjlist", gdb.COMMAND_FILES)

    def invoke(self, arg, from_tty):
        _require_running("cjlist")
        n = int(arg) if (arg or "").strip().isdigit() else 5
        if stack_depth() == 0:
            gdb.write("cjlist: not in a Cajeta frame.\n")
            return
        (_, _, path, line) = stack_frame(0)
        if not path:
            gdb.write("cjlist: the current frame names no file.\n")
            return
        try:
            with open(path) as fh:
                lines = fh.readlines()
        except IOError:
            gdb.write("cjlist: cannot open %s. The table stores a path relative "
                      "to the build root — run gdb from there, or open the file "
                      "yourself.\n" % path)
            return
        lo, hi = max(1, line - n), min(len(lines), line + n)
        for i in range(lo, hi + 1):
            gdb.write("%s%4d  %s" % ("->" if i == line else "  ", i,
                                     lines[i - 1]))


CjStack()
CjBreak()
CjLocals()
CjStep()
CjNext()
CjList()

gdb.write("cajeta: cjstack, cjbreak, cjlocals, cjstep, cjnext, cjlist "
          "(requires --debug-info=full)\n")
