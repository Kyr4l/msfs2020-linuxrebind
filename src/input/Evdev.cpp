#include "input/Evdev.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util/Log.hpp"

namespace lr::input {

namespace {
constexpr int NBITS(int x) { return (x + 7) / 8; }
bool testBit(const unsigned char* bits, int bit) { return (bits[bit / 8] >> (bit % 8)) & 1; }

bool isJoystickButton(int code) {
    return (code >= BTN_JOYSTICK && code <= BTN_DEAD) || (code >= BTN_GAMEPAD && code <= BTN_THUMBR) ||
           (code >= BTN_TRIGGER_HAPPY && code <= BTN_TRIGGER_HAPPY40 + 24);
}
}  // namespace

std::string hatDirName(int dir) {
    switch (dir) {
        case 0: return "Up";
        case 1: return "Right";
        case 2: return "Down";
        case 3: return "Left";
    }
    return "?";
}

const char* kindName(InputKind k) {
    switch (k) {
        case InputKind::Button: return "button";
        case InputKind::Hat: return "hat";
        case InputKind::Axis: return "axis";
    }
    return "button";
}

InputKind kindFromName(const std::string& name) {
    if (name == "hat") return InputKind::Hat;
    if (name == "axis") return InputKind::Axis;
    return InputKind::Button;
}

const char* axisName(int evdevCode) {
    // Same order as HID usages 0x30..0x37 (X Y Z Rx Ry Rz Slider Dial), which is how Linux numbers them.
    static const char* const names[] = {"X", "Y", "Z", "Rx", "Ry", "Rz", "Slider", "Dial"};
    return (evdevCode >= 0 && evdevCode < 8) ? names[evdevCode] : "?";
}

struct InputManager::Device {
    int fd = -1;
    std::string path;
    DeviceSnapshot snap;
    std::array<bool, KEY_MAX + 1> keyState{};
    std::array<int, 4> hatX{};
    std::array<int, 4> hatY{};
    std::array<bool, 16> hasAxis{};
    std::array<int, 16> axisMin{}, axisMax{};
    std::array<float, 16> axisReported{};  // last position reported to listeners (normalised)
};

InputManager::InputManager() = default;
InputManager::~InputManager() { stop(); }

void InputManager::start(EventCallback onEvent, TickCallback onTick) {
    if (running_) return;
    onEvent_ = std::move(onEvent);
    onTick_ = std::move(onTick);
    running_ = true;
    thread_ = std::thread([this] { threadMain(); });
}

void InputManager::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void InputManager::rescan() { rescanRequested_ = true; }

std::vector<DeviceSnapshot> InputManager::devices() const {
    std::lock_guard<std::mutex> lock(m_);
    return snapshots_;
}

int InputManager::buttonIndex(const std::string& deviceKey, int code) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const auto& s : snapshots_) {
        if (s.key != deviceKey) continue;
        auto it = std::find(s.buttonCodes.begin(), s.buttonCodes.end(), code);
        if (it != s.buttonCodes.end()) return static_cast<int>(it - s.buttonCodes.begin());
    }
    return -1;
}

std::string InputManager::describe(const InputId& id) const {
    if (id.kind == InputKind::Hat) return "Hat " + std::to_string(id.code + 1) + " " + hatDirName(id.dir);
    if (id.kind == InputKind::Axis) {
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& sn : snapshots_) {
            if (sn.key != id.device) continue;
            auto it = std::find(sn.axisCodes.begin(), sn.axisCodes.end(), id.code);
            if (it != sn.axisCodes.end()) return "Axis " + std::to_string(it - sn.axisCodes.begin() + 1) + " (" + axisName(id.code) + ")";
        }
        return std::string("Axis ") + axisName(id.code);
    }
    int idx = buttonIndex(id.device, id.code);
    if (idx >= 0) return "Btn " + std::to_string(idx + 1);
    // Unknown device right now: fall back to joydev-style numbering for the common ranges.
    if (id.code >= BTN_JOYSTICK && id.code <= BTN_DEAD) return "Btn " + std::to_string(id.code - BTN_JOYSTICK + 1) + "?";
    return "key " + std::to_string(id.code);
}

bool InputManager::openDevice(const std::string& path) {
    for (const auto& d : devices_)
        if (d.path == path) return true;

    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (std::find(failedPaths_.begin(), failedPaths_.end(), path) == failedPaths_.end()) {
            failedPaths_.push_back(path);
            if (errno == EACCES) LOG_WARN("Cannot open %s: permission denied (add your user to the 'input' group or install a udev uaccess rule)", path.c_str());
        }
        return false;
    }

    unsigned char evbits[NBITS(EV_MAX)]{}, keybits[NBITS(KEY_MAX)]{}, absbits[NBITS(ABS_MAX)]{}, relbits[NBITS(REL_MAX)]{};
    ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits);
    if (!testBit(evbits, EV_KEY)) { ::close(fd); return false; }
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
    if (testBit(evbits, EV_ABS)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    if (testBit(evbits, EV_REL)) ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits);

    Device d;
    for (int code = 0; code <= KEY_MAX; ++code)
        if (testBit(keybits, code) && isJoystickButton(code)) d.snap.buttonCodes.push_back(code);
    const bool looksLikeKeyboard = testBit(keybits, KEY_Q) && testBit(keybits, KEY_A) && testBit(keybits, KEY_Z);
    const bool looksLikeMouse = testBit(keybits, BTN_LEFT) && testBit(relbits, REL_X);
    if (d.snap.buttonCodes.empty() || looksLikeKeyboard || looksLikeMouse) { ::close(fd); return false; }

    for (int h = 0; h < 4; ++h)
        if (testBit(absbits, ABS_HAT0X + 2 * h) || testBit(absbits, ABS_HAT0Y + 2 * h)) d.snap.hatCount = h + 1;
    for (int a = ABS_X; a < ABS_HAT0X; ++a) {
        if (!testBit(absbits, a)) continue;
        input_absinfo ai{};
        if (ioctl(fd, EVIOCGABS(a), &ai) != 0 || ai.maximum <= ai.minimum) continue;
        d.snap.axisCount++;
        d.hasAxis[static_cast<size_t>(a)] = true;
        d.axisMin[static_cast<size_t>(a)] = ai.minimum;
        d.axisMax[static_cast<size_t>(a)] = ai.maximum;
        const float norm = static_cast<float>(ai.value - ai.minimum) / static_cast<float>(ai.maximum - ai.minimum);
        d.axisReported[static_cast<size_t>(a)] = std::clamp(norm, 0.0f, 1.0f);
        d.snap.axisCodes.push_back(a);
        d.snap.axisValue.push_back(d.axisReported[static_cast<size_t>(a)]);
    }

    char name[256] = "Unknown";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    input_id iid{};
    ioctl(fd, EVIOCGID, &iid);
    char key[320];
    snprintf(key, sizeof key, "%04x:%04x:%s", iid.vendor, iid.product, name);

    unsigned char cur[NBITS(KEY_MAX)]{};
    ioctl(fd, EVIOCGKEY(sizeof cur), cur);
    for (int code : d.snap.buttonCodes) d.keyState[code] = testBit(cur, code);
    for (int h = 0; h < d.snap.hatCount; ++h) {
        input_absinfo ai{};
        if (ioctl(fd, EVIOCGABS(ABS_HAT0X + 2 * h), &ai) == 0) d.hatX[h] = ai.value;
        if (ioctl(fd, EVIOCGABS(ABS_HAT0Y + 2 * h), &ai) == 0) d.hatY[h] = ai.value;
        d.snap.hats[h] = {d.hatX[h], d.hatY[h]};
    }

    d.fd = fd;
    d.path = path;
    d.snap.key = key;
    d.snap.name = name;
    d.snap.path = path;
    d.snap.vendor = iid.vendor;
    d.snap.product = iid.product;
    d.snap.buttonState.assign(d.snap.buttonCodes.size(), false);
    for (size_t i = 0; i < d.snap.buttonCodes.size(); ++i) d.snap.buttonState[i] = d.keyState[d.snap.buttonCodes[i]];
    LOG_INFO("Joystick: %s (%s, %zu buttons, %d hats, %d axes)", name, path.c_str(), d.snap.buttonCodes.size(), d.snap.hatCount, d.snap.axisCount);
    devices_.push_back(std::move(d));
    return true;
}

void InputManager::closeDevice(size_t idx) {
    LOG_INFO("Joystick removed: %s", devices_[idx].snap.name.c_str());
    ::close(devices_[idx].fd);
    devices_.erase(devices_.begin() + static_cast<long>(idx));
}

// Reads /sys/class/input/eventN/device/capabilities/key (world readable) to decide whether a node
// is worth opening, so we never touch keyboards/mice and do not spam permission warnings for them.
static bool sysfsLooksLikeJoystick(const std::string& eventName) {
    FILE* f = fopen(("/sys/class/input/" + eventName + "/device/capabilities/key").c_str(), "r");
    if (!f) return true;  // cannot tell, let openDevice() decide
    char line[4096];
    if (!fgets(line, sizeof line, f)) { fclose(f); return true; }
    fclose(f);
    std::vector<unsigned long long> words;
    char* save = nullptr;
    for (char* tok = strtok_r(line, " \n", &save); tok; tok = strtok_r(nullptr, " \n", &save)) words.push_back(strtoull(tok, nullptr, 16));
    auto bit = [&](int code) {
        const size_t word = static_cast<size_t>(code / 64);
        if (word >= words.size()) return false;
        return ((words[words.size() - 1 - word] >> (code % 64)) & 1ULL) != 0;
    };
    for (int c = BTN_JOYSTICK; c <= BTN_DEAD; ++c) if (bit(c)) return true;
    for (int c = BTN_GAMEPAD; c <= BTN_THUMBR; ++c) if (bit(c)) return true;
    for (int c = BTN_TRIGGER_HAPPY; c <= BTN_TRIGGER_HAPPY40 + 24; ++c) if (bit(c)) return true;
    return false;
}

void InputManager::scanDevices() {
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    std::vector<std::string> paths;
    while (dirent* e = readdir(dir)) {
        if (strncmp(e->d_name, "event", 5) == 0 && sysfsLooksLikeJoystick(e->d_name)) paths.push_back(std::string("/dev/input/") + e->d_name);
    }
    closedir(dir);
    std::sort(paths.begin(), paths.end());
    for (const auto& p : paths) openDevice(p);
    // Forget failures for paths that no longer exist so a re-plug is retried.
    failedPaths_.erase(std::remove_if(failedPaths_.begin(), failedPaths_.end(),
                                      [&](const std::string& p) { return std::find(paths.begin(), paths.end(), p) == paths.end(); }),
                       failedPaths_.end());
    {
        std::lock_guard<std::mutex> lock(m_);
        snapshots_.clear();
        for (const auto& d : devices_) snapshots_.push_back(d.snap);
    }
    scans_++;
}

void InputManager::readDevice(size_t idx) {
    Device& d = devices_[idx];
    input_event evs[64];
    for (;;) {
        ssize_t n = ::read(d.fd, evs, sizeof evs);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            closeDevice(idx);
            rescanRequested_ = true;
            return;
        }
        if (n == 0) return;
        const size_t count = static_cast<size_t>(n) / sizeof(input_event);
        bool stateChanged = false;
        for (size_t i = 0; i < count; ++i) {
            const input_event& ev = evs[i];
            if (ev.type == EV_KEY) {
                if (ev.value == 2) continue;  // key repeat
                if (!isJoystickButton(ev.code)) continue;
                const bool pressed = ev.value != 0;
                if (d.keyState[ev.code] == pressed) continue;
                d.keyState[ev.code] = pressed;
                stateChanged = true;
                if (onEvent_) {
                    InputEvent ie;
                    ie.id = {d.snap.key, InputKind::Button, ev.code, 0};
                    ie.pressed = pressed;
                    ie.time = std::chrono::steady_clock::now();
                    onEvent_(ie);
                }
            } else if (ev.type == EV_ABS && ev.code < ABS_HAT0X && d.hasAxis[ev.code]) {
                const size_t c = ev.code;
                float norm = static_cast<float>(ev.value - d.axisMin[c]) / static_cast<float>(d.axisMax[c] - d.axisMin[c]);
                norm = std::clamp(norm, 0.0f, 1.0f);
                const float prev = d.axisReported[c];
                // Report real movement only; always report arriving at either end so full travel is reachable.
                const bool atEnd = (norm == 0.0f || norm == 1.0f) && norm != prev;
                if (std::fabs(norm - prev) < kAxisReportStep && !atEnd) continue;
                d.axisReported[c] = norm;
                stateChanged = true;
                if (onEvent_) {
                    InputEvent ie;
                    ie.id = {d.snap.key, InputKind::Axis, ev.code, 0};
                    ie.axis = norm;
                    ie.axisPrev = prev;
                    ie.time = std::chrono::steady_clock::now();
                    onEvent_(ie);
                }
            } else if (ev.type == EV_ABS && ev.code >= ABS_HAT0X && ev.code <= ABS_HAT3Y) {
                const int hat = (ev.code - ABS_HAT0X) / 2;
                const bool isX = ((ev.code - ABS_HAT0X) % 2) == 0;
                int& cur = isX ? d.hatX[hat] : d.hatY[hat];
                const int newVal = ev.value < 0 ? -1 : (ev.value > 0 ? 1 : 0);
                if (cur == newVal) continue;
                const int negDir = isX ? 3 : 0;  // left / up
                const int posDir = isX ? 1 : 2;  // right / down
                auto emit = [&](int dir, bool pressed) {
                    if (!onEvent_) return;
                    InputEvent ie;
                    ie.id = {d.snap.key, InputKind::Hat, hat, dir};
                    ie.pressed = pressed;
                    ie.time = std::chrono::steady_clock::now();
                    onEvent_(ie);
                };
                if (cur == -1) emit(negDir, false);
                if (cur == 1) emit(posDir, false);
                cur = newVal;
                if (cur == -1) emit(negDir, true);
                if (cur == 1) emit(posDir, true);
                stateChanged = true;
            }
        }
        if (stateChanged) {
            std::lock_guard<std::mutex> lock(m_);
            for (auto& s : snapshots_) {
                if (s.key != d.snap.key) continue;
                for (size_t b = 0; b < s.buttonCodes.size(); ++b) s.buttonState[b] = d.keyState[s.buttonCodes[b]];
                for (int h = 0; h < 4; ++h) s.hats[h] = {d.hatX[h], d.hatY[h]};
                for (size_t a = 0; a < s.axisCodes.size(); ++a) s.axisValue[a] = d.axisReported[static_cast<size_t>(s.axisCodes[a])];
            }
        }
    }
}

void InputManager::threadMain() {
    inotifyFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd_ >= 0) inotify_add_watch(inotifyFd_, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB);

    auto rescanAt = std::chrono::steady_clock::now();
    bool rescanPending = true;
    std::vector<pollfd> pfds;
    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (rescanRequested_.exchange(false)) {
            rescanPending = true;
            rescanAt = now + std::chrono::milliseconds(300);  // let udev finish setting permissions
        }
        if (rescanPending && now >= rescanAt) {
            rescanPending = false;
            scanDevices();
        }

        pfds.clear();
        for (const auto& d : devices_) pfds.push_back({d.fd, POLLIN, 0});
        if (inotifyFd_ >= 0) pfds.push_back({inotifyFd_, POLLIN, 0});
        int rc = ::poll(pfds.data(), pfds.size(), 10);
        if (rc > 0) {
            for (size_t i = 0; i < devices_.size(); ++i) {
                if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                    readDevice(i);
                    if (i >= devices_.size()) break;
                }
            }
            if (inotifyFd_ >= 0 && (pfds.back().revents & POLLIN)) {
                char buf[4096];
                while (::read(inotifyFd_, buf, sizeof buf) > 0) {}
                rescanRequested_ = true;
            }
        }
        if (onTick_) onTick_();
    }
    for (auto& d : devices_) ::close(d.fd);
    devices_.clear();
    if (inotifyFd_ >= 0) ::close(inotifyFd_);
    inotifyFd_ = -1;
}

}  // namespace lr::input
