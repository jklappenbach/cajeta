// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- nucleo-column: Arrow C Data Interface shims (nucleo-column-spec §4) ----
// Interop is by matching the frozen Arrow C ABI — no libarrow anywhere
// (spec §1.3). Unit 1 ships the buffer-address primitive (64-byte alignment
// computation + probes); the matched ArrowSchema/ArrowArray structs, export,
// and import land in the plan's U3/U4.

// Data address of a cajeta array's element buffer. Instance @Native shim on
// Storage<T>: `self` is the leading `this` the forwarder passes, ignored on
// the C side (the KernelBuffer convention); arrays arrive as their data
// pointer.
int64_t __cajeta_arrow_addr(void* self, void* data) {
    (void) self;
    return (int64_t)(intptr_t) data;
}
