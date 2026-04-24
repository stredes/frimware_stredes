#include "c2_agent.h"

#include "core/serial_commands/cli.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <globals.h>

namespace {
constexpr uint32_t HEALTHCHECK_INTERVAL_MS = 30000;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 15000;
constexpr uint32_t COMMAND_POLL_INTERVAL_MS = 5000;
constexpr uint32_t IDLE_INTERVAL_MS = 1000;

TaskHandle_t c2AgentTaskHandle = nullptr;
String lastStatus = "stopped";
String lastResult = "";
uint32_t lastSeenMs = 0;
bool registerSent = false;
uint32_t lastHealthcheckMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastCommandPollMs = 0;

String normalizedDeviceName() {
#ifdef DEVICE_NAME
    String value = DEVICE_NAME;
#else
    String value = "bruce";
#endif
    value.toLowerCase();
    value.replace(" ", "-");
    value.replace("_", "-");
    return value;
}

String baseUrl() {
    return String(bruceConfig.c2UseTLS ? "https://" : "http://") + bruceConfig.c2Host + ":" +
           String(bruceConfig.c2Port);
}

void addAuthHeaders(HTTPClient &http) {
    http.addHeader("X-Token-Id", bruceConfig.c2TokenId);
    http.addHeader("X-Secret-Key", bruceConfig.c2SecretKey);
    http.addHeader("Content-Type", "application/json");
}

bool beginHttp(HTTPClient &http, WiFiClient &client, WiFiClientSecure &secureClient, const String &url) {
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (bruceConfig.c2UseTLS) {
        secureClient.setInsecure();
        return http.begin(secureClient, url);
    }
    return http.begin(client, url);
}

bool httpGet(const String &url, String &responseBody, int &statusCode, bool withAuth) {
    HTTPClient http;
    WiFiClient client;
    WiFiClientSecure secureClient;

    if (!beginHttp(http, client, secureClient, url)) {
        responseBody = "http.begin failed";
        statusCode = -1;
        return false;
    }

    if (withAuth) addAuthHeaders(http);
    statusCode = http.GET();
    responseBody = statusCode > 0 ? http.getString() : http.errorToString(statusCode);
    http.end();
    return statusCode > 0 && statusCode < 400;
}

bool httpPost(const String &url, const String &payload, String &responseBody, int &statusCode) {
    HTTPClient http;
    WiFiClient client;
    WiFiClientSecure secureClient;

    if (!beginHttp(http, client, secureClient, url)) {
        responseBody = "http.begin failed";
        statusCode = -1;
        return false;
    }

    addAuthHeaders(http);
    statusCode = http.POST(payload);
    responseBody = statusCode > 0 ? http.getString() : http.errorToString(statusCode);
    http.end();
    return statusCode > 0 && statusCode < 400;
}

String buildRegisterPayload() {
    JsonDocument doc;
    doc["device_id"] = c2AgentDeviceId();
    doc["label"] = String("Bruce ") + normalizedDeviceName();
    doc["platform"] = "esp32";
    doc["firmware"] = String("bruce-") + BRUCE_VERSION;
    doc["ip"] = WiFi.localIP().toString();

    String payload;
    serializeJson(doc, payload);
    return payload;
}

String buildHeartbeatPayload() {
    JsonDocument doc;
    doc["device_id"] = c2AgentDeviceId();
    doc["label"] = String("Bruce ") + normalizedDeviceName();
    doc["platform"] = "esp32";
    doc["firmware"] = String("bruce-") + BRUCE_VERSION;
    doc["ip"] = WiFi.localIP().toString();
    doc["status"] = "online";

    JsonObject metrics = doc["metrics"].to<JsonObject>();
    metrics["free_heap"] = ESP.getFreeHeap();
    metrics["free_psram"] = ESP.getFreePsram();
    metrics["rssi"] = WiFi.RSSI();
    metrics["uptime_ms"] = millis();

    String payload;
    serializeJson(doc, payload);
    return payload;
}

String buildResultPayload(const String &commandId, bool ok, const String &output) {
    JsonDocument doc;
    doc["device_id"] = c2AgentDeviceId();
    doc["command_id"] = commandId;
    doc["ok"] = ok;
    doc["output"] = output;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

bool sendHealthcheck() {
    String body;
    int statusCode = 0;
    bool ok = httpGet(baseUrl() + bruceConfig.c2HealthPath, body, statusCode, false);
    lastStatus = ok ? "health ok" : "health failed " + String(statusCode);
    Serial.printf("[C2] HEALTH status=%d ok=%d\n", statusCode, ok ? 1 : 0);
    return ok;
}

bool sendRegister() {
    String body;
    int statusCode = 0;
    bool ok = httpPost(baseUrl() + "/api/v1/register", buildRegisterPayload(), body, statusCode);
    lastStatus = ok ? "registered" : "register failed " + String(statusCode);
    Serial.printf("[C2] REGISTER status=%d ok=%d %s\n", statusCode, ok ? 1 : 0, body.c_str());
    return ok;
}

bool sendHeartbeat() {
    String body;
    int statusCode = 0;
    bool ok = httpPost(baseUrl() + "/api/v1/heartbeat", buildHeartbeatPayload(), body, statusCode);
    lastStatus = ok ? "online" : "heartbeat failed " + String(statusCode);
    if (ok) lastSeenMs = millis();
    Serial.printf("[C2] HEARTBEAT status=%d ok=%d\n", statusCode, ok ? 1 : 0);
    return ok;
}

void sendCommandResult(const String &commandId, bool ok, const String &output) {
    String body;
    int statusCode = 0;
    bool posted = httpPost(baseUrl() + "/api/v1/results", buildResultPayload(commandId, ok, output), body, statusCode);
    lastResult = output;
    if (lastResult.length() > 120) lastResult = lastResult.substring(0, 120) + "...";
    lastStatus = posted ? "result posted" : "result failed " + String(statusCode);
    Serial.printf("[C2] RESULT status=%d ok=%d %s\n", statusCode, posted ? 1 : 0, body.c_str());
}

bool executeCommand(const String &command, String &output) {
    String cmd = command;
    cmd.trim();

    if (cmd.equalsIgnoreCase("PING")) {
        output = "PONG";
        return true;
    }
    if (cmd.equalsIgnoreCase("GET_IP")) {
        output = WiFi.localIP().toString();
        return true;
    }
    if (cmd.equalsIgnoreCase("GET_HEAP")) {
        output = String(ESP.getFreeHeap());
        return true;
    }
    if (cmd.equalsIgnoreCase("GET_STATUS")) {
        output = "device=" + c2AgentDeviceId() + " ip=" + WiFi.localIP().toString() +
                 " heap=" + String(ESP.getFreeHeap()) + " rssi=" + String(WiFi.RSSI());
        return true;
    }
    if (cmd.startsWith("ECHO ")) {
        output = cmd.substring(5);
        return true;
    }
    if (cmd.startsWith("BRUCE_CLI ")) {
        String cliCommand = cmd.substring(10);
        cliCommand.trim();
        if (cliCommand.isEmpty()) {
            output = "Empty BRUCE_CLI command";
            return false;
        }
        bool parsed = serialCli.parse(cliCommand);
        output = parsed ? "CLI command accepted: " + cliCommand : "CLI command failed: " + cliCommand;
        return parsed;
    }
    if (cmd.equalsIgnoreCase("REBOOT")) {
        output = "Rebooting";
        return true;
    }

    output = "Unsupported command: " + cmd;
    return false;
}

void handleCommandQueue() {
    String body;
    int statusCode = 0;
    String url = baseUrl() + "/api/v1/commands?device_id=" + c2AgentDeviceId();
    bool ok = httpGet(url, body, statusCode, true);
    if (!ok) {
        lastStatus = "command poll failed " + String(statusCode);
        Serial.printf("[C2] COMMANDS status=%d ok=0\n", statusCode);
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
        lastStatus = "command json failed";
        Serial.printf("[C2] JSON parse error: %s\n", error.c_str());
        return;
    }

    if (!doc["command"].is<JsonObject>()) {
        lastStatus = "idle";
        return;
    }

    JsonObject commandObj = doc["command"].as<JsonObject>();
    String commandId = commandObj["command_id"] | "";
    String command = commandObj["command"] | "";
    if (command.length() == 0 || commandId.length() == 0) return;

    String output;
    bool executed = executeCommand(command, output);
    sendCommandResult(commandId, executed, output);

    if (command.equalsIgnoreCase("REBOOT")) {
        delay(500);
        ESP.restart();
    }
}

bool canRunAgent() {
    return bruceConfig.c2AgentEnabled && !bruceConfig.c2Host.isEmpty() && !bruceConfig.c2TokenId.isEmpty() &&
           !bruceConfig.c2SecretKey.isEmpty() && WiFi.status() == WL_CONNECTED;
}

void c2AgentTask(void *parameter) {
    (void)parameter;
    lastStatus = "waiting";

    while (true) {
        if (!canRunAgent()) {
            registerSent = false;
            lastStatus = bruceConfig.c2Host.isEmpty() ? "host not set" : "waiting wifi";
            vTaskDelay(pdMS_TO_TICKS(IDLE_INTERVAL_MS));
            continue;
        }

        uint32_t now = millis();
        if (!registerSent) {
            sendHealthcheck();
            registerSent = sendRegister();
        }
        if (now - lastHealthcheckMs >= HEALTHCHECK_INTERVAL_MS) {
            lastHealthcheckMs = now;
            sendHealthcheck();
        }
        if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
            lastHeartbeatMs = now;
            if (!sendHeartbeat()) registerSent = false;
        }
        if (now - lastCommandPollMs >= COMMAND_POLL_INTERVAL_MS) {
            lastCommandPollMs = now;
            handleCommandQueue();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
} // namespace

void startC2AgentTask() {
    if (c2AgentTaskHandle != nullptr) return;
    xTaskCreate(c2AgentTask, "c2Agent", 8192, nullptr, 1, &c2AgentTaskHandle);
}

bool isC2AgentRunning() { return c2AgentTaskHandle != nullptr; }

String c2AgentDeviceId() {
    if (!bruceConfig.c2DeviceId.isEmpty()) return bruceConfig.c2DeviceId;
    return normalizedDeviceName();
}

String c2AgentStatus() { return lastStatus; }

String c2AgentLastResult() { return lastResult; }

uint32_t c2AgentLastSeenMs() { return lastSeenMs; }
