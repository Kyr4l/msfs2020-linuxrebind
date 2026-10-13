#pragma once
// Dear ImGui front-end. Everything here runs on the main (GLFW) thread.
#include <memory>
#include <string>
#include <vector>

#include "app/Catalog.hpp"
#include "app/Engine.hpp"
#include "app/Profile.hpp"
#include "haptics/Haptics.hpp"
#include "haptics/WinwingHid.hpp"
#include "input/Evdev.hpp"
#include "sim/SimBridge.hpp"

namespace lr::app {

struct AppContext {
    Settings settings;
    std::string configPath;
    Catalog catalog;
    sim::SimBridge& bridge;
    Engine& engine;
    input::InputManager& input;
    haptics::HapticsEngine& haptics;
    haptics::WinwingHidManager& winwing;
    bool dirty = false;
    bool quitRequested = false;
    std::string lastSaveError;

    AppContext(sim::SimBridge& b, Engine& e, input::InputManager& i, haptics::HapticsEngine& h, haptics::WinwingHidManager& w)
        : bridge(b), engine(e), input(i), haptics(h), winwing(w) {}
    void pushProfileToEngine();
    void pushHapticsConfig();
    void markDirty();
    bool save();
};

class Ui {
public:
    explicit Ui(AppContext& ctx);
    void draw();

private:
    void drawMenuBar();
    void drawStatusBar();
    void drawBindingsTab();
    void drawDevicesTab();
    void drawCatalogTab();
    void drawLogTab();
    void drawSettingsTab();
    void drawVibrationTab();
    void drawBindingEditor();
    void drawActionList(const char* id, std::vector<Action>& actions, int listIndex);
    void drawCatalogBrowser(const char* id, bool pickMode);
    void openEditor(int bindingIndex);
    std::string deviceDisplayName(const std::string& key) const;
    bool inputIsPressed(const input::InputId& id) const;

    AppContext& ctx_;
    std::vector<input::DeviceSnapshot> devices_;
    sim::SimStatus status_;

    // binding editor state
    bool editorOpen_ = false;
    int editIndex_ = -1;
    Binding draft_;
    int catalogTarget_ = 0;  // 0 = press list, 1 = release list
    bool catalogPopupOpen_ = false;

    // catalog state
    std::string catalogFilter_;
    int catalogSelected_ = -1;

    // profile management
    std::string newProfileName_;
    bool renamingProfile_ = false;

    // vibration
    int ledTestDevice_ = -1;
    int ledTestIndex_ = 0;
    int ledTestValue_ = 128;

    // log
    int logMinLevel_ = 1;
    bool logAutoScroll_ = true;
    uint64_t logSerialSeen_ = 0;
};

}  // namespace lr::app
