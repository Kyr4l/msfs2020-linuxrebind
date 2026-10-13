#include "sim/SimConnectClient.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace lr::sim {

namespace {
constexpr size_t HEADER_SIZE = 16;  // size, protocol, type, sendId
constexpr size_t MAX_PACKET = 1 << 20;

struct OpenVersion { const char* alias; uint32_t major, minor, buildMajor, buildMinor; };
OpenVersion openVersionFor(Protocol p) {
    switch (p) {
        case Protocol::FSX_SP2: return {"XSF", 10, 0, 61259, 0};
        case Protocol::SunRise: return {"RS", 12, 2, 282174, 999};
        case Protocol::KittyHawk:
        default: return {"HK", 11, 0, 62651, 3};
    }
}

const char* const kExceptionNames[] = {
    "NONE", "ERROR", "SIZE_MISMATCH", "UNRECOGNIZED_ID", "UNOPENED", "VERSION_MISMATCH", "TOO_MANY_GROUPS",
    "NAME_UNRECOGNIZED", "TOO_MANY_EVENT_NAMES", "EVENT_ID_DUPLICATE", "TOO_MANY_MAPS", "TOO_MANY_OBJECTS",
    "TOO_MANY_REQUESTS", "WEATHER_INVALID_PORT", "WEATHER_INVALID_METAR", "WEATHER_UNABLE_TO_GET_OBSERVATION",
    "WEATHER_UNABLE_TO_CREATE_STATION", "WEATHER_UNABLE_TO_REMOVE_STATION", "INVALID_DATA_TYPE",
    "INVALID_DATA_SIZE", "DATA_ERROR", "INVALID_ARRAY", "CREATE_OBJECT_FAILED", "LOAD_FLIGHTPLAN_FAILED",
    "OPERATION_INVALID_FOR_OBJECT_TYPE", "ILLEGAL_OPERATION", "ALREADY_SUBSCRIBED", "INVALID_ENUM",
    "DEFINITION_ERROR", "DUPLICATE_ID", "DATUM_ID", "OUT_OF_BOUNDS", "ALREADY_CREATED",
    "OBJECT_OUTSIDE_REALITY_BUBBLE", "OBJECT_CONTAINER", "OBJECT_AI", "OBJECT_ATC", "OBJECT_SCHEDULE",
    "JETWAY_DATA", "ACTION_NOT_FOUND", "NOT_AN_ACTION", "INCORRECT_ACTION_PARAMS", "GET_INPUT_EVENT_FAILED",
    "SET_INPUT_EVENT_FAILED", "EVENT_NAME_RESERVED", "INTERNAL", "CAMERA_API",
};
}  // namespace

const char* exceptionName(uint32_t code) {
    if (code < sizeof(kExceptionNames) / sizeof(kExceptionNames[0])) return kExceptionNames[code];
    return "UNKNOWN";
}

SimConnectClient::~SimConnectClient() { close(); }

bool SimConnectClient::connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res); rc != 0) {
        err = std::string("resolve failed: ") + gai_strerror(rc);
        return false;
    }
    bool connected = false;
    for (addrinfo* ai = res; ai && !connected; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) { err = strerror(errno); continue; }
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) { err = strerror(errno); ::close(fd); continue; }
        if (rc < 0) {
            pollfd pfd{fd, POLLOUT, 0};
            int pr = ::poll(&pfd, 1, timeoutMs);
            if (pr <= 0) { err = pr == 0 ? "connect timeout" : strerror(errno); ::close(fd); continue; }
            int soerr = 0;
            socklen_t len = sizeof soerr;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
            if (soerr != 0) { err = strerror(soerr); ::close(fd); continue; }
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fd_ = fd;
        connected = true;
    }
    freeaddrinfo(res);
    if (connected) {
        nextSendId_ = 0;
        rx_.clear();
        tx_.clear();
        err.clear();
    }
    return connected;
}

void SimConnectClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    rx_.clear();
    tx_.clear();
}

ByteWriter SimConnectClient::begin(uint32_t packetType) {
    ByteWriter w;
    w.u32(0);  // size, patched in send()
    w.u32(static_cast<uint32_t>(protocol_));
    w.u32(0xf0000000u | packetType);
    w.u32(0);  // send id, patched in send()
    return w;
}

uint32_t SimConnectClient::send(ByteWriter& w) {
    const uint32_t id = nextSendId_++;
    w.u32At(0, static_cast<uint32_t>(w.size()));
    w.u32At(12, id);
    const auto& d = w.data();
    tx_.insert(tx_.end(), d.begin(), d.end());
    std::string err;
    flushTx(err);
    return id;
}

bool SimConnectClient::flushTx(std::string& err) {
    while (!tx_.empty() && fd_ >= 0) {
        ssize_t n = ::send(fd_, tx_.data(), tx_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            bytesSent_ += static_cast<uint64_t>(n);
            tx_.erase(tx_.begin(), tx_.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // retry later from poll()
        if (n < 0 && errno == EINTR) continue;
        err = n == 0 ? "connection closed" : strerror(errno);
        return false;
    }
    return true;
}

uint32_t SimConnectClient::open(const std::string& appName) {
    const OpenVersion v = openVersionFor(protocol_);
    ByteWriter w = begin(0x01);
    w.str(appName, 256);
    w.u32(0);
    w.u8(0);
    w.str(v.alias, 3);
    w.u32(v.major);
    w.u32(v.minor);
    w.u32(v.buildMajor);
    w.u32(v.buildMinor);
    return send(w);
}

uint32_t SimConnectClient::mapClientEventToSimEvent(uint32_t eventId, const std::string& eventName) {
    ByteWriter w = begin(0x04);
    w.u32(eventId);
    w.str(eventName, 256);
    return send(w);
}

uint32_t SimConnectClient::transmitClientEvent(uint32_t objectId, uint32_t eventId, uint32_t data, uint32_t groupId, uint32_t flags) {
    ByteWriter w = begin(0x05);
    w.u32(objectId);
    w.u32(eventId);
    w.u32(data);
    w.u32(groupId);
    w.u32(flags);
    return send(w);
}

uint32_t SimConnectClient::transmitClientEventEx1(uint32_t objectId, uint32_t eventId, uint32_t groupId, uint32_t flags,
                                                  uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4) {
    ByteWriter w = begin(0x44);
    w.u32(objectId);
    w.u32(eventId);
    w.u32(groupId);
    w.u32(flags);
    w.u32(d0); w.u32(d1); w.u32(d2); w.u32(d3); w.u32(d4);
    return send(w);
}

uint32_t SimConnectClient::addClientEventToNotificationGroup(uint32_t groupId, uint32_t eventId, bool maskable) {
    ByteWriter w = begin(0x07);
    w.u32(groupId);
    w.u32(eventId);
    w.u32(maskable ? 1 : 0);
    return send(w);
}

uint32_t SimConnectClient::setNotificationGroupPriority(uint32_t groupId, uint32_t priority) {
    ByteWriter w = begin(0x09);
    w.u32(groupId);
    w.u32(priority);
    return send(w);
}

uint32_t SimConnectClient::subscribeToSystemEvent(uint32_t eventId, const std::string& eventName) {
    ByteWriter w = begin(0x17);
    w.u32(eventId);
    w.str(eventName, 256);
    return send(w);
}

uint32_t SimConnectClient::unsubscribeFromSystemEvent(uint32_t eventId) {
    ByteWriter w = begin(0x18);
    w.u32(eventId);
    return send(w);
}

uint32_t SimConnectClient::requestSystemState(uint32_t requestId, const std::string& state) {
    ByteWriter w = begin(0x35);
    w.u32(requestId);
    w.str(state, 256);
    return send(w);
}

uint32_t SimConnectClient::addToDataDefinition(uint32_t defineId, const std::string& datumName, const std::string& unitsName,
                                               uint32_t dataType, float epsilon, uint32_t datumId) {
    ByteWriter w = begin(0x0c);
    w.u32(defineId);
    w.str(datumName, 256);
    w.str(unitsName, 256);
    w.u32(dataType);
    w.f32(epsilon);
    w.u32(datumId);
    return send(w);
}

uint32_t SimConnectClient::clearDataDefinition(uint32_t defineId) {
    ByteWriter w = begin(0x0d);
    w.u32(defineId);
    return send(w);
}

uint32_t SimConnectClient::requestDataOnSimObject(uint32_t requestId, uint32_t defineId, uint32_t objectId, uint32_t period, uint32_t flags,
                                                  uint32_t origin, uint32_t interval, uint32_t limit) {
    ByteWriter w = begin(0x0e);
    w.u32(requestId);
    w.u32(defineId);
    w.u32(objectId);
    w.u32(period);
    w.u32(flags);
    w.u32(origin);
    w.u32(interval);
    w.u32(limit);
    return send(w);
}

uint32_t SimConnectClient::mapClientDataNameToID(const std::string& name, uint32_t clientDataId) {
    ByteWriter w = begin(0x37);
    w.str(name, 256);
    w.u32(clientDataId);
    return send(w);
}

uint32_t SimConnectClient::createClientData(uint32_t clientDataId, uint32_t size, uint32_t flags) {
    ByteWriter w = begin(0x38);
    w.u32(clientDataId);
    w.u32(size);
    w.u32(flags);
    return send(w);
}

uint32_t SimConnectClient::addToClientDataDefinition(uint32_t defineId, uint32_t offset, uint32_t sizeOrType, float epsilon, uint32_t datumId) {
    ByteWriter w = begin(0x39);
    w.u32(defineId);
    w.u32(offset);
    w.u32(sizeOrType);
    w.f32(epsilon);
    w.u32(datumId);
    return send(w);
}

uint32_t SimConnectClient::clearClientDataDefinition(uint32_t defineId) {
    ByteWriter w = begin(0x3a);
    w.u32(defineId);
    return send(w);
}

uint32_t SimConnectClient::requestClientData(uint32_t clientDataId, uint32_t requestId, uint32_t defineId, uint32_t period,
                                             uint32_t flags, uint32_t origin, uint32_t interval, uint32_t limit) {
    ByteWriter w = begin(0x3b);
    w.u32(clientDataId);
    w.u32(requestId);
    w.u32(defineId);
    w.u32(period);
    w.u32(flags);
    w.u32(origin);
    w.u32(interval);
    w.u32(limit);
    return send(w);
}

uint32_t SimConnectClient::setClientData(uint32_t clientDataId, uint32_t defineId, uint32_t unitSize, const void* data, size_t len) {
    ByteWriter w = begin(0x3c);
    w.u32(clientDataId);
    w.u32(defineId);
    w.u32(0);  // reserved
    w.u32(1);  // array count
    w.u32(unitSize);
    w.bytes(data, len);
    if (len < unitSize) {
        std::vector<uint8_t> pad(unitSize - len, 0);
        w.bytes(pad.data(), pad.size());
    }
    return send(w);
}

bool SimConnectClient::poll(int timeoutMs, std::vector<RecvMessage>& out, std::string& err) {
    if (fd_ < 0) { err = "not connected"; return false; }
    if (!flushTx(err)) { close(); return false; }

    pollfd pfd{fd_, POLLIN, 0};
    if (!tx_.empty()) pfd.events |= POLLOUT;
    int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr < 0) {
        if (errno == EINTR) return true;
        err = strerror(errno);
        close();
        return false;
    }
    if (pr == 0) return true;
    if (pfd.revents & POLLOUT) {
        if (!flushTx(err)) { close(); return false; }
    }
    if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
        uint8_t buf[65536];
        for (;;) {
            ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n > 0) {
                bytesReceived_ += static_cast<uint64_t>(n);
                rx_.insert(rx_.end(), buf, buf + n);
                if (static_cast<size_t>(n) < sizeof buf) break;
                continue;
            }
            if (n == 0) { err = "connection closed by simulator"; close(); return false; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            err = strerror(errno);
            close();
            return false;
        }
    }

    // Split the stream into complete packets: [dwSize][dwVersion][dwID][payload...]
    size_t off = 0;
    while (rx_.size() - off >= 12) {
        uint32_t size = 0;
        std::memcpy(&size, rx_.data() + off, 4);
        if (size < 12 || size > MAX_PACKET) {
            err = "protocol error: bad packet size " + std::to_string(size);
            close();
            return false;
        }
        if (rx_.size() - off < size) break;
        RecvMessage m;
        std::memcpy(&m.version, rx_.data() + off + 4, 4);
        std::memcpy(&m.id, rx_.data() + off + 8, 4);
        m.body.assign(rx_.begin() + off + 12, rx_.begin() + off + size);
        out.push_back(std::move(m));
        off += size;
    }
    if (off > 0) rx_.erase(rx_.begin(), rx_.begin() + off);
    return true;
}

RecvOpen SimConnectClient::parseOpen(const RecvMessage& m) {
    ByteReader r(m.body.data(), m.body.size());
    RecvOpen o;
    o.applicationName = r.str(256);
    o.appVersionMajor = r.u32(); o.appVersionMinor = r.u32(); o.appBuildMajor = r.u32(); o.appBuildMinor = r.u32();
    o.simConnectVersionMajor = r.u32(); o.simConnectVersionMinor = r.u32();
    o.simConnectBuildMajor = r.u32(); o.simConnectBuildMinor = r.u32();
    return o;
}

RecvException SimConnectClient::parseException(const RecvMessage& m) {
    ByteReader r(m.body.data(), m.body.size());
    RecvException e;
    e.exception = r.u32(); e.sendId = r.u32(); e.index = r.u32();
    return e;
}

RecvEvent SimConnectClient::parseEvent(const RecvMessage& m) {
    ByteReader r(m.body.data(), m.body.size());
    RecvEvent e;
    e.groupId = r.u32(); e.eventId = r.u32(); e.data = r.u32();
    return e;
}

RecvEventFilename SimConnectClient::parseEventFilename(const RecvMessage& m) {
    ByteReader r(m.body.data(), m.body.size());
    RecvEventFilename e;
    e.groupId = r.u32(); e.eventId = r.u32(); e.data = r.u32();
    e.fileName = r.str(260);
    e.flags = r.u32();
    return e;
}

RecvSystemState SimConnectClient::parseSystemState(const RecvMessage& m) {
    ByteReader r(m.body.data(), m.body.size());
    RecvSystemState s;
    s.requestId = r.u32(); s.dataInteger = r.u32(); s.dataFloat = r.f32();
    s.dataString = r.str(260);
    return s;
}

RecvClientData SimConnectClient::parseClientData(const RecvMessage& m) {
    ByteReader r(m.body.data(), m.body.size());
    RecvClientData c;
    c.requestId = r.u32(); c.objectId = r.u32(); c.defineId = r.u32(); c.flags = r.u32();
    c.entryNumber = r.u32(); c.outOf = r.u32(); c.defineCount = r.u32();
    c.data.assign(r.cursor(), r.cursor() + r.remaining());
    return c;
}

}  // namespace lr::sim
