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

        void initParameter();
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

        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        CajetaClassPtr encoder;
    };
}
