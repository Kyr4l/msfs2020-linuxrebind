#include "haptics/WinwingHid.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "util/Log.hpp"

namespace lr::haptics {

namespace {
constexpr uint8_t REPORT_ID = 0x02;
constexpr uint8_t OP_SET_LEDX = 0x49;
constexpr size_t FRAME_LEN = 14;

bool isThrottlePid(uint16_t pid) { return (pid & 0xff00) == 0xb900; }

void buildSetLedx(uint8_t* f, int device, int family, int index, int value) {
    std::memset(f, 0, FRAME_LEN);
    f[0] = REPORT_ID;
    f[1] = static_cast<uint8_t>(device);
    f[2] = static_cast<uint8_t>(family);
    f[5] = 0x03;  // payload length: opcode + 2 args
    f[6] = OP_SET_LEDX;
    f[7] = static_cast<uint8_t>(index);
    f[8] = static_cast<uint8_t>(value);
}

std::string readSysfs(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[512];
    std::string out;
    while (fgets(buf, sizeof buf, f)) out += buf;
    fclose(f);
    return out;
}
}  // namespace

WinwingIdentity defaultIdentityForPid(uint16_t pid) {
    WinwingIdentity id;
    switch (pid) {
        case 0xb920:  // URSA MINOR 32 Throttle Metal L
        case 0xb930:  // URSA MINOR 32 Throttle Metal R
        case 0xb970:  // URSA MINOR Combat Throttle Metal.EX (assumed same addressing, confirm with Probe)
            id.deviceByte = 0x10;
            id.familyByte = 0xb9;
            id.leftIndex = 0x0e;
            id.rightIndex = 0x10;
            break;
        case 0xbc27:  // URSA MINOR Airline Joystick L
        case 0xbc28:  // URSA MINOR Airline Joystick R
        case 0xbc29:  // URSA MINOR Fighter Joystick (0x09)
        case 0xbc2a:  // URSA MINOR Fighter Joystick (0x0a)
            id.deviceByte = (pid & 0xff) - 0x20;
            id.familyByte = 0xbf;
            id.leftIndex = 0x00;
            id.rightIndex = 0x00;
            break;
        default:
            break;
    }
    return id;
}

std::vector<std::pair<int, int>> identityCandidates(uint16_t pid) {
    std::vector<std::pair<int, int>> c;
    auto add = [&](int d, int f) {
        if (d < 0 || d > 0xff) return;
        if (std::find(c.begin(), c.end(), std::make_pair(d, f)) == c.end()) c.emplace_back(d, f);
    };
    const int low = (pid & 0xff) - 0x20;
    if (isThrottlePid(pid)) {
        add(0x10, 0xb9);
        add(low, 0xb9);
        add(0x01, 0xb9);
        add(low, 0xbf);
    } else {
        add(low, 0xbf);
        add(low, 0xb9);
        add(0x10, 0xb9);
    }
    add(0x20, 0xbb);
    return c;
}

WinwingHidManager::WinwingHidManager() = default;
WinwingHidManager::~WinwingHidManager() {
    allOff();
    for (auto& d : devs_)
        if (d.fd >= 0) ::close(d.fd);
}

WinwingHidManager::Dev* WinwingHidManager::find(const std::string& key) {
    for (auto& d : devs_)
        if (d.info.key == key) return &d;
    return nullptr;
}

void WinwingHidManager::setIdentity(const std::string& key, const WinwingIdentity& id) {
    std::lock_guard<std::mutex> lock(m_);
    bool found = false;
    for (auto& [k, v] : overrides_)
        if (k == key) { v = id; found = true; }
    if (!found) overrides_.emplace_back(key, id);
    if (Dev* d = find(key)) {
        d->info.identity = id;
        d->info.identityIsDefault = false;
    }
}

void WinwingHidManager::rescan() {
    std::lock_guard<std::mutex> lock(m_);
    DIR* dir = opendir("/sys/class/hidraw");
    if (!dir) return;
    std::vector<std::string> seen;
    while (dirent* e = readdir(dir)) {
        if (strncmp(e->d_name, "hidraw", 6) != 0) continue;
        const std::string sys = std::string("/sys/class/hidraw/") + e->d_name + "/device/uevent";
        const std::string uevent = readSysfs(sys);
        size_t p = uevent.find("HID_ID=");
        if (p == std::string::npos) continue;
        unsigned bus = 0, vid = 0, pid = 0;
        if (sscanf(uevent.c_str() + p, "HID_ID=%x:%x:%x", &bus, &vid, &pid) != 3 || vid != WINWING_VID) continue;
        std::string name = "Winwing device";
        if (size_t n = uevent.find("HID_NAME="); n != std::string::npos) {
            size_t end = uevent.find('\n', n);
            name = uevent.substr(n + 9, end == std::string::npos ? std::string::npos : end - n - 9);
        }
        char key[32];
        snprintf(key, sizeof key, "%04x:%04x", vid, pid);
        const std::string path = std::string("/dev/") + e->d_name;
        seen.push_back(path);
        Dev* existing = nullptr;
        for (auto& d : devs_)
            if (d.info.path == path) existing = &d;
        if (existing) continue;

        Dev d;
        d.info.key = key;
        d.info.path = path;
        d.info.name = name;
        d.info.pid = static_cast<uint16_t>(pid);
        d.info.identity = defaultIdentityForPid(d.info.pid);
        for (const auto& [k, v] : overrides_)
            if (k == d.info.key) { d.info.identity = v; d.info.identityIsDefault = false; }
        d.fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (d.fd < 0) {
            d.info.error = strerror(errno);
            LOG_WARN("Winwing %s (%s): cannot open %s: %s (udev rule needed, see README)", name.c_str(), key, path.c_str(), d.info.error.c_str());
        } else {
            d.info.open = true;
            LOG_INFO("Winwing HID: %s (%s) at %s, motor address %s", name.c_str(), key, path.c_str(),
                     d.info.identity.known() ? "known" : "unknown - use Probe in the Vibration tab");
        }
        devs_.push_back(std::move(d));
    }
    closedir(dir);
    for (size_t i = 0; i < devs_.size();) {
        if (std::find(seen.begin(), seen.end(), devs_[i].info.path) == seen.end()) {
            if (devs_[i].fd >= 0) ::close(devs_[i].fd);
            LOG_INFO("Winwing HID removed: %s", devs_[i].info.name.c_str());
            devs_.erase(devs_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

std::vector<WinwingDeviceInfo> WinwingHidManager::devices() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<WinwingDeviceInfo> out;
    for (const auto& d : devs_) out.push_back(d.info);
    return out;
}

bool WinwingHidManager::writeFrame(Dev& d, const uint8_t* frame) {
    if (d.fd < 0) return false;
    ssize_t n = ::write(d.fd, frame, FRAME_LEN);
    if (n != static_cast<ssize_t>(FRAME_LEN)) {
        if (d.info.error.empty()) LOG_WARN("Winwing %s: write failed: %s", d.info.name.c_str(), strerror(errno));
        d.info.error = strerror(errno);
        return false;
    }
    d.info.error.clear();
    d.info.framesWritten++;
    return true;
}

void WinwingHidManager::drain(Dev& d, std::vector<std::vector<uint8_t>>* out) {
    uint8_t buf[256];
    for (;;) {
        ssize_t n = ::read(d.fd, buf, sizeof buf);
        if (n <= 0) break;
        if (out && n >= 3 && buf[0] == REPORT_ID) out->emplace_back(buf, buf + n);
    }
}

bool WinwingHidManager::setMotors(const std::string& key, uint8_t left, uint8_t right, bool force) {
    std::lock_guard<std::mutex> lock(m_);
    Dev* d = find(key);
    if (!d || d->fd < 0 || !d->info.identity.known()) return false;
    const WinwingIdentity& id = d->info.identity;
    bool ok = true;
    uint8_t f[FRAME_LEN];
    if (id.leftIndex == id.rightIndex) {
        const uint8_t v = std::max(left, right);
        if (force || v != d->info.lastLeft) {
            buildSetLedx(f, id.deviceByte, id.familyByte, id.leftIndex, v);
            ok = writeFrame(*d, f);
            d->info.lastLeft = d->info.lastRight = v;
        }
    } else {
        if (force || left != d->info.lastLeft) {
            buildSetLedx(f, id.deviceByte, id.familyByte, id.leftIndex, left);
            ok &= writeFrame(*d, f);
            d->info.lastLeft = left;
        }
        if (force || right != d->info.lastRight) {
            buildSetLedx(f, id.deviceByte, id.familyByte, id.rightIndex, right);
            ok &= writeFrame(*d, f);
            d->info.lastRight = right;
        }
    }
    drain(*d, nullptr);
    return ok;
}

void WinwingHidManager::allOff() {
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& d : devs_) keys.push_back(d.info.key);
    }
    for (const auto& k : keys) setMotors(k, 0, 0, true);
}

bool WinwingHidManager::sendRaw(const std::string& key, const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(m_);
    Dev* d = find(key);
    if (!d || frame.size() != FRAME_LEN) return false;
    return writeFrame(*d, frame.data());
}

bool WinwingHidManager::setLed(const std::string& key, int deviceByte, int familyByte, int index, int value) {
    std::lock_guard<std::mutex> lock(m_);
    Dev* d = find(key);
    if (!d || d->fd < 0) return false;
    uint8_t f[FRAME_LEN];
    buildSetLedx(f, deviceByte, familyByte, index, value);
    LOG_INFO("Winwing %s: SET_LEDX device=0x%02x family=0x%02x index=%d value=%d", d->info.name.c_str(), deviceByte, familyByte, index, value);
    return writeFrame(*d, f);
}

std::vector<ProbeResult> WinwingHidManager::probe(const std::string& key, int motorIndexGuess) {
    std::vector<ProbeResult> results;
    std::lock_guard<std::mutex> lock(m_);
    Dev* d = find(key);
    if (!d || d->fd < 0) return results;
    drain(*d, nullptr);
    for (const auto& [dev, fam] : identityCandidates(d->info.pid)) {
        uint8_t f[FRAME_LEN];
        buildSetLedx(f, dev, fam, motorIndexGuess, 0);  // motor off: harmless whatever it addresses
        if (!writeFrame(*d, f)) break;
        bool echoed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        while (std::chrono::steady_clock::now() < deadline && !echoed) {
            pollfd pfd{d->fd, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0) continue;
            std::vector<std::vector<uint8_t>> reports;
            drain(*d, &reports);
            for (const auto& r : reports) {
                char hex[64] = {0};
                for (size_t i = 0; i < r.size() && i < 14; ++i) snprintf(hex + i * 3, 4, "%02x ", r[i]);
                LOG_DEBUG("Winwing %s echo: %s", d->info.name.c_str(), hex);
                if (r.size() >= 3 && r[1] == static_cast<uint8_t>(dev) && (r[2] & 0xef) == static_cast<uint8_t>(fam)) echoed = true;
            }
        }
        LOG_INFO("Winwing %s probe device=0x%02x family=0x%02x: %s", d->info.name.c_str(), dev, fam, echoed ? "ECHO (accepted)" : "no answer");
        results.push_back({dev, fam, echoed});
    }
    return results;
}

}  // namespace lr::haptics
