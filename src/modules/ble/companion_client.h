#ifndef __COMPANION_CLIENT_H__
#define __COMPANION_CLIENT_H__

#include "companion_protocol.h"

struct CompanionScanResult {
    String name;
    String title;
    String address;
    int rssi = 0;
    uint8_t addressType = BLE_ADDR_PUBLIC;
    bool authorized = false;
};

class CompanionClient {
public:
    CompanionClient();
    ~CompanionClient();

    bool connect(const CompanionScanResult &device);
    void disconnect(bool deinitBle = false);
    bool isConnected() const;

    bool discoverCompanionProfile();
    bool performHandshake();

    bool requestFileList();
    bool requestCameraCapture();
    bool requestScreenCapture();

    bool hasCapability(uint32_t capabilityBit) const;
    bool isAuthorized() const;

    const CompanionScanResult &getScanResult() const { return _scanResult; }
    const CompanionProtocol::DeviceInfo &getDeviceInfo() const { return _deviceInfo; }
    uint32_t getCapabilities() const { return _capabilities; }
    uint8_t getAuthState() const { return _authState; }
    uint8_t getSessionId() const { return _sessionId; }
    int getServiceCount() const { return _serviceCount; }
    String getLastError() const { return _lastError; }
    String getLastEvent() const { return _lastEvent; }

private:
    NimBLEClient *_client = nullptr;
    NimBLERemoteService *_identityService = nullptr;
    NimBLERemoteService *_controlService = nullptr;
    NimBLERemoteService *_fileService = nullptr;
    NimBLERemoteService *_cameraService = nullptr;
    NimBLERemoteService *_screenService = nullptr;

    NimBLERemoteCharacteristic *_deviceInfoChar = nullptr;
    NimBLERemoteCharacteristic *_capabilitiesChar = nullptr;
    NimBLERemoteCharacteristic *_authStateChar = nullptr;
    NimBLERemoteCharacteristic *_deviceIdChar = nullptr;
    NimBLERemoteCharacteristic *_commandChar = nullptr;
    NimBLERemoteCharacteristic *_statusChar = nullptr;
    NimBLERemoteCharacteristic *_eventChar = nullptr;

    CompanionScanResult _scanResult;
    CompanionProtocol::DeviceInfo _deviceInfo;
    uint32_t _capabilities = 0;
    uint8_t _authState = CompanionProtocol::AUTH_UNKNOWN;
    uint8_t _sessionId = 1;
    int _serviceCount = 0;
    String _lastError = "";
    String _lastEvent = "";

    bool sendCommand(CompanionProtocol::CommandCode command, uint16_t requestId = 0, uint32_t argument = 0);
    void resetState();
};

#endif
