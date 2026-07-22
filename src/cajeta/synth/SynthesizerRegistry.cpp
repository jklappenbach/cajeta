//
// Source-synthesis facility (núcleo Layer-1a). See SynthesizerRegistry.h.
//
#include "cajeta/synth/SynthesizerRegistry.h"

#include <map>
#include <mutex>
#include <set>

#include "cajeta/error/Exception.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaMatrix.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/type/StructureProperty.h"
#include "cajeta/method/Method.h"
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

    void SynthesizerRegistry::registerMember(std::string label, MemberSynthesizer fn) {
        memberSynths.emplace_back(std::move(label), std::move(fn));
    }

    std::vector<std::pair<std::string, MemberSynthesisResult>>
    SynthesizerRegistry::collectMembers(const SynthesisContext& ctx) const {
        std::vector<std::pair<std::string, MemberSynthesisResult>> claimed;
        for (const auto& [label, fn] : memberSynths) {
            // A synthesizer that validates-first and rejects throws here; the
            // exception propagates to the caller as a user-attributed error.
            std::optional<MemberSynthesisResult> r = fn(ctx);
            if (r) claimed.emplace_back(label, std::move(*r));
        }
        return claimed;
    }

    void SynthesizerRegistry::registerCompanion(std::string label,
                                                CompanionSynthesizer fn) {
        companionSynths.emplace_back(std::move(label), std::move(fn));
    }

    std::vector<std::pair<std::string,
                          SynthesizerRegistry::CompanionSynthesisResult>>
    SynthesizerRegistry::collectCompanions(const SynthesisContext& ctx) const {
        std::vector<std::pair<std::string, CompanionSynthesisResult>> claimed;
        for (const auto& [label, fn] : companionSynths) {
            std::optional<CompanionSynthesisResult> r = fn(ctx);
            if (r) claimed.emplace_back(label, std::move(*r));
        }
        return claimed;
    }

    namespace {
        // Wrap a `bool synthesize*MethodSource(parent, name, args, params, out&)`
        // codec entry point as a BodySynthesizer. Codecs are method-template
        // instantiation synthesizers returning FULL method source — they decline
        // declaration-time contexts (ctx.method set), whose contract is a body
        // block (see BodySynthesizer in the header).
        template <typename Fn>
        BodySynthesizer wrapCodec(Fn fn) {
            return [fn](const SynthesisContext& c) -> std::optional<std::string> {
                if (c.method) return std::nullopt;
                std::string out;
                if (fn(c.parent, c.methodName, c.typeArgs, c.paramTypes, out)) {
                    return out;
                }
                return std::nullopt;
            };
        }

        // --- @Einsum (Unit 6) -------------------------------------------------

        // If `t` is a concrete Tensor<E> instantiation, return E; else null.
        CajetaTypePtr tensorElementOf(const CajetaTypePtr& t) {
            auto cls = std::dynamic_pointer_cast<CajetaClass>(t);
            if (!cls || !cls->isInstantiation()) return nullptr;
            auto origin = cls->getTemplateOrigin();
            const std::string name = (origin && origin->getQName())
                ? origin->getQName()->getTypeName()
                : (cls->getQName() ? cls->getQName()->getTypeName() : std::string());
            if (name != "Tensor" && name.rfind("Tensor<", 0) != 0) return nullptr;
            const auto& args = cls->getTypeArguments();
            return args.size() == 1 ? args[0] : nullptr;
        }

        // Validate-first (spec §4.2, §6.1): check the contraction spec against
        // the resolved signature and throw user-phrased diagnostics BEFORE any
        // body text exists. Errors name the parameter and the declared spec.
        struct EinsumPlan {
            std::vector<std::string> groups;   // one label group per parameter
            std::string outLabels;             // output label group
            std::string elemName;              // element type as spelled in source
        };
        EinsumPlan validateEinsum(const SynthesisContext& c, const std::string& spec) {
            auto fail = [&](const std::string& what) {
                throw Exception(
                    "@Einsum spec '" + spec + "' on method '" + c.methodName
                        + "': " + what,
                    "CAJETA_ERROR_SYNTH_EINSUM");
            };
            EinsumPlan plan;
            auto arrow = spec.find("->");
            if (arrow == std::string::npos || spec.find("->", arrow + 2) != std::string::npos) {
                fail("expected exactly one '->' separating inputs from output");
            }
            std::string inputs = spec.substr(0, arrow);
            plan.outLabels = spec.substr(arrow + 2);
            std::size_t pos = 0;
            while (true) {
                auto comma = inputs.find(',', pos);
                plan.groups.push_back(inputs.substr(pos,
                    comma == std::string::npos ? std::string::npos : comma - pos));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            std::set<char> inputLabels;
            for (auto& g : plan.groups) {
                if (g.empty()) fail("empty input label group");
                std::set<char> withinGroup;
                for (char l : g) {
                    if (l < 'a' || l > 'z') fail(std::string("label '") + l + "' is not a-z");
                    if (!withinGroup.insert(l).second) {
                        fail(std::string("repeated label '") + l
                            + "' within one group (diagonals are not supported in v1)");
                    }
                    inputLabels.insert(l);
                }
            }
            if (plan.outLabels.empty()) fail("scalar (empty) output is not supported in v1");
            std::set<char> outSeen;
            for (char l : plan.outLabels) {
                if (!outSeen.insert(l).second) fail(std::string("repeated output label '") + l + "'");
                if (!inputLabels.count(l)) {
                    fail(std::string("output label '") + l + "' appears in no input group");
                }
            }
            auto params = c.method->getParameterList();
            std::vector<FormalParameterPtr> formals;
            for (auto& p : params) {
                if (p && p->getName() != "this") formals.push_back(p);
            }
            if (formals.size() != plan.groups.size()) {
                fail("has " + std::to_string(plan.groups.size())
                    + " input group(s) but the method declares "
                    + std::to_string(formals.size()) + " parameter(s)");
            }
            // Per-parameter rank + type checks. Tensor<E> rank is RUNTIME
            // (tensor-spec.md — ndim/shape are fields), so it validates at the
            // dynamic boundary; the STATIC rank check fires where the declared
            // type carries one (Matrix<E,R,C> is const-generic rank-2).
            CajetaTypePtr elem;
            for (std::size_t g = 0; g < formals.size(); ++g) {
                const std::string& pname = formals[g]->getName();
                auto ptype = formals[g]->getType();
                if (std::dynamic_pointer_cast<CajetaMatrix>(ptype)) {
                    if (plan.groups[g].size() != 2) {
                        fail("expects rank-" + std::to_string(plan.groups[g].size())
                            + " input for parameter '" + pname + "'; '" + pname
                            + "' is rank-2 (" + ptype->toCanonical() + ")");
                    }
                    fail("parameter '" + pname + "' is a Matrix — @Einsum v1 "
                        "synthesizes over Tensor<E> parameters only");
                }
                auto e = tensorElementOf(ptype);
                if (!e) {
                    fail("parameter '" + pname + "' is not a Tensor<E>");
                }
                if (elem && e.get() != elem.get()) {
                    fail("parameter '" + pname + "' element type "
                        + e->toCanonical() + " disagrees with " + elem->toCanonical());
                }
                if (!elem) elem = e;
            }
            auto retElem = tensorElementOf(c.method->getReturnType());
            if (!retElem) fail("return type must be Tensor<E>");
            if (elem && retElem.get() != elem.get()) {
                fail("return element type " + retElem->toCanonical()
                    + " disagrees with the parameters' " + elem->toCanonical());
            }
            plan.elemName = retElem->getQName()
                ? retElem->getQName()->getTypeName() : std::string("float32");
            return plan;
        }

        // Emit the contraction as a source loop nest over Tensor's checked
        // accessors (Tier A — spec §7: source over primitives, never raw IR):
        // outer loops over the output labels, an accumulator loop over the
        // contracted labels, getAt/setAt element access, `return #out` transfer
        // (the Tensor.matmul / LinAlg idiom). Synth locals carry a `__e_`
        // prefix so they can't shadow user parameter names.
        std::string emitEinsumBody(const SynthesisContext& c, const EinsumPlan& plan) {
            auto params = c.method->getParameterList();
            std::vector<std::string> names;
            for (auto& p : params) {
                if (p && p->getName() != "this") names.push_back(p->getName());
            }
            const std::string& E = plan.elemName;
            // First occurrence of each label -> (param, axis); order of first
            // appearance drives the contracted-loop order (deterministic).
            std::vector<char> order;
            std::map<char, std::pair<std::size_t, std::size_t>> firstAt;
            for (std::size_t g = 0; g < plan.groups.size(); ++g) {
                for (std::size_t a = 0; a < plan.groups[g].size(); ++a) {
                    char l = plan.groups[g][a];
                    if (!firstAt.count(l)) {
                        firstAt[l] = {g, a};
                        order.push_back(l);
                    }
                }
            }
            std::vector<char> contracted;
            for (char l : order) {
                if (plan.outLabels.find(l) == std::string::npos) contracted.push_back(l);
            }
            std::string b = "{\n";
            auto dim = [](char l) { return std::string("__e_d_") + l; };
            auto idx = [](char l) { return std::string("__e_l_") + l; };
            for (char l : order) {
                b += "    int64 " + dim(l) + " = " + names[firstAt[l].first]
                    + ".shapeAt(" + std::to_string(firstAt[l].second) + ");\n";
            }
            b += "    int64[] __e_shp = heap int64[" + std::to_string(plan.outLabels.size()) + "];\n";
            for (std::size_t i = 0; i < plan.outLabels.size(); ++i) {
                b += "    __e_shp[" + std::to_string(i) + "] = " + dim(plan.outLabels[i]) + ";\n";
            }
            b += "    Tensor<" + E + "> __e_out = Tensor.zeros<" + E + ">(__e_shp);\n";
            for (std::size_t g = 0; g < plan.groups.size(); ++g) {
                b += "    int64[] __e_ix" + std::to_string(g) + " = heap int64["
                    + std::to_string(plan.groups[g].size()) + "];\n";
            }
            b += "    int64[] __e_io = heap int64[" + std::to_string(plan.outLabels.size()) + "];\n";
            std::string pad = "    ";
            for (char l : plan.outLabels) {
                b += pad + "int64 " + idx(l) + " = 0;\n";
                b += pad + "while (" + idx(l) + " < " + dim(l) + ") {\n";
                pad += "    ";
            }
            b += pad + E + " __e_acc = (" + E + ") 0;\n";
            std::string ipad = pad;
            for (char l : contracted) {
                b += ipad + "int64 " + idx(l) + " = 0;\n";
                b += ipad + "while (" + idx(l) + " < " + dim(l) + ") {\n";
                ipad += "    ";
            }
            for (std::size_t g = 0; g < plan.groups.size(); ++g) {
                for (std::size_t a = 0; a < plan.groups[g].size(); ++a) {
                    b += ipad + "__e_ix" + std::to_string(g) + "[" + std::to_string(a)
                        + "] = " + idx(plan.groups[g][a]) + ";\n";
                }
            }
            b += ipad + "__e_acc = __e_acc";
            for (std::size_t g = 0; g < plan.groups.size(); ++g) {
                b += (g == 0 ? " + " : " * ") + names[g] + ".getAt(__e_ix" + std::to_string(g) + ")";
            }
            b += ";\n";
            for (std::size_t i = contracted.size(); i > 0; --i) {
                char l = contracted[i - 1];
                b += ipad + idx(l) + " = " + idx(l) + " + 1;\n";
                ipad = ipad.substr(4);
                b += ipad + "}\n";
            }
            for (std::size_t i = 0; i < plan.outLabels.size(); ++i) {
                b += pad + "__e_io[" + std::to_string(i) + "] = " + idx(plan.outLabels[i]) + ";\n";
            }
            b += pad + "__e_out.setAt(__e_io, __e_acc);\n";
            for (std::size_t i = plan.outLabels.size(); i > 0; --i) {
                char l = plan.outLabels[i - 1];
                b += pad + idx(l) + " = " + idx(l) + " + 1;\n";
                pad = pad.substr(4);
                b += pad + "}\n";
            }
            b += "    return #__e_out;\n";
            b += "}";
            return b;
        }
    }

    void registerBuiltinSynthesizers() {
        // Registers into the shared, mutex-less instance() vectors and is
        // called from per-compile dispatch sites. A plain `static bool done`
        // let two threads (thread-safe compiler) both run the emplace_backs —
        // a data race plus duplicate synthesizers (-> spurious
        // CAJETA_ERROR_SYNTH_AMBIGUOUS). call_once runs the whole block exactly
        // once across all threads.
        static std::once_flag builtinOnce;
        std::call_once(builtinOnce, [] {
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

        // @Einsum (Unit 6, spec §4.1-4.2): declaration-time body synthesis. A
        // bodyless method annotated @Einsum("ij,jk->ik") gets a fused loop-nest
        // body over Tensor's checked primitives. Claims only declaration-time
        // contexts (ctx.method set); validates the spec against the resolved
        // signature FIRST — every diagnostic is phrased in the user's terms
        // (parameter name, declared spec) and raised before any body text
        // exists (spec §6.1) — then emits the body block the caller splices.
        reg.registerBody("einsum",
                [](const SynthesisContext& c) -> std::optional<std::string> {
            if (!c.method) return std::nullopt;
            auto ann = c.method->findAnnotation("Einsum");
            if (!ann) return std::nullopt;
            std::string spec = ann->getString();
            if (spec.empty()) {
                throw Exception(
                    "@Einsum on method '" + c.methodName
                        + "' needs a contraction spec string, e.g. "
                          "@Einsum(\"ij,jk->ik\")",
                    "CAJETA_ERROR_SYNTH_EINSUM");
            }
            EinsumPlan plan = validateEinsum(c, spec);
            return emitEinsumBody(c, plan);
        });

        // @Logged: inject a `static Logger log = Log.defaultFor("<canonical>")`
        // member. Self-selects on the annotation; respects a user-declared `log`
        // (spec §3.1-3.3). Was the hard-wired synthesizeLoggerField.
        reg.registerMember("logged",
                [](const SynthesisContext& c) -> std::optional<MemberSynthesisResult> {
            auto structure = c.parent;
            if (!structure || structure->isTemplate()) return std::nullopt;
            if (!structure->findAnnotation("Logged")) return std::nullopt;
            for (auto& prop : structure->getPropertyList()) {
                if (prop && prop->getName() == "log") return std::nullopt;  // respect-user
            }
            MemberSynthesisResult r;
            r.classBodyFragment = "{ static Logger log = Log.defaultFor(\""
                + structure->getQName()->toCanonical() + "\"); }";
            r.imports = {{"Logger", "dev.cajeta.logging"}, {"Log", "dev.cajeta.logging"}};
            return r;
        });

        // Value-type clone (element-ownership plan 6.2.2, spec §6.1.3-5):
        // synthesize a TYPED `clone()` on records / @ValueType classes so
        // `Pt b = a.clone()` types as Pt (Object.clone returns Object — a
        // record upcast slice) and copies BY VALUE through the normal
        // aggregate-copy path, whose value-copy hook (emitValueSharedOp)
        // retains shared-capable payload (Utf8/Slice) instead of byte-copying
        // — COW by provenance. `return this;` lowers through the by-value
        // aggregate return (sret/NRVO) coercion. Respect-user: a declared
        // clone() wins. Fires at declaration for concrete value types and at
        // instantiation for templated ones (the member seam runs both).
        reg.registerMember("valueClone",
                [](const SynthesisContext& c) -> std::optional<MemberSynthesisResult> {
            auto structure = c.parent;
            if (!structure || structure->isTemplate()) return std::nullopt;
            bool valueLike = structure->isRecordType()
                || structure->findAnnotation("ValueType") != nullptr;
            if (!valueLike) return std::nullopt;
            for (auto& kv : structure->getMethods()) {
                if (kv.second && kv.second->getName() == "clone") {
                    return std::nullopt;  // respect-user
                }
            }
            const std::string typeName = structure->getQName()->getTypeName();
            MemberSynthesisResult r;
            r.classBodyFragment =
                "{ public " + typeName + " clone() { return this; } }";
            return r;
        });

        // Table<T>: a member, instantiation-time synthesizer (spec §3.4, the
        // 2x2's member/instantiation-time cell). On a concrete `Table<Tick>`,
        // reflect the record type argument's fields and inject one typed column
        // accessor per field — the direct downstream consumer of the
        // T.class-in-template fix (records 7.1.2). Self-selects on the
        // instantiation of a template named `Table` with a single record arg;
        // the production binding is `dev.cajeta.nucleo.frame.Table`, but the
        // Unit-5 shell is a test-local stand-in (a reference class holding one
        // `T sample` row), so the gate is name + record-arg, package-agnostic.
        // Each accessor projects its field off `sample`; a non-record arg (no
        // fields to reflect) or a wrong arity declines. Determinism/memoization
        // rides the instantiation cache — this fires once per monomorphization.
        reg.registerMember("table",
                [](const SynthesisContext& c) -> std::optional<MemberSynthesisResult> {
            auto structure = c.parent;
            if (!structure || !structure->isInstantiation()) return std::nullopt;
            auto origin = structure->getTemplateOrigin();
            if (!origin || !origin->getQName()
                    || origin->getQName()->getTypeName() != "Table") {
                return std::nullopt;
            }
            const auto& args = structure->getTypeArguments();
            if (args.size() != 1) return std::nullopt;
            auto record = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!record || !record->isRecordType()) return std::nullopt;
            std::string frag = "{ ";
            for (auto& prop : record->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                auto ft = prop->getType();
                if (!ft || !ft->getQName()) continue;
                const std::string typeName = ft->getQName()->getTypeName();
                const std::string fieldName = prop->getName();
                frag += "public " + typeName + " " + fieldName
                    + "() { return this.sample." + fieldName + "; } ";
            }
            frag += "}";
            MemberSynthesisResult r;
            r.classBodyFragment = std::move(frag);
            return r;
        });

        // nucleo-frame U1 — the `<Record>Cols` companion: emitted per
        // `Table<Record>` instantiation, it is the typed column-expression
        // builder a relational op's lambda receives
        // (`ticks.filter((TickCols c) -> c.price() > 0.0)`). SPIKE SHAPE:
        // builder methods return per-field ordinals — the real expression
        // node family replaces the bodies in U1's 1.2.2. Same gate as the
        // `table` member synthesizer: an instantiation of a template named
        // `Table` with one record argument.
        reg.registerCompanion("tableCols",
                [](const SynthesisContext& c)
                    -> std::optional<SynthesizerRegistry::CompanionSynthesisResult> {
            auto structure = c.parent;
            if (!structure || !structure->isInstantiation()) return std::nullopt;
            auto origin = structure->getTemplateOrigin();
            if (!origin || !origin->getQName()
                    || origin->getQName()->getTypeName() != "Table") {
                return std::nullopt;
            }
            const auto& args = structure->getTypeArguments();
            if (args.size() != 1) return std::nullopt;
            auto record = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!record || !record->isRecordType()) return std::nullopt;
            SynthesizerRegistry::CompanionSynthesisResult r;
            r.className = record->getQName()->getTypeName() + "Cols";
            r.packageName = record->getQName()->getPackageName();
            // Typed builders: each field becomes a method returning the
            // field-typed column-reference NODE (the U1 1.2.2 node family).
            // float64/float32 + integers -> ColF64 v1 (a dedicated ColI64
            // lands with the executor units); String -> ColStr; any other
            // field type synthesizes NO builder yet (the member-accessor
            // synthesizer still covers direct access).
            std::string src = "import cajeta.nucleo.frame.ColF64;\n"
                "import cajeta.nucleo.frame.ColStr;\n"
                "import cajeta.nucleo.frame.Pred;\n"
                "public class " + r.className + " {\n"
                "    public " + r.className + "() { }\n";
            int32_t ord = 0;
            for (auto& prop : record->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                auto ft = prop->getType();
                std::string tn = (ft && ft->getQName())
                    ? ft->getQName()->getTypeName() : std::string();
                const std::string fieldName = prop->getName();
                if (tn == "float64" || tn == "float32" || tn == "int32"
                        || tn == "int64" || tn == "uint32" || tn == "uint64") {
                    src += "    public #ColF64 " + fieldName
                        + "() { return ColF64.colRef(" + std::to_string(ord)
                        + ", \"" + fieldName + "\"); }\n";
                } else if (tn == "String") {
                    src += "    public #ColStr " + fieldName
                        + "() { return ColStr.colRef(" + std::to_string(ord)
                        + ", \"" + fieldName + "\"); }\n";
                }
                ord = ord + 1;
            }
            src += "}\n";
            r.classSource = std::move(src);
            return r;
        });
        });  // std::call_once
    }

}
