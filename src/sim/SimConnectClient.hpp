#pragma once
// Native (no SimConnect.dll) implementation of the SimConnect TCP wire protocol, enough for
// mapping/transmitting client events, system events/state and client data areas.
// Packet layout and IDs follow the openly documented protocol as implemented by node-simconnect.
#include <cstdint>
#include <string>
#include <vector>

#include "sim/ByteBuffer.hpp"

namespace lr::sim {

// Protocol version sent in every packet header. 5 = MSFS 2020 ("KittyHawk"), 6 = MSFS 2024.
enum class Protocol : uint32_t { FSX_SP2 = 4, KittyHawk = 5, SunRise = 6 };

enum class RecvId : uint32_t {
    Null = 0, Exception = 1, Open = 2, Quit = 3, Event = 4, EventObjectAddRemove = 5, EventFilename = 6,
    EventFrame = 7, SimObjectData = 8, SimObjectDataByType = 9, WeatherObservation = 10, CloudState = 11,
    AssignedObjectId = 12, ReservedKey = 13, CustomAction = 14, SystemState = 15, ClientData = 16,
    EventWeatherMode = 17, AirportList = 18, VorList = 19, NdbList = 20, WaypointList = 21,
    EventMultiplayerServerStarted = 22, EventMultiplayerClientStarted = 23, EventMultiplayerSessionEnded = 24,
    EventRaceEnd = 25, EventRaceLap = 26, EventEx1 = 27, FacilityData = 28, FacilityDataEnd = 29,
    FacilityMinimalList = 30, JetwayData = 31, ControllersList = 32, ActionCallback = 33,
    EnumerateInputEvents = 34, GetInputEvent = 35, SubscribeInputEvent = 36, EnumerateInputEventParams = 37,
    EnumerateSimobjectAndLiveryList = 38, FlowEvent = 39, CameraData = 40, CameraStatus = 41,
    CameraDefinitionList = 42, CommBus = 43, CameraWorldLocker = 44,
};

constexpr uint32_t OBJECT_ID_USER = 0;
constexpr uint32_t UNUSED = 0xffffffffu;
constexpr uint32_t GROUP_PRIORITY_HIGHEST = 1;
constexpr uint32_t GROUP_PRIORITY_HIGHEST_MASKABLE = 10000000;
constexpr uint32_t GROUP_PRIORITY_STANDARD = 1900000000;
constexpr uint32_t GROUP_PRIORITY_DEFAULT = 2000000000;
constexpr uint32_t EVENT_FLAG_DEFAULT = 0;
constexpr uint32_t EVENT_FLAG_GROUPID_IS_PRIORITY = 0x10;

enum SimPeriod : uint32_t { PERIOD_NEVER = 0, PERIOD_ONCE = 1, PERIOD_VISUAL_FRAME = 2, PERIOD_SIM_FRAME = 3, PERIOD_SECOND = 4 };
constexpr uint32_t DATA_REQUEST_FLAG_DEFAULT = 0;
constexpr uint32_t DATA_REQUEST_FLAG_CHANGED = 1;
constexpr uint32_t DATA_REQUEST_FLAG_TAGGED = 2;
enum SimDataType : uint32_t { DATATYPE_INT32 = 1, DATATYPE_INT64 = 2, DATATYPE_FLOAT32 = 3, DATATYPE_FLOAT64 = 4 };

enum ClientDataPeriod : uint32_t { CD_PERIOD_NEVER = 0, CD_PERIOD_ONCE = 1, CD_PERIOD_VISUAL_FRAME = 2, CD_PERIOD_ON_SET = 3, CD_PERIOD_SECOND = 4 };
constexpr uint32_t CD_REQUEST_FLAG_DEFAULT = 0;
constexpr uint32_t CD_REQUEST_FLAG_CHANGED = 1;
constexpr uint32_t CD_REQUEST_FLAG_TAGGED = 2;
constexpr uint32_t CD_CREATE_FLAG_DEFAULT = 0;
constexpr uint32_t CD_CREATE_FLAG_READ_ONLY = 1;

struct RecvMessage {
    uint32_t version = 0;
    uint32_t id = 0;
    std::vector<uint8_t> body;  // everything after dwSize/dwVersion/dwID
};

struct RecvOpen {
    std::string applicationName;
    uint32_t appVersionMajor = 0, appVersionMinor = 0, appBuildMajor = 0, appBuildMinor = 0;
    uint32_t simConnectVersionMajor = 0, simConnectVersionMinor = 0, simConnectBuildMajor = 0, simConnectBuildMinor = 0;
};
struct RecvException { uint32_t exception = 0, sendId = 0, index = 0; };
struct RecvEvent { uint32_t groupId = 0, eventId = 0, data = 0; };
struct RecvEventFilename { uint32_t groupId = 0, eventId = 0, data = 0; std::string fileName; uint32_t flags = 0; };
struct RecvSystemState { uint32_t requestId = 0; uint32_t dataInteger = 0; float dataFloat = 0; std::string dataString; };
struct RecvClientData {
    uint32_t requestId = 0, objectId = 0, defineId = 0, flags = 0, entryNumber = 0, outOf = 0, defineCount = 0;
    std::vector<uint8_t> data;
};

const char* exceptionName(uint32_t code);

class SimConnectClient {
public:
    SimConnectClient() = default;
    ~SimConnectClient();
    SimConnectClient(const SimConnectClient&) = delete;
    SimConnectClient& operator=(const SimConnectClient&) = delete;

    void setProtocol(Protocol p) { protocol_ = p; }
    Protocol protocol() const { return protocol_; }

    bool connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err);
    void close();
    bool isConnected() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    // Each call returns the packet "send id" which exceptions refer back to.
    uint32_t open(const std::string& appName);
    uint32_t mapClientEventToSimEvent(uint32_t eventId, const std::string& eventName);
    uint32_t transmitClientEvent(uint32_t objectId, uint32_t eventId, uint32_t data, uint32_t groupId, uint32_t flags);
    uint32_t transmitClientEventEx1(uint32_t objectId, uint32_t eventId, uint32_t groupId, uint32_t flags,
                                    uint32_t d0, uint32_t d1 = 0, uint32_t d2 = 0, uint32_t d3 = 0, uint32_t d4 = 0);
    uint32_t addClientEventToNotificationGroup(uint32_t groupId, uint32_t eventId, bool maskable);
    uint32_t setNotificationGroupPriority(uint32_t groupId, uint32_t priority);
    uint32_t subscribeToSystemEvent(uint32_t eventId, const std::string& eventName);
    uint32_t unsubscribeFromSystemEvent(uint32_t eventId);
    uint32_t requestSystemState(uint32_t requestId, const std::string& state);
    uint32_t addToDataDefinition(uint32_t defineId, const std::string& datumName, const std::string& unitsName,
                                 uint32_t dataType = DATATYPE_FLOAT64, float epsilon = 0, uint32_t datumId = UNUSED);
    uint32_t clearDataDefinition(uint32_t defineId);
    uint32_t requestDataOnSimObject(uint32_t requestId, uint32_t defineId, uint32_t objectId, uint32_t period, uint32_t flags = 0,
                                    uint32_t origin = 0, uint32_t interval = 0, uint32_t limit = 0);
    uint32_t mapClientDataNameToID(const std::string& name, uint32_t clientDataId);
    uint32_t createClientData(uint32_t clientDataId, uint32_t size, uint32_t flags);
    uint32_t addToClientDataDefinition(uint32_t defineId, uint32_t offset, uint32_t sizeOrType, float epsilon = 0, uint32_t datumId = 0);
    uint32_t clearClientDataDefinition(uint32_t defineId);
    uint32_t requestClientData(uint32_t clientDataId, uint32_t requestId, uint32_t defineId, uint32_t period, uint32_t flags,
                               uint32_t origin = 0, uint32_t interval = 0, uint32_t limit = 0);
    uint32_t setClientData(uint32_t clientDataId, uint32_t defineId, uint32_t unitSize, const void* data, size_t len);

    // Pumps the socket for up to timeoutMs. Returns false when the connection is gone (err set).
    bool poll(int timeoutMs, std::vector<RecvMessage>& out, std::string& err);

    static RecvOpen parseOpen(const RecvMessage& m);
    static RecvException parseException(const RecvMessage& m);
    static RecvEvent parseEvent(const RecvMessage& m);
    static RecvEventFilename parseEventFilename(const RecvMessage& m);
    static RecvSystemState parseSystemState(const RecvMessage& m);
    static RecvClientData parseClientData(const RecvMessage& m);
    static RecvClientData parseSimObjectData(const RecvMessage& m) { return parseClientData(m); }  // same layout

    uint64_t bytesSent() const { return bytesSent_; }
    uint64_t bytesReceived() const { return bytesReceived_; }

private:
    ByteWriter begin(uint32_t packetType);
    uint32_t send(ByteWriter& w);
    bool flushTx(std::string& err);

    int fd_ = -1;
    Protocol protocol_ = Protocol::KittyHawk;
    uint32_t nextSendId_ = 0;
    std::vector<uint8_t> rx_;
    std::vector<uint8_t> tx_;
    std::string lastError_;
    uint64_t bytesSent_ = 0, bytesReceived_ = 0;
};

}  // namespace lr::sim
