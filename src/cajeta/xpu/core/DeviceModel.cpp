//
// DeviceModel — see header. Occupancy model + feasible/ordered candidate blocks
// + the linear conflict-free LDS pad.
//

#include "DeviceModel.h"

#include <algorithm>
#include <numeric>

namespace cajeta {
namespace xpu {

unsigned DeviceModel::occupancy(unsigned block, unsigned kernelVgpr,
                                unsigned ldsBytes) const {
    if (block == 0 || block > maxThreadsPerBlock || waveSize == 0) return 0;
    unsigned wavesPerWG = (block + waveSize - 1) / waveSize;
    if (wavesPerWG == 0) return 0;

    // Register limiter: waves a SIMD can hold given the per-thread VGPR demand,
    // capped by the hardware wave limit; scaled to the CU.
    unsigned vg = kernelVgpr == 0 ? 1 : kernelVgpr;
    unsigned wavesPerSIMD = std::min(maxWavesPerSIMD, vgprFilePerSIMD / vg);
    unsigned wavesPerCUByVgpr = wavesPerSIMD * simdsPerCU;

    // Workgroups that fit per CU: the min of the register-wave budget and the
    // LDS budget (LDS is allocated per workgroup).
    unsigned wgByVgpr = wavesPerCUByVgpr / wavesPerWG;
    unsigned wgByLds = ldsBytes == 0 ? wgByVgpr
                                     : ldsBytesPerCU / std::max(ldsBytes, 1u);
    unsigned wg = std::min(wgByVgpr, wgByLds);
    if (wg == 0) return 0;   // does not fit -> pruned

    return std::min(maxWavesPerCU, wg * wavesPerWG);
}

std::vector<unsigned> DeviceModel::candidateBlocks(unsigned kernelVgpr,
                                                   unsigned ldsBytes,
                                                   unsigned clamp) const {
    std::vector<std::pair<unsigned, unsigned>> scored;   // (block, occupancy)
    for (unsigned block = waveSize; block <= maxThreadsPerBlock;
         block += waveSize) {
        if (clamp != 0 && block > clamp) break;
        unsigned occ = occupancy(block, kernelVgpr, ldsBytes);
        if (occ == 0) continue;   // pruned: cannot fit the budgets
        scored.push_back({block, occ});
    }
    // Order best-first: highest occupancy, then the larger block (fewer, fuller
    // workgroups — less barrier/launch overhead) as a tiebreak.
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first > b.first;
              });
    std::vector<unsigned> out;
    out.reserve(scored.size());
    for (auto& [block, occ] : scored) out.push_back(block);
    return out;
}

unsigned conflictFreePad(unsigned rowElems, unsigned elemBytes,
                         unsigned bankCount, unsigned bankWidth) {
    if (elemBytes == 0 || bankWidth == 0 || bankCount == 0) return 0;
    // Smallest pad whose padded row is a whole, bank-coprime number of banks.
    for (unsigned pad = 0; pad <= bankCount; ++pad) {
        unsigned totalBytes = (rowElems + pad) * elemBytes;
        if (totalBytes % bankWidth != 0) continue;   // must align to whole banks
        unsigned dwords = totalBytes / bankWidth;
        if (std::gcd(dwords, bankCount) == 1) return pad;
    }
    return 0;
}

} // namespace xpu
} // namespace cajeta
