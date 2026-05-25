//
// CajetaCapture — see header for the design. This translation unit
// hosts the monotonic ID counter and the factory implementations.
//

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
        // Captures live in a synthetic package so their qNames can't
        // collide with anything the user writes. The simple name carries
        // the ID for readability in diagnostics / dumps.
        std::string simple = "capture#" + std::to_string(id);
        return QualifiedName::getOrInsert(simple, "__cajeta_capture__");
    }

    CajetaCapturePtr CajetaCapture::forExtendsBound(
            CajetaModulePtr module, CajetaTypePtr upperBound) {
        int64_t id = nextCaptureId();
        return make_shared<CajetaCapture>(
            module, makeCaptureName(id), id, upperBound, nullptr);
    }

    CajetaCapturePtr CajetaCapture::forSuperBound(
            CajetaModulePtr module, CajetaTypePtr lowerBound) {
        int64_t id = nextCaptureId();
        return make_shared<CajetaCapture>(
            module, makeCaptureName(id), id, nullptr, lowerBound);
    }

    CajetaCapturePtr CajetaCapture::forUnbounded(CajetaModulePtr module) {
        int64_t id = nextCaptureId();
        return make_shared<CajetaCapture>(
            module, makeCaptureName(id), id, nullptr, nullptr);
    }

} // namespace cajeta
