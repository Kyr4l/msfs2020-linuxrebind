#include "ui/Ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/Paths.hpp"
#include "util/Log.hpp"

namespace lr::app {

namespace {
const ImVec4 kGreen(0.35f, 0.85f, 0.40f, 1.0f);
const ImVec4 kYellow(0.95f, 0.80f, 0.25f, 1.0f);
const ImVec4 kRed(0.95f, 0.35f, 0.35f, 1.0f);
const ImVec4 kGrey(0.6f, 0.6f, 0.6f, 1.0f);

void dot(const ImVec4& color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.3f, ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(h, h));
    ImGui::SameLine();
}

const char* connStateText(sim::ConnState s) {
    switch (s) {
        case sim::ConnState::Disconnected: return "Disconnected";
        case sim::ConnState::Connecting: return "Connecting";
        case sim::ConnState::Opening: return "Handshaking";
        case sim::ConnState::Connected: return "Connected";
    }
    return "?";
}

std::string baseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string formatTime(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    return buf;
}

bool actionKindCombo(const char* label, ActionKind& kind) {
    static const ActionKind kinds[] = {ActionKind::SimEvent, ActionKind::HEvent, ActionKind::LVarSet, ActionKind::LVarToggle, ActionKind::CalcCode};
    constexpr int n = 5;
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (kinds[i] == kind) idx = i;
    bool changed = false;
    if (ImGui::BeginCombo(label, actionKindLabel(kind))) {
        for (int i = 0; i < n; ++i) {
            if (ImGui::Selectable(actionKindLabel(kinds[i]), i == idx)) { kind = kinds[i]; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}
}  // namespace

// ---------------------------------------------------------------- AppContext

void AppContext::pushProfileToEngine() {
    const Profile& p = settings.ensureActive();
    engine.setProfile(std::make_shared<Profile>(p));
    std::vector<std::string> vars;
    for (const auto& b : p.bindings)
        for (const auto* list : {&b.onPress, &b.onRelease})
            for (const auto& a : *list)
                if ((a.kind == ActionKind::LVarSet || a.kind == ActionKind::LVarToggle) && !a.name.empty()) vars.push_back(a.name);
    bridge.watchLVars(vars);
}

bool AppContext::suggestedAxisRange(const Binding& b, double& lo, double& hi) const {
    for (const auto& a : b.onPress) {
        if (a.kind != ActionKind::SimEvent) continue;
        for (const auto& cat : catalogs)
            for (const auto& e : cat.entries)
                if (e.hasAxisRange && e.name == a.name) { lo = e.axisMin; hi = e.axisMax; return true; }
        if (axisEventRange(a.name, lo, hi)) return true;
    }
    return false;
}

int AppContext::fixAxisRanges(Profile& p) {
    int n = 0;
    for (auto& b : p.bindings) {
        double lo, hi;
        if (b.input.kind != input::InputKind::Axis || !suggestedAxisRange(b, lo, hi) || !axisRangeLooksWrong(b.axis, lo, hi)) continue;
        b.axis.outMin = lo;
        b.axis.outMax = hi;
        b.axis.integer = true;
        ++n;
    }
    if (n) markDirty();
    return n;
}

void AppContext::warnAxisRanges() const {
    for (const auto& p : settings.profiles)
        for (const auto& b : p.bindings) {
            double lo, hi;
            if (b.input.kind == input::InputKind::Axis && suggestedAxisRange(b, lo, hi) && axisRangeLooksWrong(b.axis, lo, hi))
                LOG_WARN("Profile \"%s\", binding \"%s\": the axis event expects %g..%g but the output range is %g..%g, so the control barely moves "
                         "(open the binding and press 'Set range', or use the banner in the Bindings tab)",
                         p.name.c_str(), b.label.c_str(), lo, hi, b.axis.outMin, b.axis.outMax);
        }
}

const Catalog* AppContext::catalogFor(const std::string& aircraft) const {
    for (const auto& c : catalogs)
        if (c.aircraft == aircraft) return &c;
    for (const auto& c : catalogs)
        if (c.aircraft == kDefaultAircraft) return &c;
    return catalogs.empty() ? nullptr : &catalogs.front();
}

void AppContext::pushHapticsConfig() {
    haptics.setConfig(settings.haptics);
}

void AppContext::markDirty() {
    dirty = true;
    pushProfileToEngine();
}

bool AppContext::save() {
    std::string err;
    if (!saveSettings(configPath, settings, err)) {
        lastSaveError = err;
        LOG_ERROR("Saving %s failed: %s", configPath.c_str(), err.c_str());
        return false;
    }
    lastSaveError.clear();
    dirty = false;
    return true;
}

// ---------------------------------------------------------------- Ui

Ui::Ui(AppContext& ctx) : ctx_(ctx), startTab_(ctx.startTab) {}

std::string Ui::deviceDisplayName(const std::string& key) const {
    for (const auto& d : devices_)
        if (d.key == key) return d.name;
    size_t p = key.find(':', 5);
    return p == std::string::npos ? key : key.substr(p + 1) + " (not connected)";
}

float Ui::axisNow(const input::InputId& id) const {
    if (id.kind != input::InputKind::Axis) return -1.0f;
    for (const auto& d : devices_) {
        if (d.key != id.device) continue;
        auto it = std::find(d.axisCodes.begin(), d.axisCodes.end(), id.code);
        if (it != d.axisCodes.end()) return d.axisValue[static_cast<size_t>(it - d.axisCodes.begin())];
    }
    return -1.0f;
}

bool Ui::inputIsPressed(const input::InputId& id) const {
    if (id.kind == input::InputKind::Axis) return false;
    for (const auto& d : devices_) {
        if (d.key != id.device) continue;
        if (id.kind == input::InputKind::Button) {
            auto it = std::find(d.buttonCodes.begin(), d.buttonCodes.end(), id.code);
            return it != d.buttonCodes.end() && d.buttonState[static_cast<size_t>(it - d.buttonCodes.begin())];
        }
        if (id.code < 0 || id.code >= 4) return false;
        auto [x, y] = d.hats[id.code];
        switch (id.dir) {
            case 0: return y < 0;
            case 1: return x > 0;
            case 2: return y > 0;
            case 3: return x < 0;
        }
    }
    return false;
}

void Ui::draw() {
    devices_ = ctx_.input.devices();
    status_ = ctx_.bridge.status();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##main", nullptr, flags);
    drawMenuBar();
    drawStatusBar();
    ImGui::Separator();
    if (ImGui::BeginTabBar("tabs")) {
        auto tab = [&](const char* name, void (Ui::*draw)()) {
            ImGuiTabItemFlags flags = 0;
            if (!startTab_.empty() && startTab_ == name) { flags = ImGuiTabItemFlags_SetSelected; startTab_.clear(); }
            if (ImGui::BeginTabItem(name, nullptr, flags)) { (this->*draw)(); ImGui::EndTabItem(); }
        };
        tab("Bindings", &Ui::drawBindingsTab);
        tab("Devices", &Ui::drawDevicesTab);
        tab("Catalog", &Ui::drawCatalogTab);
        tab("Vibration", &Ui::drawVibrationTab);
        tab("Log", &Ui::drawLogTab);
        tab("Settings", &Ui::drawSettingsTab);
        ImGui::EndTabBar();
    }
    ImGui::End();

    if (!splashDone_) drawSplash();
    if (editorOpen_) drawBindingEditor();

    // Debounced auto-save.
    static double lastDirtyAt = -1;
    if (ctx_.dirty) {
        double now = ImGui::GetTime();
        if (lastDirtyAt < 0) lastDirtyAt = now;
        if (now - lastDirtyAt > 1.0) { ctx_.save(); lastDirtyAt = -1; }
    } else {
        lastDirtyAt = -1;
    }
}

void Ui::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save", "Ctrl+S")) ctx_.save();
        if (ImGui::MenuItem("Reload config")) {
            Settings s;
            std::string err;
            if (loadSettings(ctx_.configPath, s, err)) {
                ctx_.settings = std::move(s);
                ctx_.settings.ensureActive();
                ctx_.dirty = false;
                ctx_.pushProfileToEngine();
                ctx_.bridge.setEndpoint(ctx_.settings.host, static_cast<uint16_t>(ctx_.settings.port));
            } else {
                LOG_ERROR("Reload failed: %s", err.c_str());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) ctx_.quitRequested = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Simulator")) {
        if (ImGui::MenuItem("Connect now")) ctx_.bridge.connectNow();
        if (ImGui::MenuItem("Disconnect")) ctx_.bridge.disconnect();
        if (ImGui::MenuItem("Re-probe MobiFlight module")) ctx_.bridge.probeMobiFlight();
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) ctx_.save();
}

void Ui::drawStatusBar() {
    // Simulator
    ImVec4 c = kRed;
    if (status_.state == sim::ConnState::Connected) c = kGreen;
    else if (status_.state != sim::ConnState::Disconnected) c = kYellow;
    dot(c);
    if (status_.state == sim::ConnState::Connected) {
        ImGui::Text("MSFS: connected (%s %s)%s", status_.simName.c_str(), status_.simVersion.c_str(), status_.paused ? " [paused]" : "");
        if (status_.detectedPort) {
            ImGui::SameLine();
            ImGui::TextDisabled("via auto-detected %s:%u", status_.detectedHost.c_str(), status_.detectedPort);
        }
    } else {
        ImGui::Text("MSFS: %s (%s:%u)", connStateText(status_.state), status_.host.c_str(), status_.port);
        if (!status_.lastError.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "- %s", status_.lastError.c_str());
        }
    }
    ImGui::SameLine(0, 30);
    // MobiFlight
    switch (status_.mobiflight) {
        case sim::ModuleState::Detected: dot(kGreen); ImGui::TextUnformatted("MobiFlight WASM: detected"); break;
        case sim::ModuleState::Probing: dot(kYellow); ImGui::TextUnformatted("MobiFlight WASM: probing..."); break;
        case sim::ModuleState::NotFound: dot(kRed); ImGui::TextUnformatted("MobiFlight WASM: not found"); break;
        default: dot(kGrey); ImGui::TextUnformatted("MobiFlight WASM: unknown"); break;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("H: events, L: vars and calculator code need the MobiFlight WASM module\nin the MSFS Community folder. K: events (including A32NX.* custom events) work without it.");
    ImGui::SameLine(0, 30);
    ImGui::Text("Aircraft: %s", status_.aircraft.empty() ? "-" : baseName(status_.aircraft).c_str());
    if (!status_.aircraft.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status_.aircraft.c_str());
    ImGui::SameLine(0, 30);
    ImGui::Text("Devices: %zu", devices_.size());
    ImGui::SameLine(0, 30);
    bool enabled = ctx_.engine.enabled();
    if (ImGui::Checkbox("Bindings active", &enabled)) ctx_.engine.setEnabled(enabled);
    if (ctx_.dirty) { ImGui::SameLine(0, 30); ImGui::TextColored(kYellow, "unsaved"); }
    if (!ctx_.lastSaveError.empty()) { ImGui::SameLine(); ImGui::TextColored(kRed, "save failed: %s", ctx_.lastSaveError.c_str()); }
}

// ---------------------------------------------------------------- Splash

// Shown over the main window until the background threads finished their first device scans
// (joysticks via evdev, Winwing controllers via hidraw discovery) so a slow start never looks like a hang.
void Ui::drawSplash() {
    const double t = ImGui::GetTime();
    const bool inputDone = ctx_.input.scanCount() > 0;
    const bool winwingDone = ctx_.winwing.scanCount() > 0;
    if ((inputDone && winwingDone && t > 0.8) || t > 15.0) {
        splashDone_ = true;
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::Begin("##splash", nullptr, flags);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextUnformatted("MSFS Linux Rebind");
    ImGui::PopFont();
    ImGui::TextDisabled("version %s - starting up", LINUXREBIND_VERSION);
    ImGui::Spacing();
    auto line = [&](bool done, const char* what, const char* pending) {
        dot(done ? kGreen : kYellow);
        ImGui::TextUnformatted(done ? what : pending);
    };
    char joy[96];
    snprintf(joy, sizeof joy, "Joysticks: %zu found", devices_.size());
    line(inputDone, joy, "Joysticks: scanning /dev/input ...");
    const auto ww = ctx_.winwing.devices();
    char wwText[96];
    snprintf(wwText, sizeof wwText, "Winwing devices: %zu found", ww.size());
    line(winwingDone, wwText, "Winwing devices: discovering controllers over hidraw ...");
    const bool simUp = status_.state == sim::ConnState::Connected;
    line(simUp, "Simulator: connected", status_.state == sim::ConnState::Disconnected ? "Simulator: not running yet (will keep retrying)" : "Simulator: connecting ...");
    ImGui::Spacing();
    // Indeterminate progress bar.
    const float w = ImGui::GetContentRegionAvail().x;
    const float phase = static_cast<float>(std::fmod(t * 0.6, 1.0));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 6.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
    const float segW = w * 0.3f;
    float x0 = p.x + (w + segW) * phase - segW;
    float x1 = x0 + segW;
    x0 = std::max(x0, p.x);
    x1 = std::min(x1, p.x + w);
    if (x1 > x0) dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + h), ImGui::GetColorU32(ImGuiCol_ButtonActive), 3.0f);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------- Bindings

void Ui::drawAxisOptions() {
    AxisConfig& c = draft_.axis;
    ImGui::SeparatorText("Axis");
    const float now = axisNow(draft_.input);
    ImGui::TextUnformatted("Position now");
    ImGui::SameLine();
    const double t = Engine::axisPosition(c, now >= 0 ? now : 0.0f);
    char overlay[96];
    if (now < 0) snprintf(overlay, sizeof overlay, "device not connected");
    else if (c.mode == AxisMode::Value) snprintf(overlay, sizeof overlay, "%d%% of travel  ->  %s", static_cast<int>(now * 100 + 0.5f), Engine::formatValue(Engine::axisOutput(c, t), c.integer).c_str());
    else snprintf(overlay, sizeof overlay, "%d%% of travel", static_cast<int>(now * 100 + 0.5f));
    ImGui::ProgressBar(now < 0 ? 0.0f : now, ImVec2(360, 0), overlay);

    int mode = c.mode == AxisMode::Steps ? 1 : 0;
    ImGui::SetNextItemWidth(420);
    if (ImGui::Combo("Mode", &mode, "Value: the position is sent as the parameter\0Steps: up runs the + actions, down the - actions\0")) c.mode = mode == 1 ? AxisMode::Steps : AxisMode::Value;
    ImGui::Checkbox("Invert", &c.invert);

    float from = static_cast<float>(c.inMin * 100.0), to = static_cast<float>(c.inMax * 100.0);
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("Use travel from (%)", &from, 0.0f, 98.0f, "%.0f")) c.inMin = std::min<double>(from / 100.0, c.inMax - 0.02);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("to (%)", &to, 2.0f, 100.0f, "%.0f")) c.inMax = std::max<double>(to / 100.0, c.inMin + 0.02);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything outside this part of the physical travel is dead zone: it reads as the minimum or the maximum.");

    if (c.mode == AxisMode::Value) {
        struct Preset { const char* label; double lo, hi; bool integer; };
        static const Preset presets[] = {
            {"0 to 100 (percent)", 0, 100, true},
            {"-16384 to 16383 (sim axis event, with reverse)", -16384, 16383, true},
            {"0 to 16383 (sim axis event, no reverse)", 0, 16383, true},
            {"0 to 1", 0, 1, false},
        };
        const char* current = "Custom";
        for (const auto& pr : presets)
            if (c.outMin == pr.lo && c.outMax == pr.hi) current = pr.label;
        ImGui::SetNextItemWidth(380);
        if (ImGui::BeginCombo("Output range", current)) {
            for (const auto& pr : presets)
                if (ImGui::Selectable(pr.label, c.outMin == pr.lo && c.outMax == pr.hi)) { c.outMin = pr.lo; c.outMax = pr.hi; c.integer = pr.integer; }
            ImGui::EndCombo();
        }
        double lo, hi;
        if (ctx_.suggestedAxisRange(draft_, lo, hi)) {
            if (axisRangeLooksWrong(c, lo, hi)) {
                ImGui::TextColored(kYellow, "This axis event expects %g to %g, but the output range is %g to %g: the control would barely move.", lo, hi, c.outMin, c.outMax);
                ImGui::SameLine();
                if (ImGui::SmallButton("Set range")) { c.outMin = lo; c.outMax = hi; c.integer = true; }
            } else {
                ImGui::TextDisabled("This axis event expects %g to %g.", lo, hi);
            }
        }
        ImGui::SetNextItemWidth(110);
        ImGui::InputDouble("Output at start", &c.outMin, 0, 0, "%g");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputDouble("at end", &c.outMax, 0, 0, "%g");
        ImGui::SameLine();
        ImGui::Checkbox("Whole numbers", &c.integer);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("min ms between updates", &c.minIntervalMs, 0, 0);
        c.minIntervalMs = std::clamp(c.minIntervalMs, 5, 1000);
    } else {
        float step = static_cast<float>(c.stepPercent);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("One step per (% of travel)", &step, 1.0f, 50.0f, "%.0f")) c.stepPercent = step;
        ImGui::TextDisabled("Example: 5%% = 20 steps over the full travel. Sweeping fast sends at most 10 steps per movement.");
    }
}

void Ui::openEditor(int bindingIndex) {
    Profile& p = ctx_.settings.ensureActive();
    editIndex_ = bindingIndex;
    if (bindingIndex >= 0 && bindingIndex < static_cast<int>(p.bindings.size())) draft_ = p.bindings[static_cast<size_t>(bindingIndex)];
    else draft_ = Binding{};
    editorOpen_ = true;
    ctx_.engine.cancelCapture();
}

void Ui::drawBindingsTab() {
    Settings& s = ctx_.settings;
    Profile& p = s.ensureActive();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Profile");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##profile", p.name.c_str())) {
        for (const auto& prof : s.profiles) {
            const std::string label = prof.name + "   [" + prof.aircraft + "]";
            if (ImGui::Selectable(label.c_str(), prof.name == p.name)) {
                s.activeProfile = prof.name;
                viewAircraft_.clear();
                ctx_.markDirty();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Aircraft");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("##profile_aircraft", [&] {
            const Catalog* c = ctx_.catalogFor(p.aircraft);
            return (c && c->aircraft == p.aircraft) ? c->label() : p.aircraft;
        }().c_str())) {
        for (const auto& c : ctx_.catalogs) {
            if (ImGui::Selectable(c.label().c_str(), c.aircraft == p.aircraft)) {
                p.aircraft = c.aircraft;
                viewAircraft_.clear();
                ctx_.markDirty();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which aircraft this profile is for. It decides which catalog the editor offers; existing bindings are never changed.");
    ImGui::SameLine();
    if (ImGui::Button("New profile")) {
        newProfileName_ = "New profile";
        newProfileAircraft_ = p.aircraft;
        const std::string det = detectAircraft(status_.aircraft);
        if (!det.empty()) newProfileAircraft_ = det;
        renamingProfile_ = false;
        ImGui::OpenPopup("profile_name");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename")) { newProfileName_ = p.name; renamingProfile_ = true; ImGui::OpenPopup("profile_name"); }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) {
        Profile copy = p;
        copy.name = uniqueProfileName(p.name + " copy");
        s.profiles.push_back(copy);
        s.activeProfile = copy.name;
        ctx_.markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && s.profiles.size() > 1) ImGui::OpenPopup("confirm_delete_profile");
    if (ImGui::BeginPopup("profile_name")) {
        ImGui::InputText("Name", &newProfileName_);
        if (!renamingProfile_) {
            if (ImGui::BeginCombo("Aircraft##new", newProfileAircraft_.c_str())) {
                for (const auto& c : ctx_.catalogs)
                    if (ImGui::Selectable(c.label().c_str(), c.aircraft == newProfileAircraft_)) newProfileAircraft_ = c.aircraft;
                ImGui::EndCombo();
            }
        }
        if (ImGui::Button("OK") && !newProfileName_.empty()) {
            if (renamingProfile_) {
                if (newProfileName_ != p.name) { p.name = uniqueProfileName(newProfileName_); s.activeProfile = p.name; }
            } else {
                Profile created;
                created.name = uniqueProfileName(newProfileName_);
                created.aircraft = newProfileAircraft_;
                s.profiles.push_back(created);
                s.activeProfile = created.name;
                viewAircraft_.clear();
            }
            ctx_.markDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("confirm_delete_profile")) {
        ImGui::Text("Delete profile \"%s\" and its %zu bindings?", p.name.c_str(), p.bindings.size());
        if (ImGui::Button("Delete")) {
            std::string name = p.name;
            s.profiles.erase(std::remove_if(s.profiles.begin(), s.profiles.end(), [&](const Profile& x) { return x.name == name; }), s.profiles.end());
            s.activeProfile = s.profiles.front().name;
            ctx_.markDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    {
        const std::string det = detectAircraft(status_.aircraft);
        if (!det.empty() && det != p.aircraft) {
            ImGui::TextColored(kYellow, "The loaded aircraft looks like the %s, this profile is for the %s.", det.c_str(), p.aircraft.c_str());
            ImGui::SameLine();
            const Profile* match = nullptr;
            for (const auto& prof : s.profiles)
                if (prof.aircraft == det) { match = &prof; break; }
            if (match) {
                if (ImGui::SmallButton((std::string("Switch to \"") + match->name + "\"").c_str())) {
                    s.activeProfile = match->name;
                    viewAircraft_.clear();
                    ctx_.markDirty();
                }
            } else if (ImGui::SmallButton("Create a profile for it")) {
                newProfileName_ = det;
                newProfileAircraft_ = det;
                renamingProfile_ = false;
                ImGui::OpenPopup("profile_name");
            }
        }
    }
    {
        int wrong = 0;
        for (const auto& b : p.bindings) {
            double lo, hi;
            if (b.input.kind == input::InputKind::Axis && ctx_.suggestedAxisRange(b, lo, hi) && axisRangeLooksWrong(b.axis, lo, hi)) ++wrong;
        }
        if (wrong) {
            ImGui::TextColored(kYellow, "%d axis binding%s in this profile send%s an axis event with a percent-style output range, so the control barely moves.",
                               wrong, wrong == 1 ? "" : "s", wrong == 1 ? "s" : "");
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix the ranges")) ctx_.fixAxisRanges(p);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the output range of those bindings to what the event expects (for example -16383 to 16384 for a throttle axis).");
        }
    }
    ImGui::Separator();
    if (ImGui::Button("+ Add binding")) openEditor(-1);
    ImGui::SameLine();
    ImGui::TextDisabled("Rows light up while their input is held. Buttons bound here should stay unassigned in the MSFS controls menu.");

    Profile& prof = s.ensureActive();
    ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("bindings", 7, tflags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("On press", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("On release", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Repeat", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 230);
        ImGui::TableHeadersRow();

        int toDelete = -1, moveFrom = -1, moveTo = -1;
        for (int i = 0; i < static_cast<int>(prof.bindings.size()); ++i) {
            Binding& b = prof.bindings[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            if (inputIsPressed(b.input)) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.2f, 0.45f, 0.2f, 0.6f)));
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Checkbox("##on", &b.enabled)) ctx_.markDirty();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(ctx_.input.describe(b.input).c_str());
            if (b.input.kind == input::InputKind::Axis) {
                const float now = axisNow(b.input);
                ImGui::TextDisabled("%s  %s%s%s", deviceDisplayName(b.input.device).c_str(), b.axis.mode == AxisMode::Steps ? "steps" : "value",
                                    now >= 0 ? "  now " : "", now >= 0 ? (std::to_string(static_cast<int>(now * 100 + 0.5f)) + "%").c_str() : "");
            } else {
                ImGui::TextDisabled("%s", deviceDisplayName(b.input.device).c_str());
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(b.label.c_str());
            {
                double lo, hi;
                if (b.input.kind == input::InputKind::Axis && ctx_.suggestedAxisRange(b, lo, hi) && axisRangeLooksWrong(b.axis, lo, hi)) {
                    ImGui::SameLine();
                    ImGui::TextColored(kYellow, "(!)");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output range %g..%g, but the event expects %g..%g.", b.axis.outMin, b.axis.outMax, lo, hi);
                }
            }
            auto actionLine = [&](const Action& a) {
                ImGui::TextUnformatted(a.describe().c_str());
                if (a.kind == ActionKind::LVarSet || a.kind == ActionKind::LVarToggle) {
                    if (auto v = ctx_.bridge.lvarValue(a.name)) {
                        ImGui::SameLine();
                        ImGui::TextColored(*v != 0 ? kGreen : kGrey, "[now %g]", *v);
                    }
                }
            };
            ImGui::TableSetColumnIndex(3);
            for (const auto& a : b.onPress) actionLine(a);
            ImGui::TableSetColumnIndex(4);
            for (const auto& a : b.onRelease) actionLine(a);
            ImGui::TableSetColumnIndex(5);
            if (b.input.kind == input::InputKind::Axis) ImGui::TextDisabled("%s", b.axis.mode == AxisMode::Steps ? (std::to_string(static_cast<int>(b.axis.stepPercent + 0.5)) + "% step").c_str() : "axis");
            else if (b.repeat) ImGui::Text("%d ms", b.repeatIntervalMs);
            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton("Edit")) openEditor(i);
            ImGui::SameLine();
            if (ImGui::SmallButton("Test")) {
                if (b.input.kind == input::InputKind::Axis && b.axis.mode == AxisMode::Value) {
                    const float now = axisNow(b.input);
                    const double out = Engine::axisOutput(b.axis, Engine::axisPosition(b.axis, now >= 0 ? now : 0.5f));
                    ctx_.engine.fire(Engine::withValue(b.onPress, out, b.axis.integer), "test " + b.label);
                } else {
                    ctx_.engine.fire(b.onPress, "test " + b.label);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fire the press actions now");
            ImGui::SameLine();
            if (ImGui::SmallButton("Up") && i > 0) { moveFrom = i; moveTo = i - 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Down") && i + 1 < static_cast<int>(prof.bindings.size())) { moveFrom = i; moveTo = i + 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) toDelete = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (toDelete >= 0) { prof.bindings.erase(prof.bindings.begin() + toDelete); ctx_.markDirty(); }
        if (moveFrom >= 0) { std::swap(prof.bindings[static_cast<size_t>(moveFrom)], prof.bindings[static_cast<size_t>(moveTo)]); ctx_.markDirty(); }
    }
}

void Ui::drawActionList(const char* id, std::vector<Action>& actions, int listIndex) {
    ImGui::PushID(id);
    int toDelete = -1;
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        Action& a = actions[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(260);
        actionKindCombo("##kind", a.kind);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(330);
        const char* hint = a.kind == ActionKind::SimEvent ? "A32NX.FCU_AP_1_PUSH or GEAR_UP" :
                           a.kind == ActionKind::HEvent ? "A32NX_EFIS_L_CHRONO_PUSHED" :
                           a.kind == ActionKind::LVarSet ? "A32NX_AUTOBRAKES_ARMED_MODE_SET" :
                           a.kind == ActionKind::LVarToggle ? "A32NX_OVHD_APU_MASTER_SW_PB_IS_ON" : "1 (>L:A32NX_SOME_VAR)";
        ImGui::InputTextWithHint("##name", hint, &a.name);
        if (a.kind == ActionKind::SimEvent || a.kind == ActionKind::LVarSet) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::InputDouble("##value", &a.value, 0, 0, "%g");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(a.kind == ActionKind::SimEvent ? "Event parameter (0 for plain push buttons)" : "Value written to the L: var");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) toDelete = i;
        ImGui::PopID();
    }
    if (toDelete >= 0) actions.erase(actions.begin() + toDelete);
    if (ImGui::SmallButton("+ action")) actions.push_back(Action{});
    ImGui::SameLine();
    // One open-flag per list: BeginPopupModal() writes false into its p_open whenever its popup is not open, so a
    // flag shared by the press and release lists was cleared every frame by the list that was not open.
    bool& popupOpen = catalogPopupOpen_[listIndex == 0 ? 0 : 1];
    if (ImGui::SmallButton("+ from catalog")) { catalogTarget_ = listIndex; popupOpen = true; ImGui::OpenPopup("catalog_pick"); }
    ImGui::SameLine();
    if (ImGui::SmallButton("test")) {
        if (draft_.input.kind == input::InputKind::Axis && draft_.axis.mode == AxisMode::Value && listIndex == 0) {
            const float now = axisNow(draft_.input);
            const double out = Engine::axisOutput(draft_.axis, Engine::axisPosition(draft_.axis, now >= 0 ? now : 0.5f));
            ctx_.engine.fire(Engine::withValue(actions, out, draft_.axis.integer), "test (axis at its current position)");
        } else {
            ctx_.engine.fire(actions, "test");
        }
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x * 0.85f, ImGui::GetMainViewport()->WorkSize.y * 0.8f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("catalog_pick", &popupOpen, ImGuiWindowFlags_NoSavedSettings)) {
        drawCatalogBrowser("picker", true);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void Ui::drawBindingEditor() {
    ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
    const char* title = editIndex_ >= 0 ? "Edit binding###binding_editor" : "New binding###binding_editor";
    if (!ImGui::Begin(title, &editorOpen_)) { ImGui::End(); return; }

    // Input capture
    const bool capturing = ctx_.engine.capturing();
    if (auto captured = ctx_.engine.takeCaptured()) {
        draft_.input = *captured;
        // New axis binding for an axis event still on the 0..100 default: use the range the event expects.
        double lo, hi;
        if (captured->kind == input::InputKind::Axis && draft_.axis.outMin == 0 && draft_.axis.outMax == 100 &&
            ctx_.suggestedAxisRange(draft_, lo, hi) && axisRangeLooksWrong(draft_.axis, lo, hi)) {
            draft_.axis.outMin = lo;
            draft_.axis.outMax = hi;
            draft_.axis.integer = true;
        }
    }
    ImGui::TextUnformatted("Input:");
    ImGui::SameLine();
    if (draft_.input.device.empty()) ImGui::TextColored(kYellow, "none yet");
    else ImGui::Text("%s  on  %s", ctx_.input.describe(draft_.input).c_str(), deviceDisplayName(draft_.input.device).c_str());
    ImGui::SameLine();
    if (!capturing) {
        if (ImGui::Button("Listen for input")) ctx_.engine.beginCapture();
    } else {
        ImGui::TextColored(kYellow, "Press a button, move a hat, or move an axis about a quarter of its travel...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ctx_.engine.cancelCapture();
    }
    ImGui::InputTextWithHint("Label", "e.g. AP1 push", &draft_.label);

    const bool isAxis = draft_.input.kind == input::InputKind::Axis;
    const bool axisValue = isAxis && draft_.axis.mode == AxisMode::Value;
    if (isAxis) drawAxisOptions();

    ImGui::SeparatorText(!isAxis ? "On press" : axisValue ? "Actions (the axis value is the parameter)" : "Axis moves up (+)");
    if (axisValue) ImGui::TextDisabled("K: events get the value as their parameter, L: var set uses it as the value to write, and RPN code can contain {v}.");
    drawActionList("press", draft_.onPress, 0);
    if (!axisValue) {
        ImGui::SeparatorText(!isAxis ? "On release" : "Axis moves down (-)");
        if (!isAxis) ImGui::TextDisabled("Useful for 2-position switches: set a value on press, another on release.");
        drawActionList("release", draft_.onRelease, 1);
    }

    ImGui::SeparatorText("Options");
    if (!isAxis) ImGui::Checkbox("Repeat press actions while held", &draft_.repeat);
    if (!isAxis && draft_.repeat) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("delay ms", &draft_.repeatDelayMs);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("interval ms", &draft_.repeatIntervalMs);
        draft_.repeatDelayMs = std::max(0, draft_.repeatDelayMs);
        draft_.repeatIntervalMs = std::max(10, draft_.repeatIntervalMs);
    }
    ImGui::Checkbox("Enabled", &draft_.enabled);

    ImGui::Separator();
    const bool canSave = !draft_.input.device.empty();
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("Save binding", ImVec2(140, 0))) {
        Profile& p = ctx_.settings.ensureActive();
        if (editIndex_ >= 0 && editIndex_ < static_cast<int>(p.bindings.size())) p.bindings[static_cast<size_t>(editIndex_)] = draft_;
        else p.bindings.push_back(draft_);
        ctx_.markDirty();
        editorOpen_ = false;
        ctx_.engine.cancelCapture();
    }
    if (!canSave) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) { editorOpen_ = false; ctx_.engine.cancelCapture(); }
    if (!canSave) { ImGui::SameLine(); ImGui::TextDisabled("Capture an input first."); }
    ImGui::End();
    if (!editorOpen_) ctx_.engine.cancelCapture();
}

// ---------------------------------------------------------------- Devices

void Ui::drawDevicesTab() {
    if (ImGui::Button("Rescan")) ctx_.input.rescan();
    ImGui::SameLine();
    if (auto last = ctx_.engine.lastInput()) {
        if (last->id.kind == input::InputKind::Axis)
            ImGui::Text("Last input: %s on %s (moved to %d%%)", ctx_.input.describe(last->id).c_str(), deviceDisplayName(last->id.device).c_str(), static_cast<int>(last->axis * 100 + 0.5f));
        else
            ImGui::Text("Last input: %s on %s (%s)", ctx_.input.describe(last->id).c_str(), deviceDisplayName(last->id.device).c_str(), last->pressed ? "pressed" : "released");
    } else {
        ImGui::TextDisabled("Press a button or move an axis to see it here.");
    }
    if (devices_.empty()) {
        ImGui::TextColored(kYellow, "No joystick found under /dev/input. Check the Log tab for permission errors.");
    }
    for (const auto& d : devices_) {
        ImGui::PushID(d.key.c_str());
        if (ImGui::CollapsingHeader(d.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("%s   id %04x:%04x   %zu buttons, %d hats, %d axes   key: %s", d.path.c_str(), d.vendor, d.product, d.buttonCodes.size(), d.hatCount, d.axisCount, d.key.c_str());
            const float cell = ImGui::GetTextLineHeight() * 1.9f;
            const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 4)));
            for (size_t i = 0; i < d.buttonCodes.size(); ++i) {
                if (i % static_cast<size_t>(perRow) != 0) ImGui::SameLine();
                const bool on = d.buttonState[i];
                ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.2f, 0.7f, 0.3f, 1) : ImVec4(0.25f, 0.25f, 0.28f, 1));
                char label[32];
                snprintf(label, sizeof label, "%zu", i + 1);
                ImGui::Button(label, ImVec2(cell, cell));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Button %zu (evdev code %d)", i + 1, d.buttonCodes[i]);
                ImGui::PopStyleColor();
            }
            for (size_t a = 0; a < d.axisCodes.size(); ++a) {
                char label[64], overlay[16];
                snprintf(label, sizeof label, "Axis %zu (%s)", a + 1, input::axisName(d.axisCodes[a]));
                snprintf(overlay, sizeof overlay, "%d%%", static_cast<int>(d.axisValue[a] * 100 + 0.5f));
                ImGui::PushID(static_cast<int>(a));
                ImGui::SetNextItemWidth(70);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine(130);
                ImGui::ProgressBar(d.axisValue[a], ImVec2(300, 0), overlay);
                ImGui::PopID();
            }
            for (int h = 0; h < d.hatCount; ++h) {
                auto [x, y] = d.hats[static_cast<size_t>(h)];
                ImGui::Text("Hat %d: x=%d y=%d  %s%s%s%s", h + 1, x, y, y < 0 ? "Up " : "", x > 0 ? "Right " : "", y > 0 ? "Down " : "", x < 0 ? "Left" : "");
            }
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- Catalog

// One or two actions for a catalog entry: the entry itself, plus its first-officer twin when
// "both sides" is ticked and the catalog has that twin.
const Catalog& Ui::pickerCatalog() const {
    static const Catalog empty;
    const Profile* p = ctx_.settings.active();
    const Catalog* c = ctx_.catalogFor(p ? p->aircraft : kDefaultAircraft);
    return c ? *c : empty;
}

const Catalog& Ui::currentCatalog() const {
    static const Catalog empty;
    if (viewAircraft_.empty()) return pickerCatalog();
    const Catalog* c = ctx_.catalogFor(viewAircraft_);
    return c ? *c : empty;
}

std::string Ui::detectAircraft(const std::string& cfgPath) const {
    std::string low = cfgPath;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const std::pair<const char*, const char*> rules[] = {{"a380", "A380X"}, {"a320", "A32NX"}, {"a32nx", "A32NX"}};
    for (const auto& [needle, id] : rules)
        if (low.find(needle) != std::string::npos)
            for (const auto& c : ctx_.catalogs)
                if (c.aircraft == id) return id;
    return {};
}

std::string Ui::uniqueProfileName(const std::string& wanted) const {
    auto taken = [&](const std::string& n) {
        for (const auto& p : ctx_.settings.profiles)
            if (p.name == n) return true;
        return false;
    };
    if (!taken(wanted)) return wanted;
    for (int i = 2;; ++i) {
        std::string c = wanted + " " + std::to_string(i);
        if (!taken(c)) return c;
    }
}

std::vector<Action> Ui::catalogActions(const CatalogEntry& e) const {
    std::vector<Action> out = e.toActions();
    if (catalogBothSides_ && e.isCaptainSide()) {
        const std::string twin = e.firstOfficerApi();
        for (const auto& o : currentCatalog().entries)
            if (o.api == twin) { out.push_back(o.toAction()); break; }
    }
    return out;
}

static void applyEntryRangeToAxisDraft(Binding& draft, const CatalogEntry& e) {
    if (draft.input.kind == input::InputKind::Axis && draft.axis.mode == AxisMode::Value && e.hasAxisRange &&
        draft.axis.outMin == 0 && draft.axis.outMax == 100) {
        draft.axis.outMin = e.axisMin;
        draft.axis.outMax = e.axisMax;
        draft.axis.integer = true;
    }
}

void Ui::newBindingFrom(const CatalogEntry& e) {
    openEditor(-1);
    for (const auto& a : catalogActions(e)) draft_.onPress.push_back(a);
    draft_.label = e.function;
    if (e.isIncDec()) draft_.repeat = true;  // knobs on buttons: keep clicking while held
}

void Ui::drawCatalogBrowser(const char* id, bool pickMode) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(400);
    ImGui::InputTextWithHint("##filter", "filter: fcu ap, autobrake, chrono, efis ...", &catalogFilter_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu entries", currentCatalog().entries.size());
    if (pickMode) {
        ImGui::SameLine();
        ImGui::Text("-> adding to the %s list", catalogTarget_ == 0 ? "PRESS" : "RELEASE");
    }

    std::string needle = catalogFilter_;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<std::string> words;
    {
        size_t pos = 0;
        while (pos < needle.size()) {
            size_t sp = needle.find(' ', pos);
            if (sp == std::string::npos) sp = needle.size();
            if (sp > pos) words.push_back(needle.substr(pos, sp - pos));
            pos = sp + 1;
        }
    }

    const float footer = pickMode ? ImGui::GetFrameHeightWithSpacing() * 3.5f : ImGui::GetFrameHeightWithSpacing() * 2.5f;
    ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("catalog", 5, tflags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Panel", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("API", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Values / remark", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        const Catalog& cat = pickMode ? pickerCatalog() : currentCatalog();
        for (int i = 0; i < static_cast<int>(cat.entries.size()); ++i) {
            const CatalogEntry& e = cat.entries[static_cast<size_t>(i)];
            bool match = true;
            for (const auto& w : words)
                if (e.searchKey.find(w) == std::string::npos) { match = false; break; }
            if (!match) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            std::string panel = e.section.empty() ? e.panel : e.panel + " / " + e.section;
            if (ImGui::Selectable(panel.c_str(), catalogSelected_ == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                catalogSelected_ = i;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (pickMode) {
                        for (const auto& a : catalogActions(e)) (catalogTarget_ == 0 ? draft_.onPress : draft_.onRelease).push_back(a);
                        applyEntryRangeToAxisDraft(draft_, e);
                        ImGui::CloseCurrentPopup();
                    } else {
                        newBindingFrom(e);
                    }
                }
            }
            if (!e.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.description.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.function.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(e.api.c_str());
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(e.type.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(e.values.c_str());
            if (!e.remark.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", e.remark.c_str()); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const Catalog& selCat = pickMode ? pickerCatalog() : currentCatalog();
    const CatalogEntry* sel = (catalogSelected_ >= 0 && catalogSelected_ < static_cast<int>(selCat.entries.size())) ? &selCat.entries[static_cast<size_t>(catalogSelected_)] : nullptr;
    if (sel) {
        const std::vector<Action> acts = catalogActions(*sel);
        std::string desc;
        for (size_t i = 0; i < acts.size(); ++i) desc += (i ? "  +  " : "") + acts[i].describe();
        ImGui::Text("Selected: %s", desc.c_str());
        if (actionNeedsMobiFlight(acts[0].kind)) { ImGui::SameLine(); ImGui::TextColored(kYellow, "(needs MobiFlight WASM module)"); }
        if (!sel->description.empty()) ImGui::TextWrapped("%s", sel->description.c_str());
        if (sel->isCaptainSide() && !sel->firstOfficerApi().empty()) {
            ImGui::Checkbox("Both sides: also add the first officer (R) twin", &catalogBothSides_);
        }
        if (sel->isIncDec()) ImGui::TextDisabled("+/- entry: a new binding from it gets 'repeat while held' so a button acts like turning the knob.");
        if (pickMode) {
            if (ImGui::Button("Add to press")) { for (const auto& a : acts) draft_.onPress.push_back(a); applyEntryRangeToAxisDraft(draft_, *sel); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Add to release")) { for (const auto& a : acts) draft_.onRelease.push_back(a); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
        }
        if (ImGui::Button("Fire now (test)")) ctx_.engine.fire(acts, "catalog test");
        if (!pickMode) {
            ImGui::SameLine();
            if (ImGui::Button("New binding with this action")) newBindingFrom(*sel);
        }
    } else {
        ImGui::TextDisabled("Select an entry.");
    }
    if (pickMode) {
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    }
    ImGui::PopID();
}

void Ui::drawCatalogTab() {
    if (ctx_.catalogs.empty()) {
        ImGui::TextColored(kYellow, "No catalog loaded. Expected data/*_catalog.json (see tools/build_catalog.py).");
        return;
    }
    {
        const Profile* active = ctx_.settings.active();
        const std::string shown = currentCatalog().aircraft;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Aircraft");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("##catalog_aircraft", currentCatalog().label().c_str())) {
            for (const auto& c : ctx_.catalogs) {
                const bool sel = c.aircraft == shown;
                if (ImGui::Selectable(c.label().c_str(), sel)) {
                    viewAircraft_ = (active && c.aircraft == active->aircraft) ? std::string() : c.aircraft;
                    catalogSelected_ = -1;
                }
            }
            ImGui::EndCombo();
        }
        if (active && shown != active->aircraft) {
            ImGui::SameLine();
            ImGui::TextColored(kYellow, "browsing only: the active profile is for %s", active->aircraft.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("show the profile's aircraft")) { viewAircraft_.clear(); catalogSelected_ = -1; }
        }
    }
    ImGui::TextDisabled("Generated from the FlyByWire flight deck API docs plus ready-made recipes. Double-click an entry to create a binding for it; filter 'recipes' or 'baro' for the QNH helpers.");
    drawCatalogBrowser("tab", false);
}

// ---------------------------------------------------------------- Log

void Ui::drawLogTab() {
    static const char* levels[] = {"debug", "info", "warn", "error"};
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("min level", &logMinLevel_, levels, 4);
    ImGui::SameLine();
    ImGui::Checkbox("auto-scroll", &logAutoScroll_);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) Log::instance().clear();
    ImGui::SameLine();
    ImGui::Text("sent: %llu  exceptions: %llu", static_cast<unsigned long long>(status_.commandsSent), static_cast<unsigned long long>(status_.exceptions));

    const auto fires = ctx_.engine.recentFires();
    if (ImGui::BeginChild("fires", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders)) {
        ImGui::TextDisabled("Recently fired bindings");
        for (auto it = fires.rbegin(); it != fires.rend(); ++it) ImGui::TextUnformatted(it->text.c_str());
    }
    ImGui::EndChild();

    if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const auto entries = Log::instance().snapshot();
        for (const auto& e : entries) {
            if (static_cast<int>(e.level) < logMinLevel_) continue;
            ImVec4 c = e.level == LogLevel::Error ? kRed : e.level == LogLevel::Warn ? kYellow : e.level == LogLevel::Debug ? kGrey : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(c, "%s  %s", formatTime(e.time).c_str(), e.text.c_str());
        }
        uint64_t serial = Log::instance().serial();
        if (logAutoScroll_ && serial != logSerialSeen_) ImGui::SetScrollHereY(1.0f);
        logSerialSeen_ = serial;
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- Vibration

void Ui::drawVibrationTab() {
    haptics::HapticsConfig& cfg = ctx_.settings.haptics;
    const haptics::HapticsStatus hs = ctx_.haptics.status();
    bool changed = false;

    ImGui::TextWrapped("Drives the vibration motors of Winwing devices from MSFS flight data (SimAppPro-style), through the device's own "
                       "HID protocol. MSFS itself cannot vibrate these devices under Proton, so the rules below do it instead.");
    changed |= ImGui::Checkbox("Enable vibration", &cfg.enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    changed |= ImGui::SliderFloat("Master strength", &cfg.master, 0.0f, 1.5f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("Stop all motors")) ctx_.haptics.stopAll();
    if (cfg.enabled) {
        ImGui::SameLine();
        if (hs.telemetryFresh) ImGui::TextColored(kGreen, "telemetry live");
        else ImGui::TextColored(kYellow, status_.state == sim::ConnState::Connected ? "waiting for telemetry..." : "needs the simulator");
    }
    ImGui::ProgressBar(hs.left, ImVec2(220, 0), "left motor");
    ImGui::SameLine();
    ImGui::ProgressBar(hs.right, ImVec2(220, 0), "right motor");

    ImGui::SeparatorText("Devices");
    if (ImGui::Button("Rescan")) ctx_.winwing.rescan();
    const auto devices = ctx_.winwing.devices();
    if (devices.empty()) ImGui::TextColored(kYellow, "No Winwing HID device found (vendor 4098). Is the throttle/joystick plugged in?");
    for (const auto& d : devices) {
        ImGui::PushID(d.key.c_str());
        haptics::DeviceConfig& dc = cfg.devices[d.key];
        if (!dc.identity.known()) dc.identity = d.identity;
        ImGui::Text("%s", d.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s  pid %04x  %s", d.path.c_str(), d.pid, d.open ? "" : ("NOT OPEN: " + d.error).c_str());
        changed |= ImGui::Checkbox("Vibrate this device", &dc.enabled);
        const bool single = dc.identity.singleMotor();
        ImGui::SameLine();
        if (single) {
            if (ImGui::Button("Pulse")) ctx_.haptics.pulse(d.key, 0.6f, 0.6f, 500);
        } else {
            if (ImGui::Button("Pulse left")) ctx_.haptics.pulse(d.key, 0.6f, 0.0f, 500);
            ImGui::SameLine();
            if (ImGui::Button("Pulse right")) ctx_.haptics.pulse(d.key, 0.0f, 0.6f, 500);
            ImGui::SameLine();
            if (ImGui::Button("Pulse both")) ctx_.haptics.pulse(d.key, 0.6f, 0.6f, 500);
        }
        ImGui::SameLine();
        if (ImGui::Button("Motors off")) ctx_.haptics.stopAll();
        ImGui::SameLine();
        if (ImGui::Button("Discover controllers")) ctx_.winwing.discover(d.key);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Broadcast identity request: every controller behind this USB device answers with its address and name.");

        if (ImGui::TreeNode("Motor address")) {
            ImGui::TextDisabled("%s. Values are decimal. Use --motor-sweep to find the index that buzzes on an unknown product.",
                                d.identityIsDefault ? "Built-in default / auto-discovered" : "Custom");
            for (const auto& c : d.controllers) {
                ImGui::PushID(c.device);
                ImGui::Text("  controller device %d (0x%02x)  family %d  \"%s\"", c.device, c.device, c.family, c.name.c_str());
                ImGui::SameLine();
                if (single) {
                    if (ImGui::SmallButton("= motor")) { dc.identity.family = c.family; dc.identity.left = dc.identity.right = {c.device, dc.identity.left.index}; changed = true; }
                } else {
                    if (ImGui::SmallButton("= left")) { dc.identity.family = c.family; dc.identity.left.device = c.device; changed = true; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("= right")) { dc.identity.family = c.family; dc.identity.right.device = c.device; changed = true; }
                }
                ImGui::PopID();
            }
            bool two = !single;
            if (ImGui::Checkbox("Two motors (left / right)", &two)) {
                if (two) { dc.identity.right = dc.identity.left; dc.identity.right.index++; }
                else dc.identity.right = dc.identity.left;
                changed = true;
            }
            ImGui::SetNextItemWidth(70);
            changed |= ImGui::InputInt("family", &dc.identity.family, 0, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            changed |= ImGui::InputInt(single ? "device" : "L dev", &dc.identity.left.device, 0, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            changed |= ImGui::InputInt(single ? "index" : "L idx", &dc.identity.left.index, 0, 0);
            if (single) {
                dc.identity.right = dc.identity.left;
            } else {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                changed |= ImGui::InputInt("R dev", &dc.identity.right.device, 0, 0);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60);
                changed |= ImGui::InputInt("R idx", &dc.identity.right.index, 0, 0);
            }
            ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::SeparatorText("Rules");
    if (ImGui::BeginTable("rules", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Gain", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableHeadersRow();
        for (const auto& info : haptics::ruleInfos()) {
            ImGui::PushID(info.id);
            haptics::RuleConfig& rc = cfg.rules[info.id];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            changed |= ImGui::Checkbox("##on", &rc.enabled);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(info.label);
            ImGui::TextDisabled("%s", info.description);
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::SliderFloat("##gain", &rc.gain, 0.0f, 2.0f, "%.2f");
            ImGui::TableSetColumnIndex(3);
            auto it = hs.contrib.find(info.id);
            ImGui::ProgressBar(it == hs.contrib.end() ? 0.f : it->second, ImVec2(-1, 0));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Telemetry")) {
        const sim::Telemetry& t = hs.telemetry;
        if (!t.valid) ImGui::TextDisabled("no data yet");
        else
            ImGui::Text("on ground %d  GS %.0f kt  IAS %.0f kt  AGL %.0f ft  G %.2f  VS %.0f fpm\nstall %d overspeed %d  gear %.2f  flaps %.2f  spoilers %.2f  N1 %.0f/%.0f  rev %.2f/%.2f  brakes %.2f/%.2f",
                        t.onGround > 0.5, t.groundSpeedKt, t.iasKt, t.altAgl, t.gForce, t.verticalSpeedFpm, t.stallWarning > 0.5, t.overspeedWarning > 0.5,
                        t.gearPct, t.flapsPct, t.spoilersHandle, t.n1Eng1, t.n1Eng2, t.reverse1, t.reverse2, t.brakeLeft, t.brakeRight);
    }

    if (changed) {
        ctx_.dirty = true;
        ctx_.pushHapticsConfig();
    }
}

// ---------------------------------------------------------------- Settings

void Ui::drawSettingsTab() {
    Settings& s = ctx_.settings;
    ImGui::SeparatorText("SimConnect (TCP)");
    bool changed = false;
    ImGui::SetNextItemWidth(200);
    changed |= ImGui::InputText("Host", &s.host);
    ImGui::SetNextItemWidth(120);
    changed |= ImGui::InputInt("Port", &s.port);
    changed |= ImGui::Checkbox("Connect automatically", &s.autoConnect);
    if (changed) {
        s.port = std::clamp(s.port, 0, 65535);
        ctx_.bridge.setAutoConnect(s.autoConnect);
        ctx_.dirty = true;
    }
    if (ImGui::Button("Apply & reconnect")) {
        ctx_.bridge.setEndpoint(s.host, static_cast<uint16_t>(s.port));
        ctx_.bridge.connectNow();
        ctx_.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) ctx_.bridge.disconnect();
    ImGui::TextWrapped("MSFS listens where its SimConnect.xml says (default: port 500). Under Proton it cannot bind ports below 1024, so it "
                       "silently uses a random port instead; when the configured port does not answer, the tool scans wineserver's loopback "
                       "listeners for the SimConnect handshake and uses what it finds (shown in the status bar). Port 0 = scan only. "
                       "To avoid the scan, set a <Port> above 1024 in\n"
                       "steamapps/compatdata/1250410/pfx/drive_c/users/steamuser/AppData/Roaming/Microsoft Flight Simulator/SimConnect.xml");

    ImGui::SeparatorText("Files");
    ImGui::Text("Config:  %s", ctx_.configPath.c_str());
    for (const auto& c : ctx_.catalogs) ImGui::Text("Catalog (%s): %s", c.aircraft.c_str(), c.sourcePath.c_str());
    if (ctx_.catalogs.empty()) ImGui::TextUnformatted("Catalog: (none)");
    if (ImGui::Button("Save now")) ctx_.save();

    ImGui::SeparatorText("About");
    ImGui::Text("msfs-linuxrebind %s - native Linux SimConnect client, Dear ImGui %s", LINUXREBIND_VERSION, IMGUI_VERSION);
    ImGui::TextWrapped("K: events (standard and A32NX.* custom events) go straight through SimConnect. H: events, L: vars and RPN code are executed by the MobiFlight WASM module (install it in the Community folder).");
}

}  // namespace lr::app
