#include "ui/Ui.hpp"

#include <algorithm>
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
    static const ActionKind kinds[] = {ActionKind::SimEvent, ActionKind::HEvent, ActionKind::LVarSet, ActionKind::CalcCode};
    int idx = 0;
    for (int i = 0; i < 4; ++i)
        if (kinds[i] == kind) idx = i;
    bool changed = false;
    if (ImGui::BeginCombo(label, actionKindLabel(kind))) {
        for (int i = 0; i < 4; ++i) {
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

Ui::Ui(AppContext& ctx) : ctx_(ctx) {}

std::string Ui::deviceDisplayName(const std::string& key) const {
    for (const auto& d : devices_)
        if (d.key == key) return d.name;
    size_t p = key.find(':', 5);
    return p == std::string::npos ? key : key.substr(p + 1) + " (not connected)";
}

bool Ui::inputIsPressed(const input::InputId& id) const {
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
        if (ImGui::BeginTabItem("Bindings")) { drawBindingsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Devices")) { drawDevicesTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Catalog")) { drawCatalogTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Vibration")) { drawVibrationTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log")) { drawLogTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings")) { drawSettingsTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();

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

// ---------------------------------------------------------------- Bindings

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

    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Profile", p.name.c_str())) {
        for (const auto& prof : s.profiles) {
            if (ImGui::Selectable(prof.name.c_str(), prof.name == p.name)) {
                s.activeProfile = prof.name;
                ctx_.markDirty();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New profile")) { newProfileName_ = "New profile"; renamingProfile_ = false; ImGui::OpenPopup("profile_name"); }
    ImGui::SameLine();
    if (ImGui::Button("Rename")) { newProfileName_ = p.name; renamingProfile_ = true; ImGui::OpenPopup("profile_name"); }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) {
        Profile copy = p;
        copy.name = p.name + " copy";
        s.profiles.push_back(copy);
        s.activeProfile = copy.name;
        ctx_.markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && s.profiles.size() > 1) ImGui::OpenPopup("confirm_delete_profile");
    if (ImGui::BeginPopup("profile_name")) {
        ImGui::InputText("Name", &newProfileName_);
        if (ImGui::Button("OK") && !newProfileName_.empty()) {
            if (renamingProfile_) { p.name = newProfileName_; s.activeProfile = p.name; }
            else { s.profiles.push_back(Profile{newProfileName_, {}}); s.activeProfile = newProfileName_; }
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
            ImGui::TextDisabled("%s", deviceDisplayName(b.input.device).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(b.label.c_str());
            ImGui::TableSetColumnIndex(3);
            for (const auto& a : b.onPress) ImGui::TextUnformatted(a.describe().c_str());
            ImGui::TableSetColumnIndex(4);
            for (const auto& a : b.onRelease) ImGui::TextUnformatted(a.describe().c_str());
            ImGui::TableSetColumnIndex(5);
            if (b.repeat) ImGui::Text("%d ms", b.repeatIntervalMs);
            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton("Edit")) openEditor(i);
            ImGui::SameLine();
            if (ImGui::SmallButton("Test")) ctx_.engine.fire(b.onPress, "test " + b.label);
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
                           a.kind == ActionKind::LVarSet ? "A32NX_AUTOBRAKES_ARMED_MODE_SET" : "1 (>L:A32NX_SOME_VAR)";
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
    if (ImGui::SmallButton("+ from catalog")) { catalogTarget_ = listIndex; catalogPopupOpen_ = true; ImGui::OpenPopup("catalog_pick"); }
    ImGui::SameLine();
    if (ImGui::SmallButton("test")) ctx_.engine.fire(actions, "test");
    if (ImGui::BeginPopupModal("catalog_pick", &catalogPopupOpen_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x * 0.85f, ImGui::GetMainViewport()->WorkSize.y * 0.8f), ImGuiCond_Appearing);
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
    if (auto captured = ctx_.engine.takeCaptured()) draft_.input = *captured;
    ImGui::TextUnformatted("Input:");
    ImGui::SameLine();
    if (draft_.input.device.empty()) ImGui::TextColored(kYellow, "none yet");
    else ImGui::Text("%s  on  %s", ctx_.input.describe(draft_.input).c_str(), deviceDisplayName(draft_.input.device).c_str());
    ImGui::SameLine();
    if (!capturing) {
        if (ImGui::Button("Listen for input")) ctx_.engine.beginCapture();
    } else {
        ImGui::TextColored(kYellow, "Press a button or hat on your HOTAS...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ctx_.engine.cancelCapture();
    }
    ImGui::InputTextWithHint("Label", "e.g. AP1 push", &draft_.label);

    ImGui::SeparatorText("On press");
    drawActionList("press", draft_.onPress, 0);
    ImGui::SeparatorText("On release");
    ImGui::TextDisabled("Useful for 2-position switches: set a value on press, another on release.");
    drawActionList("release", draft_.onRelease, 1);

    ImGui::SeparatorText("Options");
    ImGui::Checkbox("Repeat press actions while held", &draft_.repeat);
    if (draft_.repeat) {
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
        ImGui::Text("Last input: %s on %s (%s)", ctx_.input.describe(last->id).c_str(), deviceDisplayName(last->id.device).c_str(), last->pressed ? "pressed" : "released");
    } else {
        ImGui::TextDisabled("Press a button to see it here.");
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
            for (int h = 0; h < d.hatCount; ++h) {
                auto [x, y] = d.hats[static_cast<size_t>(h)];
                ImGui::Text("Hat %d: x=%d y=%d  %s%s%s%s", h + 1, x, y, y < 0 ? "Up " : "", x > 0 ? "Right " : "", y > 0 ? "Down " : "", x < 0 ? "Left" : "");
            }
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- Catalog

void Ui::drawCatalogBrowser(const char* id, bool pickMode) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(400);
    ImGui::InputTextWithHint("##filter", "filter: fcu ap, autobrake, chrono, efis ...", &catalogFilter_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu entries from %s", ctx_.catalog.entries.size(), ctx_.catalog.aircraft.c_str());
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
        for (int i = 0; i < static_cast<int>(ctx_.catalog.entries.size()); ++i) {
            const CatalogEntry& e = ctx_.catalog.entries[static_cast<size_t>(i)];
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
                if (pickMode && ImGui::IsMouseDoubleClicked(0)) {
                    (catalogTarget_ == 0 ? draft_.onPress : draft_.onRelease).push_back(e.toAction());
                    ImGui::CloseCurrentPopup();
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

    const CatalogEntry* sel = (catalogSelected_ >= 0 && catalogSelected_ < static_cast<int>(ctx_.catalog.entries.size())) ? &ctx_.catalog.entries[static_cast<size_t>(catalogSelected_)] : nullptr;
    if (sel) {
        Action a = sel->toAction();
        ImGui::Text("Selected: %s", a.describe().c_str());
        if (actionNeedsMobiFlight(a.kind)) { ImGui::SameLine(); ImGui::TextColored(kYellow, "(needs MobiFlight WASM module)"); }
        if (!sel->description.empty()) ImGui::TextWrapped("%s", sel->description.c_str());
        if (pickMode) {
            if (ImGui::Button("Add to press")) { draft_.onPress.push_back(a); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Add to release")) { draft_.onRelease.push_back(a); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
        }
        if (ImGui::Button("Fire now (test)")) ctx_.engine.fire({a}, "catalog test");
        if (!pickMode) {
            ImGui::SameLine();
            if (ImGui::Button("New binding with this action")) {
                openEditor(-1);
                draft_.onPress.push_back(a);
                draft_.label = sel->function;
            }
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
    if (ctx_.catalog.entries.empty()) {
        ImGui::TextColored(kYellow, "No catalog loaded. Expected data/a32nx_catalog.json (see tools/build_catalog.py).");
        return;
    }
    ImGui::TextDisabled("Generated from the FlyByWire A32NX flight deck API docs. Only writable / triggerable entries are listed.");
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
        if (dc.identity.deviceByte < 0) dc.identity = d.identity;
        ImGui::Text("%s", d.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s  pid %04x  %s", d.path.c_str(), d.pid, d.open ? "" : ("NOT OPEN: " + d.error).c_str());
        changed |= ImGui::Checkbox("Vibrate this device", &dc.enabled);
        ImGui::SameLine();
        if (ImGui::Button("Pulse left")) ctx_.haptics.pulse(d.key, 0.6f, 0.0f, 500);
        ImGui::SameLine();
        if (ImGui::Button("Pulse right")) ctx_.haptics.pulse(d.key, 0.0f, 0.6f, 500);
        ImGui::SameLine();
        if (ImGui::Button("Pulse both")) ctx_.haptics.pulse(d.key, 0.6f, 0.6f, 500);
        ImGui::SameLine();
        if (ImGui::Button("Probe address")) {
            lastProbe_ = ctx_.winwing.probe(d.key, std::max(0, dc.identity.leftIndex));
            lastProbeKey_ = d.key;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sends a harmless 'motor off' frame to each candidate address and lists which ones the firmware echoes back.");
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::InputInt("device byte", &dc.identity.deviceByte, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::InputInt("family byte", &dc.identity.familyByte, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::InputInt("left motor idx", &dc.identity.leftIndex);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::InputInt("right motor idx", &dc.identity.rightIndex);
        ImGui::SameLine();
        ImGui::TextDisabled("(hex shown as decimal: 0x10=16, 0xb9=185, 0xbf=191, 0x0e=14) %s", d.identityIsDefault ? "default table" : "custom");
        if (lastProbeKey_ == d.key && !lastProbe_.empty()) {
            for (const auto& r : lastProbe_) {
                ImGui::TextColored(r.echoed ? kGreen : kGrey, "  device 0x%02x family 0x%02x: %s", r.deviceByte, r.familyByte, r.echoed ? "echo - accepted" : "no answer");
                if (r.echoed) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("use")) { dc.identity.deviceByte = r.deviceByte; dc.identity.familyByte = r.familyByte; changed = true; }
                }
            }
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
        s.port = std::clamp(s.port, 1, 65535);
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
    ImGui::TextWrapped("MSFS listens where its SimConnect.xml says. Under Proton that file lives in\n"
                       "steamapps/compatdata/1250410/pfx/drive_c/users/steamuser/AppData/Roaming/Microsoft Flight Simulator/SimConnect.xml\n"
                       "and needs an IPv4 entry with a fixed <Port> (500 by default) and <Scope>local</Scope>.");

    ImGui::SeparatorText("Files");
    ImGui::Text("Config:  %s", ctx_.configPath.c_str());
    ImGui::Text("Catalog: %s", ctx_.catalog.sourcePath.empty() ? "(none)" : ctx_.catalog.sourcePath.c_str());
    if (ImGui::Button("Save now")) ctx_.save();

    ImGui::SeparatorText("About");
    ImGui::Text("msfs-linuxrebind %s - native Linux SimConnect client, Dear ImGui %s", LINUXREBIND_VERSION, IMGUI_VERSION);
    ImGui::TextWrapped("K: events (standard and A32NX.* custom events) go straight through SimConnect. H: events, L: vars and RPN code are executed by the MobiFlight WASM module (install it in the Community folder).");
}

}  // namespace lr::app
