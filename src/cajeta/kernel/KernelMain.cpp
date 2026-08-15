#include "cajeta/kernel/KernelMain.h"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "cajeta/kernel/JupyterMessage.h"
#include "cajeta/kernel/ZmqTransport.h"

namespace cajeta::kernel {

    namespace {

        KernelTransport* g_transport = nullptr;

        void onSignal(int) {
            // Async-signal-safe enough: stop() sets an atomic and closes a
            // queue. No allocation, no locking beyond a mutex the loop holds
            // only briefly.
            if (g_transport) g_transport->stop();
        }

        bool matchOption(const std::string& arg, const char* name,
                         std::string* value) {
            std::string prefix = std::string("--") + name + "=";
            if (arg.compare(0, prefix.size(), prefix) == 0) {
                *value = arg.substr(prefix.size());
                return true;
            }
            return false;
        }

        std::filesystem::path runtimeDir() {
            // Where Jupyter itself looks for connection files, so a
            // hand-started kernel is discoverable by `jupyter console
            // --existing`.
            if (const char* runtime = std::getenv("JUPYTER_RUNTIME_DIR")) {
                return std::filesystem::path(runtime);
            }
            if (const char* home = std::getenv("HOME")) {
                return std::filesystem::path(home) / ".local" / "share"
                     / "jupyter" / "runtime";
            }
            return std::filesystem::temp_directory_path();
        }

    }  // namespace

    std::string kernelSpecJson(const std::string& executable) {
        dap::Json argv = dap::Json::array();
        argv.push_back(executable);
        argv.push_back("kernel");
        argv.push_back("-f");
        // Jupyter substitutes the real path for this token when it launches
        // the kernel. It is a literal, not a placeholder we fill in.
        argv.push_back("{connection_file}");

        dap::Json spec = dap::Json::object();
        spec["argv"] = std::move(argv);
        spec["display_name"] = "Cajeta";
        spec["language"] = "cajeta";
        spec["interrupt_mode"] = "message";
        return spec.dump();
    }

    std::string installKernelSpec(const std::string& executable, bool force,
                                  std::string* error) {
        std::filesystem::path base;
        if (const char* data = std::getenv("JUPYTER_DATA_DIR")) {
            base = std::filesystem::path(data);
        } else if (const char* home = std::getenv("HOME")) {
#ifdef __APPLE__
            base = std::filesystem::path(home) / "Library" / "Jupyter";
#else
            base = std::filesystem::path(home) / ".local" / "share" / "jupyter";
#endif
        } else if (const char* appdata = std::getenv("APPDATA")) {
            base = std::filesystem::path(appdata) / "jupyter";
        } else {
            if (error) *error = "cannot locate a Jupyter data directory "
                                "(set JUPYTER_DATA_DIR)";
            return std::string();
        }

        std::filesystem::path dir = base / "kernels" / "cajeta";
        std::filesystem::path file = dir / "kernel.json";
        if (std::filesystem::exists(file) && !force) {
            if (error) {
                *error = "kernelspec already installed at " + file.string()
                       + " (pass --force to replace it)";
            }
            return std::string();
        }

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            if (error) *error = "cannot create " + dir.string() + ": " + ec.message();
            return std::string();
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + file.string();
            return std::string();
        }
        out << kernelSpecJson(executable) << '\n';
        return file.string();
    }

    int dispatchKernel(int argc, const char* argv[]) {
        std::string connectionFile;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (matchOption(arg, "connection-file", &connectionFile)) continue;
            // Jupyter's kernelspec launches with `-f <path>`; supporting only
            // the long form would mean no frontend could start us.
            if ((arg == "-f" || arg == "--connection-file") && i + 1 < argc) {
                connectionFile = argv[++i];
                continue;
            }
            if (arg == "--help" || arg == "-h") {
                std::cout <<
                    "usage: cajeta kernel [-f <connection-file>]\n"
                    "\n"
                    "Runs a Jupyter v5.3 kernel over ZeroMQ. Started by a\n"
                    "frontend from its kernelspec; with no connection file a\n"
                    "fresh one is generated and printed for manual attach\n"
                    "(`jupyter console --existing <file>`).\n"
                    "\n"
                    "Install the kernelspec with `cajeta init --kernel`.\n";
                return 0;
            }
            std::cerr << "cajeta kernel: unknown option: " << arg << "\n";
            return 2;
        }

        ConnectionInfo info;
        bool generated = connectionFile.empty();
        std::string error;
        if (!generated) {
            if (!ConnectionInfo::load(connectionFile, &info, &error)) {
                std::cerr << "cajeta kernel: " << error << "\n";
                return 1;
            }
        } else {
            // Every port left at 0 — bind() asks the OS and writes back what
            // it got, so the file we print names ports that are actually
            // listening rather than ones we guessed were free.
            info.key = newUuid();
        }

        KernelTransport transport;
        if (!transport.bind(&info, &error)) {
            std::cerr << "cajeta kernel: " << error << "\n";
            return 1;
        }

        if (generated) {
            std::filesystem::path dir = runtimeDir();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::path path =
                dir / ("kernel-cajeta-" + std::to_string(
                    static_cast<long long>(info.shellPort)) + ".json");
            if (!info.write(path.string(), &error)) {
                std::cerr << "cajeta kernel: " << error << "\n";
                return 1;
            }
            connectionFile = path.string();
            std::cout << "cajeta kernel listening; connection file:\n"
                      << connectionFile << "\n"
                      << "attach with: jupyter console --existing "
                      << connectionFile << "\n";
            std::cout.flush();
        }

        g_transport = &transport;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        int code = transport.run();

        g_transport = nullptr;
        // A connection file WE created is ours to remove; one the frontend
        // wrote belongs to the frontend, which cleans up its own runtime dir.
        if (generated && !connectionFile.empty()) {
            std::error_code ec;
            std::filesystem::remove(connectionFile, ec);
        }
        return code;
    }

}  // namespace cajeta::kernel
