#include "custom_ir.h"
#include "TV-B-Gone.h" // for checkIrTxPin()
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/settings.h"
#include "core/type_convertion.h"
#include "ir_utils.h"
#include <IRutils.h>

uint32_t swap32(uint32_t value) {
    return ((value & 0x000000FF) << 24) | ((value & 0x0000FF00) << 8) | ((value & 0x00FF0000) >> 8) |
           ((value & 0xFF000000) >> 24);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Custom IR

static std::vector<IRCode *> codes;

void resetCodesArray() {
    for (auto code : codes) { delete code; }
    codes.clear();
}

static std::vector<IRCode *> recent_ircodes;

void addToRecentCodes(IRCode *ircode);
void selectRecentIrMenu();

namespace {
bool selectIRControlFile(FS **selectedFs, String &filepath);

struct IRControlDefinition {
    const char *label;
    const char *aliases[5];
    uint8_t aliasCount;
    int8_t left;
    int8_t right;
    int8_t up;
    int8_t down;
};

struct IRControlBox {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    bool contains(uint16_t px, uint16_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr uint8_t IR_CUSTOM_CONTROL_COUNT = 11;
IRCode *customControlAssignments[IR_CUSTOM_CONTROL_COUNT] = {nullptr};

const IRControlDefinition IR_CUSTOM_CONTROLS[IR_CUSTOM_CONTROL_COUNT] = {
    {"PWR",   {"power", "pwr", "onoff", "standby", ""},          4,  1,  1,  0,  3 },
    {"INPUT", {"input", "source", "src", "hdmi", "av"},          5,  0,  0,  1,  4 },
    {"BACK",  {"back", "return", "exit", "esc", ""},             4,  4,  3,  0,  5 },
    {"UP",    {"up", "arrowup", "cursorup", "", ""},             3,  2,  4,  0,  6 },
    {"MENU",  {"menu", "home", "smart", "tools", ""},            4,  3,  2,  1,  7 },
    {"LEFT",  {"left", "arrowleft", "cursorleft", "", ""},       3,  7,  6,  2,  8 },
    {"OK",    {"ok", "select", "sel", "enter", "confirm"},       5,  5,  7,  3,  9 },
    {"RIGHT", {"right", "arrowright", "cursorright", "", ""},    3,  6,  5,  4, 10 },
    {"VOL-",  {"volumedown", "voldown", "volminus", "vol-", "volume-"}, 5, 10, 9, 5, 8},
    {"DOWN",  {"down", "arrowdown", "cursordown", "", ""},       3,  8, 10,  6,  9 },
    {"VOL+",  {"volumeup", "volup", "volplus", "vol+", "volume+"}, 5, 9, 8, 7, 10},
};

String normalizeIRName(const String &name) {
    String normalized = "";
    for (size_t i = 0; i < name.length(); ++i) {
        char c = tolower(name[i]);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-') {
            normalized += c;
        }
    }
    return normalized;
}

bool irNameHasAliasToken(const String &rawName, const char *alias) {
    String token = "";

    for (size_t i = 0; i <= rawName.length(); ++i) {
        char c = (i < rawName.length()) ? tolower(rawName[i]) : '\0';
        bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-';

        if (keep) token += c;
        else if (token.length() > 0) {
            if (token.equals(alias)) return true;
            token = "";
        }
    }

    return false;
}

bool irCodeMatchesAlias(IRCode *code, const char *alias) {
    if (code == nullptr || alias == nullptr || alias[0] == '\0') return false;

    String rawName = code->name;
    rawName.toLowerCase();
    if (irNameHasAliasToken(rawName, alias)) return true;

    if (strlen(alias) > 3) {
        String normalized = normalizeIRName(code->name);
        if (normalized.indexOf(alias) != -1) return true;
    }

    return false;
}

bool loadIRCodesFromFile(FS *fs, const String &filepath, int maxCodes = 100) {
    if (fs == nullptr) return false;

    File databaseFile = fs->open(filepath, FILE_READ);
    if (!databaseFile) {
        Serial.println("Failed to open IR file.");
        return false;
    }

    Serial.println("Opened IR file.");
    resetCodesArray();

    int total_codes = 0;
    String line;
    String txt;
    codes.push_back(new IRCode());

    while (databaseFile.available() && total_codes < maxCodes) {
        line = databaseFile.readStringUntil('\n');
        txt = line.substring(line.indexOf(":") + 1);
        txt.trim();

        if (line.startsWith("name:")) {
            if (codes[total_codes]->name != "") {
                total_codes++;
                codes.push_back(new IRCode());
            }
            codes[total_codes]->name = txt;
            codes[total_codes]->filepath = txt + " " + filepath.substring(1 + filepath.lastIndexOf("/"));
        }

        if (line.startsWith("type:")) codes[total_codes]->type = txt;
        if (line.startsWith("protocol:")) codes[total_codes]->protocol = txt;
        if (line.startsWith("address:")) codes[total_codes]->address = txt;
        if (line.startsWith("frequency:")) codes[total_codes]->frequency = txt.toInt();
        if (line.startsWith("bits:")) codes[total_codes]->bits = txt.toInt();
        if (line.startsWith("command:")) codes[total_codes]->command = txt;
        if (line.startsWith("data:") || line.startsWith("value:") || line.startsWith("state:")) {
            codes[total_codes]->data = txt;
        }

        if (line.startsWith("#") && total_codes < (int)codes.size() && codes[total_codes]->name != "") {
            total_codes++;
            codes.push_back(new IRCode());
        }
    }

    databaseFile.close();
    setup_ir_pin(bruceConfigPins.irTx, OUTPUT);
    return true;
}

void mapIRControlCodes(IRCode *mappedControls[]) {
    for (int i = 0; i < IR_CUSTOM_CONTROL_COUNT; ++i) {
        mappedControls[i] = nullptr;
        for (auto code : codes) {
            if (code == nullptr || code->name == "") continue;
            for (uint8_t j = 0; j < IR_CUSTOM_CONTROLS[i].aliasCount; ++j) {
                if (irCodeMatchesAlias(code, IR_CUSTOM_CONTROLS[i].aliases[j])) {
                    mappedControls[i] = code;
                    break;
                }
            }
            if (mappedControls[i] != nullptr) break;
        }
    }
}

int firstMappedControl(IRCode *mappedControls[]) {
    for (int i = 0; i < IR_CUSTOM_CONTROL_COUNT; ++i) {
        if (mappedControls[i] != nullptr) return i;
    }
    return -1;
}

int moveMappedControl(int currentIndex, int direction, IRCode *mappedControls[]) {
    if (currentIndex < 0 || currentIndex >= IR_CUSTOM_CONTROL_COUNT) return firstMappedControl(mappedControls);

    int nextIndex = currentIndex;
    for (int i = 0; i < IR_CUSTOM_CONTROL_COUNT; ++i) {
        switch (direction) {
            case 0: nextIndex = IR_CUSTOM_CONTROLS[nextIndex].left; break;
            case 1: nextIndex = IR_CUSTOM_CONTROLS[nextIndex].right; break;
            case 2: nextIndex = IR_CUSTOM_CONTROLS[nextIndex].up; break;
            case 3: nextIndex = IR_CUSTOM_CONTROLS[nextIndex].down; break;
            default: return currentIndex;
        }

        if (nextIndex < 0 || nextIndex >= IR_CUSTOM_CONTROL_COUNT) break;
        if (mappedControls[nextIndex] != nullptr) return nextIndex;
    }

    return currentIndex;
}

void computeIRControlLayout(IRControlBox boxes[IR_CUSTOM_CONTROL_COUNT]) {
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

void drawIRControlButton(const IRControlBox &box, int index, bool selected, bool enabled) {
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
    tft.drawString(IR_CUSTOM_CONTROLS[index].label, box.x + box.w / 2, box.y + box.h / 2, 1);
}

void assignIRControlCode(int index, IRCode *code) {
    if (index < 0 || index >= IR_CUSTOM_CONTROL_COUNT || code == nullptr) return;
    if (customControlAssignments[index] != nullptr) delete customControlAssignments[index];
    customControlAssignments[index] = new IRCode(code);
}

int importIRControlMappingsFromFile(FS *fs, const String &filepath) {
    if (!loadIRCodesFromFile(fs, filepath)) {
        displayError("Fail to open file");
        delay(1500);
        return 0;
    }

    IRCode *mappedControls[IR_CUSTOM_CONTROL_COUNT];
    mapIRControlCodes(mappedControls);
    int imported = 0;
    for (int i = 0; i < IR_CUSTOM_CONTROL_COUNT; ++i) {
        if (mappedControls[i] == nullptr) continue;
        assignIRControlCode(i, mappedControls[i]);
        imported++;
    }
    resetCodesArray();
    return imported;
}

bool pickIRCodeForControl(int controlIndex) {
    FS *fs = nullptr;
    String filepath;
    if (!selectIRControlFile(&fs, filepath)) return false;
    if (!loadIRCodesFromFile(fs, filepath)) {
        displayError("Fail to open file");
        delay(1500);
        return false;
    }

    IRCode *selectedCode = nullptr;
    bool cancelled = false;
    int defaultIndex = 0;

    for (int i = 0; i < (int)codes.size(); ++i) {
        if (codes[i] == nullptr || codes[i]->name == "") continue;
        for (uint8_t j = 0; j < IR_CUSTOM_CONTROLS[controlIndex].aliasCount; ++j) {
            if (irCodeMatchesAlias(codes[i], IR_CUSTOM_CONTROLS[controlIndex].aliases[j])) {
                defaultIndex = i;
                break;
            }
        }
        if (defaultIndex == i) break;
    }

    options = {};
    int visualIndex = 0;
    int menuIndex = 0;
    for (auto code : codes) {
        if (code == nullptr || code->name == "") continue;
        if (visualIndex == defaultIndex) menuIndex = visualIndex;
        options.push_back({code->name.c_str(), [code, &selectedCode]() { selectedCode = code; }});
        visualIndex++;
    }
    options.push_back({"Cancel", [&]() { cancelled = true; }});

    while (true) {
        loopOptions(options, menuIndex);
        if (selectedCode != nullptr || cancelled || check(EscPress)) break;
    }

    if (selectedCode != nullptr) assignIRControlCode(controlIndex, selectedCode);
    options.clear();
    resetCodesArray();
    while (check(EscPress)) delay(10);
    return selectedCode != nullptr;
}

bool customIRControl() {
    checkIrTxPin();
    returnToMenu = true;

    int selectedIndex = firstMappedControl(customControlAssignments);
    if (selectedIndex < 0) selectedIndex = 0;

    IRControlBox boxes[IR_CUSTOM_CONTROL_COUNT];
    computeIRControlLayout(boxes);
    bool redraw = true;

    while (true) {
        if (redraw) {
            drawMainBorderWithTitle("IR CUSTOM CTRL");
            tft.setTextSize(FP);
            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawString("SEL send/load  Hold SEL reassign", 12, 52, 1);
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.drawString("NEXT/PREV move  ESC back", 12, tftHeight - 14, 1);

            for (int i = 0; i < IR_CUSTOM_CONTROL_COUNT; ++i) {
                drawIRControlButton(boxes[i], i, i == selectedIndex, customControlAssignments[i] != nullptr);
            }

            redraw = false;
        }

        if (touchPoint.pressed) {
            bool handledTouch = false;
            for (int i = 0; i < IR_CUSTOM_CONTROL_COUNT; ++i) {
                if (!boxes[i].contains(touchPoint.x, touchPoint.y)) continue;
                selectedIndex = i;
                touchPoint.Clear();
                redraw = true;
                handledTouch = true;
                if (customControlAssignments[i] != nullptr) {
                    sendIRCommand(customControlAssignments[i], true);
                    addToRecentCodes(customControlAssignments[i]);
                } else {
                    pickIRCodeForControl(i);
                }
                break;
            }
            if (!handledTouch) touchPoint.Clear();
        }

        if (check(EscPress)) break;
        if (check(UpPress)) {
            selectedIndex = moveMappedControl(selectedIndex, 2, customControlAssignments);
            redraw = true;
        }
        if (check(DownPress)) {
            selectedIndex = moveMappedControl(selectedIndex, 3, customControlAssignments);
            redraw = true;
        }
        if (check(NextPress)) {
            selectedIndex = moveMappedControl(selectedIndex, 1, customControlAssignments);
            redraw = true;
        }
        if (check(PrevPress)) {
            selectedIndex = moveMappedControl(selectedIndex, 0, customControlAssignments);
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

            if (reassign || customControlAssignments[selectedIndex] == nullptr) {
                pickIRCodeForControl(selectedIndex);
            } else {
                sendIRCommand(customControlAssignments[selectedIndex], true);
                addToRecentCodes(customControlAssignments[selectedIndex]);
            }
            redraw = true;
        }

        delay(30);
    }

    digitalWrite(bruceConfigPins.irTx, LED_OFF);
    while (check(EscPress)) delay(10);
    return false;
}

bool customIRControl(FS *fs, const String &filepath) {
    int imported = importIRControlMappingsFromFile(fs, filepath);
    if (imported <= 0) {
        displayError("No control aliases", true);
        return false;
    }

    return customIRControl();
}

bool selectIRControlFile(FS **selectedFs, String &filepath) {
    if (selectedFs == nullptr) return false;

    checkIrTxPin();
    resetCodesArray();
    filepath = "";
    *selectedFs = nullptr;
    returnToMenu = true;

    options = {
        {"Recent",   selectRecentIrMenu       },
        {"LittleFS", [&]() { *selectedFs = &LittleFS; }},
        {"Menu",     yield                    },
    };
    if (setupSdCard()) options.insert(options.begin(), {"SD Card", [&]() { *selectedFs = &SD; }});

    loopOptions(options);

    if (*selectedFs == nullptr) return false;

    if (!(**selectedFs).exists("/BruceIR")) (**selectedFs).mkdir("/BruceIR");
    filepath = loopSD(**selectedFs, true, "IR", "/BruceIR");
    return filepath != "";
}
} // namespace

void addToRecentCodes(IRCode *ircode) {
    // copy ircode -> recent_ircodes
    // if code exist in recent codes do not save it
    for (auto recent_ircode : recent_ircodes) {
        if (recent_ircode->filepath == ircode->filepath) { return; }
    }

    IRCode *ircode_copy = new IRCode(ircode);
    recent_ircodes.insert(recent_ircodes.begin(), ircode_copy);

    if (recent_ircodes.size() > 16) { // cycle
        delete recent_ircodes.back();
        recent_ircodes.pop_back();
    }
}

void selectRecentIrMenu() {
    // show menu with filenames
    checkIrTxPin();
    options = {};
    bool exit = false;
    IRCode *selected_code = NULL;
    for (auto recent_ircode : recent_ircodes) {
        if (recent_ircode->filepath == "") continue; // not inited
        // else
        options.push_back({recent_ircode->filepath.c_str(), [recent_ircode, &selected_code]() {
                               selected_code = recent_ircode;
                           }});
    }
    options.push_back({"Main Menu", [&]() { exit = true; }});

    int idx = 0;
    while (1) {
        idx = loopOptions(options, idx);
        if (selected_code != NULL) {
            sendIRCommand(selected_code);
            selected_code = NULL;
        }
        if (check(EscPress) || exit) break;
    }
    options.clear();

    return;
}

bool txIrFile(FS *fs, String filepath, bool hideDefaultUI) {
    // SPAM all codes of the file

    int total_codes = 0;
    String line;

    File databaseFile = fs->open(filepath, FILE_READ);

    setup_ir_pin(bruceConfigPins.irTx, OUTPUT);
    // digitalWrite(bruceConfigPins.irTx, LED_ON);

    if (!databaseFile) {
        Serial.println("Failed to open database file.");
        displayError("Fail to open file");
        delay(2000);
        return false;
    }
    Serial.println("Opened database file.");

    bool endingEarly = false;
    int codes_sent = 0;
    uint16_t frequency = 0;
    String rawData = "";
    String protocol = "";
    String address = "";
    String command = "";
    String value = "";
    uint8_t bits = 32;

    databaseFile.seek(0); // comes back to first position

    // count the number of codes to replay
    while (databaseFile.available()) {
        line = databaseFile.readStringUntil('\n');
        if (line.startsWith("type:")) total_codes++;
    }

    Serial.printf("\nStarted SPAM all codes with: %d codes", total_codes);
    // comes back to first position, beggining of the file
    databaseFile.seek(0);
    while (databaseFile.available()) {
        if (!hideDefaultUI) { progressHandler(codes_sent, total_codes); }
        line = databaseFile.readStringUntil('\n');
        if (line.endsWith("\r")) line.remove(line.length() - 1);

        if (line.startsWith("type:")) {
            codes_sent++;
            String type = line.substring(5);
            type.trim();
            Serial.println("Type: " + type);
            if (type == "raw") {
                Serial.println("RAW code");
                while (databaseFile.available()) {
                    line = databaseFile.readStringUntil('\n');
                    if (line.endsWith("\r")) line.remove(line.length() - 1);

                    if (line.startsWith("frequency:")) {
                        line = line.substring(10);
                        line.trim();
                        frequency = line.toInt();
                        Serial.printf("Frequency: %d\n", frequency);
                    } else if (line.startsWith("data:")) {
                        rawData = line.substring(5);
                        rawData.trim();
                        Serial.println("RawData: " + rawData);
                    } else if ((frequency != 0 && rawData != "") || line.startsWith("#")) {
                        IRCode code;
                        code.type = "raw";
                        code.data = rawData;
                        code.frequency = frequency;
                        sendIRCommand(&code, hideDefaultUI);

                        rawData = "";
                        frequency = 0;
                        type = "";
                        line = "";
                        break;
                    }
                }
            } else if (type == "parsed") {
                Serial.println("PARSED");
                while (databaseFile.available()) {
                    line = databaseFile.readStringUntil('\n');
                    if (line.endsWith("\r")) line.remove(line.length() - 1);

                    if (line.startsWith("protocol:")) {
                        protocol = line.substring(9);
                        protocol.trim();
                        Serial.println("Protocol: " + protocol);
                    } else if (line.startsWith("address:")) {
                        address = line.substring(8);
                        address.trim();
                        Serial.println("Address: " + address);
                    } else if (line.startsWith("command:")) {
                        command = line.substring(8);
                        command.trim();
                        Serial.println("Command: " + command);
                    } else if (line.startsWith("value:") || line.startsWith("state:")) {
                        value = line.substring(6);
                        value.trim();
                        Serial.println("Value: " + value);
                    } else if (line.startsWith("bits:")) {
                        bits = line.substring(strlen("bits:")).toInt();
                        Serial.println("bits: " + bits);
                    } else if (line.indexOf("#") != -1) { // TODO: also detect EOF
                        IRCode code(protocol, address, command, value, bits);
                        sendIRCommand(&code, hideDefaultUI);

                        protocol = "";
                        address = "";
                        command = "";
                        value = "";
                        bits = 32;
                        type = "";
                        line = "";
                        break;
                    }
                }
            }
        }
        // if user is pushing (holding down) TRIGGER button, stop transmission early
        if (check(SelPress)) // Pause TV-B-Gone
        {
            while (check(SelPress)) yield();
            if (!hideDefaultUI) { displayTextLine("Paused"); }

            while (!check(SelPress)) { // If Presses Select again, continues
                if (check(EscPress)) {
                    endingEarly = true;
                    break;
                }
            }
            while (check(SelPress)) { yield(); }
            if (endingEarly) break; // Cancels  custom IR Spam
            if (!hideDefaultUI) { displayTextLine("Running, Wait"); }
        }
    } // end while file has lines to process
    databaseFile.close();
    Serial.println("closed");
    Serial.println("EXTRA finished");

    resetCodesArray();
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
    return true;
}

void otherIRcodes() {
    checkIrTxPin();
    resetCodesArray();
    String filepath;
    FS *fs = NULL;

    returnToMenu = true; // make sure menu is redrawn when quitting in any point

    options = {
        {"Recent",   selectRecentIrMenu       },
        {"LittleFS", [&]() { fs = &LittleFS; }},
        {"Menu",     yield                    },
    };
    if (setupSdCard()) options.insert(options.begin(), {"SD Card", [&]() { fs = &SD; }});

    loopOptions(options);

    if (fs == NULL) { // recent or menu was selected
        return;
    }

    // select a file to tx
    if (!(*fs).exists("/BruceIR")) (*fs).mkdir("/BruceIR");

    // startPath: remember the last visited folder so the user lands back there
    // after pressing back in the command list
    String startPath = "/BruceIR";

    while (true) {
        filepath = loopSD(*fs, true, "IR", startPath);
        if (filepath == "") return; // user cancelled / pressed back at root

        // Remember the folder of the selected file for next loop iteration
        startPath = filepath.substring(0, filepath.lastIndexOf('/'));
        if (startPath == "") startPath = "/";

        // select mode
        bool exit = false;
        bool mode_cmd = true;
        bool mode_custom = false;
        options = {
            {"Choose cmd",  [&]() { mode_cmd = true; mode_custom = false; } },
            {"Custom Ctrl", [&]() { mode_cmd = false; mode_custom = true; } },
            {"Spam all",    [&]() { mode_cmd = false; mode_custom = false; }},
            {"Menu",        [&]() { exit = true; }                            },
        };

        loopOptions(options);

        if (exit) return;

        if (mode_custom) {
            customIRControl(fs, filepath);
            continue;
        }

        if (!mode_cmd) {
            // Spam all selected
            txIrFile(fs, filepath);
            // After spam, loop back to file picker in the same folder
            continue;
        }

        // Choose cmd:
        // chooseCmdIrFile returns false = short back → loop back to file browser
        //                          true  = long press / Main Menu → exit
        bool goToMain = chooseCmdIrFile(fs, filepath);
        if (goToMain) return;
        // else: loop back to loopSD, starting in the same folder (startPath)
    }
} // end of otherIRcodes

void otherIRCustomControl() {
    customIRControl();
}

// IR commands

void sendIRCommand(IRCode *code, bool hideDefaultUI) {
    setup_ir_pin(bruceConfigPins.irTx, OUTPUT);
    // https://developer.flipper.net/flipperzero/doxygen/infrared_file_format.html
    if (code->type.equalsIgnoreCase("raw")) sendRawCommand(code->frequency, code->data, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("NEC"))
        sendNECCommand(code->address, code->command, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("NECext"))
        sendNECextCommand(code->address, code->command, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("RC5") || code->protocol.equalsIgnoreCase("RC5X"))
        sendRC5Command(code->address, code->command, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("RC6"))
        sendRC6Command(code->address, code->command, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("Samsung32"))
        sendSamsungCommand(code->address, code->command, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("SIRC"))
        sendSonyCommand(code->address, code->command, 12, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("SIRC15"))
        sendSonyCommand(code->address, code->command, 15, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("SIRC20"))
        sendSonyCommand(code->address, code->command, 20, hideDefaultUI);
    else if (code->protocol.equalsIgnoreCase("Kaseikyo"))
        sendKaseikyoCommand(code->address, code->command, hideDefaultUI);
    // Others protocols of IRRemoteESP8266, not related to Flipper Zero IR File Format
    else if (code->protocol != "" && code->data != "" &&
             strToDecodeType(code->protocol.c_str()) != decode_type_t::UNKNOWN)
        sendDecodedCommand(code->protocol, code->data, code->bits, hideDefaultUI);
}

void sendNECCommand(String address, String command, bool hideDefaultUI) {
    IRsend irsend(bruceConfigPins.irTx); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }
    uint16_t addressValue = strtoul(address.substring(0, 2).c_str(), nullptr, 16);
    uint16_t commandValue = strtoul(command.substring(0, 2).c_str(), nullptr, 16);
    uint64_t data = irsend.encodeNEC(addressValue, commandValue);
    irsend.sendNEC(data, 32);

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.sendNEC(data, 32); }
    }

    Serial.println(
        "Sent NEC Command" + (bruceConfigPins.irTxRepeats > 0
                                  ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                  : "")
    );

    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

void sendNECextCommand(String address, String command, bool hideDefaultUI) {
    IRsend irsend(bruceConfigPins.irTx); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }

    int first_zero_byte_pos = address.indexOf("00", 2);
    if (first_zero_byte_pos != -1) address = address.substring(0, first_zero_byte_pos);
    first_zero_byte_pos = command.indexOf("00", 2);
    if (first_zero_byte_pos != -1) command = command.substring(0, first_zero_byte_pos);

    address.replace(" ", "");
    command.replace(" ", "");

    uint16_t addressValue = strtoul(address.c_str(), nullptr, 16);
    uint16_t commandValue = strtoul(command.c_str(), nullptr, 16);

    // Invert Endianness
    uint16_t newAddress = (addressValue >> 8) | (addressValue << 8);
    uint16_t newCommand = (commandValue >> 8) | (commandValue << 8);

    // NEC protocol bit order is LSB first
    uint16_t lsbAddress = reverseBits(newAddress, 16);
    uint16_t lsbCommand = reverseBits(newCommand, 16);

    uint32_t data = ((uint32_t)lsbAddress << 16) | lsbCommand;
    irsend.sendNEC(data, 32); // Sends MSB first

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.sendNEC(data, 32); }
    }

    Serial.println(
        "Sent NECext Command" + (bruceConfigPins.irTxRepeats > 0
                                     ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                     : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

void sendRC5Command(String address, String command, bool hideDefaultUI) {
    IRsend irsend(bruceConfigPins.irTx, true); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }
    uint8_t addressValue = strtoul(address.substring(0, 2).c_str(), nullptr, 16);
    uint8_t commandValue = strtoul(command.substring(0, 2).c_str(), nullptr, 16);
    uint16_t data = irsend.encodeRC5(addressValue, commandValue);
    irsend.sendRC5(data, 13);

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.sendRC5(data, 13); }
    }
    Serial.println(
        "Sent RC5 Command" + (bruceConfigPins.irTxRepeats > 0
                                  ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                  : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

void sendRC6Command(String address, String command, bool hideDefaultUI) {
    IRsend irsend(bruceConfigPins.irTx, true); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }
    address.replace(" ", "");
    command.replace(" ", "");
    uint32_t addressValue = strtoul(address.substring(0, 2).c_str(), nullptr, 16);
    uint32_t commandValue = strtoul(command.substring(0, 2).c_str(), nullptr, 16);
    uint64_t data = irsend.encodeRC6(addressValue, commandValue);

    irsend.sendRC6(data, 20);

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.sendRC6(data, 20); }
    }

    Serial.println(
        "Sent RC6 Command" + (bruceConfigPins.irTxRepeats > 0
                                  ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                  : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

void sendSamsungCommand(String address, String command, bool hideDefaultUI) {
    IRsend irsend(bruceConfigPins.irTx); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }
    uint8_t addressValue = strtoul(address.substring(0, 2).c_str(), nullptr, 16);
    uint8_t commandValue = strtoul(command.substring(0, 2).c_str(), nullptr, 16);
    uint64_t data = irsend.encodeSAMSUNG(addressValue, commandValue);

    irsend.sendSAMSUNG(data, 32);

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.sendSAMSUNG(data, 32); }
    }

    Serial.println(
        "Sent Samsung Command" + (bruceConfigPins.irTxRepeats > 0
                                      ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                      : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

void sendSonyCommand(String address, String command, uint8_t nbits, bool hideDefaultUI) {
    IRsend irsend(bruceConfigPins.irTx); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }

    address.replace(" ", "");
    command.replace(" ", "");

    uint32_t addressValue = strtoul(address.c_str(), nullptr, 16);
    uint32_t commandValue = strtoul(command.c_str(), nullptr, 16);

    uint16_t swappedAddr = static_cast<uint16_t>(swap32(addressValue));
    uint8_t swappedCmd = static_cast<uint8_t>(swap32(commandValue));

    uint32_t data;

    if (nbits == 12) {
        // SIRC (12 bits)
        data = ((swappedAddr & 0x1F) << 7) | (swappedCmd & 0x7F);
    } else if (nbits == 15) {
        // SIRC15 (15 bits)
        data = ((swappedAddr & 0xFF) << 7) | (swappedCmd & 0x7F);
    } else if (nbits == 20) {
        // SIRC20 (20 bits)
        data = ((swappedAddr & 0x1FFF) << 7) | (swappedCmd & 0x7F);
    } else {
        Serial.println("Invalid Sony (SIRC) protocol bit size.");
        return;
    }

    // SIRC protocol bit order is LSB First
    data = reverseBits(data, nbits);

    // 1 initial + 2 repeat
    irsend.sendSony(data, nbits, 2); // Sends MSB First

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.sendSony(data, nbits, 2); }
    }

    Serial.println(
        "Sent Sony Command" + (bruceConfigPins.irTxRepeats > 0
                                   ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                   : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

void sendKaseikyoCommand(String address, String command, bool hideDefaultUI) {
    IRsend irsend(bruceConfigPins.irTx); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }

    address.replace(" ", "");
    command.replace(" ", "");

    uint32_t addressValue = strtoul(address.c_str(), nullptr, 16);
    uint32_t commandValue = strtoul(command.c_str(), nullptr, 16);

    uint32_t newAddress = swap32(addressValue);
    uint16_t newCommand = static_cast<uint16_t>(swap32(commandValue));

    uint8_t id = (newAddress >> 24) & 0xFF;
    uint16_t vendor_id = (newAddress >> 8) & 0xFFFF;
    uint8_t genre1 = (newAddress >> 4) & 0x0F;
    uint8_t genre2 = newAddress & 0x0F;

    uint16_t data = newCommand & 0x3FF;

    byte bytes[6];
    bytes[0] = vendor_id & 0xFF;
    bytes[1] = (vendor_id >> 8) & 0xFF;

    uint8_t vendor_parity = bytes[0] ^ bytes[1];
    vendor_parity = (vendor_parity & 0xF) ^ (vendor_parity >> 4);

    bytes[2] = (genre1 << 4) | (vendor_parity & 0x0F);
    bytes[3] = ((data & 0x0F) << 4) | genre2;
    bytes[4] = ((id & 0x03) << 6) | ((data >> 4) & 0x3F);

    bytes[5] = bytes[2] ^ bytes[3] ^ bytes[4];

    uint64_t lsb_data = 0;
    for (int i = 0; i < 6; i++) { lsb_data |= (uint64_t)bytes[i] << (8 * i); }

    // LSB First --> MSB First
    uint64_t msb_data = reverseBits(lsb_data, 48);

    irsend.sendPanasonic64(msb_data, 48); // Sends MSB First

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.sendPanasonic64(msb_data, 48); }
    }

    Serial.println(
        "Sent Kaseikyo Command" + (bruceConfigPins.irTxRepeats > 0
                                       ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                       : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

bool sendDecodedCommand(String protocol, String value, uint8_t bits, bool hideDefaultUI) {
    // https://github.com/crankyoldgit/IRremoteESP8266/blob/master/examples/SmartIRRepeater/SmartIRRepeater.ino
#if !defined(LITE_VERSION)
    decode_type_t type = strToDecodeType(protocol.c_str());
    if (type == decode_type_t::UNKNOWN) return false;

    IRsend irsend(bruceConfigPins.irTx); // Set the GPIO to be used to sending the message.
    irsend.begin();
    bool success = false;
    if (!hideDefaultUI) { displayTextLine("Sending.."); }

    if (hasACState(type)) {
        // need to send the state (still passed from value)
        uint8_t state[bits / 8] = {0};
        uint16_t state_pos = 0;
        for (uint16_t i = 0; i < value.length(); i += 3) {
            // parse  value -> state
            uint8_t highNibble = hexCharToDecimal(value[i]);
            uint8_t lowNibble = hexCharToDecimal(value[i + 1]);
            state[state_pos] = (highNibble << 4) | lowNibble;
            state_pos++;
        }
        // success = irsend.send(type, state, bits / 8);
        success = irsend.send(type, state, state_pos); // safer

        if (bruceConfigPins.irTxRepeats > 0) {
            for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) {
                irsend.send(type, state, state_pos);
            }
        }

    } else {
        value.replace(" ", "");
        uint64_t value_int = strtoull(value.c_str(), nullptr, 16);

        success =
            irsend.send(type, value_int, bits); // bool send(const decode_type_t type, const uint64_t data,
                                                // const uint16_t nbits, const uint16_t repeat = kNoRepeat);

        if (bruceConfigPins.irTxRepeats > 0) {
            for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) { irsend.send(type, value_int, bits); }
        }
    }

    delay(20);
    Serial.println(
        "Sent Decoded Command" + (bruceConfigPins.irTxRepeats > 0
                                      ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                      : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
    return success;
#else
    if (!hideDefaultUI) { displayTextLine("Unavailable on this Version"); }
    delay(1000);
    return false;
#endif
}

void sendRawCommand(uint16_t frequency, String rawData, bool hideDefaultUI) {
#ifdef USE_BOOST /// ENABLE 5V OUTPUT
    PPM.enableOTG();
#endif

    IRsend irsend(bruceConfigPins.irTx); // Set the GPIO to be used to sending the message.
    irsend.begin();
    if (!hideDefaultUI) { displayTextLine("Sending.."); }

    uint16_t dataBufferSize = 1;
    for (int i = 0; i < rawData.length(); i++) {
        if (rawData[i] == ' ') dataBufferSize += 1;
    }
    uint16_t *dataBuffer = (uint16_t *)malloc((dataBufferSize) * sizeof(uint16_t));

    uint16_t count = 0;
    // Parse raw data string
    while (rawData.length() > 0 && count < dataBufferSize) {
        int delimiterIndex = rawData.indexOf(' ');
        if (delimiterIndex == -1) { delimiterIndex = rawData.length(); }
        String dataChunk = rawData.substring(0, delimiterIndex);
        rawData.remove(0, delimiterIndex + 1);
        dataBuffer[count++] = (dataChunk.toInt());
    }

    Serial.println("Parsing raw data complete.");
    // Serial.println(count);
    // Serial.println(dataBuffer[count-1]);
    // Serial.println(dataBuffer[0]);

    // Send raw command
    irsend.sendRaw(dataBuffer, count, frequency);

    if (bruceConfigPins.irTxRepeats > 0) {
        for (uint8_t i = 1; i <= bruceConfigPins.irTxRepeats; i++) {
            irsend.sendRaw(dataBuffer, count, frequency);
        }
    }

    free(dataBuffer);

    Serial.println(
        "Sent Raw Command" + (bruceConfigPins.irTxRepeats > 0
                                  ? " (1 initial + " + String(bruceConfigPins.irTxRepeats) + " repeats)"
                                  : "")
    );
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
}

bool chooseCmdIrFile(FS *fs, String filepath) {
    checkIrTxPin();
    returnToMenu = true;
    drawMainBorder();
    if (!loadIRCodesFromFile(fs, filepath)) return false;

    options = {};
    bool exit = false;
    bool goToMainMenu = false;
    bool actionTaken = false;

    for (auto code : codes) {
        if (code->name != "") {
            options.push_back({code->name.c_str(), [code, &actionTaken]() {
                               actionTaken = true;
                               sendIRCommand(code);
                               addToRecentCodes(code);
                           }});
        }
    }
    options.push_back({"Main Menu", [&]() { actionTaken = true; exit = true; goToMainMenu = true; }});

#ifdef USE_BOOST /// DISABLE 5V OUTPUT
    PPM.disableOTG();
#endif

    digitalWrite(bruceConfigPins.irTx, LED_OFF);
    int idx = 0;
    while (1) {
        actionTaken = false;
        idx = loopOptions(options, idx);

        if (exit) break;

        // loopOptions returned without any lambda running → EscPress was consumed internally
        // Treat it like a back button press
        if (!actionTaken) {
            // Distinguish short vs long press by checking if button is still held
            unsigned long pressStart = millis();
            bool longPress = false;
            while (check(EscPress)) {           // button still physically held
                if (millis() - pressStart >= 2000) {
                    longPress = true;
                    break;
                }
                delay(10);
            }
            while (check(EscPress)) delay(10);  // wait for release

            if (longPress) goToMainMenu = true;
            // Short (or already released): goToMainMenu stays false → back to file browser
            break;
        }
    }
    options.clear();
    resetCodesArray();
    // Flush any residual EscPress
    delay(100);
    while (check(EscPress)) delay(10);

    if (!goToMainMenu) {
        // Short press: going back to file browser, NOT to main menu
        // Reset returnToMenu so loopOptions chain doesn't cascade-exit everything
        returnToMenu = false;
    }
    // true  = go to main menu (long press or "Main Menu" item selected)
    // false = go back to file browser (short Esc press)
    return goToMainMenu;
}
