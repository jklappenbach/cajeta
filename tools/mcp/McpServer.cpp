#include "McpServer.h"

#include "cajeta/buildtool/Subprocess.h"
#include "cajeta/buildtool/repo/TarZstd.h"

#include <llvm/Support/Base64.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unistd.h>

namespace cajeta::mcp {

    namespace fs = std::filesystem;

    namespace {

        constexpr const char* kProtocolVersion = "2024-11-05";
        constexpr const char* kServerName = "cajeta-mcp";
        constexpr const char* kServerVersion = "0.1.0";

        Json makeResult(const Json& id, Json result) {
            Json r = Json::object();
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = std::move(result);
            return r;
        }

        Json makeError(const Json& id, int code, const std::string& message) {
            Json r = Json::object();
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            Json e = Json::object();
            e["code"] = code;
            e["message"] = message;
            r["error"] = std::move(e);
            return r;
        }

        Json textContent(const std::string& text, bool isError = false) {
            Json item = Json::object();
            item["type"] = "text";
            item["text"] = text;
            Json arr = Json::array();
            arr.push_back(std::move(item));
            Json body = Json::object();
            body["content"] = std::move(arr);
            if (isError) body["isError"] = true;
            return body;
        }

        std::optional<std::string> optStr(const Json& args, const std::string& k) {
            if (args.isObject() && args.has(k) && args.at(k).isString()) {
                return args.at(k).asString();
            }
            return std::nullopt;
        }

        std::string trimmed(const std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return "";
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        fs::path makeTempDir(const std::string& tag) {
            static std::atomic<unsigned> counter{0};
            fs::path dir = fs::temp_directory_path() / ("cajeta_mcp_" + tag + "_"
                + std::to_string((long long) ::getpid()) + "_"
                + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }

        bool decodeArchive(const std::string& b64,
                           std::vector<buildtool::TarEntry>* out, std::string* err) {
            std::vector<char> raw;
            if (llvm::Error e = llvm::decodeBase64(b64, raw)) {
                *err = "base64: " + llvm::toString(std::move(e));
                return false;
            }
            auto entries = buildtool::readTarZstd(std::string(raw.begin(), raw.end()));
            if (!entries) {
                *err = "tar.zstd: " + llvm::toString(entries.takeError());
                return false;
            }
            *out = std::move(*entries);
            return true;
        }

        void writeEntries(const fs::path& root,
                          const std::vector<buildtool::TarEntry>& entries) {
            for (const auto& e : entries) {
                fs::path target = root / e.name;
                std::error_code ec;
                fs::create_directories(target.parent_path(), ec);
                std::ofstream f(target, std::ios::binary);
                f.write(e.data.data(), (std::streamsize) e.data.size());
            }
        }

        Json diagnosticsFrom(const std::string& stderrText) {
            Json arr = Json::array();
            std::istringstream ss(stderrText);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty()) arr.push_back(line);
            }
            return arr;
        }

    } // namespace

    McpServer::McpServer() {
        compilerExePath_ = "cajeta";   // PATH lookup; overridden by dispatchMcp
        installDefaultRunner();
        registerTools();
    }

    void McpServer::installDefaultRunner() {
        cli_ = [this](const std::vector<std::string>& args) -> CliResult {
            CliResult out;
            buildtool::SubprocessOptions opt;
            opt.argv.push_back(compilerExePath_);
            for (const auto& a : args) opt.argv.push_back(a);
            opt.outData = &out.out;
            opt.errData = &out.err;
            buildtool::SubprocessResult res = runSubprocess(opt);
            out.launched = res.launched;
            out.exitCode = res.code();
            return out;
        };
    }

    void McpServer::registerTool(const std::string& name,
                                 const std::string& description, Json inputSchema,
                                 std::function<Json(const Json&)> call) {
        tools_[name] = Tool{name, description, std::move(inputSchema),
                            std::move(call)};
    }

    Json McpServer::runSkillTool(std::vector<std::string> args, const char* field) {
        CliResult r = cli_(args);
        if (!r.launched) {
            throw McpError{-32603, "failed to launch cajeta: " + r.err};
        }
        bool ok = false;
        Json parsed = Json::parse(trimmed(r.out), &ok);
        Json body = textContent(ok && parsed.isArray()
            ? std::to_string(parsed.size()) + " result(s)"
            : "no results");
        body[field] = (ok && parsed.isArray()) ? parsed : Json::array();
        return body;
    }

    void McpServer::registerTools() {
        Json obj = Json::object();
        obj["type"] = "object";

        registerTool("searchSkills",
            "Search skills by canonical name (fuzzy, typo-tolerant).",
            obj, [this](const Json& args) -> Json {
                if (!args.isObject() || !args.has("name")
                        || !args.at("name").isString()) {
                    throw McpError{-32602, "searchSkills requires string 'name'"};
                }
                std::vector<std::string> a = {"search-skill",
                    args.at("name").asString(), "--json"};
                if (auto v = optStr(args, "version")) { a.push_back("--version"); a.push_back(*v); }
                if (auto f = optStr(args, "from")) { a.push_back("--from"); a.push_back(*f); }
                if (args.has("exact") && args.at("exact").asBool()) a.push_back("--exact");
                return runSkillTool(a, "results");
            });

        registerTool("listSkills",
            "List the available skills catalog.",
            obj, [this](const Json& args) -> Json {
                std::vector<std::string> a = {"list-skills"};
                if (auto s = optStr(args, "scope")) a.push_back(*s);
                a.push_back("--json");
                return runSkillTool(a, "skills");
            });

        registerTool("getSkills",
            "Fetch skill payloads by cja-skill:// URI.",
            obj, [this](const Json& args) -> Json {
                if (!args.isObject() || !args.has("uris")
                        || !args.at("uris").isArray()) {
                    throw McpError{-32602, "getSkills requires array 'uris'"};
                }
                const Json& uriArr = args.at("uris");
                std::string joined;
                for (size_t i = 0; i < uriArr.size(); i++) {
                    if (!uriArr[i].isString()) continue;
                    if (!joined.empty()) joined += ",";
                    joined += uriArr[i].asString();
                }
                return runSkillTool({"get-skills", joined, "--json"}, "skills");
            });

        Json compileSchema = Json::object();
        compileSchema["type"] = "object";
        {
            Json props = Json::object();
            Json archiveProp = Json::object();
            archiveProp["type"] = "string";
            archiveProp["description"] = "base64-encoded tar.zstd source tree";
            props["archive"] = archiveProp;
            compileSchema["properties"] = props;
            Json req = Json::array();
            req.push_back(std::string("archive"));
            compileSchema["required"] = req;
        }
        registerTool("compile",
            "Compile a base64 tar.zstd source archive; return diagnostics + artifact.",
            compileSchema, [this](const Json& args) -> Json {
                if (!args.isObject() || !args.has("archive")
                        || !args.at("archive").isString()) {
                    throw McpError{-32602, "compile requires base64 'archive'"};
                }
                std::vector<buildtool::TarEntry> entries;
                std::string err;
                if (!decodeArchive(args.at("archive").asString(), &entries, &err)) {
                    throw McpError{-32602, "invalid archive: " + err};
                }
                if (entries.empty()) throw McpError{-32602, "archive is empty"};

                const std::string emit = optStr(args, "emit").value_or("cja");
                const std::string entry = optStr(args, "entryMethod").value_or("");
                fs::path src = makeTempDir("compile_src");
                fs::path out = makeTempDir("compile_out");
                writeEntries(src, entries);

                CliResult r = cli_({"--emit=" + emit, entry, src.string(),
                                    out.string()});

                std::optional<std::string> artifact;
                if (r.launched && r.exitCode == 0) {
                    std::error_code ec;
                    for (auto& de : fs::directory_iterator(out, ec)) {
                        if (de.path().extension() == ".cja") {
                            std::ifstream f(de.path(), std::ios::binary);
                            std::string bytes((std::istreambuf_iterator<char>(f)),
                                              std::istreambuf_iterator<char>());
                            artifact = llvm::encodeBase64(bytes);
                            break;
                        }
                    }
                }
                std::error_code ec;
                fs::remove_all(src, ec);
                fs::remove_all(out, ec);

                if (!r.launched) {
                    throw McpError{-32603, "failed to launch compiler: " + r.err};
                }
                Json body = textContent(r.exitCode == 0 ? "compiled" : r.err,
                                        /*isError=*/r.exitCode != 0);
                body["exitStatus"] = r.exitCode;
                body["diagnostics"] = diagnosticsFrom(r.err);
                if (artifact) body["artifact"] = *artifact;
                return body;
            });

        // jit_execute lands in U4; stub keeps tools/list complete.
        registerTool("jit_execute",
            "Compile and JIT-execute a source archive (process-per-execute).",
            obj, [](const Json&) -> Json {
                return textContent("not implemented", /*isError=*/true);
            });
    }

    Json McpServer::initializeResult() const {
        Json caps = Json::object();
        caps["tools"] = Json::object();
        Json info = Json::object();
        info["name"] = kServerName;
        info["version"] = kServerVersion;
        Json result = Json::object();
        result["protocolVersion"] = kProtocolVersion;
        result["capabilities"] = std::move(caps);
        result["serverInfo"] = std::move(info);
        return result;
    }

    Json McpServer::toolsList() const {
        Json arr = Json::array();
        for (const auto& [name, t] : tools_) {
            Json td = Json::object();
            td["name"] = t.name;
            td["description"] = t.description;
            td["inputSchema"] = t.inputSchema;
            arr.push_back(std::move(td));
        }
        Json body = Json::object();
        body["tools"] = std::move(arr);
        return body;
    }

    Json McpServer::callTool(const Json& params, const Json& id) {
        if (!params.isObject() || !params.has("name")) {
            return makeError(id, -32602, "tools/call requires a 'name'");
        }
        const std::string name = params.at("name").asString();
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            return makeError(id, -32602, "Unknown tool: " + name);
        }
        Json args = params.has("arguments") ? params.at("arguments")
                                            : Json::object();
        try {
            return makeResult(id, it->second.call(args));
        } catch (const McpError& e) {
            return makeError(id, e.code, e.message);
        }
    }

    Json McpServer::handle(const Json& request) {
        if (!request.isObject() || !request.has("method")) {
            return makeError(Json(), -32600, "Invalid Request");
        }
        const bool isNotification = !request.has("id");
        const Json id = isNotification ? Json() : request.at("id");
        const std::string method = request.at("method").asString();

        if (method == "initialize") {
            return makeResult(id, initializeResult());
        }
        if (method == "notifications/initialized" || method == "initialized") {
            return Json();
        }
        if (method == "tools/list") {
            return makeResult(id, toolsList());
        }
        if (method == "tools/call") {
            const Json params = request.has("params") ? request.at("params")
                                                      : Json::object();
            return callTool(params, id);
        }
        if (isNotification) return Json();
        return makeError(id, -32601, "Method not found: " + method);
    }

    int McpServer::run(std::istream& in, std::ostream& out) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            bool ok = false;
            Json request = Json::parse(line, &ok);
            if (!ok) {
                out << makeError(Json(), -32700, "Parse error").dump() << "\n";
                out.flush();
                continue;
            }
            Json response = handle(request);
            if (!response.isNull()) {
                out << response.dump() << "\n";
                out.flush();
            }
        }
        return 0;
    }

    int dispatchMcp(int argc, const char* argv[]) {
        McpServer server;
        // Locate the `cajeta` compiler: --cajeta=PATH > $CAJETA_BIN > "cajeta".
        std::string cajeta = "cajeta";
        if (const char* e = std::getenv("CAJETA_BIN"); e && *e) cajeta = e;
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            const std::string pfx = "--cajeta=";
            if (a.rfind(pfx, 0) == 0) cajeta = a.substr(pfx.size());
        }
        server.setCompilerExePath(std::move(cajeta));
        return server.run(std::cin, std::cout);
    }

} // namespace cajeta::mcp
