#include "companion_ui.h"

#include "core/scrollableTextArea.h"
#include "core/utils.h"

namespace CompanionUI {

static void showDeviceInfo(CompanionClient &client) {
    ScrollableTextArea area("COMPANION INFO");
    const auto &info = client.getDeviceInfo();

    area.addLine("Name: " + info.deviceName);
    area.addLine("ID: " + info.deviceId);
    area.addLine("Address: " + client.getScanResult().address);
    area.addLine("RSSI: " + String(client.getScanResult().rssi) + " dBm");
    area.addLine("Protocol: " + String(info.protocolMajor) + "." + String(info.protocolMinor));
    area.addLine("MTU Pref: " + String(info.mtuPreferred));
    area.addLine("Auth: " + CompanionProtocol::authStateToString(client.getAuthState()));
    area.addLine("Services: " + String(client.getServiceCount()));
    if (!client.getLastEvent().isEmpty()) area.addLine("Event: " + client.getLastEvent());
    area.show();
}

static void showCapabilitiesList(CompanionClient &client) {
    ScrollableTextArea area("CAPABILITIES");
    uint32_t caps = client.getCapabilities();

    if (!caps) area.addLine("No capabilities exposed.");
    for (uint32_t bit = 1; bit <= CompanionProtocol::CAP_SCREEN_SHOT; bit <<= 1) {
        if (caps & bit) area.addLine(CompanionProtocol::capabilityToString(bit));
    }
    area.show();
}

static void openRemoteFiles(CompanionClient &client) {
    options.clear();
    options.push_back({"List Authorized Files", [&]() {
                           if (client.requestFileList()) displayInfo("Companion file list requested", true);
                           else displayError(client.getLastError(), true);
                       }});
    options.push_back({"Back", []() {}});
    loopOptions(options, MENU_TYPE_SUBMENU, "Remote Files", 0, false);
}

static void openRemoteCamera(CompanionClient &client) {
    options.clear();
    options.push_back({"Capture Image", [&]() {
                           if (client.requestCameraCapture()) displayInfo("Companion capture requested", true);
                           else displayError(client.getLastError(), true);
                       }});
    options.push_back({"Back", []() {}});
    loopOptions(options, MENU_TYPE_SUBMENU, "Remote Camera", 0, false);
}

static void openRemoteScreen(CompanionClient &client) {
    options.clear();
    options.push_back({"Capture Screen", [&]() {
                           if (client.requestScreenCapture()) displayInfo("Companion screenshot requested", true);
                           else displayError(client.getLastError(), true);
                       }});
    options.push_back({"Back", []() {}});
    loopOptions(options, MENU_TYPE_SUBMENU, "Remote Screen", 0, false);
}

void showCapabilities(CompanionClient &client) {
    while (client.isConnected()) {
        options.clear();
        options.push_back({"Device Info", [&]() { showDeviceInfo(client); }});
        options.push_back({"Capabilities", [&]() { showCapabilitiesList(client); }});
        if (client.hasCapability(CompanionProtocol::CAP_FILES_LIST))
            options.push_back({"Remote Files", [&]() { openRemoteFiles(client); }});
        if (client.hasCapability(CompanionProtocol::CAP_CAMERA_SHOT))
            options.push_back({"Remote Camera", [&]() { openRemoteCamera(client); }});
        if (client.hasCapability(CompanionProtocol::CAP_SCREEN_SHOT))
            options.push_back({"Remote Screen", [&]() { openRemoteScreen(client); }});
        options.push_back({"Disconnect", [&]() { client.disconnect(false); }});

        int result = loopOptions(options, MENU_TYPE_SUBMENU, "Capabilities", 0, false);
        if (result < 0 || !client.isConnected()) break;
    }
}

} // namespace CompanionUI
