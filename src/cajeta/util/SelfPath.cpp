#include "cajeta/util/SelfPath.h"

#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <cstdint>
#else
#  include <unistd.h>
#endif

namespace cajeta::util {

    std::string runningExecutablePath() {
        char buf[4096];
#if defined(_WIN32)
        DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            return std::string(buf, n);
        }
#elif defined(__APPLE__)
        std::uint32_t size = sizeof(buf);
        if (::_NSGetExecutablePath(buf, &size) == 0) {
            return std::string(buf);
        }
#else
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            return std::string(buf);
        }
#endif
        return "";
    }

} // namespace cajeta::util
