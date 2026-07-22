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

        // nucleo-frame U3 — the PRODUCTION Table<T> member synthesizer
        // (frame spec §2, §3.1; plan 3.2.2 — the U5 test-shell retarget).
        // Keyed on `cajeta.nucleo.frame.Table` instantiations only. Per
        // schema-record field it injects, in layout order (inherited fields
        // first — the prefix `Table<? extends T>` relies on):
        //   - a public typed accessor FIELD with the mapped physical column
        //     type (`ticks.price` — a typo fails the compile);
        //   - a constructor taking the columns in schema order (`#` owned —
        //     zero-copy adoption) with a loud row-length check;
        //   - the introspection methods colCount/colNameAt/colTypeAt/
        //     colNullableAt over compile-time schema facts.
        // Physical mapping (plan 3.2.3): primitives -> Column<that>;
        // Instant -> Column<int64> (epoch-nanos); Utf8 -> StringColumn
        // (utf8); @Nullable on a mappable primitive -> NullableColumn.
        // A non-record schema arg is CAJETA_ERROR_FRAME_SCHEMA; a field
        // with no mapping is CAJETA_ERROR_FRAME_UNMAPPED_FIELD.
        // `Table<? extends R>` handles synthesize the accessor surface FROM
        // THE BOUND `R` (fields + introspection, no constructor): every
        // `Table<R' extends R>` lays out `R`'s columns as its field prefix
        // (inherited-first enumeration below), so the covariant reads are
        // sound. Unbounded `Table<?>` declines — no schema, no accessors.
        // Determinism/memoization rides the instantiation cache — this
        // fires once per monomorphization.
        reg.registerMember("table",
                [](const SynthesisContext& c) -> std::optional<MemberSynthesisResult> {
            auto structure = c.parent;
            if (!structure || !structure->isInstantiation()) return std::nullopt;
            auto origin = structure->getTemplateOrigin();
            if (!origin || !origin->getQName()
                    || origin->getQName()->toCanonical()
                        != "cajeta.nucleo.frame.Table") {
                return std::nullopt;
            }
            const auto& args = structure->getTypeArguments();
            if (args.size() != 1 || !args[0]) return std::nullopt;
            auto arg = args[0];
            bool boundedHandle = false;
            if (arg->isWildcard()) {
                if (arg->wildcardKind() != CajetaType::WildcardKind::Extends) {
                    return std::nullopt;   // Table<?> / super-bound: no schema
                }
                arg = arg->wildcardBound();
                if (!arg) return std::nullopt;
                boundedHandle = true;
            }
            auto record = std::dynamic_pointer_cast<CajetaClass>(arg);
            if ((arg->getTypeFlags() & PRIMITIVE_FLAG) != 0
                    || (record && !record->isRecordType())) {
                throw Exception(
                    "Table<T>'s schema argument must be a record (the "
                    "type-level column descriptor); '" + arg->toCanonical()
                        + "' is not a record",
                    "CAJETA_ERROR_FRAME_SCHEMA");
            }
            if (!record) return std::nullopt;  // type variable / opaque handle

            // Schema fields in LAYOUT order: inherited (supers-first,
            // recursively) then own — `Table<? extends T>` reads the shared
            // column-field prefix, so accessor order must mirror it.
            std::vector<StructurePropertyPtr> fields;
            std::function<void(CajetaClassPtr)> collect =
                [&](CajetaClassPtr k) {
                    if (!k) return;
                    for (auto& s : k->getSuperClasses()) collect(s);
                    for (auto& p : k->getPropertyList()) {
                        if (p && !p->isStatic()) fields.push_back(p);
                    }
                };
            collect(record);

            const std::string schema = record->getQName()->toCanonical();
            struct Col {
                std::string name;      // record field / accessor name
                std::string colType;   // Column<float64> / StringColumn / ...
                std::string phys;      // physical name for colTypeAt
                bool nullable;
            };
            std::vector<Col> cols;
            for (auto& prop : fields) {
                const std::string fieldName = prop->getName();
                if (fieldName == "rows") {
                    throw Exception(
                        "Table<" + schema + ">: schema field 'rows' collides "
                        "with Table's row-count member — rename the field",
                        "CAJETA_ERROR_FRAME_SCHEMA");
                }
                auto ft = prop->getType();
                const std::string tn = (ft && ft->getQName())
                    ? ft->getQName()->getTypeName() : std::string();
                const bool nullable = prop->findAnnotation("Nullable") != nullptr;
                static const std::set<std::string> mappedPrims = {
                    "int8", "int16", "int32", "int64",
                    "uint8", "uint16", "uint32", "uint64",
                    "float32", "float64"};
                std::string phys;
                if (mappedPrims.count(tn)) {
                    phys = tn;
                } else if (tn == "Instant") {
                    phys = "int64";     // epoch-nanos
                } else if (tn == "Utf8") {
                    phys = "utf8";
                } else {
                    throw Exception(
                        "Table<" + schema + "> field '" + fieldName
                            + "': type '"
                            + (ft ? ft->toCanonical() : std::string("<unresolved>"))
                            + "' has no column mapping (mappable: numeric "
                              "primitives, Instant, Utf8)",
                        "CAJETA_ERROR_FRAME_UNMAPPED_FIELD");
                }
                if (phys == "utf8") {
                    if (nullable) {
                        throw Exception(
                            "Table<" + schema + "> field '" + fieldName
                                + "': @Nullable Utf8 columns are not supported "
                                  "yet (no nullable utf8 physical)",
                            "CAJETA_ERROR_FRAME_UNMAPPED_FIELD");
                    }
                    cols.push_back({fieldName, "StringColumn", phys, false});
                } else if (nullable) {
                    cols.push_back({fieldName,
                        "NullableColumn<" + phys + ">", phys, true});
                } else {
                    cols.push_back({fieldName,
                        "Column<" + phys + ">", phys, false});
                }
            }

            // Physical mapping helpers: DynCol tag + factory/unwrap suffix.
            auto tagOf = [](const std::string& phys) -> int {
                if (phys == "int8") return 1;
                if (phys == "int16") return 2;
                if (phys == "int32") return 3;
                if (phys == "int64") return 4;
                if (phys == "uint8") return 5;
                if (phys == "uint16") return 6;
                if (phys == "uint32") return 7;
                if (phys == "uint64") return 8;
                if (phys == "float32") return 9;
                if (phys == "float64") return 10;
                return 11;  // utf8
            };
            auto sfxOf = [](const std::string& phys) -> std::string {
                if (phys == "int8") return "I8";
                if (phys == "int16") return "I16";
                if (phys == "int32") return "I32";
                if (phys == "int64") return "I64";
                if (phys == "uint8") return "U8";
                if (phys == "uint16") return "U16";
                if (phys == "uint32") return "U32";
                if (phys == "uint64") return "U64";
                if (phys == "float32") return "F32";
                if (phys == "float64") return "F64";
                return "Str";  // utf8
            };
            auto dynFactory = [&](const Col& col) -> std::string {
                if (col.phys == "utf8") return "DynCol.ofStr";
                return std::string("DynCol.of") + (col.nullable ? "N" : "")
                    + sfxOf(col.phys);
            };
            auto dynUnwrap = [&](const Col& col) -> std::string {
                if (col.phys == "utf8") return "asStr";
                return std::string("as") + (col.nullable ? "N" : "")
                    + sfxOf(col.phys);
            };

            std::string frag = "{\n";
            for (auto& col : cols) {
                frag += "    public " + col.colType + " " + col.name + ";\n";
            }
            // The synthesized constructor IS the fromColumns schema check:
            // arity/type mismatches fail overload resolution at the call
            // site; the row-length check fails loud; `#=` adopts zero-copy.
            // It also fills the schema-agnostic dyn store (aliases of the
            // same storage — the executor's world) and installs the typed
            // rebinder closure. Bounded-wildcard handles get no constructor
            // — a `? extends` handle is read-only over the bound's column
            // prefix, never constructed directly.
            if (!boundedHandle) {
                frag += "    public Table(";
                for (std::size_t i = 0; i < cols.size(); ++i) {
                    if (i) frag += ", ";
                    frag += "#" + cols[i].colType + " " + cols[i].name;
                }
                frag += ") {\n";
                frag += "        int64 __t_rows = " + cols[0].name + ".size();\n";
                for (std::size_t i = 1; i < cols.size(); ++i) {
                    frag += "        if (" + cols[i].name + ".size() != __t_rows) {\n"
                        "            throw heap FrameException(\"Table<" + schema
                        + ">: column '" + cols[i].name
                        + "' row length does not match '" + cols[0].name + "'\");\n"
                        "        }\n";
                }
                for (auto& col : cols) {
                    frag += "        this." + col.name + " #= " + col.name + ";\n";
                }
                frag += "        this.rows = __t_rows;\n";
                const std::string w = std::to_string(cols.size());
                frag += "        String[] __nm = heap String[" + w + "];\n"
                    "        int32[] __tg = heap int32[" + w + "];\n"
                    "        boolean[] __nl = heap boolean[" + w + "];\n"
                    "        DynCol[] __dc = heap DynCol[" + w + "];\n";
                for (std::size_t i = 0; i < cols.size(); ++i) {
                    const std::string ix = std::to_string(i);
                    frag += "        __nm[" + ix + "] = \"" + cols[i].name + "\";\n"
                        "        __tg[" + ix + "] = "
                            + std::to_string(tagOf(cols[i].phys)) + ";\n"
                        "        __nl[" + ix + "] = "
                            + (cols[i].nullable ? "true" : "false") + ";\n"
                        "        __dc[" + ix + "] #= " + dynFactory(cols[i])
                            + "(this." + cols[i].name + ".alias());\n";
                }
                frag += "        this.dyn #= heap DynFrame(#__nm, #__tg, "
                        "#__nl, #__dc, " + w + ", __t_rows);\n"
                    "        this.__attachRebinder();\n";
                frag += "    }\n";
            }
            frag += "    public int32 colCount() { return "
                + std::to_string(cols.size()) + "; }\n";
            auto emitAt = [&](const std::string& mname, const std::string& retType,
                             auto valueOf) {
                frag += "    public " + retType + " " + mname + "(int32 i) {\n";
                for (std::size_t i = 0; i < cols.size(); ++i) {
                    frag += "        if (i == " + std::to_string(i)
                        + ") { return " + valueOf(cols[i]) + "; }\n";
                }
                frag += "        throw heap FrameException(\"Table<" + schema
                    + ">." + mname + ": column ordinal out of range\");\n"
                    "    }\n";
            };
            emitAt("colNameAt", "String",
                [](const Col& col) { return "\"" + col.name + "\""; });
            emitAt("colTypeAt", "String",
                [](const Col& col) { return "\"" + col.phys + "\""; });
            emitAt("colNullableAt", "boolean",
                [](const Col& col) {
                    return std::string(col.nullable ? "true" : "false");
                });

            // U4/U5 — the typed lazy surface. The lazy machinery itself
            // (collect/lazy/head/filter(#Pred)/col/as<R>/describe) is
            // TEMPLATE code over the dyn store; synthesized here are only
            // the schema-typed seams: the zero-copy typed snapshot, the
            // rebinder closure, the `.as<R>()` schema check, the typed row
            // surface, and the lambda-builder relational ops.
            const std::string recName = record->getQName()->getTypeName();
            const std::string tblType = "Table<" + recName + ">";
            auto joinCols = [&](const std::string& recv,
                                const std::string& call) {
                std::string s;
                for (std::size_t i = 0; i < cols.size(); ++i) {
                    if (i) s += ", ";
                    s += recv + "." + cols[i].name + call;
                }
                return s;
            };
            if (!boundedHandle) {
            // aliasTable: the zero-copy typed snapshot (column aliases).
            frag += "    public #" + tblType + " aliasTable() {\n"
                "        if (this.plan != null) {\n"
                "            throw heap FrameException(\"Table<" + schema
                    + ">.aliasTable: unforced plan handle - collect() first\");\n"
                "        }\n"
                "        return heap " + tblType + "("
                    + joinCols("this", ".alias()") + ");\n"
                "    }\n";
            // __attachRebinder: install the display label and an OWNED
            // `<Record>TableRebinder` companion instance — how TEMPLATE
            // code (collect/head/as<R>) turns an executor frame back into
            // a typed table without naming synthesized members (a
            // `Table<?>` monomorph compiles those same template bodies).
            // An object, not a closure: closures are frame-owned and
            // cannot be stored past their creating frame; the companion is
            // an ordinary owned instance.
            frag += "    public void __attachRebinder() {\n"
                "        this.label = \"Table<" + schema + ">\";\n"
                "        this.rebinder #= heap " + recName
                    + "TableRebinder();\n"
                "    }\n";
            // __rebindOwned: the OWNED typed rebuild — same unwrap as the
            // rebinder companion but with a concrete return type, for call
            // sites that know the schema statically (`as<R>`'s materialized
            // branch calls it on the probe instance).
            {
                std::string unwraps;
                for (std::size_t i = 0; i < cols.size(); ++i) {
                    if (i) unwraps += ", ";
                    unwraps += "d.colAt(" + std::to_string(i) + ")."
                        + dynUnwrap(cols[i]) + "()";
                }
                frag += "    public #" + tblType
                        + " __rebindOwned(#DynFrame d) {\n"
                    "        return heap " + tblType + "(" + unwraps + ");\n"
                    "    }\n";
            }
            // head/fetch: bounded terminals — force, then a zero-copy row
            // window (numeric column slices are views; utf8 copies —
            // Column.slice docs). Synthesized (not template): the typed
            // result must be allocated by schema-aware code.
            frag += "    public #" + tblType + " head(int64 n) {\n"
                "        " + tblType + " __f = this.collect();\n"
                "        int64 __m = n;\n"
                "        if (__f.rowCount() < __m) { __m = __f.rowCount(); }\n"
                "        return heap " + tblType + "("
                    + joinCols("__f", ".slice(0, __m)") + ");\n"
                "    }\n";
            frag += "    public #" + tblType + " fetch(int64 n) { "
                "return this.head(n); }\n";
            // __narrowCheck: `.as<R>()`'s schema validation (spec §4.3.1)
            // against this record — strict: same column count, every field
            // present with its exact physical and nullability. Runs on a
            // schema-only frame at plan build; never forces.
            {
                frag += "    public void __narrowCheck() {\n"
                    "        DynFrame __d = this.__schemaOf();\n"
                    "        if (__d.width() != "
                        + std::to_string(cols.size()) + ") {\n"
                    "            throw heap FrameException(\"Table.as<" + schema
                        + ">: schema \" + __d.schemaText() + \" has \" + "
                        "stack Int64((int64) __d.width()).toString() + "
                        "\" columns; " + schema + " declares "
                        + std::to_string(cols.size()) + "\");\n"
                    "        }\n";
                for (std::size_t i = 0; i < cols.size(); ++i) {
                    const std::string ix = std::to_string(i);
                    const std::string expected = cols[i].phys
                        + (cols[i].nullable ? "?" : "");
                    frag += "        int32 __i" + ix + " = __d.find(\""
                            + cols[i].name + "\");\n"
                        "        if (__i" + ix + " < 0) {\n"
                        "            throw heap FrameException(\"Table.as<"
                            + schema + ">: column '" + cols[i].name
                            + "' absent from schema \" + __d.schemaText() + \"; "
                            + schema + "." + cols[i].name + " is '" + expected
                            + "'\");\n"
                        "        }\n"
                        "        String __t" + ix + " = __d.typeNameAt(__i"
                            + ix + ");\n"
                        "        if (__d.nullableAt(__i" + ix + ")) { __t" + ix
                            + " = __t" + ix + " + \"?\"; }\n"
                        "        if (!__t" + ix + ".equals(\"" + expected
                            + "\")) {\n"
                        "            throw heap FrameException(\"Table.as<"
                            + schema + ">: column '" + cols[i].name
                            + "' has type '\" + __t" + ix + " + \"'; " + schema
                            + "." + cols[i].name + " is '" + expected + "'\");\n"
                        "        }\n";
                }
                frag += "    }\n";
            }
            // The typed relational ops — the U1-decided lambda-param DSL:
            // the builder companion is the lambda's typed parameter, so a
            // schema typo or type mismatch is a COMPILE error. All three
            // are lazy (they build nodes via the template machinery).
            frag += "    public #" + tblType + " filter((" + recName
                    + "Cols) -> #Pred fn) {\n"
                "        " + recName + "Cols __c = heap " + recName + "Cols();\n"
                "        Pred __p = fn(__c);\n"
                "        return this.filter(#__p);\n"
                "    }\n";
            frag += "    public #Table<?> select((" + recName
                    + "Cols, Sels) -> void fn) {\n"
                "        " + recName + "Cols __c = heap " + recName + "Cols();\n"
                "        Sels __s = heap Sels();\n"
                "        fn(__c, __s);\n"
                "        int32 __n = __s.count();\n"
                "        return this.__project(__s.take(), __n, false);\n"
                "    }\n";
            frag += "    public #Table<?> with((" + recName
                    + "Cols) -> #Sel fn) {\n"
                "        " + recName + "Cols __c = heap " + recName + "Cols();\n"
                "        Sel __e = fn(__c);\n"
                "        return this.__project(#__e, 1, true);\n"
                "    }\n";
            // groupBy/agg (U8): keys reuse the Sels collector — a key is a
            // passthrough reference, which is exactly what Sels collects —
            // so single and multi-column grouping share one signature. The
            // handle stays typed between the two calls so `agg`'s builder
            // resolves; the aggregated RESULT is erased.
            frag += "    public #" + tblType + " groupBy((" + recName
                    + "Cols, Sels) -> void fn) {\n"
                "        " + recName + "Cols __c = heap " + recName + "Cols();\n"
                "        Sels __s = heap Sels();\n"
                "        fn(__c, __s);\n"
                "        int32 __n = __s.count();\n"
                "        return this.__groupBy(__s.take(), __n);\n"
                "    }\n";
            frag += "    public #Table<?> agg((" + recName
                    + "Cols, Aggs) -> void fn) {\n"
                "        " + recName + "Cols __c = heap " + recName + "Cols();\n"
                "        Aggs __a = heap Aggs();\n"
                "        fn(__c, __a);\n"
                "        int32 __n = __a.count();\n"
                "        return this.__agg(__a.take(), __n);\n"
                "    }\n";
            // rowAt: one TYPED row — the record — reconstructed from the
            // physicals (Instant from epoch-nanos, Utf8 from utf8; a
            // nullable field yields its physical value, validity stays a
            // column-accessor fact).
            frag += "    public " + recName + " rowAt(int64 i) {\n"
                "        " + tblType + " __f = this.collect();\n"
                "        if (__f.erased) {\n"
                "            throw heap FrameException(\"Table<" + schema
                    + ">.rowAt: schema-erased result - narrow with as<R>() "
                    "first\");\n"
                "        }\n"
                "        if (i < 0 || i >= __f.rowCount()) {\n"
                "            throw heap FrameException(\"Table<" + schema
                    + ">.rowAt: row index out of range\");\n"
                "        }\n"
                "        return " + recName + " { ";
            {
                bool firstField = true;
                for (auto& prop : fields) {
                    auto ft = prop->getType();
                    const std::string tn = (ft && ft->getQName())
                        ? ft->getQName()->getTypeName() : std::string();
                    const std::string read =
                        "__f." + prop->getName() + ".get(i)";
                    std::string value;
                    if (tn == "Instant") {
                        value = "stack Instant(" + read + " / 1000000000, "
                            "(int32) (" + read + " % 1000000000))";
                    } else if (tn == "Utf8") {
                        value = "Utf8.of(" + read + ")";
                    } else {
                        value = read;
                    }
                    if (!firstField) frag += ", ";
                    frag += prop->getName() + ": " + value;
                    firstField = false;
                }
            }
            frag += " };\n"
                "    }\n";
            // rows: the iteration terminal — a typed cursor over a forced
            // snapshot (the for-each protocol covers arrays only today; the
            // `for (row : table)` sugar is recorded for syntax-sugar).
            frag += "    public #" + recName + "Rows rows() {\n"
                "        " + tblType + " __f = this.collect();\n"
                "        return heap " + recName + "Rows(__f.aliasTable());\n"
                "    }\n";
            }  // !boundedHandle
            frag += "}";

            MemberSynthesisResult r;
            r.classBodyFragment = std::move(frag);
            r.imports = {{"Column", "cajeta.nucleo.column"},
                         {"NullableColumn", "cajeta.nucleo.column"},
                         {"StringColumn", "cajeta.nucleo.column"},
                         {"DynCol", "cajeta.nucleo.column"},
                         {"FrameException", "cajeta.nucleo.frame"},
                         {"Plan", "cajeta.nucleo.frame"},
                         {"DynFrame", "cajeta.nucleo.frame"},
                         {"Pred", "cajeta.nucleo.frame"},
                         {"Sel", "cajeta.nucleo.frame"},
                         {"Sels", "cajeta.nucleo.frame"},
                         {"Agg", "cajeta.nucleo.frame"},
                         {"Aggs", "cajeta.nucleo.frame"},
                         {"Int64", "cajeta.lang"},
                         {"Utf8", "cajeta.lang"},
                         {"Instant", "cajeta.time"}};
            if (!boundedHandle) {
                r.imports.emplace_back(recName + "Cols",
                    record->getQName()->getPackageName());
                r.imports.emplace_back(recName + "Rows",
                    record->getQName()->getPackageName());
                r.imports.emplace_back(recName + "TableRebinder",
                    record->getQName()->getPackageName());
            }
            return r;
        });

        // nucleo-frame U5 — the `<Record>TableRebinder` companion: the
        // typed-rebuild seam behind the frame `Rebinder<T>` interface.
        // Template code dispatches `rebinder.rebind(frame)` (it cannot name
        // synthesized members — a `Table<?>` monomorph compiles the same
        // bodies); this companion unwraps the frame's columns zero-copy in
        // schema order and calls the synthesized column constructor.
        // Concrete instantiations only.
        reg.registerCompanion("tableRebinder",
                [](const SynthesisContext& c)
                    -> std::optional<SynthesizerRegistry::CompanionSynthesisResult> {
            auto structure = c.parent;
            if (!structure || !structure->isInstantiation()) return std::nullopt;
            if (structure->isWildcardInstantiation()) return std::nullopt;
            auto origin = structure->getTemplateOrigin();
            if (!origin || !origin->getQName()
                    || origin->getQName()->toCanonical()
                        != "cajeta.nucleo.frame.Table") {
                return std::nullopt;
            }
            const auto& args = structure->getTypeArguments();
            if (args.size() != 1) return std::nullopt;
            auto record = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!record || !record->isRecordType()) return std::nullopt;

            std::vector<StructurePropertyPtr> fields;
            std::function<void(CajetaClassPtr)> collect =
                [&](CajetaClassPtr k) {
                    if (!k) return;
                    for (auto& s : k->getSuperClasses()) collect(s);
                    for (auto& p : k->getPropertyList()) {
                        if (p && !p->isStatic()) fields.push_back(p);
                    }
                };
            collect(record);

            SynthesizerRegistry::CompanionSynthesisResult r;
            const std::string recName = record->getQName()->getTypeName();
            r.className = recName + "TableRebinder";
            r.packageName = record->getQName()->getPackageName();
            r.imports.emplace_back("Table", "cajeta.nucleo.frame");
            r.imports.emplace_back("Rebinder", "cajeta.nucleo.frame");
            r.imports.emplace_back("DynFrame", "cajeta.nucleo.frame");
            const std::string tblType = "Table<" + recName + ">";
            std::string unwraps;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                auto ft = fields[i]->getType();
                const std::string tn = (ft && ft->getQName())
                    ? ft->getQName()->getTypeName() : std::string();
                const bool nullable =
                    fields[i]->findAnnotation("Nullable") != nullptr;
                std::string acc;
                if (tn == "Instant") acc = "asI64";
                else if (tn == "Utf8") acc = "asStr";
                else {
                    static const std::map<std::string, std::string> sfx = {
                        {"int8", "I8"}, {"int16", "I16"}, {"int32", "I32"},
                        {"int64", "I64"}, {"uint8", "U8"}, {"uint16", "U16"},
                        {"uint32", "U32"}, {"uint64", "U64"},
                        {"float32", "F32"}, {"float64", "F64"}};
                    auto it = sfx.find(tn);
                    if (it == sfx.end()) return std::nullopt;  // unmapped: the
                        // member synthesizer already threw the named error
                    acc = std::string("as") + (nullable ? "N" : "")
                        + it->second;
                }
                if (i) unwraps += ", ";
                unwraps += "d.colAt(" + std::to_string(i) + ")." + acc + "()";
            }
            r.imports.emplace_back("RebindSlot", "cajeta.nucleo.frame");
            r.classSource = "public class " + r.className
                + " implements Rebinder {\n"
                "    public " + r.className + "() { }\n"
                "    public void rebindInto(DynFrame d, RebindSlot slot) {\n"
                "        slot.put(heap " + tblType + "(" + unwraps + "));\n"
                "    }\n"
                "}\n";
            return r;
        });

        // nucleo-frame U4 — the `<Record>Rows` companion: the typed row
        // cursor `Table<Record>.rows()` returns. Holds an owned zero-copy
        // snapshot; `next()` reconstructs typed rows via `rowAt`. Concrete
        // instantiations only (the cursor names the concrete table type).
        reg.registerCompanion("tableRows",
                [](const SynthesisContext& c)
                    -> std::optional<SynthesizerRegistry::CompanionSynthesisResult> {
            auto structure = c.parent;
            if (!structure || !structure->isInstantiation()) return std::nullopt;
            if (structure->isWildcardInstantiation()) return std::nullopt;
            auto origin = structure->getTemplateOrigin();
            if (!origin || !origin->getQName()
                    || origin->getQName()->toCanonical()
                        != "cajeta.nucleo.frame.Table") {
                return std::nullopt;
            }
            const auto& args = structure->getTypeArguments();
            if (args.size() != 1) return std::nullopt;
            auto record = std::dynamic_pointer_cast<CajetaClass>(args[0]);
            if (!record || !record->isRecordType()) return std::nullopt;
            SynthesizerRegistry::CompanionSynthesisResult r;
            const std::string recName = record->getQName()->getTypeName();
            r.className = recName + "Rows";
            r.packageName = record->getQName()->getPackageName();
            r.imports.emplace_back("Table", "cajeta.nucleo.frame");
            r.imports.emplace_back("FrameException", "cajeta.nucleo.frame");
            const std::string tblType = "Table<" + recName + ">";
            r.classSource = "public class " + r.className + " {\n"
                "    " + tblType + " t;\n"
                "    int64 i;\n"
                "    int64 n;\n"
                "    public " + r.className + "(#" + tblType + " t) {\n"
                "        this.t #= t;\n"
                "        this.i = 0;\n"
                "        this.n = this.t.rowCount();\n"
                "    }\n"
                "    public boolean hasNext() { return this.i < this.n; }\n"
                "    public " + recName + " next() {\n"
                "        if (this.i >= this.n) {\n"
                "            throw heap FrameException(\"" + r.className
                    + ".next: past the end\");\n"
                "        }\n"
                "        " + recName + " r = this.t.rowAt(this.i);\n"
                "        this.i = this.i + 1;\n"
                "        return r;\n"
                "    }\n"
                "}\n";
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
            // float64/float32 -> ColF64; integers + Instant (epoch-nanos)
            // -> ColI64 (EXACT comparisons, no float64 round-trip); Utf8
            // (the record-legal text field; `String` kept for the U1 test
            // shells) -> ColStr; any other field type synthesizes NO
            // builder (the member-accessor synthesizer still covers direct
            // access). Fields enumerate supers-first recursively — the SAME
            // layout order the table synthesizer derives columns in, so
            // builder ordinals always match column ordinals.
            r.imports.emplace_back("ColF64", "cajeta.nucleo.frame");
            r.imports.emplace_back("ColI64", "cajeta.nucleo.frame");
            r.imports.emplace_back("ColStr", "cajeta.nucleo.frame");
            r.imports.emplace_back("Pred", "cajeta.nucleo.frame");
            std::vector<StructurePropertyPtr> colsFields;
            std::function<void(CajetaClassPtr)> collectFields =
                [&](CajetaClassPtr k) {
                    if (!k) return;
                    for (auto& s : k->getSuperClasses()) collectFields(s);
                    for (auto& p : k->getPropertyList()) {
                        if (p && !p->isStatic()) colsFields.push_back(p);
                    }
                };
            collectFields(record);
            std::string src = "public class " + r.className + " {\n"
                "    public " + r.className + "() { }\n";
            int32_t ord = 0;
            for (auto& prop : colsFields) {
                auto ft = prop->getType();
                std::string tn = (ft && ft->getQName())
                    ? ft->getQName()->getTypeName() : std::string();
                const std::string fieldName = prop->getName();
                if (tn == "float64" || tn == "float32") {
                    src += "    public #ColF64 " + fieldName
                        + "() { return ColF64.colRef(" + std::to_string(ord)
                        + ", \"" + fieldName + "\"); }\n";
                } else if (tn == "int8" || tn == "int16" || tn == "int32"
                        || tn == "int64" || tn == "uint8" || tn == "uint16"
                        || tn == "uint32" || tn == "uint64"
                        || tn == "Instant") {
                    src += "    public #ColI64 " + fieldName
                        + "() { return ColI64.colRef(" + std::to_string(ord)
                        + ", \"" + fieldName + "\"); }\n";
                } else if (tn == "String" || tn == "Utf8") {
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
