#include "companion_client.h"

CompanionClient::CompanionClient() {}

CompanionClient::~CompanionClient() { disconnect(false); }

void CompanionClient::resetState() {
    _identityService = nullptr;
    _controlService = nullptr;
    _fileService = nullptr;
    _cameraService = nullptr;
    _screenService = nullptr;
    _deviceInfoChar = nullptr;
    _capabilitiesChar = nullptr;
    _authStateChar = nullptr;
    _deviceIdChar = nullptr;
    _commandChar = nullptr;
    _statusChar = nullptr;
    _eventChar = nullptr;
    _deviceInfo = {};
    _capabilities = 0;
    _authState = CompanionProtocol::AUTH_UNKNOWN;
    _serviceCount = 0;
    _lastError = "";
    _lastEvent = "";
}

bool CompanionClient::connect(const CompanionScanResult &device) {
    disconnect(false);

    if (!NimBLEDevice::isInitialized()) BLEDevice::init("");

    _scanResult = device;
    _client = NimBLEDevice::createClient();
    if (!_client) {
        _lastError = "Companion client create failed";
        return false;
    }

    _client->setConnectTimeout(8);
    _client->setConnectionParams(12, 12, 0, 400);

    NimBLEAddress target(std::string(device.address.c_str()), device.addressType);
    if (!_client->connect(target, false)) {
        _lastError = "Companion connect failed";
        disconnect(false);
        return false;
    }

    _serviceCount = _client->getServices(true).size();
    if (!discoverCompanionProfile()) {
        disconnect(false);
        return false;
    }

    if (!performHandshake()) {
        disconnect(false);
        return false;
    }

    return true;
}

void CompanionClient::disconnect(bool deinitBle) {
    if (_client) {
        if (_client->isConnected()) _client->disconnect();
        NimBLEDevice::deleteClient(_client);
        _client = nullptr;
    }
    resetState();

    if (deinitBle) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
        esp_bt_controller_deinit();
#else
        BLEDevice::deinit();
#endif
    }
}

bool CompanionClient::isConnected() const { return _client && _client->isConnected(); }

bool CompanionClient::discoverCompanionProfile() {
    if (!isConnected()) {
        _lastError = "Companion not connected";
        return false;
    }

    _identityService = _client->getService(NimBLEUUID(CompanionProtocol::IDENTITY_SERVICE_UUID));
    _controlService = _client->getService(NimBLEUUID(CompanionProtocol::CONTROL_SERVICE_UUID));
    _fileService = _client->getService(NimBLEUUID(CompanionProtocol::FILE_SERVICE_UUID));
    _cameraService = _client->getService(NimBLEUUID(CompanionProtocol::CAMERA_SERVICE_UUID));
    _screenService = _client->getService(NimBLEUUID(CompanionProtocol::SCREEN_SERVICE_UUID));

    if (!_identityService || !_controlService) {
        _lastError = "Companion profile incomplete";
        return false;
    }

    _deviceInfoChar = _identityService->getCharacteristic(NimBLEUUID(CompanionProtocol::DEVICE_INFO_CHAR_UUID));
    _capabilitiesChar =
        _identityService->getCharacteristic(NimBLEUUID(CompanionProtocol::CAPABILITIES_CHAR_UUID));
    _authStateChar = _identityService->getCharacteristic(NimBLEUUID(CompanionProtocol::AUTH_STATE_CHAR_UUID));
    _deviceIdChar = _identityService->getCharacteristic(NimBLEUUID(CompanionProtocol::DEVICE_ID_CHAR_UUID));
    _commandChar = _controlService->getCharacteristic(NimBLEUUID(CompanionProtocol::COMMAND_CHAR_UUID));
    _statusChar = _controlService->getCharacteristic(NimBLEUUID(CompanionProtocol::STATUS_CHAR_UUID));
    _eventChar = _controlService->getCharacteristic(NimBLEUUID(CompanionProtocol::EVENT_CHAR_UUID));

    if (!_deviceInfoChar || !_capabilitiesChar || !_authStateChar || !_commandChar) {
        _lastError = "Companion chars missing";
        return false;
    }

    return true;
}

bool CompanionClient::performHandshake() {
    if (!_deviceInfoChar || !_capabilitiesChar || !_authStateChar || !_commandChar) {
        _lastError = "Handshake unavailable";
        return false;
    }

    _deviceInfo = CompanionProtocol::parseDeviceInfo(_deviceInfoChar->readValue());
    _capabilities = CompanionProtocol::parseCapabilities(_capabilitiesChar->readValue());
    _authState = CompanionProtocol::parseAuthState(_authStateChar->readValue());
    if (_deviceIdChar) _deviceInfo.deviceId = CompanionProtocol::parseDeviceId(_deviceIdChar->readValue());

    if (_deviceInfo.deviceName.isEmpty()) _deviceInfo.deviceName = _scanResult.name;
    if (_deviceInfo.deviceId.isEmpty()) _deviceInfo.deviceId = _scanResult.address;

    if (!sendCommand(CompanionProtocol::CMD_HELLO)) {
        _lastError = "HELLO write failed";
        return false;
    }

    if (_statusChar && _statusChar->canRead()) {
        std::string statusRaw = _statusChar->readValue();
        if (!statusRaw.empty() && static_cast<uint8_t>(statusRaw[0]) == CompanionProtocol::STATUS_ERROR) {
            _lastError = "Companion status error";
            return false;
        }
    }

    if (_eventChar && _eventChar->canRead()) {
        _lastEvent = String(_eventChar->readValue().c_str());
        _lastEvent.trim();
    }

    if (_authState == CompanionProtocol::AUTH_REJECTED) {
        _lastError = "Companion rejected";
        return false;
    }

    return true;
}

bool CompanionClient::sendCommand(CompanionProtocol::CommandCode command, uint16_t requestId, uint32_t argument) {
    if (!_commandChar) return false;
    std::string packet = CompanionProtocol::buildCommand(command, _sessionId, requestId, argument);
    return _commandChar->writeValue(packet, true);
}

bool CompanionClient::requestFileList() {
    if (!hasCapability(CompanionProtocol::CAP_FILES_LIST)) {
        _lastError = "Files list capability missing";
        return false;
    }
    return sendCommand(CompanionProtocol::CMD_LIST_FILES);
}

bool CompanionClient::requestCameraCapture() {
    if (!hasCapability(CompanionProtocol::CAP_CAMERA_SHOT)) {
        _lastError = "Camera capability missing";
        return false;
    }
    return sendCommand(CompanionProtocol::CMD_CAPTURE_CAMERA);
}

bool CompanionClient::requestScreenCapture() {
    if (!hasCapability(CompanionProtocol::CAP_SCREEN_SHOT)) {
        _lastError = "Screen capability missing";
        return false;
    }
    return sendCommand(CompanionProtocol::CMD_CAPTURE_SCREEN);
}

bool CompanionClient::hasCapability(uint32_t capabilityBit) const { return (_capabilities & capabilityBit) != 0; }

bool CompanionClient::isAuthorized() const { return _authState == CompanionProtocol::AUTHORIZED; }
