#include "app/Engine.hpp"

#include <algorithm>
#include <cmath>

#include "util/Log.hpp"

namespace lr::app {

using clock = std::chrono::steady_clock;

Engine::Engine(sim::SimBridge& bridge) : bridge_(bridge) {}

void Engine::setProfile(std::shared_ptr<const Profile> profile) {
    std::lock_guard<std::mutex> lock(m_);
    profile_ = std::move(profile);
    held_.clear();
    axisState_.clear();
}

void Engine::setEnabled(bool on) {
    std::lock_guard<std::mutex> lock(m_);
    enabled_ = on;
    if (!on) held_.clear();
}

void Engine::beginCapture() {
    std::lock_guard<std::mutex> lock(m_);
    capturing_ = true;
    captured_.reset();
    captureBase_.clear();
}

void Engine::cancelCapture() {
    std::lock_guard<std::mutex> lock(m_);
    capturing_ = false;
    captured_.reset();
}

bool Engine::capturing() const {
    std::lock_guard<std::mutex> lock(m_);
    return capturing_;
}

std::optional<input::InputId> Engine::takeCaptured() {
    std::lock_guard<std::mutex> lock(m_);
    auto r = captured_;
    captured_.reset();
    return r;
}

std::optional<input::InputEvent> Engine::lastInput() const {
    std::lock_guard<std::mutex> lock(m_);
    return lastInput_;
}

std::vector<FiredRecord> Engine::recentFires() const {
    std::lock_guard<std::mutex> lock(m_);
    return {fires_.begin(), fires_.end()};
}

void Engine::record(const std::string& text) {
    fires_.push_back({clock::now(), text});
    while (fires_.size() > 50) fires_.pop_front();
}

void Engine::recordLatest(const std::string& key, const std::string& text) {
    if (!fires_.empty() && fires_.back().text.rfind(key, 0) == 0) {
        fires_.back() = {clock::now(), text};
        return;
    }
    record(text);
}

double Engine::axisPosition(const AxisConfig& c, double raw01) {
    const double span = c.inMax - c.inMin;
    double t = span > 1e-6 ? (raw01 - c.inMin) / span : raw01;
    t = std::clamp(t, 0.0, 1.0);
    return c.invert ? 1.0 - t : t;
}

double Engine::axisOutput(const AxisConfig& c, double t) {
    const double v = c.outMin + t * (c.outMax - c.outMin);
    return c.integer ? std::round(v) : v;
}

std::string Engine::formatValue(double v, bool integer) {
    char buf[64];
    if (integer) snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
    else snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::vector<Action> Engine::withValue(const std::vector<Action>& actions, double v, bool integer) {
    std::vector<Action> out = actions;
    const std::string text = formatValue(v, integer);
    for (auto& a : out) {
        switch (a.kind) {
            case ActionKind::SimEvent:
            case ActionKind::LVarSet: a.value = v; break;
            case ActionKind::CalcCode: {
                size_t pos = 0;
                while ((pos = a.name.find("{v}", pos)) != std::string::npos) { a.name.replace(pos, 3, text); pos += text.size(); }
                break;
            }
            case ActionKind::HEvent:
            case ActionKind::LVarToggle: break;
        }
    }
    return out;
}

void Engine::sendAxisValue(const Binding& b, double out) {
    const std::string origin = b.label.empty() ? std::string("axis") : b.label;
    fire(withValue(b.onPress, out, b.axis.integer), origin);
    std::lock_guard<std::mutex> lock(m_);
    recordLatest(origin + " =", origin + " = " + formatValue(out, b.axis.integer));
}

void Engine::onAxis(const input::InputEvent& ev) {
    std::shared_ptr<const Profile> profile;
    {
        std::lock_guard<std::mutex> lock(m_);
        lastInput_ = ev;
        if (!enabled_) return;
        profile = profile_;
    }
    if (!profile) return;
    const auto now = clock::now();
    for (size_t i = 0; i < profile->bindings.size(); ++i) {
        const Binding& b = profile->bindings[i];
        if (!b.enabled || !(b.input == ev.id)) continue;
        const AxisConfig& c = b.axis;
        const double t = axisPosition(c, ev.axis);
        if (c.mode == AxisMode::Value) {
            const double out = axisOutput(c, t);
            bool sendNow = false;
            {
                std::lock_guard<std::mutex> lock(m_);
                AxisState& st = axisState_[i];
                const bool changed = !st.hasSent || (c.integer ? std::llround(out) != std::llround(st.lastSent)
                                                                : std::fabs(out - st.lastSent) > 1e-6 * (std::fabs(c.outMax - c.outMin) + 1));
                if (!changed) { st.hasPending = false; continue; }
                if (st.hasSent && now - st.lastTime < std::chrono::milliseconds(c.minIntervalMs)) {
                    st.pending = out;
                    st.hasPending = true;  // tick() sends the latest value once the interval has passed
                } else {
                    st.hasSent = true;
                    st.lastSent = out;
                    st.lastTime = now;
                    st.hasPending = false;
                    sendNow = true;
                }
            }
            if (sendNow) sendAxisValue(b, out);
        } else {
            int steps = 0, dir = 0;
            const double step = std::max(0.005, c.stepPercent / 100.0);
            {
                std::lock_guard<std::mutex> lock(m_);
                AxisState& st = axisState_[i];
                if (!st.anchorValid) { st.anchor = axisPosition(c, ev.axisPrev); st.anchorValid = true; }
                const double delta = t - st.anchor;
                const int n = static_cast<int>(std::fabs(delta) / step);
                if (n < 1) continue;
                dir = delta > 0 ? 1 : -1;
                steps = std::min(n, 10);
                st.anchor = n > 10 ? t : st.anchor + dir * n * step;  // a huge jump resynchronises instead of replaying
            }
            for (int k = 0; k < steps; ++k) dispatch(b, dir > 0, dir > 0 ? " (+)" : " (-)");
        }
    }
}

void Engine::fire(const std::vector<Action>& actions, const std::string& origin) {
    for (const auto& a : actions) {
        switch (a.kind) {
            case ActionKind::SimEvent: bridge_.sendEvent(a.name, a.value, origin); break;
            case ActionKind::HEvent: bridge_.sendHEvent(a.name, origin); break;
            case ActionKind::LVarSet: bridge_.setLVar(a.name, a.value, origin); break;
            case ActionKind::LVarToggle: bridge_.toggleLVar(a.name, origin); break;
            case ActionKind::CalcCode: bridge_.execCalculatorCode(a.name, origin); break;
        }
    }
}

void Engine::dispatch(const Binding& b, bool press, const char* why) {
    const auto& actions = press ? b.onPress : b.onRelease;
    if (actions.empty()) return;
    std::string origin = b.label.empty() ? std::string("binding") : b.label;
    origin += why;
    fire(actions, origin);
    std::string text = origin + ": ";
    for (size_t i = 0; i < actions.size(); ++i) text += (i ? ", " : "") + actions[i].describe();
    std::lock_guard<std::mutex> lock(m_);
    record(text);
}

void Engine::onInput(const input::InputEvent& ev) {
    if (ev.id.kind == input::InputKind::Axis) {
        bool capturing;
        {
            std::lock_guard<std::mutex> lock(m_);
            capturing = capturing_;
        }
        if (!capturing) {
            onAxis(ev);
            return;
        }
    }
    std::shared_ptr<const Profile> profile;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (capturing_) {
            lastInput_ = ev;
            if (ev.id.kind == input::InputKind::Axis) {
                // An axis counts when it is moved a good part of its travel from where it rested, so a noisy or
                // slowly drifting axis is never picked up by accident.
                const std::string key = ev.id.device + "#" + std::to_string(ev.id.code);
                auto it = captureBase_.find(key);
                if (it == captureBase_.end()) it = captureBase_.emplace(key, ev.axisPrev).first;
                if (std::fabs(ev.axis - it->second) >= 0.20f) {
                    captured_ = ev.id;
                    capturing_ = false;
                }
            } else if (ev.pressed) {
                captured_ = ev.id;
                capturing_ = false;
            }
            return;  // never fire bindings while learning a new input
        }
        lastInput_ = ev;
        if (!enabled_) return;
        profile = profile_;
        if (!ev.pressed) {
            held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.id == ev.id; }), held_.end());
        }
    }
    if (!profile) return;
    for (size_t i = 0; i < profile->bindings.size(); ++i) {
        const Binding& b = profile->bindings[i];
        if (!b.enabled || !(b.input == ev.id)) continue;
        dispatch(b, ev.pressed, ev.pressed ? "" : " (release)");
        if (ev.pressed && b.repeat) {
            std::lock_guard<std::mutex> lock(m_);
            held_.push_back({ev.id, i, ev.time + std::chrono::milliseconds(b.repeatDelayMs)});
        }
    }
}

void Engine::tick() {
    std::vector<std::pair<std::shared_ptr<const Profile>, size_t>> due;
    std::vector<std::pair<std::shared_ptr<const Profile>, std::pair<size_t, double>>> axisDue;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!profile_ || (held_.empty() && axisState_.empty())) return;
        const auto now = clock::now();
        // axis values held back by the minimum interval: send the latest one once it has passed
        for (auto& [idx, st] : axisState_) {
            if (!st.hasPending || idx >= profile_->bindings.size()) continue;
            const AxisConfig& c = profile_->bindings[idx].axis;
            if (now - st.lastTime < std::chrono::milliseconds(c.minIntervalMs)) continue;
            st.hasSent = true;
            st.lastSent = st.pending;
            st.lastTime = now;
            st.hasPending = false;
            axisDue.push_back({profile_, {idx, st.pending}});
        }
        for (auto& h : held_) {
            if (now < h.nextRepeat) continue;
            if (h.bindingIndex >= profile_->bindings.size()) continue;
            const Binding& b = profile_->bindings[h.bindingIndex];
            h.nextRepeat = now + std::chrono::milliseconds(b.repeatIntervalMs < 10 ? 10 : b.repeatIntervalMs);
            due.emplace_back(profile_, h.bindingIndex);
        }
    }
    for (const auto& [profile, idx] : due) dispatch(profile->bindings[idx], true, " (repeat)");
    for (const auto& [profile, item] : axisDue) sendAxisValue(profile->bindings[item.first], item.second);
}

}  // namespace lr::app
