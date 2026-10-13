#include "app/Engine.hpp"

#include "util/Log.hpp"

namespace lr::app {

using clock = std::chrono::steady_clock;

Engine::Engine(sim::SimBridge& bridge) : bridge_(bridge) {}

void Engine::setProfile(std::shared_ptr<const Profile> profile) {
    std::lock_guard<std::mutex> lock(m_);
    profile_ = std::move(profile);
    held_.clear();
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

void Engine::fire(const std::vector<Action>& actions, const std::string& origin) {
    for (const auto& a : actions) {
        switch (a.kind) {
            case ActionKind::SimEvent: bridge_.sendEvent(a.name, a.value, origin); break;
            case ActionKind::HEvent: bridge_.sendHEvent(a.name, origin); break;
            case ActionKind::LVarSet: bridge_.setLVar(a.name, a.value, origin); break;
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
    record(text);
}

void Engine::onInput(const input::InputEvent& ev) {
    std::shared_ptr<const Profile> profile;
    {
        std::lock_guard<std::mutex> lock(m_);
        lastInput_ = ev;
        if (capturing_) {
            if (ev.pressed) {
                captured_ = ev.id;
                capturing_ = false;
            }
            return;  // never fire bindings while learning a new input
        }
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
    {
        std::lock_guard<std::mutex> lock(m_);
        if (held_.empty() || !profile_) return;
        const auto now = clock::now();
        for (auto& h : held_) {
            if (now < h.nextRepeat) continue;
            if (h.bindingIndex >= profile_->bindings.size()) continue;
            const Binding& b = profile_->bindings[h.bindingIndex];
            h.nextRepeat = now + std::chrono::milliseconds(b.repeatIntervalMs < 10 ? 10 : b.repeatIntervalMs);
            due.emplace_back(profile_, h.bindingIndex);
        }
    }
    for (const auto& [profile, idx] : due) dispatch(profile->bindings[idx], true, " (repeat)");
}

}  // namespace lr::app
