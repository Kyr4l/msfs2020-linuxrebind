#pragma once
// Joystick input straight from the Linux evdev interface (/dev/input/event*), no SDL needed.
// Runs on its own thread so bindings keep working whatever the UI thread is doing.
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lr::input {

enum class InputKind { Button, Hat };

struct InputId {
    std::string device;  // stable device key "vvvv:pppp:Name"
    InputKind kind = InputKind::Button;
    int code = 0;        // evdev key code for buttons, hat index (0..3) for hats
    int dir = 0;         // hats only: 0 up, 1 right, 2 down, 3 left
    bool operator==(const InputId& o) const { return device == o.device && kind == o.kind && code == o.code && dir == o.dir; }
};

struct InputEvent {
    InputId id;
    bool pressed = false;
    std::chrono::steady_clock::time_point time;
};

struct DeviceSnapshot {
    std::string key;
    std::string name;
    std::string path;
    uint16_t vendor = 0, product = 0;
    std::vector<int> buttonCodes;   // sorted; index = joystick button number
    std::vector<bool> buttonState;  // parallel to buttonCodes
    int hatCount = 0;
    std::array<std::pair<int, int>, 4> hats{};  // (x, y) in -1..1
    int axisCount = 0;
};

std::string hatDirName(int dir);
const char* kindName(InputKind k);

class InputManager {
public:
    using EventCallback = std::function<void(const InputEvent&)>;
    using TickCallback = std::function<void()>;

    InputManager();
    ~InputManager();

    void start(EventCallback onEvent, TickCallback onTick);
    void stop();
    void rescan();
    int scanCount() const { return scans_.load(); }  // 0 until the first scan finished

    std::vector<DeviceSnapshot> devices() const;
    // "Btn 12" / "Hat 1 Up"; falls back to the raw code when the device is unknown.
    std::string describe(const InputId& id) const;
    int buttonIndex(const std::string& deviceKey, int code) const;

private:
    struct Device;
    void threadMain();
    void scanDevices();
    void closeDevice(size_t idx);
    bool openDevice(const std::string& path);
    void readDevice(size_t idx);

    std::vector<Device> devices_;
    mutable std::mutex m_;
    std::vector<DeviceSnapshot> snapshots_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> rescanRequested_{true};
    std::atomic<int> scans_{0};
    EventCallback onEvent_;
    TickCallback onTick_;
    int inotifyFd_ = -1;
    std::vector<std::string> failedPaths_;
};

}  // namespace lr::input
