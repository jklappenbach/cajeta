//
// UcdConformanceTests — stdlib-completion plan Unit 7 (spec §7, §9.8):
// **the Unicode NormalizationTest.txt conformance file, in full** (7.1.1 /
// 7.3.1 — partial passage is failure). Runs against the REAL shipped core
// (cajeta_rt_ucd_core.c, compiled in via UcdCoreShim.c), pinned Unicode
// 16.0.0 data committed at test/data/ucd/NormalizationTest.txt.
//
// Per the file's own invariants, for each line c1..c5:
//   NFC:  c2 == NFC(c1) == NFC(c2) == NFC(c3);  c4 == NFC(c4) == NFC(c5)
//   NFD:  c3 == NFD(c1) == NFD(c2) == NFD(c3);  c5 == NFD(c4) == NFD(c5)
//   NFKC: c4 == NFKC(c1..c5)
//   NFKD: c5 == NFKD(c1..c5)
// and every assigned codepoint NOT in Part 1 must normalize to itself in
// all four forms.
//

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
int32_t __cajeta_ucd_is_normalized_buf(const uint8_t* p, int64_t n, int32_t form);
int64_t __cajeta_ucd_normalize_buf(const uint8_t* p, int64_t n, int32_t form,
                                   uint8_t** out);
int32_t __cajeta_ucd_encode_cp(uint32_t cp, uint8_t* out);
int32_t __cajeta_ucd_fold_cp(int32_t cp, uint32_t* out);
int32_t __cajeta_ucd_ccc(int32_t cp);
int32_t __cajeta_ucd_script(int32_t cp);
int32_t __cajeta_ucd_joining_type(int32_t cp);
int32_t __cajeta_ucd_bidi_class(int32_t cp);
int32_t __cajeta_ucd_default_ignorable(int32_t cp);
const char* __cajeta_ucd_script_name(int32_t id);
}

namespace {

enum { NFC = 0, NFD = 1, NFKC = 2, NFKD = 3 };

std::string encodeCps(const std::vector<uint32_t>& cps) {
    std::string out;
    uint8_t buf[4];
    for (uint32_t cp : cps) {
        int n = __cajeta_ucd_encode_cp(cp, buf);
        out.append((const char*) buf, (size_t) n);
    }
    return out;
}

std::string normalize(const std::string& s, int form) {
    uint8_t* out = nullptr;
    int64_t n = __cajeta_ucd_normalize_buf(
        (const uint8_t*) s.data(), (int64_t) s.size(), form, &out);
    if (n < 0) return "<malformed>";
    std::string r((const char*) out, (size_t) n);
    free(out);
    return r;
}

std::vector<uint32_t> parseCps(const std::string& field) {
    std::vector<uint32_t> cps;
    std::istringstream in(field);
    std::string tok;
    while (in >> tok) cps.push_back((uint32_t) strtoul(tok.c_str(), nullptr, 16));
    return cps;
}

std::string conformancePath() {
    const char* root = getenv("CAJETA_SOURCE_ROOT");
    std::string base = root ? root : ".";
    return base + "/test/data/ucd/NormalizationTest.txt";
}

} // namespace

// 7.1.1 / 7.3.1 — the conformance file passes IN FULL.
TEST(UcdConformanceTests, normalizationTestFilePassesInFull) {
    std::ifstream in(conformancePath());
    ASSERT_TRUE(in.good()) << "missing " << conformancePath();
    std::string line;
    // Header pin: the committed file must be the pinned Unicode version.
    std::getline(in, line);
    ASSERT_NE(line.find("NormalizationTest-16.0.0.txt"), std::string::npos);

    std::set<uint32_t> part1;
    bool inPart1 = false;
    int64_t cases = 0, failures = 0;
    std::string firstFailure;

    auto check = [&](const std::string& what, const std::string& got,
                     const std::string& want, int64_t lineNo) {
        if (got != want) {
            failures++;
            if (firstFailure.empty()) {
                std::ostringstream m;
                m << "line " << lineNo << " " << what;
                firstFailure = m.str();
            }
        }
    };

    int64_t lineNo = 1;
    while (std::getline(in, line)) {
        lineNo++;
        if (!line.empty() && line[0] == '@') {
            inPart1 = line.rfind("@Part1", 0) == 0;
            continue;
        }
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        if (line.find(';') == std::string::npos) continue;

        std::vector<std::string> f;
        std::istringstream ls(line);
        std::string field;
        while (std::getline(ls, field, ';')) f.push_back(field);
        if (f.size() < 5) continue;

        std::string c[6];
        std::vector<uint32_t> cps1 = parseCps(f[0]);
        c[1] = encodeCps(cps1);
        c[2] = encodeCps(parseCps(f[1]));
        c[3] = encodeCps(parseCps(f[2]));
        c[4] = encodeCps(parseCps(f[3]));
        c[5] = encodeCps(parseCps(f[4]));
        if (inPart1 && cps1.size() == 1) part1.insert(cps1[0]);

        // NFC invariants
        check("NFC(c1)", normalize(c[1], NFC), c[2], lineNo);
        check("NFC(c2)", normalize(c[2], NFC), c[2], lineNo);
        check("NFC(c3)", normalize(c[3], NFC), c[2], lineNo);
        check("NFC(c4)", normalize(c[4], NFC), c[4], lineNo);
        check("NFC(c5)", normalize(c[5], NFC), c[4], lineNo);
        // NFD invariants
        check("NFD(c1)", normalize(c[1], NFD), c[3], lineNo);
        check("NFD(c2)", normalize(c[2], NFD), c[3], lineNo);
        check("NFD(c3)", normalize(c[3], NFD), c[3], lineNo);
        check("NFD(c4)", normalize(c[4], NFD), c[5], lineNo);
        check("NFD(c5)", normalize(c[5], NFD), c[5], lineNo);
        // NFKC / NFKD invariants
        for (int i = 1; i <= 5; i++) {
            check("NFKC", normalize(c[i], NFKC), c[4], lineNo);
            check("NFKD", normalize(c[i], NFKD), c[5], lineNo);
        }
        cases++;
    }
    ASSERT_GT(cases, 18000) << "conformance file truncated?";
    EXPECT_EQ(failures, 0) << "first failure: " << firstFailure
                           << " (" << failures << " total across "
                           << cases << " cases)";

    // Part 1 closure: every codepoint NOT listed in Part 1 normalizes to
    // itself in every form (surrogates excluded — not scalar values).
    int64_t closureFailures = 0;
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        if (part1.count(cp)) continue;
        std::string s = encodeCps({cp});
        for (int form = 0; form < 4; form++) {
            if (normalize(s, form) != s) {
                closureFailures++;
                break;
            }
        }
    }
    EXPECT_EQ(closureFailures, 0);
}

// 7.1.5's machinery at the core level: the quick-check scan agrees with a
// full transform on every conformance column.
TEST(UcdConformanceTests, quickCheckAgreesWithTransform) {
    std::ifstream in(conformancePath());
    ASSERT_TRUE(in.good());
    std::string line;
    int64_t disagreements = 0, checked = 0;
    while (std::getline(in, line) && checked < 200000) {
        if (line.empty() || line[0] == '@' || line[0] == '#') continue;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::vector<std::string> f;
        std::istringstream ls(line);
        std::string field;
        while (std::getline(ls, field, ';')) f.push_back(field);
        if (f.size() < 5) continue;
        for (int i = 0; i < 5; i++) {
            std::string s = encodeCps(parseCps(f[i]));
            for (int form = 0; form < 4; form++) {
                bool qc = __cajeta_ucd_is_normalized_buf(
                    (const uint8_t*) s.data(), (int64_t) s.size(), form) == 1;
                bool same = normalize(s, form) == s;
                // QC "yes" must imply "transform is identity". (QC may say
                // no/maybe on an already-normalized string; that only costs
                // a copy, never correctness.)
                if (qc && !same) disagreements++;
                checked++;
            }
        }
    }
    EXPECT_EQ(disagreements, 0);
    EXPECT_GT(checked, 100000);
}

// 7.1.4 spot pins — full case folding differs from ASCII lowercasing on
// the canonical scripts; 7.2.8's property surface answers for shaping.
TEST(UcdConformanceTests, foldAndPropertySpotChecks) {
    // German sharp s folds to "ss"
    uint32_t out[3];
    EXPECT_EQ(__cajeta_ucd_fold_cp(0x00DF, out), 2);
    EXPECT_EQ(out[0], 0x73u);
    EXPECT_EQ(out[1], 0x73u);
    // Turkish dotted capital I folds to i + combining dot above (F mapping)
    EXPECT_EQ(__cajeta_ucd_fold_cp(0x0130, out), 2);
    EXPECT_EQ(out[0], 0x69u);
    EXPECT_EQ(out[1], 0x0307u);
    // Turkish dotless i folds to itself (identity)
    EXPECT_EQ(__cajeta_ucd_fold_cp(0x0131, out), 0);
    // ASCII I folds to i
    EXPECT_EQ(__cajeta_ucd_fold_cp('I', out), 1);
    EXPECT_EQ(out[0], (uint32_t) 'i');

    // ccc: combining acute is 230; base letters 0
    EXPECT_EQ(__cajeta_ucd_ccc(0x0301), 230);
    EXPECT_EQ(__cajeta_ucd_ccc('a'), 0);
    // script names resolve: Latin a, Arabic alef, Devanagari ka, Han
    EXPECT_STREQ(__cajeta_ucd_script_name(__cajeta_ucd_script('a')), "Latin");
    EXPECT_STREQ(__cajeta_ucd_script_name(__cajeta_ucd_script(0x0627)), "Arabic");
    EXPECT_STREQ(__cajeta_ucd_script_name(__cajeta_ucd_script(0x0915)), "Devanagari");
    EXPECT_STREQ(__cajeta_ucd_script_name(__cajeta_ucd_script(0x4E2D)), "Han");
    // joining type: Arabic alef is Right-joining (2), beh is Dual (4),
    // tatweel is Join_Causing (5), zero-width-non-joiner is U... ZWNJ is
    // 200C: listed U in ArabicShaping; combining marks are Transparent (1)
    EXPECT_EQ(__cajeta_ucd_joining_type(0x0627), 2);
    EXPECT_EQ(__cajeta_ucd_joining_type(0x0628), 4);
    EXPECT_EQ(__cajeta_ucd_joining_type(0x0640), 5);
    EXPECT_EQ(__cajeta_ucd_joining_type(0x0301), 1);
    // default-ignorables: soft hyphen and ZWJ yes, letters no
    EXPECT_EQ(__cajeta_ucd_default_ignorable(0x00AD), 1);
    EXPECT_EQ(__cajeta_ucd_default_ignorable(0x200D), 1);
    EXPECT_EQ(__cajeta_ucd_default_ignorable('x'), 0);
}
