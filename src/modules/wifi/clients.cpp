#ifndef LITE_VERSION
// SSH borrowed from https://github.com/m5stack/M5Cardputer :)

// TODO: Display is kinda glitchy :P figure out some way to show better outputs also

// SSH libs
#include "libssh_esp32.h"
#include <libssh/libssh.h>

// Telnet libs
#include "clients.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/wifi/wifi_common.h"
#include <Arduino.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <globals.h>
#include <lwip/sockets.h>
#include <string.h>

// SSH server configuration (initialize as mpty strings)
String ssh_host = "";
String ssh_user = "";
String ssh_port = "";
String ssh_password = "";
char *ssh_port_char;

String commandBuffer = "> ";
int cursorY = 0;
const int lineHeight = 32; // 32
unsigned long lastKeyPressMillis = 0;
const unsigned long debounceDelay = 200; // Adjust debounce delay as needed
String ssh_remote_cwd = "~";
String ssh_output_line = "";
String ssh_terminal_history = "";
bool ssh_line_held = true;
bool ssh_ansi_sequence = false;

// ssh_bind sshbind = (ssh_bind)state->input;

// ssh_init sshbind;
ssh_session my_ssh_session;
ssh_channel channel_ssh;

char *stringTochar(String s) {
    if (s.length() == 0) {
        return nullptr; // or handle the case where the string is empty
    }

    static char arr[14]; // Make sure it's large enough to hold the IP address
    s.toCharArray(arr, sizeof(arr));
    return arr;
}

bool filterAnsiSequences = true; // Set to false to disable ANSI sequence filtering

static String sshPrompt() {
    return ssh_user + "@" + ssh_host + ":" + ssh_remote_cwd + "$ ";
}

static void sshDrawStatusBar(const String &status) {
    tft.fillRect(0, 0, tftWidth, 18, bruceConfig.priColor);
    tft.setTextColor(bruceConfig.bgColor, bruceConfig.priColor);
    String title = ssh_user + "@" + ssh_host + " " + status;
    if (title.length() > 38) title = title.substring(0, 38);
    tft.drawString(title, 4, 5, 1);

    tft.fillRect(0, tftHeight - 16, tftWidth, 16, bruceConfig.priColor);
    tft.setTextColor(bruceConfig.bgColor, bruceConfig.priColor);
    String prompt = sshPrompt();
    if (prompt.length() > 34) prompt = "..." + prompt.substring(prompt.length() - 31);
    tft.drawString(prompt, 4, tftHeight - 13, 1);
    tft.drawRightString("SEL cmd  ESC exit", tftWidth - 4, tftHeight - 13, 1);
    tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);
}

static void sshRememberTerminalText(const String &text) {
    ssh_terminal_history += text;
    const size_t maxHistory = 2600;
    if (ssh_terminal_history.length() > maxHistory) {
        ssh_terminal_history = ssh_terminal_history.substring(ssh_terminal_history.length() - maxHistory);
    }
}

static void sshTerminalReset(const String &status, bool clearHistory = true) {
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.setTextWrap(true);
    sshDrawStatusBar(status);
    tft.setCursor(0, 22);
    ssh_output_line = "";
    ssh_line_held = true;
    ssh_ansi_sequence = false;
    if (clearHistory) ssh_terminal_history = "";
}

static void sshTerminalEnsureSpace() {
    if (tft.getCursorY() < tftHeight - 24) return;
    tft.fillRect(0, 18, tftWidth, tftHeight - 34, bruceConfig.bgColor);
    tft.setCursor(0, 22);
    tft.setTextColor(getColorVariation(bruceConfig.priColor, 2, 1), bruceConfig.bgColor);
    tft.println("[screen cleared]");
    tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);
}

static void sshTerminalWriteRaw(const String &text, bool remember = true) {
    if (remember) sshRememberTerminalText(text);
    for (size_t i = 0; i < text.length(); ++i) {
        tft.write(text[i]);
        sshTerminalEnsureSpace();
    }
}

static void sshTerminalRedraw(const String &status) {
    sshTerminalReset(status, false);
    String visibleHistory = ssh_terminal_history;
    if (visibleHistory.length() > 1600) visibleHistory = visibleHistory.substring(visibleHistory.length() - 1600);
    sshTerminalWriteRaw(visibleHistory, false);
}

static bool sshShouldSkipAnsi(char c) {
    if (!filterAnsiSequences) return false;
    if (c == 0x1B) {
        ssh_ansi_sequence = true;
        return true;
    }
    if (!ssh_ansi_sequence) return false;
    if ((c >= '@' && c <= '~')) ssh_ansi_sequence = false;
    return true;
}

static void sshTerminalWriteChar(char c) {
    static const char *pwdMarker = "__BRUCE_PWD__";

    if (sshShouldSkipAnsi(c)) return;
    if (c == '\r') return;

    if (c == '\n') {
        if (ssh_output_line.startsWith(pwdMarker)) {
            ssh_remote_cwd = ssh_output_line.substring(strlen(pwdMarker));
            ssh_remote_cwd.trim();
            if (ssh_remote_cwd.isEmpty()) ssh_remote_cwd = "~";
            sshDrawStatusBar("connected");
        } else {
            if (ssh_line_held && ssh_output_line.length() > 0) sshTerminalWriteRaw(ssh_output_line);
            sshRememberTerminalText("\n");
            tft.println();
            sshTerminalEnsureSpace();
        }
        ssh_output_line = "";
        ssh_line_held = true;
        return;
    }

    ssh_output_line += c;
    String markerPrefix = String(pwdMarker).substring(0, ssh_output_line.length());
    if (ssh_line_held && ssh_output_line == markerPrefix) return;
    if (ssh_output_line.startsWith(pwdMarker)) return;

    if (ssh_line_held) {
        sshTerminalWriteRaw(ssh_output_line);
        ssh_line_held = false;
    } else {
        tft.write(c);
        sshTerminalEnsureSpace();
    }

    if (ssh_output_line.length() > 160) ssh_output_line = ssh_output_line.substring(ssh_output_line.length() - 80);
}

static void sshSendCommand(const String &command) {
    String message = command;
    message.trim();
    if (message.isEmpty()) return;

    if (message == "cls" || message == "clear") {
        sshTerminalReset("connected");
        commandBuffer = "";
        return;
    }

    String shownPrompt = sshPrompt();
    tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
    sshTerminalWriteRaw("\n" + shownPrompt + message + "\n");
    tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);

    message += "\r";
    ssh_channel_write(channel_ssh, message.c_str(), message.length());

    const char *pwdProbe = "printf '\\n__BRUCE_PWD__%s\\n' \"$PWD\"\r";
    ssh_channel_write(channel_ssh, pwdProbe, strlen(pwdProbe));
    commandBuffer = "";
}

void ssh_setup(String host) {
    ssh_setup(host, "", "", "", true);
}

void ssh_setup(String host, String port, String user, String password, bool promptForMissing) {
    if (!wifiConnected) wifiConnectMenu();

    tft.fillScreen(bruceConfig.bgColor);
    tft.setCursor(0, 0);
    if (host != "") ssh_host = host;
    else if (promptForMissing) {
        String my_net =
            WiFi.gatewayIP().toString().substring(0, WiFi.gatewayIP().toString().lastIndexOf(".") + 1);
        ssh_host = keyboard(my_net, 100, "SSH HOST (IP or Hostname)");
    } else if (ssh_host.isEmpty()) {
        displayError("SSH host missing", true);
        returnToMenu = true;
        return;
    }

    if (port.length() > 0) ssh_port = port;
    else if (ssh_port.isEmpty()) ssh_port = "22";
    if (promptForMissing) ssh_port = num_keyboard(ssh_port, 5, "SSH PORT");

    if (user.length() > 0) ssh_user = user;
    if (promptForMissing && ssh_user.isEmpty()) ssh_user = keyboard("", 76, "SSH USER");
    else if (promptForMissing) ssh_user = keyboard(ssh_user, 76, "SSH USER");

    if (password.length() > 0) ssh_password = password;
    if (promptForMissing && ssh_password.isEmpty()) ssh_password = keyboard("", 76, "SSH PASSWORD", true);
    else if (promptForMissing) ssh_password = keyboard(ssh_password, 76, "SSH PASSWORD", true);

    if (ssh_host.isEmpty() || ssh_port.isEmpty() || ssh_user.isEmpty() || ssh_password.isEmpty()) {
        displayError("SSH config incomplete", true);
        returnToMenu = true;
        return;
    }

    IPAddress resolvedIp;
    if (WiFi.hostByName(ssh_host.c_str(), resolvedIp)) {
        ssh_host = resolvedIp.toString();
    } else {
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayRedStripe("Failed to resolve hostname.", true);
        log_e("Failed to resolve hostname: %s", ssh_host.c_str());
        returnToMenu = true;
        return;
    }

    // Connect to SSH server
    TaskHandle_t sshTaskHandle = NULL;

#if SOC_CPU_CORES_NUM > 1
    xTaskCreatePinnedToCore(ssh_loop, "SSH Task", SSH_TASK_STACK_SIZE, NULL, 1, &sshTaskHandle, 1);
#else
    xTaskCreate(ssh_loop, "SSH Task", SSH_TASK_STACK_SIZE, NULL, 1, &sshTaskHandle); // runs on core0
#endif
    if (sshTaskHandle == NULL) {
        displayError("SSH Task creation failed.", true);
        Serial.println("Failed to create SSH Task");
        return;
    }

    while (!returnToMenu) { vTaskDelay(pdMS_TO_TICKS(200)); }
}

void ssh_loop(void *pvParameters) {
    sshTerminalReset("connecting");
    cursorY = tft.getCursorY();
    log_d("BEFORE SSH");
    my_ssh_session = ssh_new();
    log_d("AFTER SSH");
    // Disable watchdog
    disableCore0WDT();
#if SOC_CPU_CORES_NUM > 1
    disableCore1WDT();
#endif
    disableLoopWDT();

    if (my_ssh_session == NULL) {
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayError("SSH Session creation failed.", true);
        log_d("SSH Session creation failed.");
        returnToMenu = true;
        vTaskDelete(NULL);
        return;
    }
    ssh_port_char = stringTochar(ssh_port);
    uint16_t ssh_port_int = atoi(ssh_port_char);

    ssh_options_set(my_ssh_session, SSH_OPTIONS_HOST, ssh_host.c_str());
    ssh_options_set(my_ssh_session, SSH_OPTIONS_PORT, &ssh_port_int);
    ssh_options_set(my_ssh_session, SSH_OPTIONS_USER, ssh_user.c_str());
    long timeoutSeconds = 12;
    ssh_options_set(my_ssh_session, SSH_OPTIONS_TIMEOUT, &timeoutSeconds);
    log_d("AFTER COMPARE AND OPTION SET");

    if (ssh_connect(my_ssh_session) != SSH_OK) {
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayError("SSH Connect error.", true);
        log_d("SSH Connect error.");
        ssh_free(my_ssh_session);
        returnToMenu = true;
        vTaskDelete(NULL);
        return;
    }

    if (ssh_userauth_password(my_ssh_session, NULL, ssh_password.c_str()) != SSH_AUTH_SUCCESS) {
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayError("SSH Authentication error.", true);
        log_d("SSH Authentication error.");
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        returnToMenu = true;
        vTaskDelete(NULL);
        return;
    }

    channel_ssh = ssh_channel_new(my_ssh_session);
    if (channel_ssh == NULL || ssh_channel_open_session(channel_ssh) != SSH_OK) {
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayError("SSH Channel open error.", true);
        log_d("SSH Channel open error.");
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        returnToMenu = true;
        vTaskDelete(NULL);
        return;
    }

    if (ssh_channel_request_pty(channel_ssh) != SSH_OK) {
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayError("SSH PTY request error.", true);
        log_d("SSH PTY request error.");
        ssh_channel_close(channel_ssh);
        ssh_channel_free(channel_ssh);
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        returnToMenu = true;
        vTaskDelete(NULL);
        return;
    }

    if (ssh_channel_request_shell(channel_ssh) != SSH_OK) {
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayError("SSH Shell request error.", true);
        log_d("SSH Shell request error.");
        ssh_channel_close(channel_ssh);
        ssh_channel_free(channel_ssh);
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        returnToMenu = true;
        vTaskDelete(NULL);
        return;
    }

    log_d("SSH setup completed.");
    sshTerminalReset("connected");
    sshDrawStatusBar("connected");
    const char *pwdProbe = "printf '\\n__BRUCE_PWD__%s\\n' \"$PWD\"\r";
    ssh_channel_write(channel_ssh, pwdProbe, strlen(pwdProbe));

    char *buffer = (char *)malloc(768);
    if (buffer == nullptr) {
        displayError("SSH buffer alloc failed.", true);
        ssh_channel_close(channel_ssh);
        ssh_channel_free(channel_ssh);
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        returnToMenu = true;
        enableCore0WDT();
#if SOC_CPU_CORES_NUM > 1
        enableCore1WDT();
#endif
        enableLoopWDT();
        feedLoopWDT();
        vTaskDelete(NULL);
        return;
    }
    int nbytes;
    keyStroke key;
    while (1) {
        if (returnToMenu || check(EscPress)) break;
#ifdef HAS_KEYBOARD
        key = _getKeyPress();
        if (key.pressed) {
            unsigned long currentMillis = millis();
            if (currentMillis - lastKeyPressMillis >= debounceDelay) {
                lastKeyPressMillis = currentMillis;
                for (auto i : key.word) {
                    commandBuffer += i;
                    tft.print(i);
                    cursorY = tft.getCursorY();
                }
                if (key.del && commandBuffer.length() > 0) {
                    commandBuffer.remove(commandBuffer.length() - 1);
                    tft.setCursor(tft.getCursorX() - 6, tft.getCursorY());
                    tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
                    tft.print(" ");
                    tft.setCursor(tft.getCursorX() - 6, tft.getCursorY());
                    cursorY = tft.getCursorY();
                } else if (key.enter) {
                    sshSendCommand(commandBuffer);
                    cursorY = tft.getCursorY(); // Update cursor position
                    if (cursorY > tftHeight) {
                        tft.setCursor(0, tftHeight - 10);
                        tft.fillRect(0, tftHeight - 11, tftWidth, 11, bruceConfig.bgColor);
                    }
                }
            }
        }

#else
        if (check(SelPress)) {

            while (check(SelPress)) { yield(); } // timerless debounce
            String message = keyboard("", 96, sshPrompt());
            while (check(SelPress)) { yield(); } // timerless debounce
            sshTerminalRedraw("connected");
            sshSendCommand(message);
            log_d("%s", message.c_str());

            sshDrawStatusBar("connected");
            tft.setTextSize(FP);
        }

#endif

        // Read data from SSH server and display it, handling ANSI sequences
        nbytes = ssh_channel_read_nonblocking(channel_ssh, buffer, 768, 0);

        if (nbytes > 0) {
            String msg = "";
            tft.setTextColor(TFT_WHITE);
            for (int i = 0; i < nbytes; ++i) {
                msg += char(buffer[i]);
                sshTerminalWriteChar(buffer[i]);
                cursorY = tft.getCursorY();
            }
            log_d("%s", msg);

            cursorY = tft.getCursorY(); // Update cursor position
            sshTerminalEnsureSpace();
            tft.setTextColor(TFT_GREEN);
        }

        // Handle channel closure and other conditions
        if (nbytes < 0 || ssh_channel_is_closed(channel_ssh)) {
            log_d("Encerrando");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Clean Up
    if (buffer != nullptr) free(buffer);
    ssh_channel_close(channel_ssh);
    ssh_channel_free(channel_ssh);
    ssh_disconnect(my_ssh_session);
    ssh_free(my_ssh_session);
    check(SelPress); // Reset Button
    displayWarning("SSH session closed.", true);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    returnToMenu = true;
    enableCore0WDT();
#if SOC_CPU_CORES_NUM > 1
    enableCore1WDT();
#endif
    enableLoopWDT();
    feedLoopWDT();
    vTaskDelete(NULL);
}

String telnet_server_string = "";
String telnet_port_string = "";
char *telnet_server_ip;
char *telnet_server_port_char;

int telnet_server_port;

static int sock;

void telnet_loop() {
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(telnet_server_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(telnet_server_port);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        Serial.println("Unable to create socket");
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayRedStripe("Unable to create socket", true);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        return;
    }

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        Serial.println("Socket connection failed");
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        displayRedStripe("Socket connection failed", true);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        close(sock);
        return;
    }

    Serial.println("Connected to TELNET server");
    tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
    displayTextLine("Connected to TELNET server");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.fillScreen(bruceConfig.bgColor);
    tft.setCursor(0, 0);

    String commandInput;

    while (1) {
        tft.print("> ");
        // waitForInput(commandInput);
        commandInput = keyboard("", 76, "COMMAND");
        const char *command = commandInput.c_str();
        send(sock, command, strlen(command), 0);

        // You can also receive data from the server
        char buffer[128];
        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len > 0) {
            buffer[len] = '\0';
            // Check for Telnet negotiation commands (IAC)
            /*
if (buffer[0] == 0xFF) {
    // Skip Telnet negotiation command
    continue;
}
*/
            tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);
            Serial.printf("Received from server %s\n", buffer);
            // tft.printf("Received from server %s\n", buffer);
            for (int i = 0; i < len; i++) { Serial.printf("%02X ", buffer[i]); }
            tft.printf("%s\n", buffer);

            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void telnet_setup() {
    if (!wifiConnected) wifiConnectMenu();

    tft.fillScreen(bruceConfig.bgColor);
    tft.setCursor(0, 0);
    Serial.begin(115200); // Initialize serial communication for debugging
    Serial.println("Starting Setup");

    // auto cfg = M5.config();
    // M5Cardputer.begin(cfg, true);
    tft.setRotation(bruceConfigPins.rotation);
    tft.setTextSize(1); // Set text size

    cursorY = tft.getCursorY();

    tft.setCursor(0, 0);
    // tft.print("TELNET Host: \n");

    // Here the telnet_server_ip needs to be a char*, thats why the stringTochar()

    // waitForInput(telnet_server_string);
    telnet_server_string = keyboard("", 76, "TELNET_SERVER");
    telnet_server_ip = stringTochar(telnet_server_string);
    delay(300);
    // Serial.println(telnet_server_ip);

    // tft.print("TELNET Port: \n");
    // waitForInput(telnet_port_string);
    telnet_port_string = num_keyboard("", 76, "TELNET PORT");
    delay(300);
    char arr2[5];
    // telnet_server_port_char =
    telnet_port_string.toCharArray(arr2, sizeof(arr2));
    // telnet_server_port_char = stringTochar(telnet_port_string);
    telnet_server_port = atoi(arr2);
    Serial.println(telnet_server_ip);
    Serial.println(telnet_server_port);

    telnet_loop();
}
#endif
