/*
Thanks to thoses developers for their projects:
* @7h30th3r0n3 : https://github.com/7h30th3r0n3/Evil-M5Core2 and https://github.com/7h30th3r0n3/PwnGridSpam
* @viniciusbo : https://github.com/viniciusbo/m5-palnagotchi
* @sduenasg : https://github.com/sduenasg/pio_palnagotchi

Thanks to @bmorcelli for his help doing a better code.
*/
#if !defined(LITE_VERSION)
#include "ui.h"
#include "../wifi/sniffer.h"

#define ROW_SIZE 40
#define PADDING 10

int32_t display_w;
int32_t display_h;
int32_t canvas_h;
int32_t canvas_center_x;
int32_t canvas_top_h;
int32_t canvas_bot_h;
int32_t canvas_peers_menu_h;
int32_t canvas_peers_menu_w;

uint8_t menu_current_cmd = 0;
uint8_t menu_current_opt = 0;

static String shortenText(const String &text, size_t maxLen) {
    if (text.length() <= maxLen) return text;
    if (maxLen < 4) return text.substring(0, maxLen);
    return text.substring(0, maxLen - 3) + "...";
}

static void drawCenteredParagraph(const String &text, int16_t centerX, int16_t baselineY, uint8_t maxCharsPerLine) {
    String remaining = text;
    int16_t lineOffset = 0;

    while (remaining.length() > 0 && lineOffset < 2) {
        int splitPos = remaining.length();
        if (remaining.length() > maxCharsPerLine) {
            splitPos = remaining.lastIndexOf(' ', maxCharsPerLine);
            if (splitPos <= 0) splitPos = maxCharsPerLine;
        }

        String line = remaining.substring(0, splitPos);
        remaining = remaining.substring(splitPos);
        remaining.trim();
        tft.drawCentreString(line, centerX, baselineY + (lineOffset * 14), SMOOTH_FONT);
        lineOffset++;
    }
}

static void drawGotchiBody(int16_t centerX, int16_t y, const String &face, bool broken) {
    uint16_t color = broken ? TFT_RED : bruceConfig.priColor;
    int16_t bodyW = display_w > 280 ? 120 : 106;
    int16_t bodyH = display_h > 220 ? 84 : 72;
    int16_t x = centerX - (bodyW / 2);
    int16_t visorX = x + 16;
    int16_t visorY = y + 18;
    int16_t visorW = bodyW - 32;
    int16_t visorH = 32;

    tft.drawRoundRect(x, y, bodyW, bodyH, 12, color);
    tft.drawRoundRect(x + 8, y + 8, bodyW - 16, bodyH - 16, 8, color);
    tft.drawRoundRect(visorX, visorY, visorW, visorH, 7, color);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, bruceConfig.bgColor);
    tft.drawCentreString(face, centerX, visorY + (visorH / 2) - 8, SMOOTH_FONT);
    tft.setTextSize(1);
    tft.drawLine(x + 12, y + 24, x - 24, y + 9, color);
    tft.drawLine(x + bodyW - 12, y + 24, x + bodyW + 24, y + 9, color);
    tft.drawLine(x - 24, y + 9, x - 31, y + 22, color);
    tft.drawLine(x + bodyW + 24, y + 9, x + bodyW + 31, y + 22, color);
    tft.drawLine(x + 30, y + bodyH, x + 18, y + bodyH + 17, color);
    tft.drawLine(x + bodyW - 30, y + bodyH, x + bodyW - 18, y + bodyH + 17, color);
    tft.drawLine(x + 13, y + bodyH + 17, x + 27, y + bodyH + 17, color);
    tft.drawLine(x + bodyW - 27, y + bodyH + 17, x + bodyW - 13, y + bodyH + 17, color);
    tft.drawCircle(x + 28, y + 61, 3, color);
    tft.drawCircle(x + bodyW - 28, y + 61, 3, color);
    tft.drawLine(x + 43, y + 65, x + bodyW - 43, y + 65, color);
    tft.drawLine(centerX, y - 7, centerX, y, color);
    tft.drawCircle(centerX, y - 10, 2, color);
}

static void drawGotchiMeter(const char *label, int value, int x, int y, int w) {
    int clamped = constrain(value, 0, 100);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString(label, x, y);
    tft.drawRoundRect(x, y + 12, w, 7, 3, bruceConfig.priColor);
    tft.fillRect(x + 2, y + 14, map(clamped, 0, 100, 0, w - 4), 3, bruceConfig.priColor);
}

void initUi() {
    tft.setTextSize(1);
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextColor(bruceConfig.priColor);

    display_w = tftWidth;
    display_h = tftHeight;
    canvas_h = display_h * .78;
    canvas_center_x = display_w / 2;
    canvas_top_h = display_h * .13;
    canvas_bot_h = display_h * .84;
    canvas_peers_menu_h = display_h * .8;
    canvas_peers_menu_w = display_w * .8;
}

String getRssiBars(signed int rssi) {
    String rssi_bars = "";

    if (rssi != -1000) {
        if (rssi >= -67) {
            rssi_bars = "[####]";
        } else if (rssi >= -70) {
            rssi_bars = "[### ]";
        } else if (rssi >= -80) {
            rssi_bars = "[##  ]";
        } else {
            rssi_bars = "[#   ]";
        }
    }

    return rssi_bars;
}

void drawTime() {
    tft.drawPixel(0, 0, 0);
    tft.fillRect(display_w - 135, 0, 135, canvas_top_h - 3, bruceConfig.bgColor);
    tft.setTextDatum(TR_DATUM);
    unsigned long ellapsed = millis() / 1000;
    int8_t h = ellapsed / 3600;
    int sr = ellapsed % 3600;
    int8_t m = sr / 60;
    int8_t s = sr % 60;
    char right_str[50] = "BAT 0%  ACT 00:00:00";
    sprintf(right_str, "BAT %i%% ACT %02d:%02d:%02d", getBattery(), h, m, s);
    tft.drawString(right_str, display_w, 3);
}

void drawFooterData(uint8_t friends_run, uint8_t friends_tot, String last_friend_name, signed int rssi) {
    tft.drawPixel(0, 0, 0);
    tft.fillRect(0, canvas_bot_h + 1, display_w, display_h - canvas_bot_h, bruceConfig.bgColor);
    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.priColor);
    tft.setTextDatum(TL_DATUM);

    String rssi_bars = getRssiBars(rssi);
    String stats = "AMIGOS 0/0";
    if (friends_run > 0) {
        stats = "AMIGOS " + String(friends_run) + "/" + String(friends_tot) + " [" +
                shortenText(last_friend_name, 12) + "] " + rssi_bars;
    }

    tft.drawString(stats, 6, canvas_bot_h + 6);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("SEL: opciones", display_w - 6, canvas_bot_h + 18);
}

void updateUi(bool show_toolbars) {
    uint8_t mood_id = getCurrentMoodId();
    String mood_face = getCurrentMoodFace();
    String mood_phrase = getCurrentMoodPhrase();
    bool mood_broken = isCurrentMoodBroken();

    // Draw header and footer
    if (show_toolbars) {
        drawTopCanvas();
        if (tftHeight > 150) drawTime();
        drawFooterData(
            getPwngridRunTotalPeers(),
            getPwngridTotalPeers(),
            getPwngridLastFriendName(),
            getPwngridClosestRssi()
        );
    }

    // Draw mood
    drawMood(mood_face, mood_phrase, mood_broken);

#if defined(HAS_TOUCH)
    TouchFooter();
#endif
}

void drawTopCanvas() {
    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextDatum(TL_DATUM);
    char buffer[32];
    sprintf(buffer, "CH %02d  HS %d", ch, num_HS);
    tft.drawPixel(0, 0, 0);
    tft.fillRect(0, 0, display_w, canvas_top_h, bruceConfig.bgColor);
    tft.drawRoundRect(0, 0, display_w, canvas_top_h, 6, bruceConfig.priColor);
    tft.drawString("BRUCEGOTCHI", 8, 4);
    tft.drawString(buffer, 8, 17);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("SEL: hablar", display_w - 8, 17);
    tft.drawLine(0, canvas_top_h - 1, display_w, canvas_top_h - 1, bruceConfig.priColor);
}

void drawBottomCanvas() {
    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextDatum(TR_DATUM);
    tft.drawPixel(0, 0, 0);
    tft.fillRect(0, canvas_bot_h, display_w, display_h - canvas_bot_h, bruceConfig.bgColor);
    tft.drawRoundRect(0, canvas_bot_h, display_w, display_h - canvas_bot_h, 6, bruceConfig.priColor);
    tft.drawString("NO IA", display_w - 6, canvas_bot_h + 6);
    tft.drawLine(0, canvas_bot_h, display_w, canvas_bot_h, bruceConfig.priColor);
}

void drawMood(String face, String phrase, bool broken) {
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FG + 1);
    tft.setTextDatum(MC_DATUM);
    tft.drawPixel(0, 0, 0);
    tft.fillRect(0, canvas_top_h + 2, display_w, (canvas_bot_h - canvas_top_h) - 4, bruceConfig.bgColor);
    tft.drawRoundRect(6, canvas_top_h + 8, display_w - 12, canvas_bot_h - canvas_top_h - 16, 8, bruceConfig.priColor);
    tft.drawRoundRect(14, canvas_top_h + 15, display_w - 28, 20, 5, bruceConfig.priColor);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(broken ? "FALLA" : "ACTIVO", 22, canvas_top_h + 20);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("AMIGO WIFI", display_w - 22, canvas_top_h + 20);
    drawGotchiBody(canvas_center_x, canvas_top_h + 56, face, broken);
    drawGotchiMeter("senal", getPwngridClosestRssi() == -1000 ? 8 : constrain(100 + getPwngridClosestRssi(), 12, 90), 18, canvas_bot_h - 62, 86);
    drawGotchiMeter("animo", broken ? 18 : 78, display_w - 104, canvas_bot_h - 62, 86);
    tft.setTextDatum(BC_DATUM);
    tft.setTextSize(1);
    tft.drawPixel(0, 0, 0);
    drawCenteredParagraph(phrase, canvas_center_x, canvas_bot_h - 29, display_w > 280 ? 28 : 22);
}
#endif
