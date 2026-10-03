//
// `cajeta tune --reset <dir>` empties a tune store
// (xpu-tile-shape-selection plan 4.6.1.8, spec §7.12).
//
// A store is `<dir>/<device key>/<name>` with `<name>.for` beside each value
// (cajeta.xpu.Autotune). Reset removes every device directory's entries, so
// the next use of any tuned value or cached shape measures again. Anything
// else in `<dir>` is not the store's, and is left alone.
//
#include "gtest/gtest.h"

#include "cajeta/cli/TuneCommand.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path freshStore() {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path d = fs::temp_directory_path() / ("cajeta-tune-" + std::to_string(rng()));
    fs::create_directories(d / "cuda-128x4w32r65536l102400t1536");
    fs::create_directories(d / "cpu-unknown");
    std::ofstream(d / "cuda-128x4w32r65536l102400t1536" / "shape.fam.512x4096x4096") << "1";
    std::ofstream(d / "cuda-128x4w32r65536l102400t1536" / "shape.fam.512x4096x4096.for") << "a:x;";
    std::ofstream(d / "cpu-unknown" / "mv.rows") << "4";
    std::ofstream(d / "README.txt") << "not the store's";
    return d;
}

} // namespace

TEST(TuneCommand, resetEmptiesEveryDeviceDirectory) {
    fs::path d = freshStore();
    std::ostringstream out;
    EXPECT_EQ(cajeta::tuneReset(d.string(), out), 0) << out.str();
    EXPECT_FALSE(fs::exists(d / "cuda-128x4w32r65536l102400t1536"));
    EXPECT_FALSE(fs::exists(d / "cpu-unknown"));
    EXPECT_TRUE(fs::exists(d / "README.txt")) << "only the store's entries go";
    EXPECT_NE(out.str().find("3"), std::string::npos)
        << "it says how many entries it removed: " << out.str();
    fs::remove_all(d);
}

TEST(TuneCommand, aMissingStoreIsANamedError) {
    std::ostringstream out;
    EXPECT_NE(cajeta::tuneReset("/nonexistent/cajeta-tune-store", out), 0);
    EXPECT_NE(out.str().find("/nonexistent/cajeta-tune-store"), std::string::npos)
        << out.str();
}

TEST(TuneCommand, theSubcommandNeedsResetAndADirectory) {
    const char* noDir[] = {"cajeta", "tune", "--reset"};
    EXPECT_NE(cajeta::dispatchTune(3, noDir), 0);
    const char* noVerb[] = {"cajeta", "tune"};
    EXPECT_NE(cajeta::dispatchTune(2, noVerb), 0);
}
