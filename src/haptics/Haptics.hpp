#pragma once
// Telemetry-driven vibration ("SimAppPro style"): flight data from SimConnect is turned into
// motor levels for Winwing devices. Runs on its own thread at ~50 Hz.
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "haptics/WinwingHid.hpp"
#include "sim/SimBridge.hpp"

namespace lr::haptics {

struct RuleConfig {
    bool enabled = true;
    float gain = 1.0f;
};

struct RuleInfo {
    const char* id;
    const char* label;
    const char* description;
};
const std::vector<RuleInfo>& ruleInfos();

struct DeviceConfig {
    bool enabled = true;
    WinwingIdentity identity;  // unknown fields (-1) fall back to the built-in defaults
};

struct HapticsConfig {
    bool enabled = false;
    float master = 1.0f;
    std::map<std::string, RuleConfig> rules;
    std::map<std::string, DeviceConfig> devices;  // key "4098:b970"
    RuleConfig rule(const std::string& id) const {
        auto it = rules.find(id);
        return it == rules.end() ? RuleConfig{} : it->second;
    }
};

struct HapticsStatus {
    float left = 0, right = 0;             // last computed level 0..1 (before device gain)
    std::map<std::string, float> contrib;  // per rule 0..1
    bool telemetryFresh = false;
    sim::Telemetry telemetry;
};

class HapticsEngine {
public:
    HapticsEngine(sim::SimBridge& bridge, WinwingHidManager& hid);
    ~HapticsEngine();

    void start();
    void stop();
    void setConfig(const HapticsConfig& cfg);
    HapticsConfig config() const;
    HapticsStatus status() const;

    // Manual pulse (test buttons): level 0..1 for ms on the given motors.
    void pulse(const std::string& deviceKey, float left, float right, int ms);
    void stopAll();

private:
    struct Pulse {
        std::chrono::steady_clock::time_point until;
        float left, right;
    };
    void threadMain();
    void evaluate(const sim::Telemetry& t, float& left, float& right, std::map<std::string, float>& contrib);

    sim::SimBridge& bridge_;
    WinwingHidManager& hid_;
    mutable std::mutex m_;
    HapticsConfig cfg_;
    HapticsStatus status_;
    std::map<std::string, Pulse> pulses_;
    std::vector<Pulse> globalPulses_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // rule state
    bool prevOnGround_ = true;
    double prevGear_ = 1, prevFlaps_ = 0;
    double lastAirborneVs_ = 0;
    double gSmooth_ = 1.0;
    std::chrono::steady_clock::time_point prevTime_{};
    uint64_t prevSeq_ = 0;
    double flapsRate_ = 0;
};

}  // namespace lr::haptics
