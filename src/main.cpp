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
        "  --catalog FILE         action catalog JSON (default data/a32nx_catalog.json)\n"
        "  --quiet                do not echo the log to stderr\n"
        "  --probe-winwing        list Winwing HID devices and the controllers behind them, then exit\n"
        "  --motor-sweep [DEV]    buzz every SET_LEDX index (0..31) on each controller (or only device byte DEV, e.g. 0x04)\n"
        "                         for 0.4 s each, printing which one is being driven; tell the tool which step you felt.\n"
        "                         Lights are driven too (volatile): replug the device afterwards to restore its light profile\n"
        "  --dump-winwing-config  read-only dump of every controller's flash config words to stdout (baseline for diffs)\n"
        "  --led-sweep DEV [VAL]  light SET_LEDX indices 0..63 one at a time (1 s each, value VAL, default 255) on controller\n"
        "                         DEV (e.g. 0x40); watch which button LED lights and in what colour. Volatile; replug restores\n"
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
    bool ledSweep = false;
    int ledSweepDevice = -1, ledSweepValue = 255;
    int sweepDevice = -1;
    std::string hostOverride, profileOverride, configPath, catalogPath;
    int portOverride = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; else { usage(); std::exit(2); } };
        if (a == "--wayland") platform = GLFW_PLATFORM_WAYLAND;
        else if (a == "--x11") platform = GLFW_PLATFORM_X11;
        else if (a == "--headless") headless = true;
        else if (a == "--probe-winwing") probeWinwing = true;
        else if (a == "--dump-winwing-config") dumpConfig = true;
        else if (a == "--led-sweep") {
            ledSweep = true;
            std::string dev; next(dev);
            ledSweepDevice = static_cast<int>(std::strtol(dev.c_str(), nullptr, 0));
            if (i + 1 < argc && argv[i + 1][0] != '-') ledSweepValue = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        }
        else if (a == "--motor-sweep") {
            motorSweep = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') sweepDevice = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        }
        else if (a == "--host") next(hostOverride);
        else if (a == "--port") { std::string p; next(p); portOverride = std::atoi(p.c_str()); }
        else if (a == "--profile") next(profileOverride);
        else if (a == "--config") next(configPath);
        else if (a == "--catalog") next(catalogPath);
        else if (a == "--quiet" || a == "-q") Log::instance().echoToStderr = false;
        else if (a == "--version") { std::printf("msfs-linuxrebind %s\n", LINUXREBIND_VERSION); return 0; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (ledSweep) {
        Log::instance().echoToStderr = false;
        haptics::WinwingHidManager ww;
        ww.rescan();
        for (const auto& d : ww.devices()) {
            if (!d.open) continue;
            const haptics::ControllerInfo* ctl = nullptr;
            for (const auto& c : d.controllers) if (c.device == ledSweepDevice) ctl = &c;
            if (!ctl) { std::printf("%s: no controller with device byte 0x%02x (see --probe-winwing)\n", d.name.c_str(), ledSweepDevice); continue; }
            std::printf("LED sweep on %s controller 0x%02x \"%s\": each index is set to %d for 1 s, then back to 0.\n"
                        "Note which index lights which button and in what colour. Ctrl+C stops. Replug restores the light profile.\n",
                        d.name.c_str(), ctl->device, ctl->name.c_str(), ledSweepValue);
            for (int idx = 0; idx < 64 && !g_signalled; ++idx) {
                if ((ctl->device == d.identity.left.device && idx == d.identity.left.index) ||
                    (ctl->device == d.identity.right.device && idx == d.identity.right.index)) {
                    std::printf("   index %2d (0x%02x): motor, skipped\n", idx, idx);
                    continue;
                }
                std::printf("   index %2d (0x%02x) ... ", idx, idx);
                std::fflush(stdout);
                ww.setLed(d.key, ctl->device, ctl->family, idx, ledSweepValue);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                ww.setLed(d.key, ctl->device, ctl->family, idx, 0);
                std::printf("off\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }
        return 0;
    }

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
                    ww.setLed(d.key, c.device, c.family, idx, 150);
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                    ww.setLed(d.key, c.device, c.family, idx, 0);
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

    {
        std::string err;
        if (!app::loadSettings(ctx.configPath, ctx.settings, err)) {
            LOG_INFO("No config loaded (%s); starting with defaults", err.c_str());
            ctx.settings = app::Settings{};
            ctx.settings.profiles.push_back(app::Profile{"A32NX", {}});
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
        std::string path = catalogPath.empty() ? app::findDataFile("a32nx_catalog.json") : catalogPath;
        std::string err;
        if (path.empty() || !ctx.catalog.load(path, err)) LOG_WARN("Catalog not loaded: %s", path.empty() ? "data/a32nx_catalog.json not found" : err.c_str());
        else LOG_INFO("Catalog: %zu entries from %s", ctx.catalog.entries.size(), path.c_str());
    }

    bridge.setEndpoint(ctx.settings.host, static_cast<uint16_t>(ctx.settings.port));
    bridge.setAutoConnect(ctx.settings.autoConnect);
    bridge.start();
    ctx.pushProfileToEngine();
    hapticsEngine.setConfig(ctx.settings.haptics);  // Winwing discovery runs on the haptics thread
    hapticsEngine.start();
    input.start([&engine](const input::InputEvent& ev) { engine.onInput(ev); }, [&engine] { engine.tick(); });

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
        {
            // X11 window icon; on Wayland the compositor takes the icon from the .desktop file via the app id.
            GLFWimage icon{ui::kAppIconSize, ui::kAppIconSize, const_cast<unsigned char*>(ui::kAppIconRgba)};
            glfwSetWindowIcon(window, 1, &icon);
        }
        const int plat = glfwGetPlatform();
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
