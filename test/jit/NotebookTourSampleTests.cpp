// samples/notebook — the tour notebook, executed.
//
// A sample that claims specific outcomes is a claim under test, not
// decoration. This runs the REAL `tour.ipynb` cell by cell through a real
// session, so the notebook cannot drift from what its prose says: the cell
// sources come out of the file rather than being copied here.
//
// The tour is also the only place several behaviours are exercised
// end-to-end through the stdlib API against a signed, checksummed
// filesystem repository, which is what makes it worth a test.
//
// Everything runs in a COPY of the sample: Part 5 calls installAndSave,
// which rewrites cajeta.json, and a test must not dirty the working tree.

#include "gtest/gtest.h"
#include "../PortableEnv.h"

#include "cajeta/kernel/KernelSession.h"

#include <llvm/Support/JSON.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;
using cajeta::kernel::SessionOptions;

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() {
    if (const char* env = std::getenv("CAJETA_SOURCE_ROOT")) return env;
    auto here = fs::current_path();
    if (fs::is_directory(here / "samples" / "notebook")) return here;
    return here.parent_path();
}

std::string compilerBinary() {
    if (const char* env = std::getenv("CAJETA_BINARY")) return env;
    auto direct = fs::current_path() / "src" / "cajeta";
    if (fs::is_regular_file(direct)) return direct.string();
    return (fs::current_path() / "build" / "src" / "cajeta").string();
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// The notebook's code cells, in order, joined from each cell's source
// array. Empty when the file cannot be read or parsed.
std::vector<std::string> codeCellsOf(const fs::path& ipynb) {
    std::vector<std::string> out;
    auto parsed = llvm::json::parse(readFile(ipynb));
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        return out;
    }
    auto* root = parsed->getAsObject();
    if (!root) return out;
    auto* cells = root->getArray("cells");
    if (!cells) return out;
    for (auto& cell : *cells) {
        auto* obj = cell.getAsObject();
        if (!obj) continue;
        auto kind = obj->getString("cell_type");
        if (!kind || *kind != "code") continue;
        auto* src = obj->getArray("source");
        if (!src) continue;
        std::string body;
        for (auto& line : *src) {
            if (auto s = line.getAsString()) body += s->str();
        }
        out.push_back(body);
    }
    return out;
}

// What each code cell must do, in order. `ok == false` marks the cells the
// notebook LABELS as expected to fail — if one of those ever succeeds the
// tour is lying to the reader, which is just as much a failure.
struct Expectation {
    bool ok;
    const char* result;   // expected Out[N]; nullptr = don't care
    const char* mustSay;  // substring the failure has to contain
    const char* why;
};

const Expectation kExpected[] = {
    {true,  "42",     nullptr,                      "1 - bindings"},
    {true,  "21",     nullptr,                      "1 - class + heap"},
    {true,  "42",     nullptr,                      "1 - state persists"},
    {true,  "1.0.0",  nullptr,                      "2 - install returns version"},
    {true,  "42",     nullptr,                      "2 - import in a later cell"},
    {true,  "1.0.0",  nullptr,                      "2 - re-install is a no-op"},
    {false, nullptr,  "restart",                    "3 - version conflict"},
    {false, nullptr,  "Repositories consulted",     "3 - unknown library"},
    {true,  "85",     nullptr,                      "3 - session survived both"},
    {false, nullptr,  "next cell",                  "3 - same-cell import"},
    {true,  "1.0.0",  nullptr,                      "3 - install alone"},
    {true,  "7",      nullptr,                      "3 - import in the next cell"},
    {false, nullptr,  "signature",                  "4 - untrusted signer"},
    {true,  "1.0.0",  nullptr,                      "5 - installAndSave"},
    {true,  "1",      nullptr,                      "6 - a cell's own class"},
    {false, nullptr,  "declared by an earlier cell","6 - collision"},
    {true,  "1",      nullptr,                      "6 - the cell's class won"},
};

}  // namespace

TEST(NotebookTourSampleTests, everyCellOfTheTourBehavesAsTheProseSays) {
    auto sample = repoRoot() / "samples" / "notebook";
    ASSERT_TRUE(fs::is_directory(sample))
        << "cannot find samples/notebook from " << fs::current_path();

    // The tour must ship CLEAN: no saved outputs, no execution counts, no
    // stray trailing cell. Running it in Jupyter Lab autosaves, and "run
    // and advance" on the last cell appends an empty one — both landed in
    // the file the first time it was driven live. Saved outputs also bake
    // one machine's paths into the sample and make every run a diff.
    {
        std::string raw = readFile(sample / "notebooks" / "tour.ipynb");
        auto parsed = llvm::json::parse(raw);
        ASSERT_TRUE(!!parsed) << "tour.ipynb does not parse";
        auto* cellsArr = parsed->getAsObject()->getArray("cells");
        ASSERT_NE(nullptr, cellsArr);
        for (auto& cell : *cellsArr) {
            auto* obj = cell.getAsObject();
            auto kind = obj->getString("cell_type");
            if (!kind || *kind != "code") continue;
            auto* outputs = obj->getArray("outputs");
            EXPECT_TRUE(!outputs || outputs->empty())
                << "tour.ipynb carries saved outputs — strip them before "
                   "committing; the reader is meant to run it";
            std::string body;
            if (auto* src = obj->getArray("source")) {
                for (auto& line : *src) {
                    if (auto v = line.getAsString()) body += v->str();
                }
            }
            EXPECT_FALSE(body.find_first_not_of(" \t\r\n")
                         == std::string::npos)
                << "tour.ipynb has an EMPTY code cell — Jupyter appends one "
                   "when you run the last cell; drop it";
        }
    }

    auto cells = codeCellsOf(sample / "notebooks" / "tour.ipynb");
    ASSERT_EQ(std::size(kExpected), cells.size())
        << "the tour's code-cell count changed; update kExpected so the "
           "table still describes the notebook";

    // Work on a copy — Part 5 rewrites cajeta.json.
    auto work = fs::temp_directory_path() / "cajeta-notebook-tour";
    fs::remove_all(work);
    fs::copy(sample, work, fs::copy_options::recursive);

    // Stage the fixture repository, keys and signatures exactly as a reader
    // would. This is also what proves setup.sh still works.
    std::string setup = "cd " + work.string()
        + " && CAJETA_BINARY=" + compilerBinary()
        + " ./setup.sh > setup.log 2>&1";
    int rc = std::system(setup.c_str());
#ifndef _WIN32
    rc = WEXITSTATUS(rc);
#endif
    ASSERT_EQ(0, rc) << "setup.sh failed:\n" << readFile(work / "setup.log");

    setenv("CAJETA_TRUST_KEYS_DIR", (work / "trust").string().c_str(), 1);

    SessionOptions options;
    options.projectDir = work.string();
    std::string error;
    auto s = KernelSession::create(options, &error);
    ASSERT_NE(nullptr, s.get()) << "session create failed: " << error;

    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& want = kExpected[i];
        CellResult r = s->execute(cells[i], "tour-cell-" + std::to_string(i));

        if (want.ok) {
            ASSERT_TRUE(r.ok) << "cell " << i << " (" << want.why
                              << ") should run: " << r.errorId << ": "
                              << r.message;
            if (want.result) {
                ASSERT_TRUE(r.hasResult) << "cell " << i << " produced no value";
                EXPECT_EQ(want.result, r.result)
                    << "cell " << i << " (" << want.why << ")";
            }
        } else {
            EXPECT_FALSE(r.ok)
                << "cell " << i << " (" << want.why << ") is LABELLED "
                   "expected-to-fail in the notebook, but it succeeded — the "
                   "tour would be telling the reader something untrue";
            EXPECT_NE(std::string::npos, r.message.find(want.mustSay))
                << "cell " << i << " (" << want.why << ") must mention '"
                << want.mustSay << "'; got: " << r.message;
        }
    }

    // Part 5's claim: the dependency reached the manifest, and the file's
    // comments survived the write.
    std::string manifest = readFile(work / "cajeta.json");
    EXPECT_NE(std::string::npos, manifest.find("\"demo\""))
        << "installAndSave did not record the dependency";
    EXPECT_NE(std::string::npos, manifest.find("Tour project for the Cajeta"))
        << "the manifest's comments did not survive the write";

    s->shutdown();
    fs::remove_all(work);
}

// The bug that made the tour fail in real Jupyter but pass in-process:
// the kernel took its launch directory verbatim as the project. Jupyter
// starts a kernel in the NOTEBOOK's directory, so with the conventional
// `notebooks/` layout it found no cajeta.json, ran with no classpath, and
// resolved installs against the default central repository instead of the
// project's own. The in-process tests all passed because they set
// projectDir explicitly — nothing exercised the launch path.
TEST(NotebookTourSampleTests, aKernelLaunchedInNotebooksFindsTheProjectAbove) {
    auto root = fs::temp_directory_path() / "cajeta-kernel-launch-dir";
    fs::remove_all(root);
    auto notebooks = root / "proj" / "notebooks";
    fs::create_directories(notebooks);
    std::ofstream(root / "proj" / "cajeta.json") << "{}\n";

    // The Jupyter case: launched one level below the manifest.
    EXPECT_EQ((root / "proj").string(),
              cajeta::kernel::projectDirForLaunch(notebooks.string()))
        << "a kernel launched in notebooks/ must adopt the project above it";

    // Launched AT the project root: itself.
    EXPECT_EQ((root / "proj").string(),
              cajeta::kernel::projectDirForLaunch((root / "proj").string()));

    // No manifest anywhere on the chain: cwd, unchanged. A session with no
    // project is legal — it just has no classpath and no repositories.
    auto bare = root / "bare";
    fs::create_directories(bare);
    EXPECT_EQ(bare.string(), cajeta::kernel::projectDirForLaunch(bare.string()))
        << "with no manifest on the ancestry chain, cwd is the answer";

    fs::remove_all(root);
}
