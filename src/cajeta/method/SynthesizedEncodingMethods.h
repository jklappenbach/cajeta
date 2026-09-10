// The two methods @Encoding(EncoderClass) adds to a class T, each calling the
// encoder's statics by LLVM Function pointer. CajetaClass::synthesizeEncoding
// verifies those statics exist with the right signatures and builds these.

#pragma once

#include "Method.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    // `public T(byte[] bytes)`: decode(byte[]) into a fresh T, then memcpy its
    // body into `this` — which preserves the vtable, both being T instances.
    class SynthesizedEncodingCtor : public Method {
    public:
        SynthesizedEncodingCtor(CajetaModulePtr module,
                                 CajetaClassPtr parent,
                                 CajetaClassPtr encoder);

        // Creates the single `byte[] bytes` parameter. Idempotent, and must run
        // before generateCode, which reads argument 1 as that parameter.
        void initParameter();
        // Emits `memcpy(this, encoder.decode(bytes))`, then raw-frees the decoded
        // shell. Throws CAJETA_ERROR_ENCODING_DECODE_MISSING when decode is absent.
        void generateCode() override;

    private:
        CajetaClassPtr encoder;
    };

    // `public #byte[] toBytes()`: the encoder's fresh byte[], passed straight through.
    class SynthesizedEncodingToBytes : public Method {
    public:
        SynthesizedEncodingToBytes(CajetaModulePtr module,
                                    CajetaClassPtr parent,
                                    CajetaClassPtr encoder);

        // Returns the encoder's `encode(this)` result — an owned byte[] — verbatim.
        // Throws CAJETA_ERROR_ENCODING_ENCODE_MISSING when encode is absent.
        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        CajetaClassPtr encoder;
    };
}
