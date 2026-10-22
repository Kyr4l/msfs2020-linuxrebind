// Config compatibility test: a schema-1 file (written by 0.1.x) must load without loss, saving it must
// keep a pristine backup, and the upgraded file must round-trip. Optional argv[1]: a real config to check
// (it is copied first, never modified).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app/Profile.hpp"

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } \
    } while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

using namespace lr::app;

static void sameBindings(const Profile& a, const Profile& b) {
    CHECK(a.bindings.size() == b.bindings.size());
    for (size_t i = 0; i < a.bindings.size() && i < b.bindings.size(); ++i) {
        const Binding &x = a.bindings[i], &y = b.bindings[i];
        CHECK(x.label == y.label);
        CHECK(x.input == y.input);
        CHECK(x.enabled == y.enabled && x.repeat == y.repeat && x.repeatDelayMs == y.repeatDelayMs && x.repeatIntervalMs == y.repeatIntervalMs);
        CHECK(x.onPress.size() == y.onPress.size() && x.onRelease.size() == y.onRelease.size());
        for (size_t k = 0; k < x.onPress.size() && k < y.onPress.size(); ++k)
            CHECK(x.onPress[k].kind == y.onPress[k].kind && x.onPress[k].name == y.onPress[k].name && x.onPress[k].value == y.onPress[k].value);
        for (size_t k = 0; k < x.onRelease.size() && k < y.onRelease.size(); ++k)
            CHECK(x.onRelease[k].kind == y.onRelease[k].kind && x.onRelease[k].name == y.onRelease[k].name);
    }
}

static void checkUpgrade(const fs::path& original, const fs::path& work, bool verifyFixtureValues) {
    fs::remove(work);
    fs::remove(work.string() + ".v1.bak");
    fs::copy_file(original, work);
    const std::string before = slurp(work);

    Settings s;
    std::string err;
    CHECK(loadSettings(work.string(), s, err));
    CHECK(s.loadedVersion == 1);
    CHECK(!s.profiles.empty());
    for (const auto& p : s.profiles) CHECK(p.aircraft == "A32NX");  // old profiles are A32NX profiles

    if (verifyFixtureValues) {
        CHECK(s.profiles.size() == 1 && s.profiles[0].bindings.size() == 2);
        const Binding& b0 = s.profiles[0].bindings[0];
        CHECK(b0.label == "APU master" && b0.onPress.size() == 1 && b0.onPress[0].kind == ActionKind::LVarToggle);
        const Binding& b1 = s.profiles[0].bindings[1];
        CHECK(b1.input.kind == lr::input::InputKind::Hat && b1.input.dir == 1 && b1.repeat && !b1.enabled);
        CHECK(b1.repeatDelayMs == 300 && b1.repeatIntervalMs == 80 && b1.onRelease.size() == 1);
        CHECK(s.haptics.enabled && s.haptics.master > 0.79f && s.haptics.master < 0.81f);
        CHECK(s.haptics.rules.count("stall") && !s.haptics.rules["stall"].enabled);
        auto it = s.haptics.devices.find("4098:b970");
        CHECK(it != s.haptics.devices.end());
        if (it != s.haptics.devices.end()) CHECK(it->second.identity.left.device == 4 && it->second.identity.left.index == 14 && it->second.identity.right.device == 8 && it->second.identity.right.index == 16);
    }

    // save (the upgrade): backup must appear, byte-identical to what was on disk
    CHECK(saveSettings(work.string(), s, err));
    const std::string bak = work.string() + ".v1.bak";
    CHECK(fs::exists(bak));
    CHECK(slurp(bak) == before);

    Settings t;
    CHECK(loadSettings(work.string(), t, err));
    CHECK(t.loadedVersion == kConfigVersion);
    CHECK(t.profiles.size() == s.profiles.size());
    for (size_t i = 0; i < s.profiles.size() && i < t.profiles.size(); ++i) {
        CHECK(t.profiles[i].name == s.profiles[i].name && t.profiles[i].aircraft == s.profiles[i].aircraft);
        sameBindings(s.profiles[i], t.profiles[i]);
    }
    CHECK(t.activeProfile == s.activeProfile && t.host == s.host && t.port == s.port && t.autoConnect == s.autoConnect);

    // second save must not overwrite the backup with the new format
    t.profiles[0].aircraft = "A380X";
    CHECK(saveSettings(work.string(), t, err));
    CHECK(slurp(bak) == before);
    Settings u;
    CHECK(loadSettings(work.string(), u, err));
    CHECK(u.profiles[0].aircraft == "A380X");
    sameBindings(s.profiles[0], u.profiles[0]);
}

int main(int argc, char** argv) {
    const fs::path tmp = fs::temp_directory_path() / "msfs-linuxrebind-config-test";
    fs::create_directories(tmp);

    std::printf("fixture: schema 1 -> %d upgrade\n", kConfigVersion);
    checkUpgrade(fs::path(LINUXREBIND_TEST_FIXTURES) / "config_v1.json", tmp / "fixture.json", true);

    if (argc > 1) {
        std::printf("real config copy: %s\n", argv[1]);
        checkUpgrade(argv[1], tmp / "real.json", false);
    }

    std::printf("future schema + unknown fields load without error\n");
    {
        const fs::path f = tmp / "future.json";
        std::ofstream(f) << R"({"version": 99, "mystery": [1,2,3], "profiles": [{"name": "X", "aircraft": "B787", "extra": true, "bindings": []}]})";
        Settings s;
        std::string err;
        CHECK(loadSettings(f.string(), s, err));
        CHECK(s.profiles.size() == 1 && s.profiles[0].aircraft == "B787");
    }

    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
