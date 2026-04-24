#include "ble_common.h"
#include "companion_client.h"
#include "companion_protocol.h"
#include "companion_ui.h"
#include "core/mykeyboard.h"
#include "core/utils.h"
#include "modules/badusb_ble/ducky_typer.h"
#include "esp_mac.h"
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
};

static std::vector<BLEScanDeviceInfo> bleScanDevices;
static NimBLEClient *activeBleClient = nullptr;
static CompanionClient activeCompanionClient;

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
        if (btTitle.isEmpty()) btTitle = btAddress;
        if (btName.isEmpty()) btName = "<no name>";
        bool isCompanion = CompanionProtocol::advertisesCompanionService(advertisedDevice);

        if (bleScanDevices.size() < 250)
            bleScanDevices.push_back(
                {btTitle, btName, btAddress, advertisedDevice->getRSSI(), advertisedDevice->getAddressType(), isCompanion}
            );
        else {
            Serial.println("Memory low, stopping BLE scan...");
            pBLEScan->stop();
        }
    }
};

void ble_scan_setup() {
    if (!NimBLEDevice::isInitialized()) BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
#ifdef NIMBLE_V2_PLUS
    pBLEScan->setScanCallbacks(new AdvertisedDeviceCallbacks(), false);
#else
    pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
#endif

    // Active scan uses more power, but get results faster
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(SCAN_INT);
    // Less or equal setInterval value
    pBLEScan->setWindow(SCAN_WINDOW);

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

void ble_scan() {
    displayTextLine("Scanning..");

    options = {};
    bleScanDevices.clear();
    ble_scan_setup();
#ifdef NIMBLE_V2_PLUS
    BLEScanResults foundDevices = pBLEScan->getResults(scanTime * 1000, false);
    for (int i = 0; i < foundDevices.getCount(); i++) {
        const NimBLEAdvertisedDevice *advertisedDevice = foundDevices.getDevice(i);
        String btName = advertisedDevice->getName().c_str();
        String btTitle = btName;
        String btAddress = advertisedDevice->getAddress().toString().c_str();
        bool isCompanion = CompanionProtocol::advertisesCompanionService(advertisedDevice);

        if (btTitle.isEmpty()) btTitle = btAddress;
        if (btName.isEmpty()) btName = "<no name>";

        if (bleScanDevices.size() < 250)
            bleScanDevices.push_back(
                {btTitle, btName, btAddress, advertisedDevice->getRSSI(), advertisedDevice->getAddressType(), isCompanion}
            );
        else {
            Serial.println("Memory low, stopping BLE scan...");
            pBLEScan->stop();
        }
    }
#else
    BLEScanResults foundDevices = pBLEScan->start(scanTime, false);
#endif

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

    // Delete results fromBLEScan buffer to release memory
    pBLEScan->clearResults();
}

void ble_connect() {
    displayTextLine("Scanning companions..");

    options = {};
    bleScanDevices.clear();
    ble_scan_setup();
#ifdef NIMBLE_V2_PLUS
    BLEScanResults foundDevices = pBLEScan->getResults(scanTime * 1000, false);
    for (int i = 0; i < foundDevices.getCount(); i++) {
        const NimBLEAdvertisedDevice *advertisedDevice = foundDevices.getDevice(i);
        if (!CompanionProtocol::advertisesCompanionService(advertisedDevice)) continue;

        String btName = advertisedDevice->getName().c_str();
        String btTitle = btName;
        String btAddress = advertisedDevice->getAddress().toString().c_str();
        if (btTitle.isEmpty()) btTitle = btAddress;
        if (btName.isEmpty()) btName = "Bruce Companion";

        bleScanDevices.push_back(
            {btTitle, btName, btAddress, advertisedDevice->getRSSI(), advertisedDevice->getAddressType(), true}
        );
    }
#else
    BLEScanResults foundDevices = pBLEScan->start(scanTime, false);
    (void)foundDevices;
#endif

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
