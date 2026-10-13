#pragma once
// Turns joystick events into simulator commands according to the active profile.
// Called from the input thread; the UI thread swaps in new profiles and reads status.
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
