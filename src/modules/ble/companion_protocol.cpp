#include "companion_protocol.h"

namespace CompanionProtocol {

DeviceInfo parseDeviceInfo(const std::string &raw) {
    DeviceInfo info;
    if (raw.size() < sizeof(DeviceInfoPayload)) return info;

    const auto *payload = reinterpret_cast<const DeviceInfoPayload *>(raw.data());
    info.protocolMajor = payload->protocolMajor;
    info.protocolMinor = payload->protocolMinor;
    info.mtuPreferred = payload->mtuPreferred;
    info.deviceFlags = payload->deviceFlags;
    info.deviceName = String(payload->deviceName);
    info.deviceName.trim();
    return info;
}

uint32_t parseCapabilities(const std::string &raw) {
    if (raw.size() < sizeof(uint32_t)) return 0;
    return *reinterpret_cast<const uint32_t *>(raw.data());
}

uint8_t parseAuthState(const std::string &raw) {
    if (raw.empty()) return AUTH_UNKNOWN;
    return static_cast<uint8_t>(raw[0]);
}

String parseDeviceId(const std::string &raw) {
    String id = String(raw.c_str());
    id.trim();
    return id;
}

std::string buildCommand(CommandCode command, uint8_t sessionId, uint16_t requestId, uint32_t argument) {
    CommandPacket packet = {
        static_cast<uint8_t>(command),
        sessionId,
        requestId,
        argument,
    };
    return std::string(reinterpret_cast<const char *>(&packet), sizeof(packet));
}

bool advertisesCompanionService(const NimBLEAdvertisedDevice *device) {
    if (!device || !device->haveServiceUUID()) return false;
    return device->isAdvertisingService(NimBLEUUID(IDENTITY_SERVICE_UUID));
}

String capabilityToString(uint32_t capabilityBit) {
    switch (capabilityBit) {
        case CAP_FILES_LIST: return "Remote Files: List";
        case CAP_FILES_GET: return "Remote Files: Download";
        case CAP_FILES_PUT: return "Remote Files: Upload";
        case CAP_CAMERA_SHOT: return "Remote Camera";
        case CAP_SCREEN_SHOT: return "Remote Screen";
        default: return "Unknown Capability";
    }
}

String authStateToString(uint8_t authState) {
    switch (authState) {
        case AUTH_REQUIRED: return "AUTH REQUIRED";
        case AUTH_PENDING: return "AUTH PENDING";
        case AUTHORIZED: return "AUTHORIZED";
        case AUTH_REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

} // namespace CompanionProtocol
