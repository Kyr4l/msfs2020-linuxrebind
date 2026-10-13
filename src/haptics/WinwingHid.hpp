#pragma once
// Direct hidraw access to Winwing (vendor 0x4098) devices for the vibration motors.
// Frame format (14 bytes, report ID 0x02): [0x02, device, family, 0, 0, len, opcode, args...].
// One USB device carries several logical controllers (e.g. left grip, right grip, base), each
// addressed by a (device, family) pair. Motor/LED level: opcode 0x49 (SET_LEDX) with [index, value];
// index 0 is the motor on a grip and the backlight on a base. Accepted frames are echoed on the
// vendor input report with 0x10 added to the family byte; a request sent to the broadcast
// address (device 0x01, family 0x00) makes every controller answer with its own id.
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace lr::haptics {

constexpr uint16_t WINWING_VID = 0x4098;

struct MotorAddress {
    int device = -1;  // -1 = unknown
    int index = 0;    // SET_LEDX index
    bool known() const { return device >= 0 && index >= 0; }
};

struct WinwingIdentity {
    int family = -1;
    MotorAddress left, right;  // right == left on single-motor sticks
    int lightDevice = -1;      // controller carrying the backlight (LED test), optional
    bool known() const { return family >= 0 && left.known() && right.known(); }
};

struct ControllerInfo {
    int device = 0, family = 0;
    std::string name;      // product name read from flash (offset 0x5c), may be empty
    uint32_t typeId = 0;   // flash 0x08
};

struct WinwingDeviceInfo {
    std::string key;    // "4098:b970"
    std::string path;   // /dev/hidrawN
    std::string name;
    uint16_t pid = 0;
    bool open = false;
    std::string error;
    WinwingIdentity identity;
    bool identityIsDefault = true;  // from the built-in table / auto discovery, not set by the user
    std::vector<ControllerInfo> controllers;  // from the last discovery
    uint8_t lastLeft = 0, lastRight = 0;
    uint64_t framesWritten = 0;
};

// Built-in defaults by product id (community reverse engineering + discovery on real hardware).
WinwingIdentity defaultIdentityForPid(uint16_t pid);

class WinwingHidManager {
public:
    WinwingHidManager();
    ~WinwingHidManager();

    void rescan();
    std::vector<WinwingDeviceInfo> devices() const;
    void setIdentity(const std::string& key, const WinwingIdentity& id);

    // Writes motor levels (0..255). Only sends frames when a value changed unless force is set.
    bool setMotors(const std::string& key, uint8_t left, uint8_t right, bool force = false);
    void allOff();
    // Broadcast identity request; returns every controller behind the USB device with its name.
    std::vector<ControllerInfo> discover(const std::string& key);
    // SET_LEDX on an explicit address (visual oracle / raw motor test).
    bool setLed(const std::string& key, int device, int family, int index, int value);
    bool sendRaw(const std::string& key, const std::vector<uint8_t>& frame);

private:
    struct Dev {
        int fd = -1;
        WinwingDeviceInfo info;
    };
    bool writeFrame(Dev& d, const uint8_t* frame14);
    // Collects vendor replies (report 0x02) with the given opcode for up to ms.
    std::vector<std::vector<uint8_t>> collect(Dev& d, int opcode, int ms);
    bool readCfg(Dev& d, int device, int family, uint32_t offset, uint8_t out[4]);
    std::vector<ControllerInfo> discoverLocked(Dev& d);
    void autoAssign(Dev& d);
    Dev* find(const std::string& key);

    mutable std::mutex m_;
    std::vector<Dev> devs_;
    std::vector<std::pair<std::string, WinwingIdentity>> overrides_;
};

}  // namespace lr::haptics
