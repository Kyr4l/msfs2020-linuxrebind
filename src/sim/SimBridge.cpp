#include "sim/SimBridge.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "util/Log.hpp"

namespace lr::sim {

namespace {
using clock = std::chrono::steady_clock;

// Our client-side IDs.
constexpr uint32_t EV_SYS_SIM = 1;
constexpr uint32_t EV_SYS_PAUSE = 2;
constexpr uint32_t EV_SYS_AIRCRAFT_LOADED = 3;
constexpr uint32_t REQ_SYS_AIRCRAFT_LOADED = 1;
constexpr uint32_t REQ_SYS_SIM = 2;
constexpr uint32_t DEF_TELEMETRY = 10;
constexpr uint32_t REQ_TELEMETRY = 10;

struct TelemetryVar { const char* name; const char* units; double Telemetry::*field; };
const TelemetryVar kTelemetryVars[] = {
    {"SIM ON GROUND", "Bool", &Telemetry::onGround},
    {"GROUND VELOCITY", "Knots", &Telemetry::groundSpeedKt},
    {"G FORCE", "GForce", &Telemetry::gForce},
    {"STALL WARNING", "Bool", &Telemetry::stallWarning},
    {"OVERSPEED WARNING", "Bool", &Telemetry::overspeedWarning},
    {"GEAR TOTAL PCT EXTENDED", "Percent Over 100", &Telemetry::gearPct},
    {"TRAILING EDGE FLAPS LEFT PERCENT", "Percent Over 100", &Telemetry::flapsPct},
    {"SPOILERS HANDLE POSITION", "Percent Over 100", &Telemetry::spoilersHandle},
    {"TURB ENG N1:1", "Percent", &Telemetry::n1Eng1},
    {"TURB ENG N1:2", "Percent", &Telemetry::n1Eng2},
    {"TURB ENG REVERSE NOZZLE PERCENT:1", "Percent Over 100", &Telemetry::reverse1},
    {"TURB ENG REVERSE NOZZLE PERCENT:2", "Percent Over 100", &Telemetry::reverse2},
    {"ACCELERATION BODY Y", "Feet per second squared", &Telemetry::accelBodyY},
    {"VERTICAL SPEED", "Feet per minute", &Telemetry::verticalSpeedFpm},
    {"BRAKE LEFT POSITION", "Position", &Telemetry::brakeLeft},
    {"BRAKE RIGHT POSITION", "Position", &Telemetry::brakeRight},
    {"AIRSPEED INDICATED", "Knots", &Telemetry::iasKt},
    {"PLANE ALT ABOVE GROUND", "Feet", &Telemetry::altAgl},
};
constexpr size_t kTelemetryCount = sizeof(kTelemetryVars) / sizeof(kTelemetryVars[0]);

// MobiFlight WASM module protocol (https://github.com/MobiFlight/MobiFlight-WASM-Module).
constexpr uint32_t CD_MF_COMMAND = 100;
constexpr uint32_t CD_MF_RESPONSE = 101;
constexpr uint32_t DEF_MF_STRING = 100;
constexpr uint32_t REQ_MF_RESPONSE = 100;
constexpr uint32_t MF_MESSAGE_SIZE = 1024;
// Our private MobiFlight client (registered with "MF.Clients.Add.<name>"): own command/response areas and an
// LVars area the module fills with the variables we ask for, so other MobiFlight clients cannot shift our indices.
constexpr const char* MF_CLIENT_NAME = "msfs-linuxrebind";
constexpr uint32_t CD_OUR_COMMAND = 110;
constexpr uint32_t CD_OUR_RESPONSE = 111;
constexpr uint32_t CD_OUR_LVARS = 112;
constexpr uint32_t REQ_OUR_RESPONSE = 111;
constexpr uint32_t REQ_OUR_LVARS = 112;
constexpr uint32_t DEF_OUR_LVARS = 112;
constexpr size_t MF_MAX_LVARS = 1000;  // 4096-byte area / 4-byte floats

std::string formatNumber(double v) {
    char buf[64];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) {
        snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
    } else {
        snprintf(buf, sizeof buf, "%.6g", v);
    }
    return buf;
}

uint32_t eventParam(double v) {
    // SimConnect event parameters are DWORDs; negative values travel as two's complement.
    long long ll = std::llround(v);
    return static_cast<uint32_t>(static_cast<int64_t>(ll));
}

std::string stripPrefix(std::string s, const char* prefix) {
    const size_t n = strlen(prefix);
    if (s.size() >= n && strncasecmp(s.c_str(), prefix, n) == 0) s.erase(0, n);
    return s;
}
}  // namespace

SimBridge::SimBridge() {
    status_.host = "127.0.0.1";
    status_.port = 500;
}

SimBridge::~SimBridge() { stop(); }

void SimBridge::setEndpoint(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(m_);
    if (status_.host == host && status_.port == port) return;
    status_.host = host;
    status_.port = port;
    forceDisconnect_ = true;
    nextConnectAttempt_ = clock::now();
    cv_.notify_all();
}

void SimBridge::setAutoConnect(bool on) {
    wantConnect_ = on;
    cv_.notify_all();
}

void SimBridge::connectNow() {
    forceConnect_ = true;
    cv_.notify_all();
}

void SimBridge::disconnect() {
    forceDisconnect_ = true;
    cv_.notify_all();
}

void SimBridge::probeMobiFlight() {
    forceProbe_ = true;
    cv_.notify_all();
}

void SimBridge::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { threadMain(); });
}

void SimBridge::stop() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SimBridge::enqueue(SimCommand cmd) {
    {
        std::lock_guard<std::mutex> lock(m_);
        queue_.push_back(std::move(cmd));
    }
    cv_.notify_all();
}

void SimBridge::sendEvent(const std::string& name, double value, const std::string& origin) {
    enqueue({SimCommand::Type::SimEvent, name, value, origin});
}
void SimBridge::sendHEvent(const std::string& name, const std::string& origin) {
    enqueue({SimCommand::Type::HEvent, name, 0, origin});
}
void SimBridge::setLVar(const std::string& name, double value, const std::string& origin) {
    enqueue({SimCommand::Type::LVarSet, name, value, origin});
}
void SimBridge::execCalculatorCode(const std::string& code, const std::string& origin) {
    enqueue({SimCommand::Type::CalcCode, code, 0, origin});
}
void SimBridge::toggleLVar(const std::string& name, const std::string& origin) {
    enqueue({SimCommand::Type::LVarToggle, name, 0, origin});
}

void SimBridge::watchLVars(std::vector<std::string> names) {
    std::vector<std::string> clean;
    for (auto& n : names) {
        std::string v = stripPrefix(n, "L:");
        if (!v.empty() && std::find(clean.begin(), clean.end(), v) == clean.end()) clean.push_back(v);
    }
    std::lock_guard<std::mutex> lock(m_);
    if (clean == lvarWatch_) return;
    lvarWatch_ = std::move(clean);
    lvarWatchDirty_ = true;
    cv_.notify_all();
}

std::optional<double> SimBridge::lvarValue(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_);
    auto it = lvarValues_.find(stripPrefix(name, "L:"));
    if (it == lvarValues_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<std::string, double>> SimBridge::lvarValues() const {
    std::lock_guard<std::mutex> lock(m_);
    return {lvarValues_.begin(), lvarValues_.end()};
}

SimStatus SimBridge::status() const {
    std::lock_guard<std::mutex> lock(m_);
    return status_;
}

Telemetry SimBridge::telemetry() const {
    std::lock_guard<std::mutex> lock(m_);
    return telemetry_;
}

void SimBridge::setTelemetryEnabled(bool on) {
    telemetryWanted_ = on;
    cv_.notify_all();
}

void SimBridge::setupTelemetry() {
    for (size_t i = 0; i < kTelemetryCount; ++i) {
        rememberSend(client_.addToDataDefinition(DEF_TELEMETRY, kTelemetryVars[i].name, kTelemetryVars[i].units, DATATYPE_FLOAT64, 0,
                                                 static_cast<uint32_t>(i)),
                     std::string("define telemetry ") + kTelemetryVars[i].name);
    }
    // Tagged format: each datum arrives as (uint32 id, float64 value), so a rejected variable cannot shift the others.
    rememberSend(client_.requestDataOnSimObject(REQ_TELEMETRY, DEF_TELEMETRY, OBJECT_ID_USER, PERIOD_SIM_FRAME, DATA_REQUEST_FLAG_TAGGED, 0, 2, 0),
                 "request telemetry");
    telemetryRequested_ = true;
    LOG_INFO("Telemetry subscription sent (%zu variables)", kTelemetryCount);
}

void SimBridge::handleTelemetry(const RecvClientData& d) {
    Telemetry t;
    {
        std::lock_guard<std::mutex> lock(m_);
        t = telemetry_;
    }
    const uint8_t* p = d.data.data();
    size_t n = d.data.size();
    size_t off = 0;
    for (uint32_t i = 0; i < d.defineCount && off + 12 <= n; ++i) {
        uint32_t id;
        double v;
        std::memcpy(&id, p + off, 4);
        std::memcpy(&v, p + off + 4, 8);
        off += 12;
        if (id < kTelemetryCount) t.*(kTelemetryVars[id].field) = v;
    }
    if (!t.valid) LOG_INFO("Telemetry flowing (%u datums per update)", d.defineCount);
    t.valid = true;
    t.time = clock::now();
    t.seq++;
    std::lock_guard<std::mutex> lock(m_);
    telemetry_ = t;
}

void SimBridge::rememberSend(uint32_t sendId, std::string desc) {
    sentLog_.emplace_back(sendId, std::move(desc));
    while (sentLog_.size() > 512) sentLog_.pop_front();
}

std::string SimBridge::describeSend(uint32_t sendId) {
    for (const auto& [id, desc] : sentLog_)
        if (id == sendId) return desc;
    return "packet #" + std::to_string(sendId);
}

void SimBridge::dropConnection(const std::string& why) {
    client_.close();
    std::lock_guard<std::mutex> lock(m_);
    if (status_.state != ConnState::Disconnected) LOG_WARN("SimConnect: disconnected (%s)", why.c_str());
    status_.state = ConnState::Disconnected;
    status_.mobiflight = ModuleState::Unknown;
    status_.lastError = why;
    status_.simRunning = false;
    status_.aircraft.clear();
    telemetry_ = Telemetry{};
    telemetryRequested_ = false;
    eventIds_.clear();
    sentLog_.clear();
    mfSetupDone_ = false;
    mfClientReady_ = false;
    mfClientRequested_ = false;
    lvarRegistered_.clear();
    lvarValues_.clear();
    lvarWatchDirty_ = true;
    nextConnectAttempt_ = clock::now() + std::chrono::seconds(3);
}

namespace {
// Socket inodes held by wineserver (MSFS under Proton): the SimConnect listener is one of them.
std::set<unsigned long> wineserverSocketInodes() {
    std::set<unsigned long> inodes;
    DIR* proc = opendir("/proc");
    if (!proc) return inodes;
    while (dirent* e = readdir(proc)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::ifstream comm(std::string("/proc/") + e->d_name + "/comm");
        std::string name;
        std::getline(comm, name);
        if (name != "wineserver") continue;
        const std::string fdDir = std::string("/proc/") + e->d_name + "/fd";
        DIR* fds = opendir(fdDir.c_str());
        if (!fds) continue;
        while (dirent* f = readdir(fds)) {
            char link[64];
            ssize_t n = readlink((fdDir + "/" + f->d_name).c_str(), link, sizeof link - 1);
            if (n <= 0) continue;
            link[n] = 0;
            unsigned long ino = 0;
            if (sscanf(link, "socket:[%lu]", &ino) == 1) inodes.insert(ino);
        }
        closedir(fds);
    }
    closedir(proc);
    return inodes;
}

// LISTEN sockets on loopback / any address from /proc/net/tcp{,6}: (host, port, inode).
struct Listener { std::string host; uint16_t port; unsigned long inode; };
std::vector<Listener> loopbackListeners() {
    std::vector<Listener> out;
    for (const char* file : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        std::ifstream in(file);
        std::string line;
        std::getline(in, line);  // header
        const bool v6 = strstr(file, "tcp6") != nullptr;
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string sl, local, remote, st;
            ss >> sl >> local >> remote >> st;
            // remaining columns: tx_queue:rx_queue tr:tm->when retrnsmt uid timeout inode
            std::string txrx, trtm, retr, uid, timeout, inodeStr;
            ss >> txrx >> trtm >> retr >> uid >> timeout >> inodeStr;
            if (st != "0A") continue;  // LISTEN
            size_t colon = local.find(':');
            if (colon == std::string::npos) continue;
            const std::string addr = local.substr(0, colon);
            const uint16_t port = static_cast<uint16_t>(strtoul(local.substr(colon + 1).c_str(), nullptr, 16));
            std::string host;
            if (!v6) {
                if (addr == "0100007F") host = "127.0.0.1";
                else if (addr == "00000000") host = "127.0.0.1";
                else continue;
            } else {
                if (addr == "00000000000000000000000001000000") host = "::1";
                else if (addr == "00000000000000000000000000000000") host = "::1";
                else continue;
            }
            out.push_back({host, port, strtoul(inodeStr.c_str(), nullptr, 10)});
        }
    }
    return out;
}

// Sends a SimConnect Open to host:port and reports whether a RECV_OPEN comes back within the timeout.
bool simConnectAnswers(const std::string& host, uint16_t port, int timeoutMs) {
    SimConnectClient probe;
    std::string err;
    if (!probe.connect(host, port, 300, err)) return false;
    probe.open("msfs-linuxrebind-probe");
    const auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<RecvMessage> msgs;
    while (clock::now() < deadline) {
        msgs.clear();
        if (!probe.poll(50, msgs, err)) return false;
        for (const auto& m : msgs)
            if (static_cast<RecvId>(m.id) == RecvId::Open) return true;
    }
    return false;
}
}  // namespace

std::vector<SimBridge::Endpoint> SimBridge::scanForSimConnect() {
    std::vector<Endpoint> found;
    const auto wine = wineserverSocketInodes();
    auto listeners = loopbackListeners();
    // Prefer wineserver's sockets; without any (not Proton, or no permission) probe everything on loopback.
    std::vector<Listener> candidates;
    for (const auto& l : listeners)
        if (wine.empty() || wine.count(l.inode)) candidates.push_back(l);
    LOG_INFO("SimConnect: configured port did not answer, probing %zu loopback listener(s)%s", candidates.size(), wine.empty() ? "" : " owned by wineserver");
    for (const auto& l : candidates) {
        if (simConnectAnswers(l.host, l.port, 700)) {
            LOG_INFO("SimConnect: found the simulator on %s:%u", l.host.c_str(), l.port);
            found.push_back({l.host, l.port});
            break;
        }
    }
    return found;
}

bool SimBridge::tryConnect() {
    std::string host;
    uint16_t port;
    {
        std::lock_guard<std::mutex> lock(m_);
        host = status_.host;
        port = status_.port;
        status_.state = ConnState::Connecting;
    }
    std::string err;
    bool connected = port != 0 && client_.connect(host, port, 2000, err);
    if (!connected) {
        // Typical under Proton: MSFS cannot bind a port < 1024 (SimConnect.xml says 500) and silently falls back
        // to a dynamic port that changes every launch. Find it.
        auto eps = scanForSimConnect();
        if (!eps.empty() && client_.connect(eps[0].host, eps[0].port, 2000, err)) {
            connected = true;
            std::lock_guard<std::mutex> lock(m_);
            status_.detectedHost = eps[0].host;
            status_.detectedPort = eps[0].port;
            host = eps[0].host;
            port = eps[0].port;
        }
    }
    if (!connected) {
        std::lock_guard<std::mutex> lock(m_);
        status_.state = ConnState::Disconnected;
        status_.lastError = port == 0 ? "no SimConnect listener found" : err;
        status_.detectedPort = 0;
        nextConnectAttempt_ = clock::now() + std::chrono::seconds(3);
        return false;
    }
    LOG_INFO("SimConnect: TCP connected to %s:%u, sending Open", host.c_str(), port);
    uint32_t id = client_.open("msfs-linuxrebind");
    rememberSend(id, "Open");
    openSentAt_ = clock::now();
    std::lock_guard<std::mutex> lock(m_);
    status_.state = ConnState::Opening;
    status_.lastError.clear();
    return true;
}

void SimBridge::onConnected() {
    rememberSend(client_.subscribeToSystemEvent(EV_SYS_SIM, "Sim"), "subscribe Sim");
    rememberSend(client_.subscribeToSystemEvent(EV_SYS_PAUSE, "Pause"), "subscribe Pause");
    rememberSend(client_.subscribeToSystemEvent(EV_SYS_AIRCRAFT_LOADED, "AircraftLoaded"), "subscribe AircraftLoaded");
    rememberSend(client_.requestSystemState(REQ_SYS_AIRCRAFT_LOADED, "AircraftLoaded"), "request AircraftLoaded");
    rememberSend(client_.requestSystemState(REQ_SYS_SIM, "Sim"), "request Sim state");
    setupMobiFlight();
    if (telemetryWanted_) setupTelemetry();
}

void SimBridge::setupMobiFlight() {
    // The module creates "MobiFlight.Command" / "MobiFlight.Response" (1024-byte strings).
    rememberSend(client_.mapClientDataNameToID("MobiFlight.Command", CD_MF_COMMAND), "map MobiFlight.Command");
    rememberSend(client_.mapClientDataNameToID("MobiFlight.Response", CD_MF_RESPONSE), "map MobiFlight.Response");
    rememberSend(client_.addToClientDataDefinition(DEF_MF_STRING, 0, MF_MESSAGE_SIZE), "define MobiFlight string");
    rememberSend(client_.requestClientData(CD_MF_RESPONSE, REQ_MF_RESPONSE, DEF_MF_STRING, CD_PERIOD_ON_SET, CD_REQUEST_FLAG_CHANGED),
                 "request MobiFlight.Response");
    mfSetupDone_ = true;
    {
        std::lock_guard<std::mutex> lock(m_);
        status_.mobiflight = ModuleState::Probing;
    }
    sendMobiFlightCommand("MF.Ping", "MobiFlight ping");
    mfPingSentAt_ = clock::now();
}

void SimBridge::sendMobiFlightCommand(const std::string& text, const std::string& desc) {
    if (!mfSetupDone_) return;
    // Commands that execute code go through our own area once it exists (keeps the shared one quiet);
    // registration/ping always use the shared "MobiFlight.Command".
    const bool own = mfClientReady_ && text.rfind("MF.Clients.Add.", 0) != 0 && text != "MF.Ping";
    const uint32_t area = own ? CD_OUR_COMMAND : CD_MF_COMMAND;
    std::string payload = text;
    if (payload.size() >= MF_MESSAGE_SIZE) payload.resize(MF_MESSAGE_SIZE - 1);
    rememberSend(client_.setClientData(area, DEF_MF_STRING, MF_MESSAGE_SIZE, payload.data(), payload.size() + 1), desc);
    // The module only receives the area when its content changes, so follow every command with
    // a dummy one. Otherwise sending the same command twice in a row would be ignored.
    static const char dummy[] = "MF.DummyCmd";
    rememberSend(client_.setClientData(area, DEF_MF_STRING, MF_MESSAGE_SIZE, dummy, sizeof dummy), "MobiFlight dummy");
}

void SimBridge::registerMobiFlightClient() {
    if (mfClientRequested_) return;
    mfClientRequested_ = true;
    sendMobiFlightCommand(std::string("MF.Clients.Add.") + MF_CLIENT_NAME, "MobiFlight client registration");
}

void SimBridge::onMobiFlightClientReady() {
    const std::string n = MF_CLIENT_NAME;
    rememberSend(client_.mapClientDataNameToID(n + ".Command", CD_OUR_COMMAND), "map own MobiFlight command area");
    rememberSend(client_.mapClientDataNameToID(n + ".Response", CD_OUR_RESPONSE), "map own MobiFlight response area");
    rememberSend(client_.mapClientDataNameToID(n + ".LVars", CD_OUR_LVARS), "map own MobiFlight LVars area");
    rememberSend(client_.requestClientData(CD_OUR_RESPONSE, REQ_OUR_RESPONSE, DEF_MF_STRING, CD_PERIOD_ON_SET, CD_REQUEST_FLAG_CHANGED),
                 "request own MobiFlight response");
    mfClientReady_ = true;
    LOG_INFO("MobiFlight: private client \"%s\" registered", MF_CLIENT_NAME);
    lvarWatchDirty_ = true;
}

// (Re)registers the watched L: vars with the module and (re)defines the tagged LVars read.
void SimBridge::applyLVarWatch() {
    std::vector<std::string> wanted;
    {
        std::lock_guard<std::mutex> lock(m_);
        lvarWatchDirty_ = false;
        wanted = lvarWatch_;
    }
    if (!mfClientReady_) return;
    if (wanted.size() > MF_MAX_LVARS) wanted.resize(MF_MAX_LVARS);
    if (wanted == lvarRegistered_) return;
    if (!lvarRegistered_.empty()) {
        // Stop the previous request before changing the definition.
        rememberSend(client_.requestClientData(CD_OUR_LVARS, REQ_OUR_LVARS, DEF_OUR_LVARS, CD_PERIOD_NEVER, 0), "stop LVars request");
        rememberSend(client_.clearClientDataDefinition(DEF_OUR_LVARS), "clear LVars definition");
    }
    sendMobiFlightCommand("MF.SimVars.Clear", "MobiFlight clear vars");
    lvarRegistered_.clear();
    {
        std::lock_guard<std::mutex> lock(m_);
        lvarValues_.clear();
    }
    for (size_t i = 0; i < wanted.size(); ++i) {
        sendMobiFlightCommand("MF.SimVars.Add.(L:" + wanted[i] + ")", "watch L:" + wanted[i]);
        rememberSend(client_.addToClientDataDefinition(DEF_OUR_LVARS, static_cast<uint32_t>(i * 4), 4, 0, static_cast<uint32_t>(i)),
                     "define L:" + wanted[i]);
        lvarRegistered_.push_back(wanted[i]);
    }
    if (!wanted.empty()) {
        rememberSend(client_.requestClientData(CD_OUR_LVARS, REQ_OUR_LVARS, DEF_OUR_LVARS, CD_PERIOD_ON_SET,
                                               CD_REQUEST_FLAG_CHANGED | CD_REQUEST_FLAG_TAGGED),
                     "request LVars");
    }
    LOG_INFO("MobiFlight: watching %zu L: var(s)", wanted.size());
}

void SimBridge::handleLVarData(const RecvClientData& d) {
    const uint8_t* p = d.data.data();
    const size_t n = d.data.size();
    size_t off = 0;
    std::lock_guard<std::mutex> lock(m_);
    static bool first = true;
    if (first) { LOG_INFO("MobiFlight: L: var values flowing (%u datum(s) in first update)", d.defineCount); first = false; }
    for (uint32_t i = 0; i < d.defineCount && off + 8 <= n; ++i) {
        uint32_t id;
        float v;
        std::memcpy(&id, p + off, 4);
        std::memcpy(&v, p + off + 4, 4);
        off += 8;
        if (id < lvarRegistered_.size()) {
            auto& slot = lvarValues_[lvarRegistered_[id]];
            if (slot != v) LOG_DEBUG("L:%s = %g", lvarRegistered_[id].c_str(), v);
            slot = v;
        }
    }
}

void SimBridge::execute(const SimCommand& cmd) {
    const std::string where = cmd.origin.empty() ? std::string() : " [" + cmd.origin + "]";
    switch (cmd.type) {
        case SimCommand::Type::SimEvent: {
            std::string name = stripPrefix(cmd.name, "K:");
            auto it = eventIds_.find(name);
            uint32_t id;
            if (it == eventIds_.end()) {
                id = nextEventId_++;
                eventIds_[name] = id;
                rememberSend(client_.mapClientEventToSimEvent(id, name), "map event " + name);
            } else {
                id = it->second;
            }
            const uint32_t param = eventParam(cmd.value);
            rememberSend(client_.transmitClientEvent(OBJECT_ID_USER, id, param, GROUP_PRIORITY_HIGHEST, EVENT_FLAG_GROUPID_IS_PRIORITY),
                         "event " + name + " " + formatNumber(cmd.value));
            LOG_INFO("-> K:%s %s%s", name.c_str(), formatNumber(cmd.value).c_str(), where.c_str());
            break;
        }
        case SimCommand::Type::HEvent: {
            std::string name = stripPrefix(cmd.name, "H:");
            sendMobiFlightCommand("MF.SimVars.Set.(>H:" + name + ")", "H event " + name);
            LOG_INFO("-> H:%s%s", name.c_str(), where.c_str());
            break;
        }
        case SimCommand::Type::LVarSet: {
            std::string name = stripPrefix(cmd.name, "L:");
            const std::string v = formatNumber(cmd.value);
            sendMobiFlightCommand("MF.SimVars.Set." + v + " (>L:" + name + ")", "set L:" + name);
            LOG_INFO("-> L:%s = %s%s", name.c_str(), v.c_str(), where.c_str());
            break;
        }
        case SimCommand::Type::LVarToggle: {
            std::string name = stripPrefix(cmd.name, "L:");
            // Evaluated inside the sim, so it is correct whatever changed the switch in the meantime.
            sendMobiFlightCommand("MF.SimVars.Set.(L:" + name + ") ! (>L:" + name + ")", "toggle L:" + name);
            LOG_INFO("-> L:%s toggle%s", name.c_str(), where.c_str());
            break;
        }
        case SimCommand::Type::CalcCode: {
            sendMobiFlightCommand("MF.SimVars.Set." + cmd.name, "calculator code");
            LOG_INFO("-> calc: %s%s", cmd.name.c_str(), where.c_str());
            break;
        }
        case SimCommand::Type::MobiFlightPing: {
            sendMobiFlightCommand("MF.Ping", "MobiFlight ping");
            mfPingSentAt_ = clock::now();
            break;
        }
        case SimCommand::Type::MobiFlightRaw: {
            sendMobiFlightCommand(cmd.name, "MobiFlight raw");
            LOG_INFO("-> MF: %s%s", cmd.name.c_str(), where.c_str());
            break;
        }
    }
    std::lock_guard<std::mutex> lock(m_);
    status_.commandsSent++;
    status_.lastActivity = clock::now();
}

void SimBridge::drainQueue() {
    for (;;) {
        SimCommand cmd;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (queue_.empty()) return;
            cmd = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!client_.isConnected()) {
            LOG_WARN("Dropped command (not connected to the simulator): %s", cmd.name.c_str());
            continue;
        }
        execute(cmd);
    }
}

void SimBridge::handleMessage(const RecvMessage& m) {
    switch (static_cast<RecvId>(m.id)) {
        case RecvId::Open: {
            RecvOpen o = SimConnectClient::parseOpen(m);
            char ver[64];
            snprintf(ver, sizeof ver, "%u.%u.%u.%u", o.appVersionMajor, o.appVersionMinor, o.appBuildMajor, o.appBuildMinor);
            LOG_INFO("SimConnect: open OK, simulator \"%s\" %s (SimConnect %u.%u.%u.%u)", o.applicationName.c_str(), ver,
                     o.simConnectVersionMajor, o.simConnectVersionMinor, o.simConnectBuildMajor, o.simConnectBuildMinor);
            {
                std::lock_guard<std::mutex> lock(m_);
                status_.state = ConnState::Connected;
                status_.simName = o.applicationName;
                status_.simVersion = ver;
                status_.lastError.clear();
            }
            onConnected();
            break;
        }
        case RecvId::Quit:
            dropConnection("simulator quit");
            break;
        case RecvId::Exception: {
            RecvException e = SimConnectClient::parseException(m);
            const std::string what = describeSend(e.sendId);
            bool mfRelated = what.rfind("MobiFlight", 0) == 0 || what.find("MobiFlight") != std::string::npos ||
                             what.rfind("H event", 0) == 0 || what.rfind("set L:", 0) == 0 || what.rfind("calculator", 0) == 0;
            {
                std::lock_guard<std::mutex> lock(m_);
                status_.exceptions++;
                if (mfRelated && status_.mobiflight != ModuleState::Detected) {
                    status_.mobiflight = ModuleState::NotFound;
                }
            }
            if (mfRelated) {
                LOG_WARN("SimConnect exception %s on \"%s\" (MobiFlight WASM module missing?)", exceptionName(e.exception), what.c_str());
            } else {
                LOG_ERROR("SimConnect exception %s (index %u) on \"%s\"", exceptionName(e.exception), e.index, what.c_str());
            }
            if (what.rfind("map event ", 0) == 0) eventIds_.erase(what.substr(10));
            break;
        }
        case RecvId::Event: {
            RecvEvent e = SimConnectClient::parseEvent(m);
            std::lock_guard<std::mutex> lock(m_);
            if (e.eventId == EV_SYS_SIM) status_.simRunning = e.data != 0;
            else if (e.eventId == EV_SYS_PAUSE) status_.paused = e.data != 0;
            break;
        }
        case RecvId::EventFilename: {
            RecvEventFilename e = SimConnectClient::parseEventFilename(m);
            if (e.eventId == EV_SYS_AIRCRAFT_LOADED) {
                LOG_INFO("Aircraft loaded: %s", e.fileName.c_str());
                std::lock_guard<std::mutex> lock(m_);
                status_.aircraft = e.fileName;
            }
            break;
        }
        case RecvId::SystemState: {
            RecvSystemState s = SimConnectClient::parseSystemState(m);
            std::lock_guard<std::mutex> lock(m_);
            if (s.requestId == REQ_SYS_AIRCRAFT_LOADED) status_.aircraft = s.dataString;
            else if (s.requestId == REQ_SYS_SIM) status_.simRunning = s.dataInteger != 0;
            break;
        }
        case RecvId::SimObjectData: {
            RecvClientData d = SimConnectClient::parseSimObjectData(m);
            if (d.requestId == REQ_TELEMETRY) handleTelemetry(d);
            break;
        }
        case RecvId::ClientData: {
            RecvClientData c = SimConnectClient::parseClientData(m);
            if (c.requestId == REQ_MF_RESPONSE || c.requestId == REQ_OUR_RESPONSE) {
                std::string text(reinterpret_cast<const char*>(c.data.data()), strnlen(reinterpret_cast<const char*>(c.data.data()), c.data.size()));
                if (text == "MF.Pong") {
                    {
                        std::lock_guard<std::mutex> lock(m_);
                        if (status_.mobiflight != ModuleState::Detected) LOG_INFO("MobiFlight WASM module detected");
                        status_.mobiflight = ModuleState::Detected;
                    }
                    registerMobiFlightClient();
                } else if (text == std::string("MF.Clients.Add.") + MF_CLIENT_NAME + ".Finished") {
                    if (!mfClientReady_) onMobiFlightClientReady();
                } else {
                    LOG_DEBUG("MobiFlight response: %s", text.c_str());
                }
            } else if (c.requestId == REQ_OUR_LVARS) {
                handleLVarData(c);
            }
            break;
        }
        default:
            break;
    }
}

void SimBridge::threadMain() {
    LOG_INFO("SimBridge thread started");
    std::vector<RecvMessage> msgs;
    while (running_) {
        if (forceDisconnect_.exchange(false)) {
            dropConnection("disconnect requested");
            {
                std::lock_guard<std::mutex> lock(m_);
                status_.lastError.clear();
            }
        }
        if (!client_.isConnected()) {
            const bool force = forceConnect_.exchange(false);
            const bool due = clock::now() >= nextConnectAttempt_;
            if ((wantConnect_ && due) || force) {
                if (!tryConnect()) {
                    // keep draining so queued commands do not pile up forever
                    drainQueue();
                }
            } else {
                drainQueue();
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait_for(lock, std::chrono::milliseconds(200));
            }
            continue;
        }

        msgs.clear();
        std::string err;
        if (!client_.poll(15, msgs, err)) {
            dropConnection(err);
            continue;
        }
        for (const auto& m : msgs) handleMessage(m);

        ConnState st;
        ModuleState mf;
        {
            std::lock_guard<std::mutex> lock(m_);
            st = status_.state;
            mf = status_.mobiflight;
        }
        if (st == ConnState::Opening && clock::now() - openSentAt_ > std::chrono::seconds(8)) {
            dropConnection("no Open reply from simulator (is this really the SimConnect port?)");
            continue;
        }
        if (st == ConnState::Connected) {
            drainQueue();
            if (telemetryWanted_ && !telemetryRequested_) setupTelemetry();
            bool dirty;
            {
                std::lock_guard<std::mutex> lock(m_);
                dirty = lvarWatchDirty_;
            }
            if (dirty && mfClientReady_) applyLVarWatch();
            const auto now = clock::now();
            if (mf == ModuleState::Probing && now - mfPingSentAt_ > std::chrono::seconds(4)) {
                std::lock_guard<std::mutex> lock(m_);
                status_.mobiflight = ModuleState::NotFound;
                LOG_WARN("MobiFlight WASM module not detected: H: events, L: vars and calculator code are unavailable until it is installed in the Community folder");
            }
            const bool probe = forceProbe_.exchange(false);
            if (probe || (mf == ModuleState::NotFound && now - mfPingSentAt_ > std::chrono::seconds(15))) {
                {
                    std::lock_guard<std::mutex> lock(m_);
                    status_.mobiflight = ModuleState::Probing;
                }
                sendMobiFlightCommand("MF.Ping", "MobiFlight ping");
                mfPingSentAt_ = now;
            }
        }
    }
    client_.close();
    LOG_INFO("SimBridge thread stopped");
}

}  // namespace lr::sim
