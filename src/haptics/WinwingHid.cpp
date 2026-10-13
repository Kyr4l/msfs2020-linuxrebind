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

#include "util/Log.hpp"

namespace lr::haptics {

namespace {
constexpr uint8_t REPORT_ID = 0x02;
constexpr uint8_t OP_REQUEST_DEVICE_HW = 0x01;
constexpr uint8_t OP_READ_CFG_DATA = 0x05;
constexpr uint8_t OP_SET_LEDX = 0x49;
constexpr int ECHO_FAMILY_OFFSET = 0x10;  // echoed family = family + 0x10 (0xb9 -> 0xc9, 0xbf -> 0xcf)
constexpr int BROADCAST_DEVICE = 0x01;
constexpr int BROADCAST_FAMILY = 0x00;
constexpr uint32_t CFG_TYPE_ID = 0x08;
constexpr uint32_t CFG_NAME_START = 0x5c, CFG_NAME_END = 0x88;
constexpr size_t FRAME_LEN = 14;

void buildFrame(uint8_t* f, int device, int family, uint8_t opcode, const uint8_t* args, size_t nargs) {
    std::memset(f, 0, FRAME_LEN);
    f[0] = REPORT_ID;
    f[1] = static_cast<uint8_t>(device);
    f[2] = static_cast<uint8_t>(family);
    f[5] = static_cast<uint8_t>(1 + nargs);
    f[6] = opcode;
    for (size_t i = 0; i < nargs && 7 + i < FRAME_LEN; ++i) f[7 + i] = args[i];
}

void buildSetLedx(uint8_t* f, int device, int family, int index, int value) {
    const uint8_t args[2] = {static_cast<uint8_t>(index), static_cast<uint8_t>(value)};
    buildFrame(f, device, family, OP_SET_LEDX, args, 2);
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

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
}  // namespace

WinwingIdentity defaultIdentityForPid(uint16_t pid) {
    WinwingIdentity id;
    switch (pid) {
        case 0xb970:  // URSA MINOR Combat Throttle Metal.EX: grips L=0x04 R=0x08, base 0x40 (discovered on hardware)
            id.family = 0xb9;
            id.left = {0x04, 0};
            id.right = {0x08, 0};
            id.lightDevice = 0x40;
            break;
        case 0xb920:  // URSA MINOR 32 Throttle Metal L/R (winctrl-xplane-plugin: one controller, two motor indices)
        case 0xb930:
            id.family = 0xb9;
            id.left = {0x10, 0x0e};
            id.right = {0x10, 0x10};
            id.lightDevice = 0x10;
            break;
        case 0xbc27:  // URSA MINOR Airline Joystick L/R
        case 0xbc28:
        case 0xbc29:  // URSA MINOR Fighter Joystick
        case 0xbc2a:
            id.family = 0xbf;
            id.left = {(pid & 0xff) - 0x20, 0};
            id.right = id.left;
            id.lightDevice = 0x20;  // lighting controller is family 0xbb, handled in the LED test by the user
            break;
        default:
            break;
    }
    return id;
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

std::vector<std::vector<uint8_t>> WinwingHidManager::collect(Dev& d, int opcode, int ms,
                                                             const std::function<bool(const std::vector<uint8_t>&)>& done) {
    std::vector<std::vector<uint8_t>> out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    uint8_t buf[256];
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        pollfd pfd{d.fd, POLLIN, 0};
        const int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (::poll(&pfd, 1, rem > 0 ? rem : 0) <= 0) continue;
        ssize_t n = ::read(d.fd, buf, sizeof buf);
        if (n <= 0) continue;
        if (buf[0] != REPORT_ID || n < 7) continue;  // joystick report 1 stream
        if (opcode >= 0 && buf[6] != opcode) continue;
        out.emplace_back(buf, buf + n);
        if (done && done(out.back())) break;
    }
    return out;
}

bool WinwingHidManager::readCfg(Dev& d, int device, int family, uint32_t offset, uint8_t out[4]) {
    uint8_t f[FRAME_LEN];
    const uint8_t args[3] = {static_cast<uint8_t>(offset), static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset >> 16)};
    buildFrame(f, device, family, OP_READ_CFG_DATA, args, 3);
    if (!writeFrame(d, f)) return false;
    auto matches = [&](const std::vector<uint8_t>& r) {
        if (r.size() < 14 || r[1] != static_cast<uint8_t>(device)) return false;
        const uint32_t roff = r[7] | (r[8] << 8) | (r[9] << 16);
        return roff == offset;
    };
    for (const auto& r : collect(d, OP_READ_CFG_DATA, 250, matches)) {
        if (!matches(r)) continue;
        std::memcpy(out, r.data() + 10, 4);
        return true;
    }
    return false;
}

std::vector<ControllerInfo> WinwingHidManager::discoverLocked(Dev& d) {
    std::vector<ControllerInfo> found;
    if (d.fd < 0) return found;
    collect(d, -1, 30);  // flush stale replies
    uint8_t f[FRAME_LEN];
    buildFrame(f, BROADCAST_DEVICE, BROADCAST_FAMILY, OP_REQUEST_DEVICE_HW, nullptr, 0);
    if (!writeFrame(d, f)) return found;
    std::vector<std::vector<uint8_t>> replies;
    for (int quiet = 0; quiet < 3; ++quiet) {  // controllers answer within a few ms of each other; stop after 120 ms of silence
        auto batch = collect(d, OP_REQUEST_DEVICE_HW, 120);
        if (batch.empty() && !replies.empty()) break;
        replies.insert(replies.end(), batch.begin(), batch.end());
    }
    for (const auto& r : replies) {
        ControllerInfo c;
        c.device = r[1];
        c.family = (r[2] - ECHO_FAMILY_OFFSET) & 0xff;
        bool dup = false;
        for (const auto& o : found) dup |= o.device == c.device && o.family == c.family;
        if (!dup) found.push_back(c);
    }
    for (auto& c : found) {
        uint8_t v[4];
        if (readCfg(d, c.device, c.family, CFG_TYPE_ID, v)) c.typeId = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
        std::string name;
        for (uint32_t off = CFG_NAME_START; off < CFG_NAME_END; off += 4) {
            if (!readCfg(d, c.device, c.family, off, v)) break;
            bool end = false;
            for (int i = 0; i < 4; ++i) {
                if (v[i] == 0 || v[i] == 0xff) { end = true; break; }
                name.push_back(static_cast<char>(v[i]));
            }
            if (end) break;
        }
        c.name = name;
        LOG_INFO("Winwing %s: controller device=0x%02x family=0x%02x type=0x%x \"%s\"", d.info.name.c_str(), c.device, c.family, c.typeId, c.name.c_str());
    }
    if (found.empty()) LOG_WARN("Winwing %s: no controller answered the broadcast identity request", d.info.name.c_str());
    d.info.controllers = found;
    return found;
}

// Fills a default identity from discovery when the product id is not in the table:
// controllers named "... L" / "... R" become the left/right motors (index 0), a single
// grip-family (0xbf) controller becomes both, the remaining one is the light controller.
void WinwingHidManager::autoAssign(Dev& d) {
    if (d.info.identity.known() || d.info.controllers.empty()) return;
    WinwingIdentity id;
    for (const auto& c : d.info.controllers) {
        if (endsWith(c.name, " L")) { id.left = {c.device, 0}; id.family = c.family; }
        else if (endsWith(c.name, " R")) { id.right = {c.device, 0}; id.family = c.family; }
        else if (c.family == 0xbf) { id.left = id.right = {c.device, 0}; id.family = c.family; }
        else if (id.lightDevice < 0) id.lightDevice = c.device;
    }
    if (id.left.known() && !id.right.known()) id.right = id.left;
    if (id.right.known() && !id.left.known()) id.left = id.right;
    if (id.known()) {
        d.info.identity = id;
        LOG_INFO("Winwing %s: motor address auto-assigned from discovery (family 0x%02x, left 0x%02x, right 0x%02x)", d.info.name.c_str(), id.family, id.left.device, id.right.device);
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
        bool exists = false;
        for (auto& d : devs_) exists |= d.info.path == path;
        if (exists) continue;

        Dev d;
        d.info.key = key;
        d.info.path = path;
        d.info.name = name;
        d.info.pid = static_cast<uint16_t>(pid);
        d.info.identity = defaultIdentityForPid(d.info.pid);
        bool overridden = false;
        for (const auto& [k, v] : overrides_)
            if (k == d.info.key) { d.info.identity = v; d.info.identityIsDefault = false; overridden = true; }
        d.fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (d.fd < 0) {
            d.info.error = strerror(errno);
            LOG_WARN("Winwing %s (%s): cannot open %s: %s (udev rule needed, see README)", name.c_str(), key, path.c_str(), d.info.error.c_str());
        } else {
            d.info.open = true;
            LOG_INFO("Winwing HID: %s (%s) at %s", name.c_str(), key, path.c_str());
            discoverLocked(d);
            if (!overridden) autoAssign(d);
            if (!d.info.identity.known()) LOG_WARN("Winwing %s: motor address unknown, set it in the Vibration tab", name.c_str());
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
    scans_++;
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

bool WinwingHidManager::setMotors(const std::string& key, uint8_t left, uint8_t right, bool force) {
    std::lock_guard<std::mutex> lock(m_);
    Dev* d = find(key);
    if (!d || d->fd < 0 || !d->info.identity.known()) return false;
    const WinwingIdentity& id = d->info.identity;
    bool ok = true;
    uint8_t f[FRAME_LEN];
    const bool single = id.left.device == id.right.device && id.left.index == id.right.index;
    if (single) {
        const uint8_t v = std::max(left, right);
        if (force || v != d->info.lastLeft) {
            buildSetLedx(f, id.left.device, id.family, id.left.index, v);
            ok = writeFrame(*d, f);
            d->info.lastLeft = d->info.lastRight = v;
        }
    } else {
        if (force || left != d->info.lastLeft) {
            buildSetLedx(f, id.left.device, id.family, id.left.index, left);
            ok &= writeFrame(*d, f);
            d->info.lastLeft = left;
        }
        if (force || right != d->info.lastRight) {
            buildSetLedx(f, id.right.device, id.family, id.right.index, right);
            ok &= writeFrame(*d, f);
            d->info.lastRight = right;
        }
    }
    collect(*d, -1, 0);  // drop echoes
    LOG_DEBUG("Winwing %s: motors L=%u R=%u (family 0x%02x, L dev 0x%02x idx %d, R dev 0x%02x idx %d) %s", d->info.name.c_str(), left, right,
              id.family, id.left.device, id.left.index, id.right.device, id.right.index, ok ? "sent" : "WRITE FAILED");
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

bool WinwingHidManager::setLed(const std::string& key, int device, int family, int index, int value) {
    std::lock_guard<std::mutex> lock(m_);
    Dev* d = find(key);
    if (!d || d->fd < 0) return false;
    uint8_t f[FRAME_LEN];
    buildSetLedx(f, device, family, index, value);
    const bool ok = writeFrame(*d, f);
    bool echoed = false;
    for (const auto& r : collect(*d, OP_SET_LEDX, 150)) echoed |= r[1] == static_cast<uint8_t>(device);
    LOG_INFO("Winwing %s: SET_LEDX device=0x%02x family=0x%02x index=%d value=%d -> %s", d->info.name.c_str(), device, family, index, value,
             echoed ? "accepted (echo)" : "no echo");
    return ok;
}

std::vector<ControllerInfo> WinwingHidManager::discover(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_);
    Dev* d = find(key);
    if (!d) return {};
    auto r = discoverLocked(*d);
    if (d->info.identityIsDefault) autoAssign(*d);
    return r;
}

}  // namespace lr::haptics
