#pragma once
// Data model for bindings/profiles plus JSON (de)serialization of the config file.
#include <string>
#include <vector>

#include "haptics/Haptics.hpp"
#include "input/Evdev.hpp"

namespace lr::app {

enum class ActionKind { SimEvent, HEvent, LVarSet, LVarToggle, CalcCode };

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

// How an axis input acts. Value: the position is rescaled to [outMin, outMax] and substituted into the actions
// (event parameter, L: var value, or {v} inside RPN code). Steps: moving up runs the "press" actions and moving
// down the "release" actions, once per stepPercent of travel (a knob made from an axis).
enum class AxisMode { Value, Steps };

struct AxisConfig {
    AxisMode mode = AxisMode::Value;
    bool invert = false;
    double inMin = 0.0, inMax = 1.0;      // part of the physical travel that is used (0..1); the rest is dead zone
    double outMin = 0.0, outMax = 100.0;  // Value mode: output range
    bool integer = true;                  // Value mode: round the output (event parameters are integers)
    int minIntervalMs = 40;               // Value mode: at most one update per interval (the last value is never lost)
    double stepPercent = 5.0;             // Steps mode: travel per step, in percent of the used travel
};

struct Binding {
    std::string label;
    input::InputId input;
    AxisConfig axis;                      // only used when input.kind == Axis
    std::vector<Action> onPress;
    std::vector<Action> onRelease;
    bool repeat = false;        // re-fire onPress while held (knob turning)
    int repeatDelayMs = 400;
    int repeatIntervalMs = 100;
    bool enabled = true;
};

// Aircraft ids match the "aircraft" field of the catalogs in data/ (a32nx_catalog.json -> "A32NX").
constexpr const char* kDefaultAircraft = "A32NX";

struct Profile {
    std::string name = "Default";
    std::string aircraft = kDefaultAircraft;  // which catalog the editor offers; old configs without it are A32NX
    std::vector<Binding> bindings;
};

// Config schema: 1 = original, 2 = profiles carry an "aircraft". Newer files stay readable by older
// builds (unknown fields are ignored); loading a file older than kConfigVersion never loses data.
constexpr int kConfigVersion = 2;

struct Settings {
    int loadedVersion = kConfigVersion;  // schema version of the file this was read from
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

// Parameter range that a sim axis event expects, judged from its name alone (used when the catalog does not know
// the event): THROTTLEn_AXIS_SET_EX1 and friends take -16384..16383, AXIS_*_SET take -16383..16383.
bool axisEventRange(const std::string& eventName, double& lo, double& hi);

// True when a value-mode axis binding drives an event that expects `lo..hi` (span >= 1000) but its output range
// is far smaller (the 0..100 default is the classic mistake: the lever then moves under 1 % of its travel).
bool axisRangeLooksWrong(const AxisConfig& axis, double lo, double hi);

bool loadSettings(const std::string& path, Settings& out, std::string& err);
bool saveSettings(const std::string& path, const Settings& s, std::string& err);

}  // namespace lr::app
