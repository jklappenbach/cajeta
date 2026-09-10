// CajetaCapture — see header; this TU hosts the monotonic ID counter and the factories.

#include "CajetaCapture.h"

#include <atomic>

namespace cajeta {

    int64_t CajetaCapture::nextCaptureId() {
        static std::atomic<int64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    CajetaCapture::CajetaCapture(CajetaModulePtr module,
                                  QualifiedNamePtr qName,
                                  int64_t id,
                                  CajetaTypePtr upper,
                                  CajetaTypePtr lower)
        : CajetaClass(module, qName, {}, {}),
          captureId(id),
          upperBound(upper),
          lowerBound(lower) {
    }

    static QualifiedNamePtr makeCaptureName(int64_t id) {
        // Captures live in a synthetic package so their qNames cannot collide with user names; the simple name carries the ID.
        std::string simple = "capture#" + std::to_string(id);
        return QualifiedName::getOrInsert(simple, "__cajeta_capture__");
    }

    CajetaCapturePtr CajetaCapture::forExtendsBound(
            CajetaModulePtr module, CajetaTypePtr upperBound) {
        int64_t id = nextCaptureId();
        auto qName = makeCaptureName(id);
        // Registered as a wildcard-flavored type, so the existing wildcard machinery
        // (bounds, substitution-stable hashes, the PECS check) accepts captures uniformly.
        CajetaType::registerWildcardInfo(
            qName->toCanonical(), WildcardKind::Extends, upperBound);
        return make_shared<CajetaCapture>(
            module, qName, id, upperBound, nullptr);
    }

    CajetaCapturePtr CajetaCapture::forSuperBound(
            CajetaModulePtr module, CajetaTypePtr lowerBound) {
        int64_t id = nextCaptureId();
        auto qName = makeCaptureName(id);
        CajetaType::registerWildcardInfo(
            qName->toCanonical(), WildcardKind::Super, lowerBound);
        return make_shared<CajetaCapture>(
            module, qName, id, nullptr, lowerBound);
    }

    CajetaCapturePtr CajetaCapture::forUnbounded(CajetaModulePtr module) {
        int64_t id = nextCaptureId();
        auto qName = makeCaptureName(id);
        CajetaType::registerWildcardInfo(
            qName->toCanonical(), WildcardKind::Unbounded, nullptr);
        return make_shared<CajetaCapture>(
            module, qName, id, nullptr, nullptr);
    }

} // namespace cajeta
