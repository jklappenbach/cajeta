// Synthesized methods for @Encoding(EncoderClass) — Phase B.
//
// Two methods are added to the @Encoding-annotated class T:
//
//   - SynthesizedEncodingCtor: `public T(byte[] bytes)`. Calls the
//     encoder's static `decode(byte[])` to get a fresh T, then
//     memcpys the decoded body into `this`. Memcpy preserves the
//     vtable (slot 0) since both source and destination are T
//     instances. The decoded temp's shell leaks in v1 (~24 B per
//     decode — tracked for future fix); its owned fields are
//     aliased into `this` and the auto-field-drop live-set
//     discrimination keeps double-free safe.
//
//   - SynthesizedEncodingToBytes: `public #byte[] toBytes()`. Calls
//     the encoder's static `encode(T)` and returns the result.
//     The `#` on the return matches the new return-ownership rule
//     (return heap X requires # — the encoder's encode returns a
//     fresh byte[] which we pass straight through).
//
// Both methods reference the encoder class's static methods by
// LLVM Function pointer. CajetaClass::synthesizeEncoding looks up
// the encoder, verifies the required methods exist with the
// correct signatures, and creates these synthesizers.

#pragma once

#include "Method.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedEncodingCtor : public Method {
    public:
        SynthesizedEncodingCtor(CajetaModulePtr module,
                                 CajetaClassPtr parent,
                                 CajetaClassPtr encoder);

        void initParameter();
        void generateCode() override;

    private:
        CajetaClassPtr encoder;
    };

    class SynthesizedEncodingToBytes : public Method {
    public:
        SynthesizedEncodingToBytes(CajetaModulePtr module,
                                    CajetaClassPtr parent,
                                    CajetaClassPtr encoder);

        void generateCode() override;

    private:
        CajetaClassPtr encoder;
    };
}
