#pragma once
// Data model for bindings/profiles plus JSON (de)serialization of the config file.
#include <string>
#include <vector>

#include "haptics/Haptics.hpp"
#include "input/Evdev.hpp"

namespace lr::app {

enum class ActionKind { SimEvent, HEvent, LVarSet, CalcCode };

const char* actionKindName(ActionKind k);   // "sim_event" ... (stable, used in JSON)
const char* actionKindLabel(ActionKind k);  // "K: event" ... (UI)
bool actionKindFromName(const std::string& s, ActionKind& out);
bool actionNeedsMobiFlight(ActionKind k);

struct Action {
    ActionKind kind = ActionKind::SimEvent;
    std::string name;   // event name / L var / H event / RPN code
    double value = 0;   // event parameter or L var value
    std::string describe() const;
};

struct Binding {
    std::string label;
    input::InputId input;
    std::vector<Action> onPress;
    std::vector<Action> onRelease;
    bool repeat = false;        // re-fire onPress while held (knob turning)
    int repeatDelayMs = 400;
    int repeatIntervalMs = 100;
    bool enabled = true;
};

struct Profile {
    std::string name = "Default";
    std::vector<Binding> bindings;
};

struct Settings {
    std::string host = "127.0.0.1";
    int port = 500;
    bool autoConnect = true;
    std::string activeProfile;
    std::vector<Profile> profiles;
    haptics::HapticsConfig haptics;

    Profile* active();
    const Profile* active() const;
    Profile& ensureActive();
};

bool loadSettings(const std::string& path, Settings& out, std::string& err);
bool saveSettings(const std::string& path, const Settings& s, std::string& err);

}  // namespace lr::app
