#pragma once
// Direct hidraw access to Winwing (vendor 0x4098) devices for the vibration motors.
// Frame format (14 bytes, report ID 0x02): [0x02, device, family, 0, 0, len, opcode, args...].
// Motor/LED level: opcode 0x49 (SET_LEDX) with args [index, value]. The firmware echoes accepted
// frames on the IN endpoint with bit 0x10 set on the family byte, which lets us probe which
// (device, family) address a given product answers to.
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lr::haptics {

constexpr uint16_t WINWING_VID = 0x4098;

struct WinwingIdentity {
    int deviceByte = -1;   // -1 = unknown
    int familyByte = -1;
    int leftIndex = -1;    // SET_LEDX index of the left (or only) motor
    int rightIndex = -1;   // right motor, equal to leftIndex on single-motor grips
    bool known() const { return deviceByte >= 0 && familyByte >= 0 && leftIndex >= 0 && rightIndex >= 0; }
};

struct WinwingDeviceInfo {
    std::string key;    // "4098:b970"
    std::string path;   // /dev/hidrawN
    std::string name;
    uint16_t pid = 0;
    bool open = false;
    std::string error;
    WinwingIdentity identity;
    bool identityIsDefault = true;  // true when it comes from the built-in table, not confirmed by a probe
    uint8_t lastLeft = 0, lastRight = 0;
    uint64_t framesWritten = 0;
};

struct ProbeResult {
    int deviceByte, familyByte;
    bool echoed;
};

// Built-in defaults by product id (from community reverse engineering, see README).
WinwingIdentity defaultIdentityForPid(uint16_t pid);
std::vector<std::pair<int, int>> identityCandidates(uint16_t pid);

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
    // Sends a harmless "motor off" frame to each candidate address and reports which ones echo.
    std::vector<ProbeResult> probe(const std::string& key, int motorIndexGuess = 0);
    bool sendRaw(const std::string& key, const std::vector<uint8_t>& frame);

private:
    struct Dev {
        int fd = -1;
        WinwingDeviceInfo info;
    };
    bool writeFrame(Dev& d, const uint8_t* frame14);
    void drain(Dev& d, std::vector<std::vector<uint8_t>>* out);
    Dev* find(const std::string& key);

    mutable std::mutex m_;
    std::vector<Dev> devs_;
    std::vector<std::pair<std::string, WinwingIdentity>> overrides_;
};

}  // namespace lr::haptics
