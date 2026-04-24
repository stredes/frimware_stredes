#include "BrucegotchiMenu.h"

#include "modules/pwnagotchi/pwnagotchi.h"

void BrucegotchiMenu::optionsMenu() {
    brucegotchi_start();
}

void BrucegotchiMenu::drawIcon(float scale) {
    clearIconArea();

    int headW = scale * 48;
    int headH = scale * 34;
    int headX = iconCenterX - headW / 2;
    int headY = iconCenterY - headH / 2 - scale * 6;
    int eyeR = max(2, (int)(scale * 3));
    int antennaH = scale * 12;

    tft.drawLine(iconCenterX, headY, iconCenterX, headY - antennaH, bruceConfig.secColor);
    tft.fillCircle(iconCenterX, headY - antennaH, max(2, (int)(scale * 3)), bruceConfig.secColor);

    tft.drawRoundRect(headX, headY, headW, headH, scale * 7, bruceConfig.priColor);
    tft.drawRoundRect(headX + 3, headY + 3, headW - 6, headH - 6, scale * 5, bruceConfig.secColor);

    tft.fillCircle(iconCenterX - scale * 12, iconCenterY - scale * 4, eyeR, bruceConfig.priColor);
    tft.fillCircle(iconCenterX + scale * 12, iconCenterY - scale * 4, eyeR, bruceConfig.priColor);
    tft.drawFastHLine(iconCenterX - scale * 10, iconCenterY + scale * 9, scale * 20, bruceConfig.secColor);

    int bodyY = headY + headH + scale * 5;
    tft.drawRoundRect(iconCenterX - scale * 18, bodyY, scale * 36, scale * 18, scale * 5, bruceConfig.priColor);
    tft.drawLine(iconCenterX - scale * 18, bodyY + scale * 8, iconCenterX - scale * 30, bodyY + scale * 2, bruceConfig.secColor);
    tft.drawLine(iconCenterX + scale * 18, bodyY + scale * 8, iconCenterX + scale * 30, bodyY + scale * 2, bruceConfig.secColor);
}
