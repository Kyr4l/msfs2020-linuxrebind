#include "app/Paths.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace lr::app {

namespace fs = std::filesystem;

std::string configDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home) base = fs::path(home) / ".config";
    else base = ".";
    return (base / "msfs-linuxrebind").string();
}

std::string configFile() { return (fs::path(configDir()) / "config.json").string(); }
std::string imguiIniFile() { return (fs::path(configDir()) / "imgui.ini").string(); }

static fs::path exeDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return fs::current_path();
    buf[n] = 0;
    return fs::path(buf).parent_path();
}

std::string findDataFile(const std::string& name) {
    const fs::path exe = exeDir();
    const fs::path candidates[] = {
        fs::path(configDir()) / name,                               // user override
        exe / "data" / name,                                        // run from build dir with data copied
        exe / ".." / "share" / "msfs-linuxrebind" / "data" / name,  // installed
#ifdef LINUXREBIND_SOURCE_DATA_DIR
        fs::path(LINUXREBIND_SOURCE_DATA_DIR) / name,               // dev build
#endif
        fs::current_path() / "data" / name,
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return fs::weakly_canonical(c, ec).string();
    }
    return {};
}

}  // namespace lr::app
