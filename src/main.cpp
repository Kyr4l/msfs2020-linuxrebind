// msfs-linuxrebind: bind HOTAS buttons to MSFS 2020 cockpit events from Linux.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app/Catalog.hpp"
#include "app/Engine.hpp"
#include "app/Paths.hpp"
#include "app/Profile.hpp"
#include "haptics/Haptics.hpp"
#include "haptics/WinwingHid.hpp"
#include "input/Evdev.hpp"
#include "sim/SimBridge.hpp"
#include "ui/AppIcon.hpp"
#include "ui/Ui.hpp"
#include "util/Log.hpp"

namespace {
volatile std::sig_atomic_t g_signalled = 0;
void onSignal(int) { g_signalled = 1; }

void usage() {
    std::puts(
        "msfs-linuxrebind [options]\n"
        "  --wayland | --x11      force the GLFW platform (default: Wayland if available, else X11)\n"
        "  --headless             no window; run the active profile until SIGINT\n"
        "  --host HOST            SimConnect host (default from config, 127.0.0.1)\n"
        "  --port PORT            SimConnect TCP port (default from config, 500)\n"
        "  --profile NAME         activate this profile for the session\n"
        "  --config FILE          config file (default $XDG_CONFIG_HOME/msfs-linuxrebind/config.json)\n"
        "  --catalog FILE         extra action catalog JSON (all data/*_catalog.json are loaded automatically)\n"
        "  --tab NAME             start on a tab: Bindings, Devices, Catalog, Vibration, Log, Settings\n"
        "  --quiet                do not echo the log to stderr\n"
        "  --probe-winwing        list Winwing HID devices and the controllers behind them, then exit\n"
        "  --motor-sweep [DEV]    buzz every SET_LEDX index (0..31) on each controller (or only device byte DEV, e.g. 0x04)\n"
        "                         for 0.4 s each, printing which one is being driven; tell the tool which step you felt.\n"
        "                         Lights are driven too (volatile): replug the device afterwards to restore its light profile\n"

        "  --dump-winwing-config  read-only dump of every controller's flash config words to stdout (baseline for diffs)\n"
        "  --fire KIND:NAME[=V]   connect, fire one action and exit. KIND: sim_event, h_event, lvar_set, lvar_toggle, calc_code\n"
        "                         e.g. --fire lvar_toggle:A32NX_OVHD_APU_MASTER_SW_PB_IS_ON   --fire sim_event:A32NX.FCU_AP_1_PUSH\n"
        "  --read NAME            connect, print the value of L:NAME (needs MobiFlight) and exit\n"

        "  --version, --help");
}

void glfwErrorCallback(int code, const char* desc) { LOG_ERROR("GLFW error %d: %s", code, desc); }
}  // namespace

int main(int argc, char** argv) {
    using namespace lr;
    int platform = 0;
    bool headless = false;
    bool probeWinwing = false;
    bool motorSweep = false;
    bool dumpConfig = false;
    std::vector<std::string> fireSpecs, readNames;
    int sweepDevice = -1;
    std::string hostOverride, profileOverride, configPath, catalogPath, startTab;
    int portOverride = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; else { usage(); std::exit(2); } };
        if (a == "--wayland") platform = GLFW_PLATFORM_WAYLAND;
        else if (a == "--x11") platform = GLFW_PLATFORM_X11;
        else if (a == "--headless") headless = true;
        else if (a == "--probe-winwing") probeWinwing = true;
        else if (a == "--dump-winwing-config") dumpConfig = true;
        else if (a == "--fire") { std::string v; next(v); fireSpecs.push_back(v); }
        else if (a == "--read") { std::string v; next(v); readNames.push_back(v); }
        else if (a == "--motor-sweep") {
            motorSweep = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') sweepDevice = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        }
        else if (a == "--host") next(hostOverride);
        else if (a == "--port") { std::string p; next(p); portOverride = std::atoi(p.c_str()); }
        else if (a == "--profile") next(profileOverride);
        else if (a == "--config") next(configPath);
        else if (a == "--catalog") next(catalogPath);
        else if (a == "--tab") next(startTab);
        else if (a == "--quiet" || a == "-q") Log::instance().echoToStderr = false;
        else if (a == "--version") { std::printf("msfs-linuxrebind %s\n", LINUXREBIND_VERSION); return 0; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (dumpConfig) {
        Log::instance().echoToStderr = false;
        haptics::WinwingHidManager ww;
        ww.rescan();
        for (const auto& d : ww.devices()) {
            std::printf("# %s pid %04x %s\n", d.name.c_str(), d.pid, d.path.c_str());
            for (const auto& l : ww.dumpConfig(d.key)) std::printf("%s\n", l.c_str());
        }
        return 0;
    }

    if (motorSweep) {
        Log::instance().echoToStderr = false;
        haptics::WinwingHidManager ww;
        ww.rescan();
        const auto devs = ww.devices();
        if (devs.empty()) { std::printf("no Winwing HID device found\n"); return 1; }
        std::printf("Motor sweep: each step drives one SET_LEDX index at value 150 for 0.4 s. Keep a hand on the grips.\n"
                    "Note: LED indices are driven too. That state is volatile; replug the device afterwards to restore its lights.\n");
        for (const auto& d : devs) {
            if (!d.open) { std::printf("%s: not open (%s)\n", d.name.c_str(), d.error.c_str()); continue; }
            for (const auto& c : d.controllers) {
                if (sweepDevice >= 0 && c.device != sweepDevice) continue;
                std::printf("\n== %s  controller device 0x%02x family 0x%02x \"%s\"\n", d.name.c_str(), c.device, c.family, c.name.c_str());
                for (int idx = 0; idx < 32; ++idx) {
                    std::printf("   index %2d (0x%02x) ... ", idx, idx);
                    std::fflush(stdout);
                    ww.setLevel(d.key, c.device, c.family, idx, 150);
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                    ww.setLevel(d.key, c.device, c.family, idx, 0);
                    std::printf("done\n");
                    std::this_thread::sleep_for(std::chrono::milliseconds(700));
                    if (g_signalled) return 0;
                }
            }
        }
        std::printf("\nSweep finished. Report the controller/index that vibrated (and any LED that changed).\n");
        return 0;
    }

    if (probeWinwing) {
        haptics::WinwingHidManager ww;
        ww.rescan();
        const auto devs = ww.devices();
        if (devs.empty()) std::printf("no Winwing HID device found\n");
        for (const auto& d : devs) {
            std::printf("%s  pid %04x  %s  %s\n", d.name.c_str(), d.pid, d.path.c_str(), d.open ? "open" : ("not open: " + d.error).c_str());
            if (!d.open) continue;
            for (const auto& c : d.controllers)
                std::printf("  controller device 0x%02x family 0x%02x type 0x%x  \"%s\"\n", c.device, c.family, c.typeId, c.name.c_str());
            if (d.identity.known())
                std::printf("  motors: family 0x%02x  left device 0x%02x index %d  right device 0x%02x index %d  (%s)\n", d.identity.family,
                            d.identity.left.device, d.identity.left.index, d.identity.right.device, d.identity.right.index,
                            d.identityIsDefault ? "built-in default / auto" : "from config");
            else
                std::printf("  motors: address unknown\n");
        }
        return 0;
    }

    sim::SimBridge bridge;
    app::Engine engine(bridge);
    input::InputManager input;
    haptics::WinwingHidManager winwing;
    haptics::HapticsEngine hapticsEngine(bridge, winwing);
    app::AppContext ctx(bridge, engine, input, hapticsEngine, winwing);
    ctx.configPath = configPath.empty() ? app::configFile() : configPath;
    ctx.startTab = startTab;

    {
        std::string err;
        if (!app::loadSettings(ctx.configPath, ctx.settings, err)) {
            LOG_INFO("No config loaded (%s); starting with defaults", err.c_str());
            ctx.settings = app::Settings{};
            { app::Profile first; first.name = "A32NX"; first.aircraft = "A32NX"; ctx.settings.profiles.push_back(first); }
            ctx.settings.activeProfile = "A32NX";
            ctx.dirty = true;
        }
        ctx.settings.ensureActive();
        if (!hostOverride.empty()) ctx.settings.host = hostOverride;
        if (portOverride > 0) ctx.settings.port = portOverride;
        if (!profileOverride.empty()) {
            bool found = false;
            for (const auto& p : ctx.settings.profiles) found |= p.name == profileOverride;
            if (found) ctx.settings.activeProfile = profileOverride;
            else LOG_WARN("Profile \"%s\" not found in config", profileOverride.c_str());
        }
        LOG_INFO("Config: %s (profile \"%s\", %zu bindings)", ctx.configPath.c_str(), ctx.settings.activeProfile.c_str(), ctx.settings.ensureActive().bindings.size());
    }
    {
        std::vector<std::string> paths = app::listDataFiles("_catalog.json");
        if (!catalogPath.empty()) paths.push_back(catalogPath);  // --catalog adds (or replaces, same aircraft) one more
        for (const auto& path : paths) {
            app::Catalog cat;
            std::string err;
            if (!cat.load(path, err)) { LOG_WARN("Catalog %s not loaded: %s", path.c_str(), err.c_str()); continue; }
            bool replaced = false;
            for (auto& existing : ctx.catalogs)
                if (existing.aircraft == cat.aircraft) { existing = cat; replaced = true; }
            if (!replaced) ctx.catalogs.push_back(std::move(cat));
            LOG_INFO("Catalog %s: %zu entries from %s", ctx.catalogs.back().aircraft.c_str(), ctx.catalogs.back().entries.size(), path.c_str());
        }
        if (ctx.catalogs.empty()) LOG_WARN("No action catalog found (expected data/*_catalog.json)");
    }

    ctx.warnAxisRanges();
    bridge.setEndpoint(ctx.settings.host, static_cast<uint16_t>(ctx.settings.port));
    bridge.setAutoConnect(ctx.settings.autoConnect);
    bridge.start();
    ctx.pushProfileToEngine();
    hapticsEngine.setConfig(ctx.settings.haptics);  // Winwing discovery runs on the haptics thread
    hapticsEngine.start();
    input.start([&engine](const input::InputEvent& ev) { engine.onInput(ev); }, [&engine] { engine.tick(); });

    if (!fireSpecs.empty() || !readNames.empty()) {
        // One-shot scripting mode: wait for the sim (and the MobiFlight module when needed), act, exit.
        std::vector<std::string> watch;
        for (const auto& r : readNames) watch.push_back(r);
        bridge.watchLVars(watch);
        bool needModule = !readNames.empty();
        std::vector<app::Action> actions;
        for (const auto& spec : fireSpecs) {
            app::Action act;
            size_t colon = spec.find(':');
            std::string kind = colon == std::string::npos ? "sim_event" : spec.substr(0, colon);
            std::string rest = colon == std::string::npos ? spec : spec.substr(colon + 1);
            if (!app::actionKindFromName(kind, act.kind)) { std::fprintf(stderr, "unknown action kind %s\n", kind.c_str()); return 2; }
            size_t eq = rest.rfind('=');
            if (eq != std::string::npos && act.kind != app::ActionKind::CalcCode) { act.name = rest.substr(0, eq); act.value = std::atof(rest.c_str() + eq + 1); }
            else act.name = rest;
            needModule |= app::actionNeedsMobiFlight(act.kind);
            actions.push_back(act);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (;;) {
            auto st = bridge.status();
            if (st.state == sim::ConnState::Connected && (!needModule || st.mobiflight == sim::ModuleState::Detected)) break;
            if (std::chrono::steady_clock::now() > deadline || g_signalled) { std::fprintf(stderr, "simulator%s not available\n", needModule ? " / MobiFlight module" : ""); return 1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!actions.empty()) {
            engine.fire(actions, "cli");
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
        if (!readNames.empty()) {
            // give the module a moment to register and publish the values
            for (int i = 0; i < 40; ++i) {
                bool all = true;
                for (const auto& r : readNames) all &= bridge.lvarValue(r).has_value();
                if (all) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            for (const auto& r : readNames) {
                auto v = bridge.lvarValue(r);
                if (v) std::printf("L:%s = %g\n", r.c_str(), *v);
                else std::printf("L:%s = (no value received)\n", r.c_str());
            }
        }
        input.stop();
        hapticsEngine.stop();
        bridge.stop();
        return 0;
    }

    if (headless) {
        LOG_INFO("Headless mode: press Ctrl+C to quit");
        while (!g_signalled) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
        glfwSetErrorCallback(glfwErrorCallback);
        if (platform != 0) {
            if (!glfwPlatformSupported(platform)) {
                LOG_ERROR("Requested platform is not supported by this GLFW build");
                return 1;
            }
            glfwInitHint(GLFW_PLATFORM, platform);
        }
        glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_PREFER_LIBDECOR);
        if (!glfwInit()) {
            LOG_ERROR("glfwInit failed (no display? use --headless)");
            return 1;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "msfs-linuxrebind");
        glfwWindowHintString(GLFW_X11_CLASS_NAME, "msfs-linuxrebind");
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "msfs-linuxrebind");
        GLFWwindow* window = glfwCreateWindow(1280, 800, "MSFS Linux Rebind", nullptr, nullptr);
        if (!window) {
            LOG_ERROR("Window creation failed");
            glfwTerminate();
            return 1;
        }
        const int plat = glfwGetPlatform();
        if (plat == GLFW_PLATFORM_X11) {
            // Wayland has no per-window icon API (GLFW reports an error); compositors use the .desktop icon via the app id.
            GLFWimage icon{ui::kAppIconSize, ui::kAppIconSize, const_cast<unsigned char*>(ui::kAppIconRgba)};
            glfwSetWindowIcon(window, 1, &icon);
        }
        LOG_INFO("GLFW platform: %s", plat == GLFW_PLATFORM_WAYLAND ? "Wayland" : plat == GLFW_PLATFORM_X11 ? "X11" : "other");
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        static std::string iniPath = app::imguiIniFile();
        io.IniFilename = iniPath.c_str();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        float xs = 1.f, ys = 1.f;
        glfwGetWindowContentScale(window, &xs, &ys);
        ImGui::GetStyle().ScaleAllSizes(xs);
#if IMGUI_VERSION_NUM >= 19200
        ImGui::GetStyle().FontScaleDpi = xs;
#endif
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");

        app::Ui ui(ctx);
        while (!glfwWindowShouldClose(window) && !g_signalled && !ctx.quitRequested) {
            // The UI only needs to refresh a few times a second when idle; bindings run on their own threads.
            glfwWaitEventsTimeout(1.0 / 20.0);
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (ctx.dirty) ctx.save();
                continue;
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ui.draw();
            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }

    input.stop();
    hapticsEngine.stop();
    bridge.stop();
    if (ctx.dirty) ctx.save();
    return 0;
}
