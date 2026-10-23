#include "app/Paths.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <thread>
#include <filesystem>

extern char** environ;  // POSIX; must be declared at global scope

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

static std::vector<fs::path> dataDirs() {
    const fs::path exe = exeDir();
    return {
        fs::path(configDir()),                               // user override
        exe / "data",                                        // run from build dir with data copied
        exe / ".." / "share" / "msfs-linuxrebind" / "data",  // installed
#ifdef LINUXREBIND_SOURCE_DATA_DIR
        fs::path(LINUXREBIND_SOURCE_DATA_DIR),               // dev build
#endif
        fs::current_path() / "data",
    };
}

std::string findDataFile(const std::string& name) {
    for (const auto& dir : dataDirs()) {
        const fs::path c = dir / name;
        std::error_code ec;
        if (fs::exists(c, ec)) return fs::weakly_canonical(c, ec).string();
    }
    return {};
}

std::vector<std::string> listDataFiles(const std::string& suffix) {
    std::vector<std::string> out;
    std::vector<std::string> names;
    for (const auto& dir : dataDirs()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (n.size() < suffix.size() || n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            if (std::find(names.begin(), names.end(), n) != names.end()) continue;  // earlier dir already provided it
            names.push_back(n);
            out.push_back(fs::weakly_canonical(e.path(), ec).string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool openDocsUrl(const std::string& url) {
    static const std::string prefix = "https://docs.flybywiresim.com/";
    if (url.compare(0, prefix.size(), prefix) != 0 || url.size() > 300) return false;
    for (char c : url.substr(prefix.size()))
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '-' || c == '_' || c == '.')) return false;
    char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(url.c_str()), nullptr};
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) != 0) return false;
    // do not wait: xdg-open may stay around while the browser starts; reap it in the background
    std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    return true;
}

}  // namespace lr::app
