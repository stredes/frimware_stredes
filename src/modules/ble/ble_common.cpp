#include "ble_common.h"
#include "companion_client.h"
#include "companion_protocol.h"
#include "companion_ui.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/utils.h"
#include "modules/badusb_ble/ducky_typer.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "esp_mac.h"
#include <map>
#define SERVICE_UUID "1bc68b2a-f3e3-11e9-81b4-2a2ae2dbcce4"
#define CHARACTERISTIC_RX_UUID "1bc68da0-f3e3-11e9-81b4-2a2ae2dbcce4"
#define CHARACTERISTIC_TX_UUID "1bc68efe-f3e3-11e9-81b4-2a2ae2dbcce4"

BLEScan *pBLEScan = nullptr;
int scanTime = SCANTIME; // In seconds

#if __has_include(<NimBLEExtAdvertising.h>)
#define NIMBLE_V2_PLUS 1
#endif

#define ENDIAN_CHANGE_U16(x) ((((x) & 0xFF00) >> 8) + (((x) & 0xFF) << 8))

BLEServer *pServer = NULL;
BLEService *pService = NULL;
BLECharacteristic *pTxCharacteristic;
BLECharacteristic *pRxCharacteristic;
bool bleDataTransferEnabled = false;

bool deviceConnected = false;
bool oldDeviceConnected = false;
static bool bleTransitioning = false;
static uint32_t bleTransitionStartedAt = 0;

struct BLEScanDeviceInfo {
    String title;
    String name;
    String address;
    int rssi;
    uint8_t addressType = BLE_ADDR_PUBLIC;
    bool isCompanion = false;
    String manufacturerData;
    String serviceUuid;
};

static std::vector<BLEScanDeviceInfo> bleScanDevices;
static NimBLEClient *activeBleClient = nullptr;
static CompanionClient activeCompanionClient;

static String bytesToHex(const std::string &data) {
    static const char hex[] = "0123456789ABCDEF";
    String out = "";
    for (uint8_t byte : data) {
        out += hex[(byte >> 4) & 0x0F];
        out += hex[byte & 0x0F];
    }
    return out;
}

static bool sameBleDevice(const BLEScanDeviceInfo &a, const BLEScanDeviceInfo &b) {
    return a.address == b.address && a.addressType == b.addressType;
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) { deviceConnected = true; };

    void onDisconnect(BLEServer *pServer) { deviceConnected = false; }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    NimBLEAttValue data;
    void onWrite(NimBLECharacteristic *pCharacteristic) { data = pCharacteristic->getValue(); }
};

uint8_t sta_mac[6];
char strID[18];
char strAddl[200];

void ble_info(String name, String address, String signal) {
    drawMainBorderWithTitle("BLE DEVICE");
    tft.setTextColor(bruceConfig.priColor);
    tft.drawString("Name:", 12, 42);
    tft.drawString(name, 12, 58);
    tft.drawString("Address:", 12, 86);
    tft.drawString(address, 12, 102);
    tft.drawString("Signal:", 12, 130);
    tft.drawString(String(signal) + " dBm", 12, 146);
    printCenterFootnote("SEL/ESC volver");

    delay(250);
    while (!check(SelPress) && !check(EscPress)) yield();
}
#ifdef NIMBLE_V2_PLUS
class AdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
#else
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
#endif
    void onResult(NimBLEAdvertisedDevice *advertisedDevice) {
        String btName = advertisedDevice->getName().c_str();
        String btTitle = btName;
        String btAddress = advertisedDevice->getAddress().toString().c_str();
        String manufacturerData = bytesToHex(advertisedDevice->getManufacturerData());
        String serviceUuid = advertisedDevice->haveServiceUUID() ? advertisedDevice->getServiceUUID().toString().c_str()
                                                                 : "";
        if (btTitle.isEmpty()) btTitle = btAddress;
        if (btName.isEmpty()) btName = "<no name>";
        bool isCompanion = CompanionProtocol::advertisesCompanionService(advertisedDevice);

        if (bleScanDevices.size() < 250)
            bleScanDevices.push_back(
                {
                    btTitle,
                    btName,
                    btAddress,
                    advertisedDevice->getRSSI(),
                    advertisedDevice->getAddressType(),
                    isCompanion,
                    manufacturerData,
                    serviceUuid,
                }
            );
        else {
            Serial.println("Memory low, stopping BLE scan...");
            pBLEScan->stop();
        }
    }
};

static AdvertisedDeviceCallbacks bleAdvertisedCallbacks;

void ble_scan_setup() {
    if (!NimBLEDevice::isInitialized()) BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
#ifdef NIMBLE_V2_PLUS
    pBLEScan->setScanCallbacks(&bleAdvertisedCallbacks, false);
#else
    pBLEScan->setAdvertisedDeviceCallbacks(&bleAdvertisedCallbacks);
#endif

    // Active scan uses more power, but get results faster
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(SCAN_INT);
    // Less or equal setInterval value
    pBLEScan->setWindow(SCAN_WINDOW);
    pBLEScan->setMaxResults(48);

    // Bluetooth MAC Address
#ifdef NIMBLE_V2_PLUS
    esp_read_mac(sta_mac, ESP_MAC_BT);
#else
    esp_read_mac(sta_mac, ESP_MAC_BT);
#endif

    sprintf(
        strID,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        sta_mac[0],
        sta_mac[1],
        sta_mac[2],
        sta_mac[3],
        sta_mac[4],
        sta_mac[5]
    );
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

static std::vector<BLEScanDeviceInfo> runBleDiscovery(int durationSeconds) {
    bleScanDevices.clear();

#ifdef NIMBLE_V2_PLUS
    BLEScanResults foundDevices = pBLEScan->getResults(durationSeconds * 1000, false);
    for (int i = 0; i < foundDevices.getCount(); i++) {
        const NimBLEAdvertisedDevice *advertisedDevice = foundDevices.getDevice(i);
        String btName = advertisedDevice->getName().c_str();
        String btTitle = btName;
        String btAddress = advertisedDevice->getAddress().toString().c_str();
        bool isCompanion = CompanionProtocol::advertisesCompanionService(advertisedDevice);
        String manufacturerData = bytesToHex(advertisedDevice->getManufacturerData());
        String serviceUuid = advertisedDevice->haveServiceUUID() ? advertisedDevice->getServiceUUID().toString().c_str()
                                                                 : "";

        if (btTitle.isEmpty()) btTitle = btAddress;
        if (btName.isEmpty()) btName = "<no name>";

        if (bleScanDevices.size() < 250) {
            bleScanDevices.push_back(
                {
                    btTitle,
                    btName,
                    btAddress,
                    advertisedDevice->getRSSI(),
                    advertisedDevice->getAddressType(),
                    isCompanion,
                    manufacturerData,
                    serviceUuid,
                }
            );
        } else {
            Serial.println("Memory low, stopping BLE scan...");
            pBLEScan->stop();
            break;
        }
    }
#else
    BLEScanResults foundDevices = pBLEScan->start(durationSeconds, false);
    (void)foundDevices;
#endif

    std::vector<BLEScanDeviceInfo> results = bleScanDevices;
    bleScanDevices.clear();
    if (pBLEScan) pBLEScan->clearResults();
    return results;
}

static bool beginBleTransition() {
    if (bleTransitioning && bleTransitionStartedAt > 0 &&
        (millis() - bleTransitionStartedAt) > 8000) {
        bleTransitioning = false;
        bleTransitionStartedAt = 0;
    }
    if (bleTransitioning) return false;
    bleTransitioning = true;
    bleTransitionStartedAt = millis();
    return true;
}

static void endBleTransition() { bleTransitioning = false; }

static void deinitBleStack() {
    if (!NimBLEDevice::isInitialized()) return;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    esp_bt_controller_deinit();
#else
    BLEDevice::deinit(true);
#endif
}

static void cleanupBleClient(bool deinitBle = false) {
    if (pBLEScan && pBLEScan->isScanning()) pBLEScan->stop();
    activeCompanionClient.disconnect(false);
    if (activeBleClient) {
        if (activeBleClient->isConnected()) activeBleClient->disconnect();
        NimBLEDevice::deleteClient(activeBleClient);
        activeBleClient = nullptr;
    }

    BLEConnected = false;

    if (deinitBle) deinitBleStack();
}

void ble_disconnect() {
    Serial.printf(
        "[BLE] Disconnect requested. initialized=%d connected=%d heap=%lu\n",
        NimBLEDevice::isInitialized(),
        BLEConnected,
        (unsigned long)ESP.getFreeHeap()
    );

    cleanupBleClient(false);

    if (hid_ble) {
        delete hid_ble;
        hid_ble = nullptr;
    }
    if (_Ask_for_restart == 1) _Ask_for_restart = 2;

    if (pBLEScan && pBLEScan->isScanning()) pBLEScan->stop();
    deviceConnected = false;
    oldDeviceConnected = false;
    bleDataTransferEnabled = false;
    pServer = nullptr;
    pService = nullptr;
    pTxCharacteristic = nullptr;
    pRxCharacteristic = nullptr;
    bleTransitioning = false;
    bleTransitionStartedAt = 0;
    BLEConnected = false;

    deinitBleStack();
}

static void showBleConnectionScreen(const BLEScanDeviceInfo &device, int serviceCount) {
    bool needsRedraw = true;

    while (true) {
        if (needsRedraw) {
            drawMainBorderWithTitle("BLE CONNECT");
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawString("Device:", 12, 42);
            tft.drawString(device.name, 12, 58);
            tft.drawString("Address:", 12, 86);
            tft.drawString(device.address, 12, 102);
            tft.drawString("RSSI:", 12, 130);
            tft.drawString(String(device.rssi) + " dBm", 12, 146);
            tft.drawString("Services:", 12, 174);
            tft.drawString(String(serviceCount), 12, 190);
            printCenterFootnote("SEL desconecta | ESC vuelve");
            needsRedraw = false;
        }

        if (check(SelPress)) {
            cleanupBleClient();
            displayInfo("BLE disconnected", true);
            return;
        }
        if (check(EscPress)) return;

        if (!activeBleClient || !activeBleClient->isConnected()) {
            cleanupBleClient(false);
            displayWarning("BLE link lost", true);
            return;
        }
        delay(50);
    }
}

static void connectToScannedBleDevice(const BLEScanDeviceInfo &device) {
    if (!beginBleTransition()) {
        displayWarning("BLE busy", true);
        return;
    }

    displayTextLine("Connecting BLE...");
    cleanupBleClient(false);

    if (!NimBLEDevice::isInitialized()) BLEDevice::init("");
    activeBleClient = NimBLEDevice::createClient();
    if (!activeBleClient) {
        endBleTransition();
        displayError("BLE client create failed", true);
        return;
    }

    activeBleClient->setConnectTimeout(8);
    activeBleClient->setConnectionParams(12, 12, 0, 400);

    NimBLEAddress target(std::string(device.address.c_str()), device.addressType);
    if (!activeBleClient->connect(target, false)) {
        cleanupBleClient();
        endBleTransition();
        displayError("BLE connection failed", true);
        return;
    }

    BLEConnected = true;
    int serviceCount = activeBleClient->getServices(true).size();
    endBleTransition();
    showBleConnectionScreen(device, serviceCount);
}

static void openBleScanDeviceMenu(const BLEScanDeviceInfo &device) {
    options.clear();
    options.push_back({"Connect", [=]() { connectToScannedBleDevice(device); }});
    options.push_back({"Info", [=]() { ble_info(device.name, device.address, String(device.rssi)); }});
    options.push_back({"Back", []() {}});
    loopOptions(options, MENU_TYPE_SUBMENU, device.title.c_str(), 0, false);
}

static void connectToCompanionDevice(const BLEScanDeviceInfo &device) {
    if (!beginBleTransition()) {
        displayWarning("BLE busy", true);
        return;
    }

    displayTextLine("Connecting companion...");
    cleanupBleClient(false);

    CompanionScanResult result = {
        device.name,
        device.title,
        device.address,
        device.rssi,
        device.addressType,
        false,
    };

    if (!activeCompanionClient.connect(result)) {
        endBleTransition();
        displayError(activeCompanionClient.getLastError(), true);
        return;
    }

    BLEConnected = true;
    endBleTransition();
    CompanionUI::showCapabilities(activeCompanionClient);
    activeCompanionClient.disconnect(false);
    BLEConnected = false;
}

static void openCompanionDeviceMenu(const BLEScanDeviceInfo &device) {
    options.clear();
    options.push_back({"Connect", [=]() { connectToCompanionDevice(device); }});
    options.push_back({"Info", [=]() { ble_info(device.name, device.address, String(device.rssi)); }});
    options.push_back({"Back", []() {}});
    loopOptions(options, MENU_TYPE_SUBMENU, device.title.c_str(), 0, false);
}

static const char *BLE_CAPTURE_DIR = "/BruceBLE";
static const char *BLE_CAPTURE_FILE = "/BruceBLE/learned_devices.txt";
static const char *BLE_RAW_CONTROL_FILE = "/BruceBLE/raw_controls.txt";
static const char *BLE_LEARNED_SD_DIR = "/BruceBLE/Learned";
static const char *BLE_PROBE_SD_DIR = "/BruceBLE/Probes";

namespace {
struct LearnedBleDevice {
    String title;
    String name;
    String address;
    uint8_t addressType = BLE_ADDR_PUBLIC;
    int rssi = 0;
    String serviceUuid;
    String manufacturerData;
};

struct BLEControlDefinition {
    const char *label;
    int8_t left;
    int8_t right;
    int8_t up;
    int8_t down;
};

struct BLEControlBox {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    bool contains(uint16_t px, uint16_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

struct BLERawControlConfig {
    String actionName;
    String deviceTitle;
    String address;
    uint8_t addressType = BLE_ADDR_PUBLIC;
    String serviceUuid;
    String characteristicUuid;
    String payloadHex;
    bool withResponse = true;
    bool configured = false;
};

constexpr uint8_t BLE_RAW_CONTROL_COUNT = 11;
BLERawControlConfig bleRawControlConfigs[BLE_RAW_CONTROL_COUNT];

const BLEControlDefinition BLE_RAW_CONTROLS[BLE_RAW_CONTROL_COUNT] = {
    {"PWR",   1,  1,  0,  3 },
    {"INPUT", 0,  0,  1,  4 },
    {"BACK",  4,  3,  0,  5 },
    {"UP",    2,  4,  0,  6 },
    {"MENU",  3,  2,  1,  7 },
    {"LEFT",  7,  6,  2,  8 },
    {"OK",    5,  7,  3,  9 },
    {"RIGHT", 6,  5,  4, 10 },
    {"VOL-", 10,  9,  5,  8 },
    {"DOWN",  8, 10,  6,  9 },
    {"VOL+",  9,  8,  7, 10 },
};

bool keyboardCancelled(const String &value) {
    return value.length() == 1 && value[0] == 27;
}

String sanitizeBleFilename(String value) {
    value.trim();
    String out = "";

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_';
        if (keep) out += c;
        else if (c == ' ' || c == '.') out += '_';
    }

    while (out.indexOf("__") != -1) out.replace("__", "_");
    if (out.isEmpty()) out = "ble_device";
    return out;
}

String normalizeUuid(String value) {
    value.trim();
    value.toLowerCase();

    String out = "";
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (hex || c == '-') out += c;
    }

    return out;
}

String normalizeHexPayload(String value) {
    value.trim();
    value.toUpperCase();

    String out = "";
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (hex) out += c;
    }

    if ((out.length() % 2) != 0) out = "0" + out;
    return out;
}

bool parseHexPayload(const String &hex, std::vector<uint8_t> &bytes) {
    bytes.clear();
    if (hex.isEmpty() || (hex.length() % 2) != 0) return false;

    for (size_t i = 0; i < hex.length(); i += 2) {
        char hi = hex[i];
        char lo = hex[i + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return -1;
        };
        int high = nibble(hi);
        int low = nibble(lo);
        if (high < 0 || low < 0) return false;
        bytes.push_back((uint8_t)((high << 4) | low));
    }

    return !bytes.empty();
}

bool loadLearnedBleDevices(std::vector<LearnedBleDevice> &devices) {
    devices.clear();
    if (!LittleFS.begin(true) || !LittleFS.exists(BLE_CAPTURE_FILE)) return false;

    File file = LittleFS.open(BLE_CAPTURE_FILE, FILE_READ);
    if (!file) return false;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        int firstSep = line.indexOf('|');
        int secondSep = (firstSep >= 0) ? line.indexOf('|', firstSep + 1) : -1;
        if (firstSep < 0 || secondSep < 0) continue;

        String title = line.substring(0, firstSep);
        String payload = line.substring(secondSep + 1);

        JsonDocument doc;
        if (deserializeJson(doc, payload) != DeserializationError::Ok) continue;

        LearnedBleDevice device;
        device.title = title;
        device.name = doc["name"] | title;
        device.address = doc["address"] | "";
        device.addressType = doc["addressType"] | BLE_ADDR_PUBLIC;
        device.rssi = doc["rssi"] | 0;
        device.serviceUuid = doc["serviceUuid"] | "";
        device.manufacturerData = doc["manufacturerData"] | "";
        if (!device.address.isEmpty()) devices.push_back(device);
    }

    file.close();
    return !devices.empty();
}

bool loadLearnedBleDeviceFromFile(FS &fs, const String &filepath, LearnedBleDevice &device) {
    File file = fs.open(filepath, FILE_READ);
    if (!file) return false;

    String payload = file.readString();
    file.close();
    payload.trim();
    if (payload.isEmpty()) return false;

    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) return false;

    device.title = doc["title"] | "";
    device.name = doc["name"] | device.title;
    device.address = doc["address"] | "";
    device.addressType = doc["addressType"] | BLE_ADDR_PUBLIC;
    device.rssi = doc["rssi"] | 0;
    device.serviceUuid = doc["serviceUuid"] | "";
    device.manufacturerData = doc["manufacturerData"] | "";
    return !device.address.isEmpty();
}

bool saveLearnedBleDeviceToSd(const BLEScanDeviceInfo &device, const String &name) {
    if (!setupSdCard()) return false;
    if (!SD.exists(BLE_CAPTURE_DIR)) SD.mkdir(BLE_CAPTURE_DIR);
    if (!SD.exists(BLE_LEARNED_SD_DIR)) SD.mkdir(BLE_LEARNED_SD_DIR);

    String filename = sanitizeBleFilename(name) + ".ble";
    String filepath = String(BLE_LEARNED_SD_DIR) + "/" + filename;

    if (SD.exists(filepath)) SD.remove(filepath);
    File file = SD.open(filepath, FILE_WRITE);
    if (!file) return false;

    JsonDocument doc;
    doc["title"] = name;
    doc["name"] = device.name;
    doc["address"] = device.address;
    doc["addressType"] = device.addressType;
    doc["rssi"] = device.rssi;
    doc["serviceUuid"] = device.serviceUuid;
    doc["manufacturerData"] = device.manufacturerData;
    serializeJson(doc, file);
    file.close();
    return true;
}

void saveBleRawControlConfigs() {
    if (!LittleFS.begin(true)) return;
    if (!LittleFS.exists(BLE_CAPTURE_DIR)) LittleFS.mkdir(BLE_CAPTURE_DIR);
    if (LittleFS.exists(BLE_RAW_CONTROL_FILE)) LittleFS.remove(BLE_RAW_CONTROL_FILE);

    File out = LittleFS.open(BLE_RAW_CONTROL_FILE, FILE_WRITE);
    if (!out) return;

    for (int i = 0; i < BLE_RAW_CONTROL_COUNT; ++i) {
        const auto &cfg = bleRawControlConfigs[i];
        if (!cfg.configured) continue;

        JsonDocument doc;
        doc["actionName"] = cfg.actionName;
        doc["deviceTitle"] = cfg.deviceTitle;
        doc["address"] = cfg.address;
        doc["addressType"] = cfg.addressType;
        doc["serviceUuid"] = cfg.serviceUuid;
        doc["characteristicUuid"] = cfg.characteristicUuid;
        doc["payloadHex"] = cfg.payloadHex;
        doc["withResponse"] = cfg.withResponse;
        String payload;
        serializeJson(doc, payload);
        out.println(String(i) + "|" + payload);
    }

    out.close();
}

void loadBleRawControlConfigs() {
    for (auto &cfg : bleRawControlConfigs) cfg = BLERawControlConfig();

    if (!LittleFS.begin(true) || !LittleFS.exists(BLE_RAW_CONTROL_FILE)) return;

    File file = LittleFS.open(BLE_RAW_CONTROL_FILE, FILE_READ);
    if (!file) return;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        int sep = line.indexOf('|');
        if (sep < 0) continue;
        int index = line.substring(0, sep).toInt();
        if (index < 0 || index >= BLE_RAW_CONTROL_COUNT) continue;

        JsonDocument doc;
        if (deserializeJson(doc, line.substring(sep + 1)) != DeserializationError::Ok) continue;

        auto &cfg = bleRawControlConfigs[index];
        cfg.actionName = doc["actionName"] | "";
        cfg.deviceTitle = doc["deviceTitle"] | "";
        cfg.address = doc["address"] | "";
        cfg.addressType = doc["addressType"] | BLE_ADDR_PUBLIC;
        cfg.serviceUuid = normalizeUuid(doc["serviceUuid"] | "");
        cfg.characteristicUuid = normalizeUuid(doc["characteristicUuid"] | "");
        cfg.payloadHex = normalizeHexPayload(doc["payloadHex"] | "");
        cfg.withResponse = doc["withResponse"] | true;
        cfg.configured = !cfg.address.isEmpty() && !cfg.serviceUuid.isEmpty() &&
                         !cfg.characteristicUuid.isEmpty() && !cfg.payloadHex.isEmpty();
    }

    file.close();
}

bool selectLearnedBleDevice(LearnedBleDevice &selectedDevice) {
    if (!setupSdCard()) {
        displayWarning("Insert SD card", true);
        return false;
    }
    if (!SD.exists(BLE_CAPTURE_DIR)) SD.mkdir(BLE_CAPTURE_DIR);
    if (!SD.exists(BLE_LEARNED_SD_DIR)) SD.mkdir(BLE_LEARNED_SD_DIR);

    String filepath = loopSD(SD, true, "BLE", BLE_LEARNED_SD_DIR);
    if (filepath == "") return false;

    if (!loadLearnedBleDeviceFromFile(SD, filepath, selectedDevice)) {
        displayError("Invalid BLE file", true);
        return false;
    }
    return true;
}

void computeBleControlLayout(BLEControlBox boxes[BLE_RAW_CONTROL_COUNT]) {
    int margin = 12;
    int gap = 8;
    int topY = 74;
    int topButtonH = max(28, (tftHeight > 210) ? 34 : 30);
    int topButtonW = (tftWidth - margin * 2 - gap) / 2;
    int padY = topY + topButtonH + 16;
    int padButtonW = (tftWidth - margin * 2 - gap * 2) / 3;
    int padButtonH = max(26, min(padButtonW, (tftHeight - padY - 34) / 3));

    boxes[0] = {(int16_t)margin, (int16_t)topY, (int16_t)topButtonW, (int16_t)topButtonH};
    boxes[1] = {(int16_t)(margin + topButtonW + gap), (int16_t)topY, (int16_t)topButtonW, (int16_t)topButtonH};

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            int idx = 2 + row * 3 + col;
            boxes[idx] = {
                (int16_t)(margin + col * (padButtonW + gap)),
                (int16_t)(padY + row * (padButtonH + gap)),
                (int16_t)padButtonW,
                (int16_t)padButtonH,
            };
        }
    }
}

void drawBleControlButton(const BLEControlBox &box, int index, bool selected, bool enabled) {
    uint16_t border = enabled ? bruceConfig.priColor : TFT_DARKGREY;
    uint16_t fill = bruceConfig.bgColor;
    uint16_t textColor = border;

    if (index == 0 && enabled) {
        border = TFT_RED;
        textColor = TFT_RED;
    }

    if (selected) {
        fill = border;
        textColor = bruceConfig.bgColor;
    }

    tft.fillRoundRect(box.x, box.y, box.w, box.h, 8, fill);
    tft.drawRoundRect(box.x, box.y, box.w, box.h, 8, border);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(textColor, fill);
    tft.drawString(BLE_RAW_CONTROLS[index].label, box.x + box.w / 2, box.y + box.h / 2, 1);
}

int moveBleControl(int currentIndex, int direction) {
    if (currentIndex < 0 || currentIndex >= BLE_RAW_CONTROL_COUNT) return 0;

    switch (direction) {
        case 0: return BLE_RAW_CONTROLS[currentIndex].left;
        case 1: return BLE_RAW_CONTROLS[currentIndex].right;
        case 2: return BLE_RAW_CONTROLS[currentIndex].up;
        case 3: return BLE_RAW_CONTROLS[currentIndex].down;
        default: return currentIndex;
    }
}

bool chooseWriteMode(bool currentMode) {
    bool selected = currentMode;
    bool changed = false;
    options.clear();
    options.emplace_back("Write Response", [&]() {
        selected = true;
        changed = true;
    });
    options.emplace_back("Write NoResp", [&]() {
        selected = false;
        changed = true;
    });
    options.emplace_back("Cancel", []() {});
    loopOptions(options, MENU_TYPE_SUBMENU, "Write Mode", currentMode ? 0 : 1, false);
    options.clear();
    return changed ? selected : currentMode;
}

bool configureBleRawControl(int index) {
    if (index < 0 || index >= BLE_RAW_CONTROL_COUNT) return false;

    LearnedBleDevice learned;
    if (!selectLearnedBleDevice(learned)) return false;

    String actionName = keyboard(
        bleRawControlConfigs[index].actionName,
        24,
        "Button name:"
    );
    if (keyboardCancelled(actionName)) return false;
    actionName.trim();
    if (actionName.isEmpty()) actionName = BLE_RAW_CONTROLS[index].label;

    String serviceUuid = keyboard(
        bleRawControlConfigs[index].serviceUuid.isEmpty() ? learned.serviceUuid : bleRawControlConfigs[index].serviceUuid,
        40,
        "Service UUID:"
    );
    if (keyboardCancelled(serviceUuid)) return false;
    serviceUuid = normalizeUuid(serviceUuid);
    if (serviceUuid.isEmpty()) {
        displayWarning("Service UUID empty", true);
        return false;
    }

    String characteristicUuid = keyboard(
        bleRawControlConfigs[index].characteristicUuid,
        40,
        "Characteristic UUID:"
    );
    if (keyboardCancelled(characteristicUuid)) return false;
    characteristicUuid = normalizeUuid(characteristicUuid);
    if (characteristicUuid.isEmpty()) {
        displayWarning("Char UUID empty", true);
        return false;
    }

    String payloadHex = hex_keyboard(
        bleRawControlConfigs[index].payloadHex,
        96,
        "Payload HEX:"
    );
    if (keyboardCancelled(payloadHex)) return false;
    payloadHex = normalizeHexPayload(payloadHex);
    if (payloadHex.isEmpty()) {
        displayWarning("Payload empty", true);
        return false;
    }

    BLERawControlConfig cfg;
    cfg.actionName = actionName;
    cfg.deviceTitle = learned.title;
    cfg.address = learned.address;
    cfg.addressType = learned.addressType;
    cfg.serviceUuid = serviceUuid;
    cfg.characteristicUuid = characteristicUuid;
    cfg.payloadHex = payloadHex;
    cfg.withResponse = chooseWriteMode(bleRawControlConfigs[index].withResponse);
    cfg.configured = true;

    bleRawControlConfigs[index] = cfg;
    saveBleRawControlConfigs();
    return true;
}

bool sendBleRawControl(const BLERawControlConfig &cfg) {
    if (!cfg.configured) {
        displayWarning("Button not configured", true);
        return false;
    }

    std::vector<uint8_t> payload;
    if (!parseHexPayload(cfg.payloadHex, payload)) {
        displayError("Invalid payload", true);
        return false;
    }

    if (!beginBleTransition()) {
        displayWarning("BLE busy", true);
        return false;
    }

    displayTextLine("Sending BLE...");
    cleanupBleClient(false);

    if (!NimBLEDevice::isInitialized()) BLEDevice::init("");
    NimBLEClient *client = NimBLEDevice::createClient();
    if (!client) {
        endBleTransition();
        displayError("BLE client fail", true);
        return false;
    }

    client->setConnectTimeout(8);
    client->setConnectionParams(12, 12, 0, 400);

    bool sent = false;
    NimBLEAddress target(std::string(cfg.address.c_str()), cfg.addressType);
    if (client->connect(target, false)) {
        NimBLERemoteService *service = client->getService(NimBLEUUID(std::string(cfg.serviceUuid.c_str())));
        if (service) {
            NimBLERemoteCharacteristic *characteristic =
                service->getCharacteristic(NimBLEUUID(std::string(cfg.characteristicUuid.c_str())));
            if (characteristic && characteristic->canWrite()) {
                sent = characteristic->writeValue(payload.data(), payload.size(), cfg.withResponse);
            } else if (characteristic && characteristic->canWriteNoResponse()) {
                sent = characteristic->writeValue(payload.data(), payload.size(), false);
            }
        }
    }

    if (client->isConnected()) client->disconnect();
    NimBLEDevice::deleteClient(client);
    endBleTransition();

    if (sent) displaySuccess("BLE sent", true);
    else displayError("BLE send failed", true);
    return sent;
}

bool replayBleAdvertisement(const BLEScanDeviceInfo &device, uint32_t durationMs = 3000) {
    if (!beginBleTransition()) {
        displayWarning("BLE busy", true);
        return false;
    }

    cleanupBleClient(false);
    if (!NimBLEDevice::isInitialized()) BLEDevice::init("");

    auto *advertising = NimBLEDevice::getAdvertising();
    if (!advertising) {
        endBleTransition();
        displayError("ADV unavailable", true);
        return false;
    }

    advertising->stop();
    advertising->clearData();

    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);

    String advName = device.name;
    if (advName == "<no name>") advName = "";
    if (!advName.isEmpty()) advData.setName(std::string(advName.c_str()), true);
    if (!device.serviceUuid.isEmpty()) advData.addServiceUUID(device.serviceUuid.c_str());

    std::vector<uint8_t> manufacturerBytes;
    if (parseHexPayload(normalizeHexPayload(device.manufacturerData), manufacturerBytes)) {
        advData.setManufacturerData(manufacturerBytes);
    }

    bool ok = advertising->setAdvertisementData(advData);
    if (ok) ok = advertising->start();
    endBleTransition();

    if (!ok) {
        advertising->stop();
        displayError("ADV replay failed", true);
        return false;
    }

    drawMainBorderWithTitle("ADV REPLAY");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString("Replaying advertisement", 12, 56, 1);
    tft.drawString("Test the target now", 12, 76, 1);
    printCenterFootnote("SEL/ESC stop");

    uint32_t startedAt = millis();
    while ((millis() - startedAt) < durationMs) {
        if (check(SelPress) || check(EscPress)) break;
        delay(25);
    }

    advertising->stop();
    displayInfo("ADV replay done", true);
    return true;
}

bool saveBleProbeToSd(
    const LearnedBleDevice &device, const std::vector<String> &serviceSummaries, const std::vector<String> &writeCandidates
) {
    if (!setupSdCard()) return false;
    if (!SD.exists(BLE_CAPTURE_DIR)) SD.mkdir(BLE_CAPTURE_DIR);
    if (!SD.exists(BLE_PROBE_SD_DIR)) SD.mkdir(BLE_PROBE_SD_DIR);

    String filepath = String(BLE_PROBE_SD_DIR) + "/" + sanitizeBleFilename(device.title) + "_probe.json";
    if (SD.exists(filepath)) SD.remove(filepath);

    File file = SD.open(filepath, FILE_WRITE);
    if (!file) return false;

    JsonDocument doc;
    doc["title"] = device.title;
    doc["name"] = device.name;
    doc["address"] = device.address;
    doc["addressType"] = device.addressType;

    JsonArray services = doc["services"].to<JsonArray>();
    for (const auto &entry : serviceSummaries) services.add(entry);

    JsonArray candidates = doc["writeCandidates"].to<JsonArray>();
    for (const auto &entry : writeCandidates) candidates.add(entry);

    serializeJson(doc, file);
    file.close();
    return true;
}

void showBleProbeCandidates(const std::vector<String> &writeCandidates) {
    options.clear();
    if (writeCandidates.empty()) {
        options.emplace_back("No writable chars", []() {});
    } else {
        for (const auto &entry : writeCandidates) options.emplace_back(entry, []() {});
    }
    options.emplace_back("Back", []() {});
    loopOptions(options, MENU_TYPE_SUBMENU, "Probe Result", 0, false);
    options.clear();
}
} // namespace

static bool saveLearnedBleDevice(const BLEScanDeviceInfo &device) {
    if (!LittleFS.begin(true)) return false;
    if (!LittleFS.exists(BLE_CAPTURE_DIR)) LittleFS.mkdir(BLE_CAPTURE_DIR);

    std::vector<String> lines;
    bool replaced = false;

    if (LittleFS.exists(BLE_CAPTURE_FILE)) {
        File existing = LittleFS.open(BLE_CAPTURE_FILE, FILE_READ);
        while (existing && existing.available()) {
            String line = existing.readStringUntil('\n');
            line.trim();
            if (line.isEmpty()) continue;

            int firstSep = line.indexOf('|');
            int secondSep = (firstSep >= 0) ? line.indexOf('|', firstSep + 1) : -1;
            String address = (secondSep >= 0) ? line.substring(firstSep + 1, secondSep) : "";

            if (address == device.address) {
                replaced = true;
                continue;
            }
            lines.push_back(line);
        }
        if (existing) existing.close();
    }

    StaticJsonDocument<256> doc;
    doc["name"] = device.name;
    doc["address"] = device.address;
    doc["addressType"] = device.addressType;
    doc["rssi"] = device.rssi;
    doc["serviceUuid"] = device.serviceUuid;
    doc["manufacturerData"] = device.manufacturerData;
    String payload;
    serializeJson(doc, payload);

    lines.push_back(device.title + "|" + device.address + "|" + payload);

    if (LittleFS.exists(BLE_CAPTURE_FILE)) LittleFS.remove(BLE_CAPTURE_FILE);
    File out = LittleFS.open(BLE_CAPTURE_FILE, FILE_WRITE);
    if (!out) return false;
    for (const auto &line : lines) out.println(line);
    out.close();
    (void)replaced;
    return true;
}

static std::vector<BLEScanDeviceInfo> compactBleDevices(
    const std::vector<BLEScanDeviceInfo> &devices, size_t maxDevices = 48
) {
    std::vector<BLEScanDeviceInfo> compacted;
    compacted.reserve(min(devices.size(), maxDevices));

    for (const auto &device : devices) {
        bool updated = false;
        for (auto &existing : compacted) {
            if (!sameBleDevice(existing, device)) continue;
            if (device.rssi > existing.rssi) existing = device;
            updated = true;
            break;
        }

        if (!updated && compacted.size() < maxDevices) compacted.push_back(device);
    }

    return compacted;
}

bool likelyPairedBleRemote(const BLEScanDeviceInfo &device) {
    String title = device.title;
    String name = device.name;
    String serviceUuid = device.serviceUuid;
    title.toLowerCase();
    name.toLowerCase();
    serviceUuid.toLowerCase();

    bool namedRemote = title.indexOf("remote") != -1 || title.indexOf("control") != -1 || title.indexOf("voice") != -1 ||
                       title.indexOf("rcu") != -1 || title.indexOf("roku") != -1 || title.indexOf("deco") != -1 ||
                       name.indexOf("remote") != -1 || name.indexOf("control") != -1 || name.indexOf("voice") != -1;
    bool hidLike = serviceUuid.indexOf("1812") != -1 || serviceUuid.indexOf("1843") != -1;
    bool strongSignal = device.rssi >= -70;

    return namedRemote || hidLike || strongSignal;
}

std::vector<BLEScanDeviceInfo> buildBleLearnCandidates(
    const std::vector<BLEScanDeviceInfo> &visibleDevices
) {
    std::vector<BLEScanDeviceInfo> filtered;
    filtered.reserve(visibleDevices.size());

    for (const auto &device : visibleDevices) {
        if (!likelyPairedBleRemote(device)) continue;
        filtered.push_back(device);
    }

    if (filtered.empty()) filtered = visibleDevices;

    std::sort(
        filtered.begin(),
        filtered.end(),
        [](const BLEScanDeviceInfo &a, const BLEScanDeviceInfo &b) {
            bool aLikely = likelyPairedBleRemote(a);
            bool bLikely = likelyPairedBleRemote(b);
            if (aLikely != bLikely) return aLikely > bLikely;
            return a.rssi > b.rssi;
        }
    );

    return filtered;
}

void ble_learn() {
    drawMainBorderWithTitle("BLE LEARN");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString("1. Keep remote near device", 12, 44, 1);
    tft.drawString("2. Press and hold button", 12, 60, 1);
    tft.drawString("3. Start short BLE scan", 12, 76, 1);
    tft.drawString("4. Save detected device", 12, 92, 1);
    printCenterFootnote("SEL start | ESC back");

    while (!check(SelPress) && !check(EscPress)) delay(20);
    if (check(EscPress)) return;

    cleanupBleClient(false);
    ble_scan_setup();
    if (pBLEScan) pBLEScan->setDuplicateFilter(true);
    drawMainBorderWithTitle("PRESS BUTTON");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString("Hold the remote button now", 12, 56, 1);
    tft.drawString("Scanning nearby BLE devices", 12, 76, 1);
    printCenterFootnote("ESC cancel");

    if (check(EscPress)) return;
    std::vector<BLEScanDeviceInfo> visibleDevices = compactBleDevices(runBleDiscovery(3), 24);
    if (pBLEScan) pBLEScan->setDuplicateFilter(false);

    if (visibleDevices.empty()) {
        displayWarning("No BLE devices found", true);
        return;
    }

    std::vector<BLEScanDeviceInfo> candidates = buildBleLearnCandidates(visibleDevices);

    options.clear();
    BLEScanDeviceInfo selectedDevice;
    bool hasSelection = false;
    bool fallbackMode = true;
    for (const auto &device : candidates) {
        String label = device.title + " [" + String(device.rssi) + "]";
        if (device.serviceUuid != "") label += " [UUID]";
        if (likelyPairedBleRemote(device)) label += " [PAIR]";
        options.emplace_back(label, [&, device]() {
            selectedDevice = device;
            hasSelection = true;
        });
    }
    options.emplace_back("Cancel", []() {});
    if (candidates.size() == visibleDevices.size()) {
        drawMainBorderWithTitle("BLE LEARN");
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.drawString("No adv button event", 12, 48, 1);
        tft.drawString("Showing visible BLE devices", 12, 64, 1);
        tft.drawString("Use Probe after saving", 12, 80, 1);
        delay(900);
    } else {
        fallbackMode = false;
    }
    loopOptions(options, MENU_TYPE_SUBMENU, fallbackMode ? "Visible BLE" : "Detected BLE", 0, false);
    options.clear();

    if (!hasSelection) return;

    bool done = false;
    while (!done) {
        bool testNow = false;
        bool saveNow = false;
        options.clear();
        options.emplace_back("Test ADV", [&]() { testNow = true; });
        options.emplace_back("Save", [&]() { saveNow = true; });
        options.emplace_back("Cancel", [&]() { done = true; });
        loopOptions(options, MENU_TYPE_SUBMENU, "BLE Learn", 0, false);
        options.clear();

        if (testNow) {
            replayBleAdvertisement(selectedDevice);
            continue;
        }

        if (saveNow) {
            String saveName = keyboard(selectedDevice.title, 24, "Save name:");
            if (keyboardCancelled(saveName)) return;
            saveName.trim();
            if (saveName.isEmpty()) saveName = selectedDevice.title;

            bool savedToSd = saveLearnedBleDeviceToSd(selectedDevice, saveName);
            bool savedLegacy = saveLearnedBleDevice(selectedDevice);

            if (savedToSd) displaySuccess("BLE saved to SD", true);
            else if (savedLegacy) displayWarning("Saved only in flash", true);
            else displayError("BLE save failed", true);
            done = true;
        } else if (!done) {
            break;
        }
    }
}

void ble_raw_control() {
    loadBleRawControlConfigs();
    returnToMenu = true;

    int selectedIndex = 0;
    BLEControlBox boxes[BLE_RAW_CONTROL_COUNT];
    computeBleControlLayout(boxes);
    bool redraw = true;

    while (true) {
        if (redraw) {
            drawMainBorderWithTitle("BLE RAW CTRL");
            tft.setTextSize(FP);
            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawString("SEL send/load  Hold SEL reassign", 12, 52, 1);
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.drawString("NEXT/PREV move  ESC back", 12, tftHeight - 14, 1);

            for (int i = 0; i < BLE_RAW_CONTROL_COUNT; ++i) {
                drawBleControlButton(boxes[i], i, i == selectedIndex, bleRawControlConfigs[i].configured);
            }

            if (bleRawControlConfigs[selectedIndex].configured) {
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                String actionLine = bleRawControlConfigs[selectedIndex].actionName;
                if (actionLine.isEmpty()) actionLine = BLE_RAW_CONTROLS[selectedIndex].label;
                if (actionLine.length() > 18) actionLine = actionLine.substring(0, 18) + "...";
                String deviceLine = bleRawControlConfigs[selectedIndex].deviceTitle;
                if (deviceLine.length() > 18) deviceLine = deviceLine.substring(0, 18) + "...";
                tft.drawString(actionLine, 12, 192, 1);
                tft.drawString(deviceLine, 12, 206, 1);
            }

            redraw = false;
        }

        if (touchPoint.pressed) {
            bool handledTouch = false;
            for (int i = 0; i < BLE_RAW_CONTROL_COUNT; ++i) {
                if (!boxes[i].contains(touchPoint.x, touchPoint.y)) continue;
                selectedIndex = i;
                touchPoint.Clear();
                redraw = true;
                handledTouch = true;
                if (bleRawControlConfigs[i].configured) sendBleRawControl(bleRawControlConfigs[i]);
                else configureBleRawControl(i);
                break;
            }
            if (!handledTouch) touchPoint.Clear();
        }

        if (check(EscPress)) break;
        if (check(UpPress)) {
            selectedIndex = moveBleControl(selectedIndex, 2);
            redraw = true;
        }
        if (check(DownPress)) {
            selectedIndex = moveBleControl(selectedIndex, 3);
            redraw = true;
        }
        if (check(NextPress)) {
            selectedIndex = moveBleControl(selectedIndex, 1);
            redraw = true;
        }
        if (check(PrevPress)) {
            selectedIndex = moveBleControl(selectedIndex, 0);
            redraw = true;
        }

        if (check(SelPress, false)) {
            unsigned long pressStart = millis();
            bool reassign = false;
            while (check(SelPress, false)) {
                if (millis() - pressStart >= 500) {
                    reassign = true;
                    break;
                }
                delay(10);
            }
            check(SelPress);

            if (reassign || !bleRawControlConfigs[selectedIndex].configured) {
                configureBleRawControl(selectedIndex);
            } else {
                sendBleRawControl(bleRawControlConfigs[selectedIndex]);
            }
            redraw = true;
        }

        delay(30);
    }

    while (check(EscPress)) delay(10);
}

void ble_probe() {
    LearnedBleDevice learned;
    if (!selectLearnedBleDevice(learned)) return;

    if (!beginBleTransition()) {
        displayWarning("BLE busy", true);
        return;
    }

    displayTextLine("Probing BLE...");
    cleanupBleClient(false);

    if (!NimBLEDevice::isInitialized()) BLEDevice::init("");
    NimBLEClient *client = NimBLEDevice::createClient();
    if (!client) {
        endBleTransition();
        displayError("BLE client fail", true);
        return;
    }

    client->setConnectTimeout(8);
    client->setConnectionParams(12, 12, 0, 400);

    std::vector<String> serviceSummaries;
    std::vector<String> writeCandidates;

    NimBLEAddress target(std::string(learned.address.c_str()), learned.addressType);
    if (!client->connect(target, false)) {
        NimBLEDevice::deleteClient(client);
        endBleTransition();
        displayError("BLE connect failed", true);
        return;
    }

    const std::vector<NimBLERemoteService *> &services = client->getServices(true);
    for (const auto *service : services) {
        String serviceUuid = String(service->getUUID().toString().c_str());
        serviceSummaries.push_back("S:" + serviceUuid);

        const std::vector<NimBLERemoteCharacteristic *> &chars = service->getCharacteristics(true);
        for (const auto *ch : chars) {
            String charUuid = String(ch->getUUID().toString().c_str());
            String props = "";
            if (ch->canRead()) props += "R";
            if (ch->canWrite()) props += "W";
            if (ch->canWriteNoResponse()) props += "N";
            if (ch->canNotify()) props += "T";
            if (ch->canIndicate()) props += "I";

            String entry = serviceUuid + " > " + charUuid + " [" + props + "]";
            serviceSummaries.push_back(entry);
            if (ch->canWrite() || ch->canWriteNoResponse()) writeCandidates.push_back(entry);
        }
    }

    if (client->isConnected()) client->disconnect();
    NimBLEDevice::deleteClient(client);
    endBleTransition();

    bool saved = saveBleProbeToSd(learned, serviceSummaries, writeCandidates);
    if (!saved) displayWarning("Probe not saved", true);
    showBleProbeCandidates(writeCandidates);
}

void ble_scan() {
    displayTextLine("Scanning..");

    options = {};
    ble_scan_setup();
    bleScanDevices = runBleDiscovery(scanTime);

    for (const auto &device : bleScanDevices) {
        String label = device.title;
        label += " [" + String(device.rssi) + "]";
        if (device.isCompanion) label += " [CMP]";
        options.emplace_back(label, [=]() { openBleScanDeviceMenu(device); });
    }

    addOptionToMainMenu();

    loopOptions(options);
    options.clear();
    bleScanDevices.clear();

}

void ble_connect() {
    displayTextLine("Scanning companions..");

    options = {};
    ble_scan_setup();
    bleScanDevices = runBleDiscovery(scanTime);

    bool hasCompanions = false;
    for (const auto &device : bleScanDevices) {
        if (device.isCompanion) {
            hasCompanions = true;
            break;
        }
    }

    if (!hasCompanions) {
        pBLEScan->clearResults();
        displayWarning("No companion devices found", true);
        return;
    }

    for (const auto &device : bleScanDevices) {
        if (!device.isCompanion) continue;
        String label = device.title + " [" + String(device.rssi) + "]";
        options.emplace_back(label, [=]() { openCompanionDeviceMenu(device); });
    }

    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "BLE Connect", 0, false);
    options.clear();
    bleScanDevices.clear();
    pBLEScan->clearResults();
}

bool initBLEServer() {
    uint64_t chipid = ESP.getEfuseMac();
    String blename = "Bruce-" + String((uint8_t)(chipid >> 32), HEX);

    BLEDevice::init(blename.c_str());
    // BLEDevice::setPower(ESP_PWR_LVL_N12);
    pServer = BLEDevice::createServer();

    pServer->setCallbacks(new MyServerCallbacks());
    pService = pServer->createService(SERVICE_UUID);
    pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_RX_UUID, NIMBLE_PROPERTY::NOTIFY);

    pTxCharacteristic->addDescriptor(new NimBLE2904());
    pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_TX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    return true;
}

void disPlayBLESend() {
    uint8_t senddata[2] = {0};
    tft.fillScreen(bruceConfig.bgColor);
    drawMainBorder(); // Moved up to avoid drawing screen issues
    tft.setTextSize(1);

    pService->start();
    pServer->getAdvertising()->start();

    uint64_t chipid = ESP.getEfuseMac();
    String blename = "Bruce-" + String((uint8_t)(chipid >> 32), HEX);

    BLEConnected = true;

    bool wasConnected = false;
    bool first_run = true;
    while (!check(EscPress)) {
        if (deviceConnected) {
            if (!wasConnected) {
                tft.fillRect(10, 26, tftWidth - 20, tftHeight - 36, TFT_BLACK);
                drawBLE_beacon(180, 28, TFT_BLUE);
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                tft.setTextSize(FM);
                tft.setCursor(12, 50);
                // tft.printf("BLE connect!\n");
                tft.printf("BLE Send\n");
                tft.setTextSize(FM);
            }
            tft.fillRect(10, 100, tftWidth - 20, 28, TFT_BLACK);
            tft.setCursor(12, 100);
            if (senddata[0] % 4 == 0) {
                tft.printf("0x%02X>    ", senddata[0]);
            } else if (senddata[0] % 4 == 1) {
                tft.printf("0x%02X>>   ", senddata[0]);
            } else if (senddata[0] % 4 == 2) {
                tft.printf("0x%02X >>  ", senddata[0]);
            } else if (senddata[0] % 4 == 3) {
                tft.printf("0x%02X  >  ", senddata[0]);
            }

            senddata[1]++;
            if (senddata[1] > 3) {
                senddata[1] = 0;
                senddata[0]++;
                pTxCharacteristic->setValue(senddata, 1);
                pTxCharacteristic->notify();
            }
            wasConnected = true;
        } else {
            if (wasConnected or first_run) {
                first_run = false;
                tft.fillRect(10, 26, tftWidth - 20, tftHeight - 36, TFT_BLACK);
                tft.setTextSize(FM);
                tft.setCursor(12, 50);
                tft.setTextColor(TFT_RED);
                tft.printf("BLE disconnect\n");
                tft.setCursor(12, 75);
                tft.setTextColor(tft.color565(18, 150, 219));

                tft.printf(String("Name:" + blename + "\n").c_str());
                tft.setCursor(12, 100);
                tft.printf("UUID:1bc68b2a\n");
                drawBLE_beacon(180, 40, TFT_DARKGREY);
            }
            wasConnected = false;
        }
    }

    tft.setTextColor(TFT_WHITE);
    if (pServer) pServer->getAdvertising()->stop();
    pServer = nullptr;
    pService = nullptr;
    pTxCharacteristic = nullptr;
    pRxCharacteristic = nullptr;
    deinitBleStack();
    BLEConnected = false;
}

static bool is_ble_inited = false;

void ble_test() {
    printf("ble test\n");

    // if (!is_ble_inited)
    // {
    printf("Init ble server\n");
    initBLEServer();
    delay(100);
    is_ble_inited = true;
    // }

    disPlayBLESend();

    printf("Quit ble test\n");
}
