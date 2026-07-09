//
// Source-synthesis facility (núcleo Layer-1a). See SynthesizerRegistry.h.
//
#include "cajeta/synth/SynthesizerRegistry.h"

#include "cajeta/error/Exception.h"
#include "cajeta/codec/JsonSynthesizer.h"
#include "cajeta/codec/CsvSynthesizer.h"
#include "cajeta/codec/ProtobufSynthesizer.h"
#include "cajeta/codec/IonSynthesizer.h"
#include "cajeta/codec/AvroSynthesizer.h"

namespace cajeta::synth {

    SynthesizerRegistry& SynthesizerRegistry::instance() {
        static SynthesizerRegistry inst;
        return inst;
    }

    void SynthesizerRegistry::registerBody(std::string label, BodySynthesizer fn) {
        bodySynths.emplace_back(std::move(label), std::move(fn));
    }

    std::optional<std::string> SynthesizerRegistry::dispatchBody(
            const SynthesisContext& ctx) const {
        std::optional<std::string> matchedBody;
        std::string matchedLabel;
        for (const auto& [label, fn] : bodySynths) {
            std::optional<std::string> r = fn(ctx);
            if (!r) continue;
            if (matchedBody) {
                throw Exception(
                    "two body synthesizers both claim method '" + ctx.methodName
                        + "': '" + matchedLabel + "' and '" + label
                        + "' — a method has exactly one body; no silent precedence",
                    "CAJETA_ERROR_SYNTH_AMBIGUOUS");
            }
            matchedBody = std::move(r);
            matchedLabel = label;
        }
        return matchedBody;
    }

    namespace {
        // Wrap a `bool synthesize*MethodSource(parent, name, args, params, out&)`
        // codec entry point as a BodySynthesizer.
        template <typename Fn>
        BodySynthesizer wrapCodec(Fn fn) {
            return [fn](const SynthesisContext& c) -> std::optional<std::string> {
                std::string out;
                if (fn(c.parent, c.methodName, c.typeArgs, c.paramTypes, out)) {
                    return out;
                }
                return std::nullopt;
            };
        }
    }

    void registerBuiltinSynthesizers() {
        static bool done = false;
        if (done) return;
        done = true;
        auto& reg = SynthesizerRegistry::instance();
        // Order mirrors the former MethodTemplateInstantiator if-else chain. The
        // codecs are mutually exclusive by declaring class, so at most one ever
        // matches — the registry's at-most-one guarantee is a safety net, not a
        // behaviour change.
        reg.registerBody("json",     wrapCodec(synthesizeJsonMethodSource));
        reg.registerBody("csv",      wrapCodec(synthesizeCsvMethodSource));
        reg.registerBody("protobuf", wrapCodec(synthesizeProtobufMethodSource));
        reg.registerBody("ion",      wrapCodec(synthesizeIonMethodSource));
        reg.registerBody("avro",     wrapCodec(synthesizeAvroMethodSource));
    }

}
