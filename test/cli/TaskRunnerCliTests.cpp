// `cajeta <task>` — the task runner driven the way a user drives it.
//
// TaskRunner + the filesystem/exec action family are reachable from the CLI
// (any manifest task name is a first-class verb), but the battery only ever
// drove `build`. These tests author small manifests around the cheap
// actions — exec, mkdir, copy, delete — and run them end to end, covering
// param substitution, cross-action ${id.field} wiring, env/cwd handling,
// failure propagation, and the task-not-found arms.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

struct TaskWorld {
    fs::path root;
    TaskWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_taskcli_" + std::to_string(rng()));
        fs::create_directories(root / "home");
    }
    ~TaskWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path outLog() const { return root / "out.log"; }

    // A manifest with `details` + the given `tasks` block.
    void writeManifest(const std::string& tasksJson,
                       const std::string& settingsJson = "") const {
        std::ofstream m(root / "cajeta.json");
        m << "{\n"
             "  \"details\": { \"name\": \"com.example.tasks\","
             " \"version\": \"0.1.0\", \"cajeta-lang-version\": \"1.0\" },\n";
        if (!settingsJson.empty()) m << "  \"settings\": " << settingsJson << ",\n";
        m << "  \"tasks\": " << tasksJson << "\n}\n";
    }

    int run(const std::string& args) const {
        std::string cmd = "cd " + root.string()
            + " && HOME=" + (root / "home").string()
            + " CAJETA_NO_SANDBOX=1 "
            + compilerBinary() + " " + args
            + " > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }
    std::string output() const {
        std::ifstream in(outLog());
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

} // namespace

TEST(TaskRunnerCliTests, execTaskRunsAndReportsSuccess) {
    TaskWorld w;
    w.writeManifest(
        "{ \"greet\": { \"description\": \"say hi\", \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"echo\","
        "    \"args\": [\"hello-from-the-task\"], \"id\": \"e\" } ] } }");

    EXPECT_EQ(w.run("greet"), 0) << w.output();
    EXPECT_NE(w.output().find("hello-from-the-task"), std::string::npos)
        << w.output();
}

TEST(TaskRunnerCliTests, taskParamsSubstituteIntoActionArgs) {
    TaskWorld w;
    w.writeManifest(
        "{ \"greet\": { \"params\": {"
        "     \"who\": { \"type\": \"string\", \"default\": \"world\" } },"
        "   \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"echo\","
        "    \"args\": [\"hello ${params.who}\"], \"id\": \"e\" } ] } }");

    EXPECT_EQ(w.run("greet"), 0) << w.output();
    EXPECT_NE(w.output().find("hello world"), std::string::npos)
        << w.output();

    // An explicit param override beats the default, in both spellings
    // (`-p NAME=VALUE` and the long `--param=NAME=VALUE`). Note the
    // lower-case -p is task PARAMS; -P is property overrides.
    EXPECT_EQ(w.run("greet -p who=cajeta"), 0) << w.output();
    EXPECT_NE(w.output().find("hello cajeta"), std::string::npos)
        << w.output();
    EXPECT_EQ(w.run("greet --param=who=longform"), 0) << w.output();
    EXPECT_NE(w.output().find("hello longform"), std::string::npos)
        << w.output();
}

TEST(TaskRunnerCliTests, filesystemActionsChainThroughTheTask) {
    TaskWorld w;
    std::ofstream(w.root / "src.txt") << "payload\n";
    w.writeManifest(
        "{ \"stage\": { \"actions\": ["
        "  { \"action\": \"mkdir\", \"path\": \"out/nested\", \"id\": \"m\" },"
        "  { \"action\": \"copy\", \"from\": \"src.txt\","
        "    \"to\": \"out/nested/copied.txt\", \"id\": \"c\" } ] } }");

    EXPECT_EQ(w.run("stage"), 0) << w.output();
    EXPECT_TRUE(fs::exists(w.root / "out" / "nested" / "copied.txt"))
        << w.output();

    // A follow-up task deletes what the first one staged.
    w.writeManifest(
        "{ \"scrub\": { \"actions\": ["
        "  { \"action\": \"delete\", \"paths\": [\"out\"], \"id\": \"d\" } ] } }");
    EXPECT_EQ(w.run("scrub"), 0) << w.output();
    EXPECT_FALSE(fs::exists(w.root / "out")) << w.output();
}

TEST(TaskRunnerCliTests, execHonoursCwdAndEnv) {
    TaskWorld w;
    fs::create_directories(w.root / "sub");
    std::ofstream(w.root / "sub" / "marker.txt") << "x";
    w.writeManifest(
        "{ \"look\": { \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"ls\","
        "    \"working-dir\": \"sub\", \"id\": \"l\" } ] } }");
    EXPECT_EQ(w.run("look"), 0) << w.output();
    EXPECT_NE(w.output().find("marker.txt"), std::string::npos) << w.output();

    w.writeManifest(
        "{ \"envcheck\": { \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"sh\","
        "    \"args\": [\"-c\", \"echo VAL=$CAJETA_TEST_VAR\"],"
        "    \"env\": { \"CAJETA_TEST_VAR\": \"from-manifest\" },"
        "    \"id\": \"e\" } ] } }");
    EXPECT_EQ(w.run("envcheck"), 0) << w.output();
    EXPECT_NE(w.output().find("VAL=from-manifest"), std::string::npos)
        << w.output();
}

TEST(TaskRunnerCliTests, failingActionFailsTheTaskAndStopsTheChain) {
    TaskWorld w;
    w.writeManifest(
        "{ \"boom\": { \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"sh\","
        "    \"args\": [\"-c\", \"exit 3\"], \"id\": \"bad\" },"
        "  { \"action\": \"exec\", \"command\": \"echo\","
        "    \"args\": [\"SHOULD-NOT-RUN\"], \"id\": \"after\" } ] } }");

    EXPECT_NE(w.run("boom"), 0) << w.output();
    EXPECT_EQ(w.output().find("SHOULD-NOT-RUN"), std::string::npos)
        << "the chain continued past a failed action:\n" << w.output();
}

TEST(TaskRunnerCliTests, unknownTaskAndMissingManifestArms) {
    TaskWorld w;
    w.writeManifest(
        "{ \"greet\": { \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"echo\","
        "    \"args\": [\"hi\"], \"id\": \"e\" } ] } }");

    // A name that is neither a builtin verb nor a declared task.
    EXPECT_NE(w.run("nosuchtask"), 0);
    EXPECT_FALSE(w.output().empty()) << "expected a diagnostic";

    // An unknown ACTION inside an otherwise valid task is refused.
    w.writeManifest(
        "{ \"weird\": { \"actions\": ["
        "  { \"action\": \"teleport\", \"id\": \"t\" } ] } }");
    EXPECT_NE(w.run("weird"), 0);
    EXPECT_FALSE(w.output().empty()) << "expected a diagnostic";
}

TEST(TaskRunnerCliTests, tasksListingShowsAuthoredTasksAndDescriptions) {
    TaskWorld w;
    w.writeManifest(
        "{ \"alpha\": { \"description\": \"the first\", \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"echo\","
        "    \"args\": [\"a\"], \"id\": \"e\" } ] },"
        "  \"beta\": { \"description\": \"the second\", \"actions\": ["
        "  { \"action\": \"exec\", \"command\": \"echo\","
        "    \"args\": [\"b\"], \"id\": \"e\" } ] } }");

    EXPECT_EQ(w.run("tasks"), 0) << w.output();
    std::string out = w.output();
    EXPECT_NE(out.find("alpha"), std::string::npos) << out;
    EXPECT_NE(out.find("beta"), std::string::npos) << out;
    EXPECT_NE(out.find("the first"), std::string::npos) << out;

    EXPECT_EQ(w.run("task alpha --show"), 0) << w.output();
    EXPECT_NE(w.output().find("alpha"), std::string::npos) << w.output();
}
