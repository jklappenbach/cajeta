// The generated `main` calls an entry method with the trailing transfer word its ABI carries.
// Without it a `run(String[] args)` entry got one argument too few, and the release inliner
// segfaulted on the call (cajeta-llm 15.2.19.3.3.4). A static `main` keeps the plain C ABI.

#include <gtest/gtest.h>

#include <regex>
#include <string>
#include "AotProgram.h"

using namespace cajeta_test_aot;

namespace {

std::string entrySource(const std::string& name) {
    return "package app;\n"
           "public class Main {\n"
           "    public static int32 " + name + "(String[] args) {\n"
           "        System.stdout.println(\"args \" + args.count());\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

std::string mainBody(const Built& b) { return bodiesOf(dumpedIr(b), "@main("); }

}  // namespace

TEST(EntryTransferWord, aNamedEntryReceivesALendingTransferWord) {
    Workspace w;
    w.write("src/app/Main.cajeta", entrySource("run"));
    Built b = compile(w, "o0", "--emit=exe", "app.Main.run", "src");
    ASSERT_TRUE(b.ok) << b.log;
    std::regex call(R"re(call i32 @"app\.Main::run\(args:cajeta\.lang\.String\[\]\)"\(ptr %[0-9a-z.]+, i64 0\))re");
    EXPECT_TRUE(std::regex_search(mainBody(b), call)) << mainBody(b);
    EXPECT_EQ(run(b).out, "args 0\n");
}

TEST(EntryTransferWord, aReleaseNamedEntryBuildsAndRuns) {
    Workspace w;
    w.write("src/app/Main.cajeta", entrySource("run"));
    Built b = compile(w, "rel", "--emit=exe --release", "app.Main.run", "src");
    ASSERT_TRUE(b.ok) << b.log;
    EXPECT_EQ(run(b).out, "args 0\n");
}

TEST(EntryTransferWord, aStaticMainKeepsThePlainCall) {
    Workspace w;
    w.write("src/app/Main.cajeta", entrySource("main"));
    Built b = compile(w, "o0", "--emit=exe", "app.Main.main", "src");
    ASSERT_TRUE(b.ok) << b.log;
    std::regex call(R"re(call i32 @"app\.Main::main\(args:cajeta\.lang\.String\[\]\)"\(ptr %[0-9a-z.]+\))re");
    EXPECT_TRUE(std::regex_search(mainBody(b), call)) << mainBody(b);
    EXPECT_EQ(run(b).out, "args 0\n");
}
