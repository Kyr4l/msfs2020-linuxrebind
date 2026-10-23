// Axis binding logic: mapping, value substitution, rate limiting, step counting, capture, config round trip.
#include <chrono>
#include <cmath>
#include <fstream>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>

#include "app/Engine.hpp"
#include "app/Paths.hpp"
#include "app/Profile.hpp"
#include "sim/SimBridge.hpp"

using namespace lr;
using namespace lr::app;
using lr::input::InputEvent;
using lr::input::InputId;
using lr::input::InputKind;

static int failures = 0;
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } \
    } while (0)

static InputEvent axisEvent(const char* dev, int code, float prev, float now) {
    InputEvent e;
    e.id = {dev, InputKind::Axis, code, 0};
    e.axis = now;
    e.axisPrev = prev;
    e.time = std::chrono::steady_clock::now();
    return e;
}

static size_t firesNamed(Engine& eng, const std::string& needle) {
    size_t n = 0;
    for (const auto& f : eng.recentFires())
        if (f.text.find(needle) != std::string::npos) ++n;
    return n;
}

static std::string lastFire(Engine& eng) {
    auto f = eng.recentFires();
    return f.empty() ? std::string() : f.back().text;
}

int main() {
    // ---- pure helpers
    {
        AxisConfig c;  // value mode 0..100, integer
        CHECK(Engine::axisPosition(c, 0.25) == 0.25);
        CHECK(Engine::axisOutput(c, 0.256) == 26.0);
        c.invert = true;
        CHECK(Engine::axisPosition(c, 0.25) == 0.75);
        c.invert = false;
        c.inMin = 0.1;
        c.inMax = 0.9;  // 10% dead zone at each end
        CHECK(Engine::axisPosition(c, 0.05) == 0.0);
        CHECK(Engine::axisPosition(c, 0.95) == 1.0);
        CHECK(std::fabs(Engine::axisPosition(c, 0.5) - 0.5) < 1e-9);
        c.outMin = 100;
        c.outMax = 0;  // reversed output range
        CHECK(Engine::axisOutput(c, 0.25) == 75.0);
        CHECK(Engine::formatValue(12.0, true) == "12");
        CHECK(Engine::formatValue(0.5, false) == "0.5");
    }
    {
        std::vector<Action> acts(5);
        acts[0].kind = ActionKind::SimEvent; acts[0].name = "X";
        acts[1].kind = ActionKind::LVarSet; acts[1].name = "Y";
        acts[2].kind = ActionKind::CalcCode; acts[2].name = "{v} (>L:Z) {v} (>L:W)";
        acts[3].kind = ActionKind::HEvent; acts[3].name = "H";
        acts[4].kind = ActionKind::LVarToggle; acts[4].name = "T"; acts[4].value = 7;
        auto out = Engine::withValue(acts, 42.0, true);
        CHECK(out[0].value == 42.0 && out[1].value == 42.0);
        CHECK(out[2].name == "42 (>L:Z) 42 (>L:W)");
        CHECK(out[3].name == "H" && out[4].value == 7);
        CHECK(acts[2].name == "{v} (>L:Z) {v} (>L:W)");  // the original is untouched
    }

    // ---- axis event ranges (the 0..100 default on a -16384..16383 event is what left the throttle levers motionless)
    {
        double lo = 0, hi = 0;
        CHECK(axisEventRange("THROTTLE1_AXIS_SET_EX1", lo, hi) && lo == -16384 && hi == 16383);
        CHECK(axisEventRange("K:THROTTLE4_AXIS_SET_EX1", lo, hi));
        CHECK(axisEventRange("AXIS_LEFT_BRAKE_SET", lo, hi) && lo == -16383 && hi == 16383);
        CHECK(!axisEventRange("GEAR_UP", lo, hi));
        CHECK(!axisEventRange("A32NX.FCU_HDG_SET", lo, hi));
        AxisConfig c;  // default 0..100
        CHECK(axisRangeLooksWrong(c, -16384, 16383));
        CHECK(!axisRangeLooksWrong(c, 100, 49000 - 48000));  // span below 1000: not an axis event
        c.outMin = -16384;
        c.outMax = 16383;
        CHECK(!axisRangeLooksWrong(c, -16384, 16383));
        c.mode = AxisMode::Steps;
        c.outMin = 0;
        c.outMax = 100;
        CHECK(!axisRangeLooksWrong(c, -16384, 16383));  // steps mode has no output range
    }

    // ---- the panel "photos >" link only ever opens the FlyByWire docs site and never goes through a shell
    {
        CHECK(!openDocsUrl("https://example.com/"));
        CHECK(!openDocsUrl("http://docs.flybywiresim.com/pilots-corner/"));
        CHECK(!openDocsUrl("https://docs.flybywiresim.com.evil.example/x"));
        CHECK(!openDocsUrl("https://docs.flybywiresim.com/a;touch /tmp/pwned"));
        CHECK(!openDocsUrl("https://docs.flybywiresim.com/a$(id)"));
        CHECK(!openDocsUrl("https://docs.flybywiresim.com/a b"));
        CHECK(!openDocsUrl("https://docs.flybywiresim.com/a?x=1"));
        CHECK(!openDocsUrl(""));
    }

    // ---- engine behaviour
    sim::SimBridge bridge;  // never started: commands are queued/dropped, only the engine's decisions are checked
    Engine eng(bridge);
    const char* dev = "4098:b970:test";

    auto prof = std::make_shared<Profile>();
    {
        Binding v;  // value mode
        v.label = "Brightness";
        v.input = {dev, InputKind::Axis, 3, 0};
        v.axis.mode = AxisMode::Value;
        v.axis.minIntervalMs = 40;
        Action a;
        a.kind = ActionKind::LVarSet;
        a.name = "BRT";
        v.onPress = {a};
        prof->bindings.push_back(v);

        Binding s;  // steps mode
        s.label = "Knob";
        s.input = {dev, InputKind::Axis, 4, 0};
        s.axis.mode = AxisMode::Steps;
        s.axis.stepPercent = 10;
        Action up, down;
        up.kind = ActionKind::SimEvent; up.name = "INC";
        down.kind = ActionKind::SimEvent; down.name = "DEC";
        s.onPress = {up};
        s.onRelease = {down};
        prof->bindings.push_back(s);
    }
    eng.setProfile(prof);

    // value: first move sends at once with the mapped value
    eng.onInput(axisEvent(dev, 3, 0.0f, 0.50f));
    CHECK(firesNamed(eng, "Brightness = 50") == 1);
    // quick follow-ups are held back, only the latest is sent after the interval (and replaces the entry)
    eng.onInput(axisEvent(dev, 3, 0.50f, 0.60f));
    eng.onInput(axisEvent(dev, 3, 0.60f, 0.70f));
    CHECK(lastFire(eng).find("= 50") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    eng.tick();
    CHECK(lastFire(eng).find("Brightness = 70") != std::string::npos);
    // a change too small to change the rounded output sends nothing
    const size_t before = eng.recentFires().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    eng.onInput(axisEvent(dev, 3, 0.70f, 0.701f));
    eng.tick();
    CHECK(eng.recentFires().size() == before);

    // steps: 10% per step
    eng.onInput(axisEvent(dev, 4, 0.50f, 0.55f));
    CHECK(firesNamed(eng, "Knob (+)") == 0);            // 5% < one step
    eng.onInput(axisEvent(dev, 4, 0.55f, 0.72f));
    CHECK(firesNamed(eng, "Knob (+)") == 2);            // 22% travelled from the anchor = two steps
    // the anchor moved to 0.70 after the two steps up; 0.70 -> 0.51 is 1.9 steps, which counts as one (no rounding up)
    eng.onInput(axisEvent(dev, 4, 0.72f, 0.51f));
    CHECK(firesNamed(eng, "Knob (-)") == 1);
    eng.onInput(axisEvent(dev, 4, 0.51f, 0.39f));       // anchor 0.60 -> 0.39 = 2.1 steps = two more
    CHECK(firesNamed(eng, "Knob (-)") == 3);
    eng.onInput(axisEvent(dev, 4, 0.39f, 0.99f));       // anchor 0.40 -> 0.99 = 5.9 steps = five
    CHECK(firesNamed(eng, "Knob (+)") == 2 + 5);
    eng.onInput(axisEvent(dev, 4, 0.99f, 0.0f));        // a huge sweep sends at most ten steps, then resynchronises
    CHECK(firesNamed(eng, "Knob (-)") <= 3 + 10);
    CHECK(firesNamed(eng, "Knob (-)") >= 3 + 8);  // 0.9 of travel is 8-9 steps depending on float rounding

    // a different axis / device does not trigger anything
    const size_t n = eng.recentFires().size();
    eng.onInput(axisEvent(dev, 5, 0.0f, 1.0f));
    eng.onInput(axisEvent("other", 3, 0.0f, 1.0f));
    CHECK(eng.recentFires().size() == n);

    // capture: small drift is ignored, a real move is captured
    eng.beginCapture();
    eng.onInput(axisEvent(dev, 6, 0.50f, 0.54f));
    CHECK(eng.capturing());
    eng.onInput(axisEvent(dev, 6, 0.54f, 0.80f));
    CHECK(!eng.capturing());
    auto got = eng.takeCaptured();
    CHECK(got && got->kind == InputKind::Axis && got->code == 6);
    // capturing never fires bindings
    eng.beginCapture();
    const size_t m = eng.recentFires().size();
    eng.onInput(axisEvent(dev, 3, 0.0f, 1.0f));
    CHECK(eng.recentFires().size() == m);
    eng.cancelCapture();

    // ---- config round trip with an axis binding; plain bindings stay unchanged on disk
    {
        Settings st;
        st.profiles.push_back(*prof);
        st.activeProfile = prof->name;
        Binding btn;
        btn.label = "plain";
        btn.input = {dev, InputKind::Button, 288, 0};
        st.profiles[0].bindings.push_back(btn);
        st.profiles[0].bindings[0].axis.invert = true;
        st.profiles[0].bindings[0].axis.outMax = 255;
        st.profiles[0].bindings[1].axis.stepPercent = 7.5;
        const auto path = (std::filesystem::temp_directory_path() / "msfs-linuxrebind-axis-test.json").string();
        std::string err;
        CHECK(saveSettings(path, st, err));
        Settings back;
        CHECK(loadSettings(path, back, err));
        const auto& b = back.profiles[0].bindings;
        CHECK(b.size() == 3);
        CHECK(b[0].input.kind == InputKind::Axis && b[0].axis.invert && b[0].axis.outMax == 255 && b[0].axis.mode == AxisMode::Value);
        CHECK(b[1].axis.mode == AxisMode::Steps && b[1].axis.stepPercent == 7.5 && b[1].onRelease.size() == 1);
        CHECK(b[2].input.kind == InputKind::Button);
        std::ifstream in(path);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        // exactly two "axis" objects are written (the two axis bindings), none for the plain button
        size_t count = 0;
        for (size_t pos = 0; (pos = text.find("\"step_percent\"", pos)) != std::string::npos; ++pos) ++count;
        CHECK(count == 2);
        std::filesystem::remove(path);
    }

    std::printf(failures ? "\n%d check(s) FAILED\n" : "all axis checks passed\n", failures);
    return failures ? 1 : 0;
}
