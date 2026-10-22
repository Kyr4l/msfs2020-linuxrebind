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
    std::vector<Catalog> catalogs;  // one per aircraft, from data/*_catalog.json
    sim::SimBridge& bridge;
    Engine& engine;
    input::InputManager& input;
    haptics::HapticsEngine& haptics;
    haptics::WinwingHidManager& winwing;
    bool dirty = false;
    std::string startTab;  // --tab NAME: tab selected on the first frame
    bool quitRequested = false;
    std::string lastSaveError;

    AppContext(sim::SimBridge& b, Engine& e, input::InputManager& i, haptics::HapticsEngine& h, haptics::WinwingHidManager& w)
        : bridge(b), engine(e), input(i), haptics(h), winwing(w) {}
    const Catalog* catalogFor(const std::string& aircraft) const;  // exact match, else the default aircraft, else the first
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
    void drawSplash();
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
    std::string viewAircraft_;  // catalog tab: aircraft being browsed; empty = follow the active profile
    const Catalog& currentCatalog() const;
    const Catalog& pickerCatalog() const;  // the active profile's aircraft
    std::string detectAircraft(const std::string& cfgPath) const;  // "A32NX"/"A380X" from the loaded .cfg path, or empty
    std::string uniqueProfileName(const std::string& wanted) const;
    std::string newProfileAircraft_ = kDefaultAircraft;
    std::string catalogFilter_;
    int catalogSelected_ = -1;
    bool catalogBothSides_ = false;  // add the first-officer twin of captain-side EFIS entries too
    std::vector<Action> catalogActions(const CatalogEntry& e) const;
    void newBindingFrom(const CatalogEntry& e);

    // profile management
    std::string newProfileName_;
    bool renamingProfile_ = false;

    bool splashDone_ = false;
    std::string startTab_;


    // log
    int logMinLevel_ = 1;
    bool logAutoScroll_ = true;
    uint64_t logSerialSeen_ = 0;
};

}  // namespace lr::app
