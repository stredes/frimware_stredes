#include "core/main_menu.h"
#include <globals.h>

#include "core/powerSave.h"
#include "core/serial_commands/cli.h"
#include "core/utils.h"
#include "current_year.h"
#include "esp32-hal-psram.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "modules/NRF24/nrf_common.h"
#include <functional>
#include <string>
#include <vector>
io_expander ioExpander;
BruceConfig bruceConfig;
BruceConfigPins bruceConfigPins;

SerialCli serialCli;
USBSerial USBserial;
SerialDevice *serialDevice = &USBserial;

StartupApp startupApp;
String startupAppJSInterpreterFile = "";

MainMenu mainMenu;
SPIClass sdcardSPI;
#ifdef USE_HSPI_PORT
#ifndef VSPI
#define VSPI FSPI
#endif
SPIClass CC_NRF_SPI(VSPI);
#else
SPIClass CC_NRF_SPI(HSPI);
#endif

// Navigation Variables
volatile bool NextPress = false;
volatile bool PrevPress = false;
volatile bool UpPress = false;
volatile bool DownPress = false;
volatile bool SelPress = false;
volatile bool EscPress = false;
volatile bool AnyKeyPress = false;
volatile bool NextPagePress = false;
volatile bool PrevPagePress = false;
volatile bool LongPress = false;
volatile bool SerialCmdPress = false;
volatile int forceMenuOption = -1;
volatile uint8_t menuOptionType = 0;
String menuOptionLabel = "";
#ifdef HAS_ENCODER_LED
volatile int EncoderLedChange = 0;
#endif

TouchPoint touchPoint;

keyStroke KeyStroke;

TaskHandle_t xHandle;
void __attribute__((weak)) taskInputHandler(void *parameter) {
    auto timer = millis();
    while (true) {
        checkPowerSaveTime();
        // Sometimes this task run 2 or more times before looptask,
        // and navigation gets stuck, the idea here is run the input detection
        // if AnyKeyPress is false, or rerun if it was not renewed within 75ms (arbitrary)
        // because AnyKeyPress will be true if didn´t passed through a check(bool var)
        if (!AnyKeyPress || millis() - timer > 75) {
            NextPress = false;
            PrevPress = false;
            UpPress = false;
            DownPress = false;
            SelPress = false;
            EscPress = false;
            AnyKeyPress = false;
            SerialCmdPress = false;
            NextPagePress = false;
            PrevPagePress = false;
            touchPoint.pressed = false;
            touchPoint.Clear();
#ifndef USE_TFT_eSPI_TOUCH
            InputHandler();
#endif
            timer = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
// Public Globals Variables
unsigned long previousMillis = millis();
int prog_handler; // 0 - Flash, 1 - LittleFS, 3 - Download
String cachedPassword = "";
int8_t interpreter_state = -1;
bool sdcardMounted = false;
bool gpsConnected = false;

// wifi globals
// TODO put in a namespace
bool wifiConnected = false;
bool isWebUIActive = false;
String wifiIP;

bool BLEConnected = false;
bool returnToMenu;
bool isSleeping = false;
bool isScreenOff = false;
bool dimmer = false;
char timeStr[16];
time_t localTime;
struct tm *timeInfo;
#if defined(HAS_RTC)
#if defined(HAS_RTC_PCF85063A)
pcf85063_RTC _rtc;
#else
cplus_RTC _rtc;
#endif
RTC_TimeTypeDef _time;
RTC_DateTypeDef _date;
bool clock_set = true;
#else
ESP32Time rtc;
bool clock_set = false;
#endif

std::vector<Option> options;
// Protected global variables
#if defined(HAS_SCREEN)
tft_logger tft = tft_logger(); // Invoke custom library
tft_sprite sprite = tft_sprite(&tft);
tft_sprite draw = tft_sprite(&tft);
volatile int tftWidth = TFT_HEIGHT;
#ifdef HAS_TOUCH
volatile int tftHeight =
    TFT_WIDTH - 20; // 20px to draw the TouchFooter(), were the btns are being read in touch devices.
#else
volatile int tftHeight = TFT_WIDTH;
#endif
#else
tft_logger tft;
SerialDisplayClass &sprite = tft;
SerialDisplayClass &draw = tft;
volatile int tftWidth = VECTOR_DISPLAY_DEFAULT_HEIGHT;
volatile int tftHeight = VECTOR_DISPLAY_DEFAULT_WIDTH;
#endif

#include "core/display.h"
#include "core/led_control.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/serialcmds.h"
#include "core/settings.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_common.h"
#include "modules/bjs_interpreter/interpreter.h" // for JavaScript interpreter
#include "modules/others/audio.h"                // for playAudioFile
#include "modules/rf/rf_utils.h"                 // for initCC1101once
#include <Wire.h>

/*********************************************************************
 **  Function: begin_storage
 **  Config LittleFS and SD storage
 *********************************************************************/
void begin_storage() {
    if (!LittleFS.begin(true)) { LittleFS.format(), LittleFS.begin(); }
    bool checkFS = setupSdCard();
    bruceConfig.fromFile(checkFS);
    bruceConfigPins.fromFile(checkFS);
}

/*********************************************************************
 **  Function: _setup_gpio()
 **  Sets up a weak (empty) function to be replaced by /ports/* /interface.h
 *********************************************************************/
void _setup_gpio() __attribute__((weak));
void _setup_gpio() {}

/*********************************************************************
 **  Function: _post_setup_gpio()
 **  Sets up a weak (empty) function to be replaced by /ports/* /interface.h
 *********************************************************************/
void _post_setup_gpio() __attribute__((weak));
void _post_setup_gpio() {}

/*********************************************************************
 **  Function: setup_gpio
 **  Setup GPIO pins
 *********************************************************************/
void setup_gpio() {

    // init setup from /ports/*/interface.h
    _setup_gpio();

    // Smoochiee v2 uses a AW9325 tro control GPS, MIC, Vibro and CC1101 RX/TX powerlines
    ioExpander.init(IO_EXPANDER_ADDRESS, &Wire);

#if TFT_MOSI > 0
    if (bruceConfigPins.CC1101_bus.mosi == (gpio_num_t)TFT_MOSI)
        initCC1101once(&tft.getSPIinstance()); // (T_EMBED), CORE2 and others
    else
#endif
        if (bruceConfigPins.CC1101_bus.mosi == bruceConfigPins.SDCARD_bus.mosi)
        initCC1101once(&sdcardSPI); // (ARDUINO_M5STACK_CARDPUTER) and (ESP32S3DEVKITC1) and devices that
                                    // share CC1101 pin with only SDCard
    else initCC1101once(NULL);
    // (ARDUINO_M5STICK_C_PLUS) || (ARDUINO_M5STICK_C_PLUS2) and others that doesn´t share SPI with
    // other devices (need to change it when Bruce board comes to shore)
}

/*********************************************************************
 **  Function: begin_tft
 **  Config tft
 *********************************************************************/
void begin_tft() {
    tft.setRotation(bruceConfigPins.rotation); // sometimes it misses the first command
    tft.invertDisplay(bruceConfig.colorInverted);
    tft.setRotation(bruceConfigPins.rotation);
    tftWidth = tft.width();
#ifdef HAS_TOUCH
    tftHeight = tft.height() - 20;
#else
    tftHeight = tft.height();
#endif
    resetTftDisplay();
    setBrightness(bruceConfig.bright, false);
}

/*********************************************************************
 **  Function: boot_screen
 **  Draw boot screen
 *********************************************************************/
void boot_screen() {
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FM);
    tft.drawPixel(0, 0, bruceConfig.bgColor);
    tft.drawCentreString("Bruce", tftWidth / 2, 10, 1);
    tft.setTextSize(FP);
    tft.drawCentreString(BRUCE_VERSION, tftWidth / 2, 25, 1);
    tft.setTextSize(FM);
    tft.drawCentreString(
        "PREDATORY FIRMWARE", tftWidth / 2, tftHeight + 2, 1
    ); // will draw outside the screen on non touch devices
}

/*********************************************************************
 **  Function: boot_screen_anim
 **  Draw boot screen
 *********************************************************************/
void boot_screen_anim() {
    boot_screen();
    int i = millis();
    // checks for boot.jpg in SD and LittleFS for customization
    int boot_img = 0;
    bool drawn = false;
    if (sdcardMounted) {
        if (SD.exists("/boot.jpg")) boot_img = 1;
        else if (SD.exists("/boot.gif")) boot_img = 3;
    }
    if (boot_img == 0 && LittleFS.exists("/boot.jpg")) boot_img = 2;
    else if (boot_img == 0 && LittleFS.exists("/boot.gif")) boot_img = 4;
    if (bruceConfig.theme.boot_img) boot_img = 5; // override others

    tft.drawPixel(0, 0, 0);       // Forces back communication with TFT, to avoid ghosting
                                  // Start image loop
    while (millis() < i + 7000) { // boot image lasts for 5 secs
        if ((millis() - i > 2000) && !drawn) {
            tft.fillRect(0, 45, tftWidth, tftHeight - 45, bruceConfig.bgColor);
            if (boot_img > 0 && !drawn) {
                tft.fillScreen(bruceConfig.bgColor);
                if (boot_img == 5) {
                    drawImg(
                        *bruceConfig.themeFS(),
                        bruceConfig.getThemeItemImg(bruceConfig.theme.paths.boot_img),
                        0,
                        0,
                        true,
                        3600
                    );
                    Serial.println("Image from SD theme");
                } else if (boot_img == 1) {
                    drawImg(SD, "/boot.jpg", 0, 0, true);
                    Serial.println("Image from SD");
                } else if (boot_img == 2) {
                    drawImg(LittleFS, "/boot.jpg", 0, 0, true);
                    Serial.println("Image from LittleFS");
                } else if (boot_img == 3) {
                    drawImg(SD, "/boot.gif", 0, 0, true, 3600);
                    Serial.println("Image from SD");
                } else if (boot_img == 4) {
                    drawImg(LittleFS, "/boot.gif", 0, 0, true, 3600);
                    Serial.println("Image from LittleFS");
                }
                tft.drawPixel(0, 0, 0); // Forces back communication with TFT, to avoid ghosting
            }
            drawn = true;
        }
#if !defined(LITE_VERSION)
        if (!boot_img && (millis() - i > 2200) && (millis() - i) < 2700)
            tft.drawRect(2 * tftWidth / 3, tftHeight / 2, 2, 2, bruceConfig.priColor);
        if (!boot_img && (millis() - i > 2700) && (millis() - i) < 2900)
            tft.fillRect(0, 45, tftWidth, tftHeight - 45, bruceConfig.bgColor);
        if (!boot_img && (millis() - i > 2900) && (millis() - i) < 3400)
            tft.drawXBitmap(
                2 * tftWidth / 3 - 30,
                5 + tftHeight / 2,
                bruce_small_bits,
                bruce_small_width,
                bruce_small_height,
                bruceConfig.bgColor,
                bruceConfig.priColor
            );
        if (!boot_img && (millis() - i > 3400) && (millis() - i) < 3600) tft.fillScreen(bruceConfig.bgColor);
        if (!boot_img && (millis() - i > 3600))
            tft.drawXBitmap(
                (tftWidth - 238) / 2,
                (tftHeight - 133) / 2,
                bits,
                bits_width,
                bits_height,
                bruceConfig.bgColor,
                bruceConfig.priColor
            );
#endif
        if (check(AnyKeyPress)) // If any key or M5 key is pressed, it'll jump the boot screen
        {
            tft.fillScreen(bruceConfig.bgColor);
            delay(10);
            return;
        }
    }

    // Clear splashscreen
    tft.fillScreen(bruceConfig.bgColor);
}

struct BootCheckItem {
    String label;
    String status;
    String detail;
    uint16_t color;
};

static void drawBootProgressBar(int current, int total) {
    int barX = 16;
    int barY = tftHeight - 34;
    int barW = tftWidth - 32;
    int barH = 12;
    int fillW = map(current, 0, total, 0, barW - 4);

    tft.drawRoundRect(barX, barY, barW, barH, 4, bruceConfig.priColor);
    tft.fillRect(barX + 2, barY + 2, fillW, barH - 4, bruceConfig.priColor);
}

static void drawBootCheckWindow(const std::vector<BootCheckItem> &checks, size_t activeIndex) {
    int listTop = 62;
    int rowH = 20;
    int barTop = tftHeight - 40;
    int visibleRows = max(3, (barTop - listTop - 18) / rowH);
    int first = 0;

    if (checks.size() > static_cast<size_t>(visibleRows) && activeIndex >= static_cast<size_t>(visibleRows)) {
        first = activeIndex - visibleRows + 1;
    }

    tft.fillRect(0, listTop - 2, tftWidth, barTop - listTop, bruceConfig.bgColor);
    tft.setTextSize(1);

    for (int row = 0; row < visibleRows; row++) {
        int idx = first + row;
        if (idx >= static_cast<int>(checks.size())) break;

        int y = listTop + row * rowH;
        bool active = idx == static_cast<int>(activeIndex);
        uint16_t rowColor = active ? TFT_YELLOW : bruceConfig.priColor;
        tft.setTextColor(rowColor, bruceConfig.bgColor);
        tft.drawString(active ? ">" : " ", 8, y);
        tft.drawString(checks[idx].label, 20, y);
        tft.setTextColor(checks[idx].color, bruceConfig.bgColor);
        tft.drawRightString(checks[idx].status, tftWidth - 14, y);
    }

    tft.setTextColor(TFT_DARKGREY, bruceConfig.bgColor);
    tft.drawCentreString(
        String(activeIndex + 1) + "/" + String(checks.size()) + "  " + checks[activeIndex].detail,
        tftWidth / 2,
        barTop - 15,
        1
    );
}

static void logBootCheck(const BootCheckItem &item) {
    Serial.printf(
        "[BOOT][CHECK] %-10s | %-8s | %s\n", item.label.c_str(), item.status.c_str(), item.detail.c_str()
    );
}

static String gpioLabel(gpio_num_t pin) { return pin == GPIO_NUM_NC ? String("NC") : String((int)pin); }

static bool isNmRfHatSharedSpiPinout(const BruceConfigPins::SPIPins &bus) {
    return bus.sck == (gpio_num_t)14 && bus.miso == (gpio_num_t)12 && bus.mosi == (gpio_num_t)13 &&
           bus.cs == (gpio_num_t)27 && bus.io0 == (gpio_num_t)22;
}

static String describeSpiPinout(const BruceConfigPins::SPIPins &bus, const char *io0Label) {
    String profile = isNmRfHatSharedSpiPinout(bus) ? "NM-RF-HAT/CYD " : "";
    return profile + "SCK:" + gpioLabel(bus.sck) + " MISO:" + gpioLabel(bus.miso) +
           " MOSI:" + gpioLabel(bus.mosi) + " CS:" + gpioLabel(bus.cs) + " " + io0Label + ":" +
           gpioLabel(bus.io0);
}

static BootCheckItem buildNrf24BootCheck() {
#if defined(USE_NRF24_VIA_SPI) && defined(NRF24_CE_PIN) && defined(NRF24_SS_PIN)
    if (NRF24_CE_PIN < 0 || NRF24_SS_PIN < 0) {
        return BootCheckItem{"NRF24", "N/A", "nRF24 pins unavailable", TFT_DARKGREY};
    }

    bool radioReady = nrf_start(NRF_MODE_SPI);
    bool chipConnected = radioReady && NRFradio.isChipConnected();

    if (radioReady) {
        NRFradio.stopListening();
        NRFradio.powerDown();
        pinMode(bruceConfigPins.NRF24_bus.cs, OUTPUT);
        digitalWrite(bruceConfigPins.NRF24_bus.cs, HIGH);
        pinMode(bruceConfigPins.NRF24_bus.io0, OUTPUT);
        digitalWrite(bruceConfigPins.NRF24_bus.io0, LOW);
    }

    return BootCheckItem{
        "NRF24",
        chipConnected ? "OK" : "MISS",
        (chipConnected ? String("nRF Hat detected | ") : String("nRF Hat not responding | ")) +
            describeSpiPinout(bruceConfigPins.NRF24_bus, "CE"),
        chipConnected ? static_cast<uint16_t>(TFT_GREEN) : static_cast<uint16_t>(TFT_YELLOW)
    };
#else
    return BootCheckItem{"NRF24", "N/A", "nRF24 not enabled in build", TFT_DARKGREY};
#endif
}

static std::vector<BootCheckItem> buildBootChecks() {
    std::vector<BootCheckItem> checks = {
        BootCheckItem{"DISPLAY",  "OK",                                      "TFT initialized",         TFT_GREEN},
#if defined(HAS_TOUCH)
        BootCheckItem{"TOUCH",    "READY",                                   "Touch interface enabled", TFT_GREEN},
#else
        BootCheckItem{"TOUCH", "N/A", "No touch hardware in build", TFT_DARKGREY},
#endif
        BootCheckItem{
                      "LITTLEFS", LittleFS.totalBytes() > 0 ? "OK" : "FAIL",
                      LittleFS.totalBytes() > 0 ? "Filesystem mounted" : "Filesystem unavailable",
                      LittleFS.totalBytes() > 0 ? TFT_GREEN : TFT_RED
        },
        BootCheckItem{
                      "SD",       sdcardMounted ? "READY" : "MISS",
                      sdcardMounted ? "SD card mounted" : "No SD card detected",
                      sdcardMounted ? TFT_GREEN : TFT_YELLOW
        },
        BootCheckItem{"WIFI",     "READY",                                   "WiFi stack available",    TFT_GREEN},
        BootCheckItem{"BLE",      "READY",                                   "BLE stack available",     TFT_GREEN},
    };

#if defined(HAS_RTC)
    checks.push_back(BootCheckItem{"RTC", "READY", "RTC hardware enabled", TFT_GREEN});
#else
    checks.push_back(
        BootCheckItem{
            "RTC",
            bruceConfig.lastKnownEpoch > 0 ? "CACHE" : "N/A",
            bruceConfig.lastKnownEpoch > 0 ? "Clock snapshot fallback enabled" : "No RTC hardware",
            TFT_YELLOW
        }
    );
#endif

    checks.push_back(buildNrf24BootCheck());

#if defined(USE_CC1101_VIA_SPI) && defined(CC1101_SS_PIN) && defined(CC1101_GDO0_PIN)
    checks.push_back(
        BootCheckItem{
            "CC1101",
            (CC1101_SS_PIN >= 0 && CC1101_GDO0_PIN >= 0) ? "READY" : "N/A",
            (CC1101_SS_PIN >= 0 && CC1101_GDO0_PIN >= 0)
                ? "CC1101 SPI configured | " + describeSpiPinout(bruceConfigPins.CC1101_bus, "GDO0")
                : "CC1101 pins unavailable",
            (CC1101_SS_PIN >= 0 && CC1101_GDO0_PIN >= 0) ? TFT_GREEN : TFT_DARKGREY
        }
    );
#else
    checks.push_back(BootCheckItem{"CC1101", "N/A", "CC1101 not enabled in build", TFT_DARKGREY});
#endif

#if defined(USE_W5500_VIA_SPI) && defined(W5500_SS_PIN) && defined(W5500_INT_PIN)
    checks.push_back(
        BootCheckItem{
            "W5500",
            (W5500_SS_PIN >= 0 && W5500_INT_PIN >= 0) ? "READY" : "N/A",
            (W5500_SS_PIN >= 0 && W5500_INT_PIN >= 0)
                ? "Ethernet controller configured | " + describeSpiPinout(bruceConfigPins.W5500_bus, "INT")
                : "W5500 pins unavailable",
            (W5500_SS_PIN >= 0 && W5500_INT_PIN >= 0) ? TFT_GREEN : TFT_DARKGREY
        }
    );
#else
    checks.push_back(BootCheckItem{"W5500", "N/A", "Ethernet controller not enabled", TFT_DARKGREY});
#endif

#if defined(LORA_CS) && defined(LORA_RST) && defined(LORA_DIO0)
    checks.push_back(
        BootCheckItem{
            "LORA",
            (LORA_CS >= 0 && LORA_RST >= 0 && LORA_DIO0 >= 0) ? "READY" : "N/A",
            (LORA_CS >= 0 && LORA_RST >= 0 && LORA_DIO0 >= 0)
                ? "LoRa pins configured | " + describeSpiPinout(bruceConfigPins.LoRa_bus, "RST")
                : "LoRa pins unavailable",
            (LORA_CS >= 0 && LORA_RST >= 0 && LORA_DIO0 >= 0) ? TFT_GREEN : TFT_DARKGREY
        }
    );
#else
    checks.push_back(BootCheckItem{"LORA", "N/A", "LoRa not enabled in build", TFT_DARKGREY});
#endif

#if defined(GPS_SERIAL_TX) && defined(GPS_SERIAL_RX)
    checks.push_back(
        BootCheckItem{
            "GPS",
            (GPS_SERIAL_TX >= 0 && GPS_SERIAL_RX >= 0) ? "READY" : "N/A",
            gpsConnected ? "GPS runtime link active | RX:" + gpioLabel(bruceConfigPins.gps_bus.rx) +
                               " TX:" + gpioLabel(bruceConfigPins.gps_bus.tx)
                         : ((GPS_SERIAL_TX >= 0 && GPS_SERIAL_RX >= 0)
                                ? "GPS UART configured | RX:" + gpioLabel(bruceConfigPins.gps_bus.rx) +
                                      " TX:" + gpioLabel(bruceConfigPins.gps_bus.tx)
                                : "GPS pins unavailable"),
            gpsConnected ? static_cast<uint16_t>(TFT_GREEN)
                         : ((GPS_SERIAL_TX >= 0 && GPS_SERIAL_RX >= 0) ? static_cast<uint16_t>(TFT_YELLOW)
                                                                       : static_cast<uint16_t>(TFT_DARKGREY))
        }
    );
#else
    checks.push_back(BootCheckItem{"GPS", "N/A", "GPS not enabled in build", TFT_DARKGREY});
#endif

#if defined(MIC_SPM1423) || defined(MIC_INMP441)
    checks.push_back(BootCheckItem{"MIC", "READY", "Microphone support enabled", TFT_GREEN});
#else
    checks.push_back(BootCheckItem{"MIC", "N/A", "Microphone not enabled in build", TFT_DARKGREY});
#endif

    return checks;
}

static void showSystemLoader() {
    std::vector<BootCheckItem> checks = buildBootChecks();

    Serial.println("[BOOT] Starting subsystem verification...");
    Serial.printf("[BOOT] Bruce version: %s\n", BRUCE_VERSION);
    Serial.printf("[BOOT] Free heap: %lu\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("[BOOT] Free PSRAM: %lu\n", (unsigned long)ESP.getFreePsram());

    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(2);
    tft.drawCentreString("SYSTEM CHECK", tftWidth / 2, 18, 1);
    tft.setTextSize(1);
    tft.drawCentreString("Validating Bruce subsystems", tftWidth / 2, 42, 1);

    for (size_t i = 0; i < checks.size(); i++) {
        logBootCheck(checks[i]);
        drawBootCheckWindow(checks, i);
        drawBootProgressBar(i + 1, checks.size());
        delay(220);
        if (check(AnyKeyPress)) break;
    }

    Serial.println("[BOOT] Subsystem verification completed.");

    delay(250);
}

static bool startupStatusCheckRan = false;

void runStartupStatusCheck() {
    startupStatusCheckRan = true;
    showSystemLoader();
    tft.fillScreen(bruceConfig.bgColor);
}

bool hasStartupStatusCheckRun() { return startupStatusCheckRan; }

static void showBootAsciiSplash() {
    static const char *art[] = {
        "          _,.-----.,_",
        "       ,-~           ~-.",
        "     ,^___           ___^.",
        "    /~\"   ~\"   .   \"~   \"~\\",
        "    Y  ,--._    I    _.--.  Y",
        "    | Y     ~-. | ,-~     Y |",
        "    | |        }:{        | |",
        "    j l       / | \\       ! l",
        " .-~  (__,.--\" .^. \"--.,__) ~-.",
        "(           / / | \\ \\          )",
        " \\.____,   ~  \\/\"\\/  ~.____,/",
        "  ^.____                 ____.^",
        "     | |T ~\\  !   !  /~ T| |",
        "     | |l   _ _ _ _ _   !| |",
        "     | l \\/V V V V V V\\/ j |",
        "     l  \\ \\|_|_|_|_|_|/ /  !",
        "      \\  \\[T T T T T T]/  /",
        "       \\  `^-^-^-^-^-^'  /",
        "        \\               /",
        "         \\.           ,/",
        "           \"^-.___,-^\""
    };

    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(1);

    int lineHeight = 9;
    int artLines = sizeof(art) / sizeof(art[0]);
    int maxChars = 0;
    for (int i = 0; i < artLines; i++) { maxChars = max(maxChars, static_cast<int>(String(art[i]).length())); }
    int artHeight = artLines * lineHeight;
    int artWidth = maxChars * 6;
    int startY = 8;
    int startX = max(0, (tftWidth - artWidth) / 2);
    if (artHeight < tftHeight - 34) startY = max(6, (tftHeight - 26 - artHeight) / 2);

    for (int i = 0; i < artLines; i++) { tft.drawString(art[i], startX, startY + i * lineHeight, 1); }

    tft.drawCentreString("Iniciando Bruce...", tftWidth / 2, tftHeight - 18, 1);
    delay(1800);
}

static void boot_loader_sequence() {
    runStartupStatusCheck();
    showBootAsciiSplash();
    tft.fillScreen(bruceConfig.bgColor);
}

/*********************************************************************
 **  Function: init_clock
 **  Clock initialisation for propper display in menu
 *********************************************************************/
void init_clock() {
#if defined(HAS_RTC)
    _rtc.begin();
#if defined(HAS_RTC_BM8563)
    _rtc.GetBm8563Time();
#endif
#if defined(HAS_RTC_PCF85063A)
    _rtc.GetPcf85063Time();
#endif
    _rtc.GetTime(&_time);
    _rtc.GetDate(&_date);

    struct tm timeinfo = {};
    timeinfo.tm_sec = _time.Seconds;
    timeinfo.tm_min = _time.Minutes;
    timeinfo.tm_hour = _time.Hours;
    timeinfo.tm_mday = _date.Date;
    timeinfo.tm_mon = _date.Month > 0 ? _date.Month - 1 : 0;
    timeinfo.tm_year = _date.Year >= 1900 ? _date.Year - 1900 : 0;
    time_t epoch = mktime(&timeinfo);
    struct timeval tv = {.tv_sec = epoch};
    settimeofday(&tv, nullptr);
#else
    if (!restoreClockFromSnapshot()) {
        struct tm timeinfo = {};
        timeinfo.tm_year = CURRENT_YEAR - 1900;
        timeinfo.tm_mon = 0;
        timeinfo.tm_mday = 1;
        time_t epoch = mktime(&timeinfo);
        rtc.setTime(epoch);
        clock_set = true;
        struct timeval tv = {.tv_sec = epoch};
        settimeofday(&tv, nullptr);
    }
#endif
}

/*********************************************************************
 **  Function: init_led
 **  Led initialisation
 *********************************************************************/
void init_led() {
#ifdef HAS_RGB_LED
    beginLed();
#endif
}

/*********************************************************************
 **  Function: startup_sound
 **  Play sound or tone depending on device hardware
 *********************************************************************/
void startup_sound() {
    if (bruceConfig.soundEnabled == 0) return; // if sound is disabled, do not play sound
#if !defined(LITE_VERSION)
#if defined(BUZZ_PIN)
    // Bip M5 just because it can. Does not bip if splashscreen is bypassed
    _tone(5000, 50);
    delay(200);
    _tone(5000, 50);
    /*  2fix: menu infinite loop */
#elif defined(HAS_NS4168_SPKR)
    // play a boot sound
    if (bruceConfig.theme.boot_sound) {
        playAudioFile(bruceConfig.themeFS(), bruceConfig.getThemeItemImg(bruceConfig.theme.paths.boot_sound));
    } else if (SD.exists("/boot.wav")) {
        playAudioFile(&SD, "/boot.wav");
    } else if (LittleFS.exists("/boot.wav")) {
        playAudioFile(&LittleFS, "/boot.wav");
    }
#endif
#endif
}

/*********************************************************************
 **  Function: setup
 **  Where the devices are started and variables set
 *********************************************************************/
void setup() {
    Serial.setRxBufferSize(
        SAFE_STACK_BUFFER_SIZE / 4
    ); // Must be invoked before Serial.begin(). Default is 256 chars
    Serial.begin(115200);

    Serial.printf("[BOOT] Reset reason: %d\n", static_cast<int>(esp_reset_reason()));
    log_d("Total heap: %d", ESP.getHeapSize());
    log_d("Free heap: %d", ESP.getFreeHeap());
    if (psramInit()) log_d("PSRAM Started");
    if (psramFound()) log_d("PSRAM Found");
    else log_d("PSRAM Not Found");
    log_d("Total PSRAM: %d", ESP.getPsramSize());
    log_d("Free PSRAM: %d", ESP.getFreePsram());

    // declare variables
    prog_handler = 0;
    sdcardMounted = false;
    wifiConnected = false;
    BLEConnected = false;
    bruceConfig.bright = 100; // theres is no value yet
    bruceConfigPins.rotation = ROTATION;
    setup_gpio();
#if defined(HAS_SCREEN)
    tft.init();
    tft.setRotation(bruceConfigPins.rotation);
    tft.fillScreen(TFT_BLACK);
    // bruceConfig is not read yet.. just to show something on screen due to long boot time
    tft.setTextColor(TFT_PURPLE, TFT_BLACK);
    tft.drawCentreString("Booting", tft.width() / 2, tft.height() / 2, 1);
#else
    tft.begin();
#endif
    begin_storage();
    begin_tft();
    init_clock();
    init_led();

    options.reserve(20); // preallocate some options space to avoid fragmentation

    // Set WiFi country to avoid warnings and ensure max power
    const wifi_country_t country = {
        .cc = "US",
        .schan = 1,
        .nchan = 14,
#ifdef CONFIG_ESP_PHY_MAX_TX_POWER
        .max_tx_power = CONFIG_ESP_PHY_MAX_TX_POWER, // 20
#endif
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };

    esp_wifi_set_max_tx_power(80); // 80 translates to 20dBm
    esp_wifi_set_country(&country);

    // Some GPIO Settings (such as CYD's brightness control must be set after tft and sdcard)
    _post_setup_gpio();
    // end of post gpio begin

    // #ifndef USE_TFT_eSPI_TOUCH
    // This task keeps running all the time, will never stop
    xTaskCreate(
        taskInputHandler,              // Task function
        "InputHandler",                // Task Name
        INPUT_HANDLER_TASK_STACK_SIZE, // Stack size
        NULL,                          // Task parameters
        2,                             // Task priority (0 to 3), loopTask has priority 2.
        &xHandle                       // Task handle (not used)
    );
    // #endif
#if defined(HAS_SCREEN)
    bruceConfig.openThemeFile(bruceConfig.themeFS(), bruceConfig.themePath, false);
    if (!bruceConfig.instantBoot) {
        boot_loader_sequence();
        startup_sound();
    } else {
        showBootAsciiSplash();
        tft.fillScreen(bruceConfig.bgColor);
    }
    if (bruceConfig.wifiAtStartup) {
        log_i("Loading Wifi at Startup");
        xTaskCreate(
            wifiConnectTask,   // Task function
            "wifiConnectTask", // Task Name
            4096,              // Stack size
            NULL,              // Task parameters
            2,                 // Task priority (0 to 3), loopTask has priority 2.
            NULL               // Task handle (not used)
        );
    }
#endif
    //  start a task to handle serial commands while the webui is running
    startSerialCommandsHandlerTask(true);

    wakeUpScreen();
    if (bruceConfig.startupApp != "" && !startupApp.startApp(bruceConfig.startupApp)) {
        bruceConfig.setStartupApp("");
    }
}

/**********************************************************************
 **  Function: loop
 **  Main loop
 **********************************************************************/
#if defined(HAS_SCREEN)
void loop() {
#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
    if (interpreter_state > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        interpreter_state = 2;
        Serial.println("Entering interpreter...");
        while (interpreter_state > 0) { vTaskDelay(pdMS_TO_TICKS(500)); }
        if (interpreter_state == 0) {
            Serial.println("Interpreter put to background.");
        } else {
            Serial.println("Exiting interpreter...");
        }
        if (interpreter_state == -1) { interpreterTaskHandler = NULL; }
        previousMillis = millis(); // ensure that will not dim screen when get back to menu
    }
#endif
    tft.fillScreen(bruceConfig.bgColor);

    mainMenu.begin();
    delay(1);
}
#else

void loop() {
    tft.setLogging();
    Serial.println(
        "\n"
        "██████  ██████  ██    ██  ██████ ███████ \n"
        "██   ██ ██   ██ ██    ██ ██      ██      \n"
        "██████  ██████  ██    ██ ██      █████   \n"
        "██   ██ ██   ██ ██    ██ ██      ██      \n"
        "██████  ██   ██  ██████   ██████ ███████ \n"
        "                                         \n"
        "         PREDATORY FIRMWARE\n\n"
        "Tips: Connect to the WebUI for better experience\n"
        "      Add your network by sending: wifi add ssid password\n\n"
        "At your command:"
    );

    // Enable navigation through webUI
    tft.fillScreen(bruceConfig.bgColor);
    mainMenu.begin();
    vTaskDelay(10 / portTICK_PERIOD_MS);
}
#endif
