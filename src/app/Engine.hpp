#pragma once
// Turns joystick events into simulator commands according to the active profile.
// Called from the input thread; the UI thread swaps in new profiles and reads status.
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/Profile.hpp"
#include "input/Evdev.hpp"
#include "sim/SimBridge.hpp"

namespace lr::app {

struct FiredRecord {
    std::chrono::steady_clock::time_point time;
    std::string text;
};

class Engine {
public:
    explicit Engine(sim::SimBridge& bridge);

    void setProfile(std::shared_ptr<const Profile> profile);
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    // input-thread entry points
    void onInput(const input::InputEvent& ev);
    void tick();

    // UI helpers
    // Pure helpers (also used by the UI and the tests)
    static double axisPosition(const AxisConfig& c, double raw01);            // 0..1 inside the used travel, inverted if asked
    static double axisOutput(const AxisConfig& c, double position01);        // rescaled (and rounded if integer)
    static std::vector<Action> withValue(const std::vector<Action>& actions, double v, bool integer);  // substitute the axis value
    static std::string formatValue(double v, bool integer);

    void beginCapture();
    void cancelCapture();
    bool capturing() const;
    std::optional<input::InputId> takeCaptured();
    std::optional<input::InputEvent> lastInput() const;
    std::vector<FiredRecord> recentFires() const;
    void fire(const std::vector<Action>& actions, const std::string& origin);

private:
    struct Held {
        input::InputId id;
        size_t bindingIndex;
        std::chrono::steady_clock::time_point nextRepeat;
    };
    void dispatch(const Binding& b, bool press, const char* why);
    void record(const std::string& text);
    void recordLatest(const std::string& key, const std::string& text);  // replaces the previous entry of the same key
    void onAxis(const input::InputEvent& ev);
    void sendAxisValue(const Binding& b, double out);

    struct AxisState {
        bool hasSent = false;
        double lastSent = 0;
        bool hasPending = false;
        double pending = 0;
        std::chrono::steady_clock::time_point lastTime{};
        bool anchorValid = false;
        double anchor = 0;
    };
    std::unordered_map<size_t, AxisState> axisState_;           // by binding index in the current profile
    std::unordered_map<std::string, float> captureBase_;        // axis resting values seen since beginCapture()

    sim::SimBridge& bridge_;
    mutable std::mutex m_;
    std::shared_ptr<const Profile> profile_;
    std::vector<Held> held_;
    bool enabled_ = true;
    bool capturing_ = false;
    std::optional<input::InputId> captured_;
    std::optional<input::InputEvent> lastInput_;
    std::deque<FiredRecord> fires_;
};

}  // namespace lr::app
