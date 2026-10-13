#pragma once
// Owns the SimConnect connection on its own thread: reconnects, maps event names to IDs,
// talks to the MobiFlight WASM module (for H: events, L: vars and calculator code) and
// executes queued actions coming from the input engine or the UI.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sim/SimConnectClient.hpp"

namespace lr::sim {

enum class ConnState { Disconnected, Connecting, Opening, Connected };
enum class ModuleState { Unknown, Probing, Detected, NotFound };

struct SimStatus {
    ConnState state = ConnState::Disconnected;
    ModuleState mobiflight = ModuleState::Unknown;
    std::string host;
    uint16_t port = 0;
    uint16_t detectedPort = 0;    // port found by scanning when the configured one did not answer
    std::string detectedHost;
    std::string simName;         // from RECV_OPEN
    std::string simVersion;      // "11.0.282174.999"
    std::string aircraft;        // path of the loaded aircraft .cfg (AircraftLoaded)
    bool simRunning = false;     // "Sim" system event
    bool paused = false;
    std::string lastError;
    uint64_t commandsSent = 0;
    uint64_t exceptions = 0;
    std::chrono::steady_clock::time_point lastActivity{};
};

// Flight telemetry used by the vibration engine (all requested as FLOAT64, tagged).
struct Telemetry {
    bool valid = false;
    std::chrono::steady_clock::time_point time{};
    uint64_t seq = 0;
    double onGround = 0, groundSpeedKt = 0, gForce = 1, stallWarning = 0, overspeedWarning = 0, gearPct = 1, flapsPct = 0,
           spoilersHandle = 0, n1Eng1 = 0, n1Eng2 = 0, reverse1 = 0, reverse2 = 0, accelBodyY = 0, verticalSpeedFpm = 0,
           brakeLeft = 0, brakeRight = 0, iasKt = 0, altAgl = 0;
};

struct SimCommand {
    enum class Type { SimEvent, HEvent, LVarSet, CalcCode, MobiFlightPing, MobiFlightRaw };
    Type type = Type::SimEvent;
    std::string name;    // event name / L var / H event / calculator code / raw MF command
    double value = 0;    // event parameter or L var value
    std::string origin;  // free text for the log ("binding: AP1", "test")
};

class SimBridge {
public:
    SimBridge();
    ~SimBridge();

    void setEndpoint(const std::string& host, uint16_t port);
    void setAutoConnect(bool on);
    void connectNow();
    void disconnect();

    void start();
    void stop();

    void enqueue(SimCommand cmd);
    void sendEvent(const std::string& name, double value, const std::string& origin = {});
    void sendHEvent(const std::string& name, const std::string& origin = {});
    void setLVar(const std::string& name, double value, const std::string& origin = {});
    void execCalculatorCode(const std::string& code, const std::string& origin = {});
    void probeMobiFlight();

    SimStatus status() const;
    Telemetry telemetry() const;
    void setTelemetryEnabled(bool on);  // subscribe to flight data (needed for vibration)

private:
    void threadMain();
    bool tryConnect();
    struct Endpoint { std::string host; uint16_t port; };
    std::vector<Endpoint> scanForSimConnect();  // probes loopback listeners (wineserver's first) with an Open handshake
    void onConnected();
    void handleMessage(const RecvMessage& m);
    void drainQueue();
    void execute(const SimCommand& cmd);
    void sendMobiFlightCommand(const std::string& text, const std::string& desc);
    void setupMobiFlight();
    void rememberSend(uint32_t sendId, std::string desc);
    std::string describeSend(uint32_t sendId);
    void dropConnection(const std::string& why);

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> wantConnect_{true};
    std::atomic<bool> forceConnect_{false};
    std::atomic<bool> forceDisconnect_{false};
    std::atomic<bool> forceProbe_{false};

    void setupTelemetry();
    void handleTelemetry(const RecvClientData& d);

    SimConnectClient client_;
    SimStatus status_;
    Telemetry telemetry_;
    std::atomic<bool> telemetryWanted_{false};
    bool telemetryRequested_ = false;
    std::deque<SimCommand> queue_;

    std::unordered_map<std::string, uint32_t> eventIds_;
    uint32_t nextEventId_ = 1000;
    std::deque<std::pair<uint32_t, std::string>> sentLog_;

    std::chrono::steady_clock::time_point nextConnectAttempt_{};
    std::chrono::steady_clock::time_point openSentAt_{};
    std::chrono::steady_clock::time_point lastPing_{};
    std::chrono::steady_clock::time_point mfPingSentAt_{};
    bool mfSetupDone_ = false;
};

}  // namespace lr::sim
