// Native Windows from Git Bash, measured 2026-10-17 with the published v0.43.1
// cajeta.exe: `--classpath=/d/a.cja,/d/b.cja` reached the compiler unconverted
// (CAJETA_ERROR_CLASSPATH_UNREADABLE), and `--emit=exe` found no C driver
// because Git Bash's PATH holds none. These pin the two pure helpers the fixes
// stand on; they run on every host.

#include "cajeta/compile/WindowsHostPaths.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace cajeta;

TEST(WindowsHostPathsTests, msysDrivePathBecomesWindowsPath) {
    EXPECT_EQ(msysDriveToWindows("/d/cajeta-xgb-win/build/.unit-cache/dev.cajeta.unit-0.3.6.cja"),
              "D:/cajeta-xgb-win/build/.unit-cache/dev.cajeta.unit-0.3.6.cja");
    EXPECT_EQ(msysDriveToWindows("/c/msys64"), "C:/msys64");
    EXPECT_EQ(msysDriveToWindows("/D/x"), "D:/x");
}

TEST(WindowsHostPathsTests, bareDriveBecomesDriveRoot) {
    EXPECT_EQ(msysDriveToWindows("/d"), "D:/");
    EXPECT_EQ(msysDriveToWindows("/d/"), "D:/");
}

TEST(WindowsHostPathsTests, otherSpellingsAreUnchanged) {
    EXPECT_EQ(msysDriveToWindows("/home/julian/a.cja"), "/home/julian/a.cja");
    EXPECT_EQ(msysDriveToWindows("D:/x/a.cja"), "D:/x/a.cja");
    EXPECT_EQ(msysDriveToWindows("C:\\x\\a.cja"), "C:\\x\\a.cja");
    EXPECT_EQ(msysDriveToWindows("build/a.cja"), "build/a.cja");
    EXPECT_EQ(msysDriveToWindows("/1/x"), "/1/x");
    EXPECT_EQ(msysDriveToWindows("//server/share/a.cja"), "//server/share/a.cja");
    EXPECT_EQ(msysDriveToWindows(""), "");
}

TEST(WindowsHostPathsTests, linkFallbacksListPresentMingwDriversGccFirst) {
    std::set<std::string> present = {"C:/msys64/mingw64/bin/gcc.exe",
                                     "C:/msys64/mingw64/bin/clang.exe"};
    auto exists = [&](const std::string& p) { return present.count(p) > 0; };
    std::vector<std::string> want = {"C:/msys64/mingw64/bin/gcc.exe",
                                     "C:/msys64/mingw64/bin/clang.exe"};
    EXPECT_EQ(msysLinkDriverFallbacks({"C:/msys64"}, exists), want);
}

TEST(WindowsHostPathsTests, linkFallbacksSkipAbsentDriversAndRoots) {
    std::set<std::string> present = {"D:/msys64/mingw64/bin/gcc.exe"};
    auto exists = [&](const std::string& p) { return present.count(p) > 0; };
    std::vector<std::string> want = {"D:/msys64/mingw64/bin/gcc.exe"};
    EXPECT_EQ(msysLinkDriverFallbacks({"C:/msys64", "D:/msys64"}, exists), want);
    EXPECT_TRUE(msysLinkDriverFallbacks({"C:/msys64"}, exists).empty());
}
