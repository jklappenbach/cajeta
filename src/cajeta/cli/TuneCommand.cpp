#include "TuneCommand.h"

#include <filesystem>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

namespace cajeta {

    int tuneReset(const std::string& dir, std::ostream& out) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            out << "cajeta tune: " << dir << " is not a tune store directory\n";
            return 1;
        }
        // The store is `<dir>/<device key>/<entries>`: every subdirectory is
        // one device's entries, and a file directly in `dir` is not the
        // store's.
        std::size_t removed = 0, devices = 0;
        for (const auto& dev : fs::directory_iterator(dir, ec)) {
            if (!dev.is_directory(ec)) continue;
            for (const auto& e : fs::directory_iterator(dev.path(), ec))
                if (e.is_regular_file(ec) && fs::remove(e.path(), ec)) ++removed;
            fs::remove(dev.path(), ec);
            if (ec) {
                out << "cajeta tune: could not remove " << dev.path().string()
                    << ": " << ec.message() << "\n";
                return 1;
            }
            ++devices;
        }
        out << "cajeta tune: removed " << removed << " entries for " << devices
            << " device(s) from " << dir << "\n";
        return 0;
    }

    int dispatchTune(int argc, const char* argv[]) {
        const char* usage =
            "usage: cajeta tune --reset <dir>\n"
            "  Empty the tune store at <dir>, the directory an application\n"
            "  passes to cajeta.xpu.Autotune and ShapeChoice. Every tuned value\n"
            "  and cached shape is measured again on next use.\n";
        if (argc == 3 && (std::string(argv[2]) == "--help"
                          || std::string(argv[2]) == "-h")) {
            std::cout << usage;
            return 0;
        }
        if (argc != 4 || std::string(argv[2]) != "--reset") {
            std::cerr << usage;
            return 2;
        }
        return tuneReset(argv[3], std::cout);
    }

}
