#ifndef __COMPANION_PROTOCOL_H__
#define __COMPANION_PROTOCOL_H__

#include <Arduino.h>
#include <NimBLEDevice.h>

namespace CompanionProtocol {

static constexpr const char *BASE_UUID = "7d4f0000-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *IDENTITY_SERVICE_UUID = "7d4f1000-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *CONTROL_SERVICE_UUID = "7d4f2000-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *TRANSFER_SERVICE_UUID = "7d4f3000-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *FILE_SERVICE_UUID = "7d4f4000-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *CAMERA_SERVICE_UUID = "7d4f5000-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *SCREEN_SERVICE_UUID = "7d4f6000-8a6d-4b1e-a9c2-0f4e3b120000";

static constexpr const char *DEVICE_INFO_CHAR_UUID = "7d4f1001-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *CAPABILITIES_CHAR_UUID = "7d4f1002-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *AUTH_STATE_CHAR_UUID = "7d4f1003-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *DEVICE_ID_CHAR_UUID = "7d4f1004-8a6d-4b1e-a9c2-0f4e3b120000";

static constexpr const char *COMMAND_CHAR_UUID = "7d4f2001-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *STATUS_CHAR_UUID = "7d4f2002-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *EVENT_CHAR_UUID = "7d4f2003-8a6d-4b1e-a9c2-0f4e3b120000";

static constexpr const char *TRANSFER_META_CHAR_UUID = "7d4f3001-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *TX_CHUNK_CHAR_UUID = "7d4f3002-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *RX_CHUNK_CHAR_UUID = "7d4f3003-8a6d-4b1e-a9c2-0f4e3b120000";
static constexpr const char *TRANSFER_ACK_CHAR_UUID = "7d4f3004-8a6d-4b1e-a9c2-0f4e3b120000";

enum CapabilityBits : uint32_t {
    CAP_FILES_LIST = 1 << 0,
    CAP_FILES_GET = 1 << 1,
    CAP_FILES_PUT = 1 << 2,
    CAP_CAMERA_SHOT = 1 << 3,
    CAP_SCREEN_SHOT = 1 << 4
};

enum CommandCode : uint8_t {
    CMD_HELLO = 0x01,
    CMD_AUTH_BEGIN = 0x02,
    CMD_AUTH_CONFIRM = 0x03,
    CMD_LIST_FILES = 0x10,
    CMD_GET_FILE = 0x11,
    CMD_PUT_FILE = 0x12,
    CMD_CAPTURE_CAMERA = 0x20,
    CMD_CAPTURE_SCREEN = 0x21,
    CMD_CANCEL_TRANSFER = 0x30
};

enum AuthState : uint8_t {
    AUTH_UNKNOWN = 0,
    AUTH_REQUIRED = 1,
    AUTH_PENDING = 2,
    AUTHORIZED = 3,
    AUTH_REJECTED = 4
};

enum StatusCode : uint8_t {
    STATUS_IDLE = 0,
    STATUS_OK = 1,
    STATUS_BUSY = 2,
    STATUS_ERROR = 3
};

enum TransferFlags : uint8_t {
    FLAG_FIRST_CHUNK = 1 << 0,
    FLAG_LAST_CHUNK = 1 << 1,
    FLAG_ACK_REQUIRED = 1 << 2,
    FLAG_ERROR = 1 << 3
};

struct DeviceInfo {
    uint8_t protocolMajor = 0;
    uint8_t protocolMinor = 0;
    uint16_t mtuPreferred = 0;
    uint32_t deviceFlags = 0;
    String deviceName = "";
    String deviceId = "";
};

#pragma pack(push, 1)
struct DeviceInfoPayload {
    uint8_t protocolMajor;
    uint8_t protocolMinor;
    uint16_t mtuPreferred;
    uint32_t deviceFlags;
    char deviceName[32];
};

struct CommandPacket {
    uint8_t opCode;
    uint8_t sessionId;
    uint16_t requestId;
    uint32_t argument;
};

struct TransferHeader {
    uint8_t opCode;
    uint8_t sessionId;
    uint16_t transferId;
    uint32_t totalSize;
    uint16_t chunkIndex;
    uint16_t chunkSize;
    uint8_t flags;
};
#pragma pack(pop)

DeviceInfo parseDeviceInfo(const std::string &raw);
uint32_t parseCapabilities(const std::string &raw);
uint8_t parseAuthState(const std::string &raw);
String parseDeviceId(const std::string &raw);
std::string buildCommand(CommandCode command, uint8_t sessionId = 1, uint16_t requestId = 0, uint32_t argument = 0);
bool advertisesCompanionService(const NimBLEAdvertisedDevice *device);
String capabilityToString(uint32_t capabilityBit);
String authStateToString(uint8_t authState);

} // namespace CompanionProtocol

#endif
