#include "C2Menu.h"

#include "../mykeyboard.h"
#include "core/c2_agent.h"
#include "core/display.h"
#include "core/utils.h"
#include "core/wifi/wifi_common.h"
#include "modules/wifi/clients.h"
#include "modules/wifi/tcp_utils.h"
#include "core/scrollableTextArea.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {
bool ensureC2Network() {
    if (WiFi.status() == WL_CONNECTED) return true;
    if (wifiConnecttoKnownNet()) return true;
    return wifiConnectMenu(WIFI_STA);
}

String c2BaseUrl() {
    String scheme = bruceConfig.c2UseTLS ? "https://" : "http://";
    return scheme + bruceConfig.c2Host + ":" + String(bruceConfig.c2Port);
}

bool probeC2Tcp(String &detail) {
    if (bruceConfig.c2Host.isEmpty()) {
        detail = "Server host not configured";
        return false;
    }

    if (!ensureC2Network()) {
        detail = "WiFi not connected";
        return false;
    }

    if (bruceConfig.c2UseTLS) {
        WiFiClientSecure client;
        client.setInsecure();
        bool ok = client.connect(bruceConfig.c2Host.c_str(), bruceConfig.c2Port);
        detail = ok ? "TCP/TLS reachable" : "TCP/TLS unreachable";
        client.stop();
        return ok;
    }

    WiFiClient client;
    bool ok = client.connect(bruceConfig.c2Host.c_str(), bruceConfig.c2Port);
    detail = ok ? "TCP reachable" : "TCP unreachable";
    client.stop();
    return ok;
}

bool probeC2Http(int &statusCode, String &responsePreview) {
    statusCode = -1;
    responsePreview = "";

    if (bruceConfig.c2UseTLS) return false;
    if (bruceConfig.c2Host.isEmpty()) return false;
    if (!ensureC2Network()) return false;

    HTTPClient http;
    String url = c2BaseUrl() + bruceConfig.c2HealthPath;
    http.setConnectTimeout(2500);
    http.begin(url);
    statusCode = http.GET();
    if (statusCode > 0) {
        responsePreview = http.getString();
        responsePreview.replace("\r", " ");
        responsePreview.replace("\n", " ");
        if (responsePreview.length() > 96) responsePreview = responsePreview.substring(0, 96) + "...";
    }
    http.end();
    return statusCode > 0;
}

bool ensureC2TargetConfigured() {
    if (!bruceConfig.c2Host.isEmpty()) return true;

    String host = keyboard(bruceConfig.c2Host, 96, "C2 Host/IP:");
    if (host.length() == 0) return false;
    bruceConfig.setC2Host(host);
    return true;
}

bool ensureC2SshConfigured() {
    if (bruceConfig.c2User.isEmpty()) {
        String user = keyboard(bruceConfig.c2User, 64, "SSH User:");
        if (user.length() == 0) return false;
        bruceConfig.setC2User(user);
    }

    if (bruceConfig.c2Password.isEmpty()) {
        String password = keyboard("", 96, "SSH Password:", true);
        if (password.length() == 0) return false;
        bruceConfig.setC2Password(password);
    }

    return true;
}

void runTcpConsole(const String &serverHost, uint16_t port) {
    if (!ensureC2Network()) {
        displayError("WiFi unavailable", true);
        return;
    }

    if (serverHost.isEmpty() || port == 0) {
        displayError("Invalid host or port", true);
        return;
    }

    WiFiClient client;
    if (!client.connect(serverHost.c_str(), port)) {
        displayError("TCP connection failed", true);
        return;
    }

    bool inputMode = false;
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("TCP connected to:");
    tft.println(serverHost + ":" + String(port));
    tft.println("");
    tft.println("SEL = send | ESC = exit");

    while (client.connected()) {
        if (inputMode) {
            String keyString = keyboard("", 128, "Send TCP data:");
            inputMode = false;
            tft.fillScreen(TFT_BLACK);
            tft.setCursor(0, 0);
            tft.println("TCP connected to:");
            tft.println(serverHost + ":" + String(port));
            tft.println("");
            if (keyString.length() > 0) {
                client.print(keyString);
                Serial.print(keyString);
            }
        } else {
            while (client.available()) {
                char incomingChar = client.read();
                tft.print(incomingChar);
                Serial.print(incomingChar);
            }
            if (check(SelPress)) inputMode = true;
        }

        if (check(EscPress)) {
            client.stop();
            break;
        }
        delay(10);
    }

    displayInfo("TCP session closed", true);
}
} // namespace

void C2Menu::optionsMenu() {
    options = {
        {"Connect",       [this]() { connectMenu(); }     },
        {"Server Status", [this]() { showServerStatus(); }},
        {"Ping Agent",    [this]() { pingAgent(); }      },
        {"Config",        [this]() { configMenu(); }     },
    };

    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "Command & Control");
}

void C2Menu::connectMenu() {
    while (true) {
        std::vector<Option> localOptions = {
            {"SSH",          [this]() { openSSH(); }        },
            {"TCP Client",   [this]() { openTcpClient(); }  },
            {"HTTP Health",  [this]() { openHttpHealth(); } },
            {"Edit IP/Host", []() {
                 String host = keyboard(bruceConfig.c2Host, 96, "C2 Host/IP:");
                 if (host.length() > 0) bruceConfig.setC2Host(host);
             }                                               },
            {"Back",         []() {}                         },
        };

        int selected = loopOptions(localOptions, MENU_TYPE_SUBMENU, "Connect Via");
        if (selected == -1 || selected == localOptions.size() - 1) return;
    }
}

void C2Menu::configMenu() {
    while (true) {
        std::vector<Option> localOptions = {
            {String("Agent: ") + (bruceConfig.c2AgentEnabled ? "ON" : "OFF"),
             []() { bruceConfig.setC2AgentEnabled(!bruceConfig.c2AgentEnabled); }                                },
            {String("IP/Host: ") + (bruceConfig.c2Host.isEmpty() ? "<unset>" : bruceConfig.c2Host),
             []() {
                 String host = keyboard(bruceConfig.c2Host, 96, "C2 Host/IP:");
                 if (host.length() > 0) bruceConfig.setC2Host(host);
             }                                                                                                },
            {String("Port: ") + String(bruceConfig.c2Port),
             []() {
                 String port = num_keyboard(String(bruceConfig.c2Port), 5, "C2 Port:");
                 if (port.length() > 0) bruceConfig.setC2Port((uint16_t)port.toInt());
             }                                                                                                },
            {String("SSH User: ") + (bruceConfig.c2User.isEmpty() ? "<unset>" : bruceConfig.c2User),
             []() {
                 String user = keyboard(bruceConfig.c2User, 64, "SSH User:");
                 if (user.length() > 0) bruceConfig.setC2User(user);
             }                                                                                                },
            {String("SSH Password: ") + (bruceConfig.c2Password.isEmpty() ? "<unset>" : "<saved>"),
             []() {
                 String password = keyboard("", 96, "SSH Password:", true);
                 if (password.length() > 0) bruceConfig.setC2Password(password);
             }                                                                                                },
            {String("Health: ") + bruceConfig.c2HealthPath,
             []() {
                 String path = keyboard(bruceConfig.c2HealthPath, 64, "Health path:");
                 if (path.length() > 0) bruceConfig.setC2HealthPath(path);
             }                                                                                                },
            {String("TLS: ") + (bruceConfig.c2UseTLS ? "ON" : "OFF"),
             []() { bruceConfig.setC2UseTLS(!bruceConfig.c2UseTLS); }                                         },
            {String("Token: ") + (bruceConfig.c2TokenId.isEmpty() ? "<unset>" : bruceConfig.c2TokenId),
             []() {
                 String token = keyboard(bruceConfig.c2TokenId, 64, "C2 Token ID:");
                 if (token.length() > 0) bruceConfig.setC2TokenId(token);
             }                                                                                                },
            {String("Secret: ") + (bruceConfig.c2SecretKey.isEmpty() ? "<unset>" : "<saved>"),
             []() {
                 String secret = keyboard("", 96, "C2 Secret:", true);
                 if (secret.length() > 0) bruceConfig.setC2SecretKey(secret);
             }                                                                                                },
            {String("Device ID: ") + c2AgentDeviceId(),
             []() {
                 String deviceId = keyboard(c2AgentDeviceId(), 64, "C2 Device ID:");
                 if (deviceId.length() > 0) bruceConfig.setC2DeviceId(deviceId);
             }                                                                                                },
            {"Back", []() {}},
        };

        int selected = loopOptions(localOptions, MENU_TYPE_SUBMENU, "C2 Config");
        if (selected == -1 || selected == localOptions.size() - 1) return;
    }
}

void C2Menu::showServerStatus() {
    ScrollableTextArea area = ScrollableTextArea("C2 STATUS");
    area.addLine("Mode: Command & Control");
    area.addLine("Host: " + (bruceConfig.c2Host.isEmpty() ? String("<unset>") : bruceConfig.c2Host));
    area.addLine("Port: " + String(bruceConfig.c2Port));
    area.addLine("SSH User: " + (bruceConfig.c2User.isEmpty() ? String("<unset>") : bruceConfig.c2User));
    area.addLine("SSH Password: " + String(bruceConfig.c2Password.isEmpty() ? "<unset>" : "<saved>"));
    area.addLine("TLS: " + String(bruceConfig.c2UseTLS ? "ON" : "OFF"));
    area.addLine("Health: " + bruceConfig.c2HealthPath);
    area.addLine("WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "offline"));
    area.addLine("Agent: " + String(bruceConfig.c2AgentEnabled ? "enabled" : "disabled"));
    area.addLine("Agent task: " + String(isC2AgentRunning() ? "running" : "stopped"));
    area.addLine("Device ID: " + c2AgentDeviceId());
    area.addLine("Token: " + (bruceConfig.c2TokenId.isEmpty() ? String("<unset>") : bruceConfig.c2TokenId));
    area.addLine("Secret: " + String(bruceConfig.c2SecretKey.isEmpty() ? "<unset>" : "<saved>"));
    area.addLine("Agent status: " + c2AgentStatus());
    if (c2AgentLastSeenMs() > 0) area.addLine("Last heartbeat: " + String((millis() - c2AgentLastSeenMs()) / 1000) + "s ago");
    if (!c2AgentLastResult().isEmpty()) area.addLine("Last result: " + c2AgentLastResult());
    area.addLine("");

    String reachability;
    bool tcpOk = probeC2Tcp(reachability);
    area.addLine("Reachability: " + reachability);

    int httpStatus = -1;
    String httpPreview;
    if (tcpOk && probeC2Http(httpStatus, httpPreview)) {
        area.addLine("HTTP: " + String(httpStatus));
        if (httpPreview.length() > 0) area.addLine("Reply: " + httpPreview);
    } else if (!bruceConfig.c2UseTLS) {
        area.addLine("HTTP: not available");
    } else {
        area.addLine("HTTP: skipped on TLS");
    }

    area.show();
}

void C2Menu::pingAgent() {
    String detail;
    bool ok = probeC2Tcp(detail);
    if (ok) displaySuccess("C2 reachable", true);
    else displayError(detail, true);
}

void C2Menu::openSSH() {
    if (!ensureC2TargetConfigured()) return;
    if (!ensureC2SshConfigured()) return;
    if (!ensureC2Network()) {
        displayError("WiFi unavailable", true);
        return;
    }
    ssh_setup(
        bruceConfig.c2Host, String(bruceConfig.c2Port), bruceConfig.c2User, bruceConfig.c2Password, false
    );
}

void C2Menu::openTcpClient() {
    if (!ensureC2TargetConfigured()) return;
    runTcpConsole(bruceConfig.c2Host, bruceConfig.c2Port);
}

void C2Menu::openHttpHealth() {
    if (!ensureC2TargetConfigured()) return;

    int statusCode = -1;
    String preview;
    if (probeC2Http(statusCode, preview)) {
        String msg = "HTTP " + String(statusCode);
        if (!preview.isEmpty()) msg += "\n" + preview;
        displaySuccess(msg, true);
    } else {
        displayError("HTTP health failed", true);
    }
}

void C2Menu::drawIcon(float scale) {
    clearIconArea();

    int baseW = scale * 46;
    int baseH = scale * 30;
    int leftX = iconCenterX - baseW / 2;
    int topY = iconCenterY - baseH / 2;
    int rightX = leftX + baseW;
    int bottomY = topY + baseH;
    int nodeR = max(3, (int)(scale * 4));

    tft.drawRoundRect(leftX, topY, baseW, baseH, 6, bruceConfig.priColor);
    tft.drawFastHLine(leftX + 8, iconCenterY, baseW - 16, bruceConfig.priColor);
    tft.fillCircle(leftX, iconCenterY, nodeR, bruceConfig.priColor);
    tft.fillCircle(rightX, topY, nodeR, bruceConfig.priColor);
    tft.fillCircle(rightX, bottomY, nodeR, bruceConfig.priColor);
    tft.drawLine(leftX, iconCenterY, rightX, topY, bruceConfig.priColor);
    tft.drawLine(leftX, iconCenterY, rightX, bottomY, bruceConfig.priColor);
    tft.drawCentreString("C2", iconCenterX, bottomY + 10, 1);
}
