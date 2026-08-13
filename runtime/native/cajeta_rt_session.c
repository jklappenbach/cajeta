// Session-binding registry (script-units spec §4).
//
// Top-level bindings of a script unit are owned by the SESSION, not by the
// synthesized entry's drop frame: codegen registers each owner here instead
// of pushing a function-local drop entry, so the value survives the entry's
// return. The host decides the session's end: `cajeta run` after the entry
// returns; the Jupyter kernel at shutdown/reset. Rebinding a name drops the
// previous occupant immediately and keeps the name's ORIGINAL position, so
// `__cajeta_session_drop_all` fires in reverse FIRST-binding order — the
// deterministic mirror of scope-exit drops (spec §4.3/§4.4).
//
// Single-threaded by contract: bindings are created and dropped on the
// session's execution thread (the same thread that owns the compiler
// front-end reuse machinery). No locking here.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* name;                 // owned copy
    void* obj;                  // the bound value (null after drop_all)
    void (*drop_fn)(void*);     // same signature drop entries use
} cajeta_session_slot;

static cajeta_session_slot* __cajeta_session_slots = 0;
static int64_t __cajeta_session_len = 0;
static int64_t __cajeta_session_cap = 0;

static cajeta_session_slot* cajeta_session_find(const char* name) {
    for (int64_t i = 0; i < __cajeta_session_len; ++i) {
        if (strcmp(__cajeta_session_slots[i].name, name) == 0) {
            return &__cajeta_session_slots[i];
        }
    }
    return 0;
}

void __cajeta_session_bind(const char* name, void* obj,
                           void (*drop_fn)(void*)) {
    cajeta_session_slot* slot = cajeta_session_find(name);
    if (slot) {
        // Rebind: drop the old occupant NOW (spec §4.3); the name keeps its
        // original position for reverse-order drop_all.
        if (slot->obj && slot->drop_fn) slot->drop_fn(slot->obj);
        slot->obj = obj;
        slot->drop_fn = drop_fn;
        return;
    }
    if (__cajeta_session_len == __cajeta_session_cap) {
        int64_t cap = __cajeta_session_cap ? __cajeta_session_cap * 2 : 8;
        cajeta_session_slot* next = (cajeta_session_slot*) realloc(
            __cajeta_session_slots, (size_t) cap * sizeof(cajeta_session_slot));
        if (!next) return;  // OOM: leak rather than corrupt
        __cajeta_session_slots = next;
        __cajeta_session_cap = cap;
    }
    cajeta_session_slot* s = &__cajeta_session_slots[__cajeta_session_len++];
    size_t n = strlen(name) + 1;
    s->name = (char*) malloc(n);
    if (s->name) memcpy(s->name, name, n);
    s->obj = obj;
    s->drop_fn = drop_fn;
}

// Drop for a BOXED primitive (see __cajeta_session_bind_value): the box is a
// plain malloc'd buffer, so releasing it is just free.
static void cajeta_session_free_box(void* p) {
    free(p);
}

// Bind a PRIMITIVE by value. A primitive top-level binding has no drop entry
// — nothing owns it, so the owner-promotion path never sees it — and its
// storage is a slot in the unit entry's frame, which is gone by the time a
// later unit reads the name. Copy the bytes into a session-owned box instead,
// and register that with the ordinary bind path so rebinding, reverse-order
// drop_all, and first-binding position all behave exactly as they do for an
// owner. `__cajeta_session_get` then returns the box, which IS the address of
// the value — the reader loads through it directly.
void __cajeta_session_bind_value(const char* name, const void* src,
                                 int64_t size) {
    if (!src || size <= 0) return;
    void* box = malloc((size_t) size);
    if (!box) return;  // OOM: leave the previous binding intact
    memcpy(box, src, (size_t) size);
    __cajeta_session_bind(name, box, cajeta_session_free_box);
}

// Ownership left the session (a `#` transfer moved the binding's title to a
// new owner): quiet the slot WITHOUT dropping — the new owner drops. The
// name keeps its position; a later rebind reoccupies the same slot.
void __cajeta_session_disarm(const char* name) {
    cajeta_session_slot* slot = cajeta_session_find(name);
    if (!slot) return;
    slot->obj = 0;
    slot->drop_fn = 0;
}

void* __cajeta_session_get(const char* name) {
    cajeta_session_slot* slot = cajeta_session_find(name);
    return slot ? slot->obj : 0;
}

int64_t __cajeta_session_count(void) {
    return __cajeta_session_len;
}

// Drop every live binding in reverse first-binding order, then reset the
// registry to empty (names freed). Safe to call repeatedly.
void __cajeta_session_drop_all(void) {
    for (int64_t i = __cajeta_session_len - 1; i >= 0; --i) {
        cajeta_session_slot* s = &__cajeta_session_slots[i];
        if (s->obj && s->drop_fn) s->drop_fn(s->obj);
        s->obj = 0;
        s->drop_fn = 0;
        free(s->name);
        s->name = 0;
    }
    __cajeta_session_len = 0;
}
