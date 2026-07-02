#include "cajeta/buildtool/Provenance.h"

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        constexpr const char* kStatementType =
            "https://in-toto.io/Statement/v1";
        constexpr const char* kPredicateType =
            "https://slsa.dev/provenance/v1";
        constexpr const char* kBuildType =
            "https://cajeta.org/build/v1";

        // Strip the `sha256:` prefix iff present — the SLSA spec's
        // digest object keys are bare algorithm names with bare
        // hex values, not the cajeta-internal `sha256:<hex>`
        // composite form.
        std::string stripSha256Prefix(const std::string& s) {
            if (s.size() >= 7 && s.compare(0, 7, "sha256:") == 0) {
                return s.substr(7);
            }
            return s;
        }

    } // namespace

    std::string composeProvenanceJson(const ProvenanceInputs& in) {
        llvm::json::Object envelope;
        envelope["_type"] = kStatementType;

        llvm::json::Array subjects;
        {
            llvm::json::Object subject;
            subject["name"] = in.archiveName;
            subject["digest"] = llvm::json::Object{
                {"sha256", stripSha256Prefix(in.archiveSha256)},
            };
            subjects.push_back(std::move(subject));
        }
        envelope["subject"] = llvm::json::Value(std::move(subjects));
        envelope["predicateType"] = kPredicateType;

        llvm::json::Object predicate;
        predicate["buildDefinition"] = llvm::json::Object{
            {"buildType", kBuildType},
            {"externalParameters", llvm::json::Object{
                {"manifest-checksum", in.manifestChecksum},
                {"lockfile-checksum", in.lockfileChecksum},
            }},
            {"internalParameters", llvm::json::Object{
                {"compiler-version", in.compilerVersion},
                {"flavor", in.flavor},
                {"target", in.target},
            }},
        };
        predicate["runDetails"] = llvm::json::Object{
            {"builder", llvm::json::Object{
                {"id", in.builderId.empty()
                       ? std::string("https://cajeta.org/local-build")
                       : in.builderId},
            }},
            {"metadata", llvm::json::Object{
                {"startedOn",  in.startedOn},
                {"finishedOn", in.finishedOn},
            }},
        };
        envelope["predicate"] = llvm::json::Value(std::move(predicate));

        std::string out;
        {
            llvm::raw_string_ostream os(out);
            os << llvm::formatv("{0:2}",
                                llvm::json::Value(std::move(envelope)));
        }
        if (!out.empty() && out.back() != '\n') out += '\n';
        return out;
    }

    llvm::Expected<ProvenanceVerifyResult> verifyProvenanceJson(
        const std::string& jsonDoc,
        const std::string& expectedSha256) {
        auto val = llvm::json::parse(jsonDoc);
        if (!val) {
            return err("provenance: parse error");
        }
        const auto* root = val->getAsObject();
        if (!root) {
            return err("provenance: top-level value is not an object");
        }
        auto ty = root->getString("_type");
        if (!ty || ty->str() != kStatementType) {
            return err("provenance: _type mismatch — expected '" +
                       std::string(kStatementType) + "'");
        }
        auto pty = root->getString("predicateType");
        if (!pty || pty->str() != kPredicateType) {
            return err("provenance: predicateType mismatch — "
                       "expected '" + std::string(kPredicateType) + "'");
        }
        const auto* subj = root->getArray("subject");
        if (!subj || subj->empty()) {
            return err("provenance: subject[] is empty");
        }
        const auto* s0 = (*subj)[0].getAsObject();
        if (!s0) return err("provenance: subject[0] is not an object");
        const auto* digest = s0->getObject("digest");
        if (!digest) return err("provenance: subject[0].digest missing");
        auto got = digest->getString("sha256");
        if (!got) return err("provenance: subject[0].digest.sha256 missing");
        if (got->str() != stripSha256Prefix(expectedSha256)) {
            return err("provenance: archive digest mismatch — "
                       "claimed sha256:" + got->str() +
                       ", actual " + expectedSha256);
        }
        const auto* pred = root->getObject("predicate");
        if (!pred) return err("provenance: predicate missing");
        const auto* bd = pred->getObject("buildDefinition");
        if (!bd) return err("provenance: predicate.buildDefinition missing");

        ProvenanceVerifyResult out;
        if (auto v = bd->getString("buildType")) out.buildType = v->str();
        if (out.buildType != kBuildType) {
            return err("provenance: buildType '" + out.buildType +
                       "' is not the cajeta build type");
        }
        if (const auto* ext = bd->getObject("externalParameters")) {
            if (auto v = ext->getString("manifest-checksum"))
                out.manifestChecksum = v->str();
            if (auto v = ext->getString("lockfile-checksum"))
                out.lockfileChecksum = v->str();
        }
        if (const auto* in = bd->getObject("internalParameters")) {
            if (auto v = in->getString("compiler-version"))
                out.compilerVersion = v->str();
            if (auto v = in->getString("flavor"))
                out.flavor = v->str();
            if (auto v = in->getString("target"))
                out.target = v->str();
        }
        if (out.compilerVersion.empty()) {
            return err("provenance: internalParameters.compiler-version "
                       "is empty — required field");
        }
        if (out.manifestChecksum.empty()) {
            return err("provenance: externalParameters.manifest-checksum "
                       "is empty — required field");
        }
        return out;
    }

} // namespace cajeta::buildtool
