#include "haptics/Haptics.hpp"

#include <algorithm>
#include <cmath>

#include "util/Log.hpp"

namespace lr::haptics {

using clock = std::chrono::steady_clock;

const std::vector<RuleInfo>& ruleInfos() {
    static const std::vector<RuleInfo> infos = {
        {"ground_roll", "Ground roll", "Rumble proportional to ground speed while on the ground"},
        {"touchdown", "Touchdown", "Short jolt when the wheels touch, scaled by sink rate"},
        {"stall", "Stall warning", "Pulsing buffet while the stall warning is active"},
        {"overspeed", "Overspeed", "Steady vibration while the overspeed warning is active"},
        {"gear", "Gear transit", "Light vibration while the gear moves, bump when it locks"},
        {"flaps", "Flaps moving", "Light vibration while flaps/slats are in motion"},
        {"speedbrake", "Speed brake", "Airflow buffet from deployed spoilers in flight"},
        {"reverse", "Reverse thrust", "Vibration while the reversers are deployed"},
        {"engine", "Takeoff thrust", "Rumble from high N1 while on the ground"},
        {"turbulence", "Turbulence / G", "Vibration from vertical load factor changes in flight"},
        {"brakes", "Wheel braking", "Vibration while braking at speed"},
    };
    return infos;
}

HapticsEngine::HapticsEngine(sim::SimBridge& bridge, WinwingHidManager& hid) : bridge_(bridge), hid_(hid) {}
HapticsEngine::~HapticsEngine() { stop(); }

void HapticsEngine::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { threadMain(); });
}

void HapticsEngine::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    hid_.allOff();
}

void HapticsEngine::setConfig(const HapticsConfig& cfg) {
    {
        std::lock_guard<std::mutex> lock(m_);
        cfg_ = cfg;
    }
    bridge_.setTelemetryEnabled(cfg.enabled);
    for (const auto& [key, dc] : cfg.devices) {
        if (dc.identity.deviceByte >= 0) hid_.setIdentity(key, dc.identity);
    }
}

HapticsConfig HapticsEngine::config() const {
    std::lock_guard<std::mutex> lock(m_);
    return cfg_;
}

HapticsStatus HapticsEngine::status() const {
    std::lock_guard<std::mutex> lock(m_);
    return status_;
}

void HapticsEngine::pulse(const std::string& deviceKey, float left, float right, int ms) {
    std::lock_guard<std::mutex> lock(m_);
    pulses_[deviceKey] = {clock::now() + std::chrono::milliseconds(ms), left, right};
}

void HapticsEngine::stopAll() {
    {
        std::lock_guard<std::mutex> lock(m_);
        pulses_.clear();
        globalPulses_.clear();
    }
    hid_.allOff();
}

static float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

void HapticsEngine::evaluate(const sim::Telemetry& t, float& left, float& right, std::map<std::string, float>& contrib) {
    const auto now = clock::now();
    const double dt = prevTime_.time_since_epoch().count() == 0 ? 0.02 : std::chrono::duration<double>(now - prevTime_).count();
    prevTime_ = now;
    const bool onGround = t.onGround > 0.5;
    const double phase = std::chrono::duration<double>(now.time_since_epoch()).count();

    auto add = [&](const char* id, double level, bool both = true, bool l = true, bool r = true) {
        const RuleConfig rc = cfg_.rule(id);
        if (!rc.enabled) return;
        const float v = clamp01(level * rc.gain);
        contrib[id] = std::max(contrib[id], v);
        if (both || l) left = std::max(left, v);
        if (both || r) right = std::max(right, v);
    };

    // --- state updates ---
    if (!onGround) lastAirborneVs_ = t.verticalSpeedFpm;
    if (dt > 0) {
        const double rate = (t.flapsPct - prevFlaps_) / dt;
        flapsRate_ = flapsRate_ * 0.7 + rate * 0.3;
    }
    gSmooth_ = gSmooth_ * 0.6 + t.gForce * 0.4;

    // --- rules ---
    if (onGround && t.groundSpeedKt > 2) add("ground_roll", 0.08 + 0.45 * std::min(1.0, t.groundSpeedKt / 90.0));

    if (onGround && !prevOnGround_) {
        const double sink = std::fabs(lastAirborneVs_);
        const float level = clamp01(0.25 + 0.75 * std::min(1.0, sink / 700.0));
        globalPulses_.push_back({now + std::chrono::milliseconds(350), level, level});
        LOG_INFO("Touchdown detected (%.0f fpm) -> vibration pulse %.0f%%", sink, level * 100);
    }

    if (t.stallWarning > 0.5) add("stall", std::fmod(phase * 5.0, 1.0) < 0.5 ? 0.9 : 0.3);
    if (t.overspeedWarning > 0.5) add("overspeed", 0.55);

    const bool gearTransit = t.gearPct > 0.02 && t.gearPct < 0.98;
    if (gearTransit) add("gear", 0.25);
    const bool prevTransit = prevGear_ > 0.02 && prevGear_ < 0.98;
    if (prevTransit && !gearTransit) {
        const RuleConfig rc = cfg_.rule("gear");
        if (rc.enabled) globalPulses_.push_back({now + std::chrono::milliseconds(180), clamp01(0.5 * rc.gain), clamp01(0.5 * rc.gain)});
    }

    if (std::fabs(flapsRate_) > 0.01) add("flaps", 0.15);

    if (!onGround && t.spoilersHandle > 0.05) add("speedbrake", 0.08 + 0.35 * t.spoilersHandle * std::min(1.0, t.iasKt / 250.0));

    const double rev = std::max(t.reverse1, t.reverse2);
    if (rev > 0.05) add("reverse", 0.25 + 0.5 * rev);

    const double n1 = (t.n1Eng1 + t.n1Eng2) * 0.5;
    if (onGround && n1 > 35) add("engine", (n1 - 35.0) / 65.0 * 0.35);

    if (!onGround) {
        const double d = std::fabs(gSmooth_ - 1.0);
        if (d > 0.06) add("turbulence", std::min(1.0, (d - 0.06) * 2.5) * 0.8);
    }

    const double brake = std::max(t.brakeLeft, t.brakeRight);
    if (onGround && t.groundSpeedKt > 15 && brake > 0.25) add("brakes", 0.1 + 0.3 * brake);

    // --- pulses ---
    const RuleConfig td = cfg_.rule("touchdown");
    for (size_t i = 0; i < globalPulses_.size();) {
        if (now >= globalPulses_[i].until) { globalPulses_.erase(globalPulses_.begin() + static_cast<long>(i)); continue; }
        const float gain = td.enabled ? td.gain : 0.f;
        const float l = clamp01(globalPulses_[i].left * gain), r = clamp01(globalPulses_[i].right * gain);
        contrib["touchdown"] = std::max(contrib["touchdown"], std::max(l, r));
        left = std::max(left, l);
        right = std::max(right, r);
        ++i;
    }

    prevOnGround_ = onGround;
    prevGear_ = t.gearPct;
    prevFlaps_ = t.flapsPct;
}

void HapticsEngine::threadMain() {
    LOG_INFO("Haptics thread started");
    auto nextScan = clock::now();
    std::map<std::string, std::pair<uint8_t, uint8_t>> lastSent;
    while (running_) {
        const auto now = clock::now();
        if (now >= nextScan) {
            hid_.rescan();
            nextScan = now + std::chrono::seconds(3);
        }
        HapticsConfig cfg;
        std::map<std::string, Pulse> pulses;
        {
            std::lock_guard<std::mutex> lock(m_);
            cfg = cfg_;
            pulses = pulses_;
        }
        const sim::Telemetry t = bridge_.telemetry();
        const bool fresh = t.valid && (now - t.time) < std::chrono::seconds(2);

        float left = 0, right = 0;
        std::map<std::string, float> contrib;
        if (cfg.enabled && fresh) {
            std::lock_guard<std::mutex> lock(m_);
            evaluate(t, left, right, contrib);
        } else {
            prevTime_ = {};
            globalPulses_.clear();
        }
        left = clamp01(left * cfg.master);
        right = clamp01(right * cfg.master);

        for (const auto& dev : hid_.devices()) {
            float l = left, r = right;
            auto dc = cfg.devices.find(dev.key);
            const bool devEnabled = dc == cfg.devices.end() ? true : dc->second.enabled;
            if (!cfg.enabled || !devEnabled) { l = r = 0; }
            auto p = pulses.find(dev.key);
            if (p != pulses.end()) {
                if (now < p->second.until) { l = std::max(l, p->second.left); r = std::max(r, p->second.right); }
                else { std::lock_guard<std::mutex> lock(m_); pulses_.erase(dev.key); }
            }
            const uint8_t lb = static_cast<uint8_t>(std::lround(l * 255)), rb = static_cast<uint8_t>(std::lround(r * 255));
            auto& last = lastSent[dev.key];
            if (last.first != lb || last.second != rb) {
                hid_.setMotors(dev.key, lb, rb);
                last = {lb, rb};
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            status_.left = left;
            status_.right = right;
            status_.contrib = contrib;
            status_.telemetryFresh = fresh;
            status_.telemetry = t;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    hid_.allOff();
    LOG_INFO("Haptics thread stopped");
}

}  // namespace lr::haptics
