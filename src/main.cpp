#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// WiFi配置
const char* ssid = "ChinaNet-GWpJ";
const char* password = "12345678";

TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;

// 触摸屏引脚
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define TFT_BL 21

WiFiUDP ntpUDP;
NTPClient* timeClient = nullptr;
WiFiUDP lyricUDP;
unsigned int lyric_port = 330;

// 状态变量
String currentLyric = "";
String currentTranslation = "";
uint16_t currentLyricColor = TFT_YELLOW;
String currentTimeStr = "00:00:00";
String currentDateStr = "2024-01-01";
bool lyricActive = false;
bool screenHidden = false;
unsigned long lastLyricTime = 0;
unsigned long lastTimeUpdate = 0;

int currentPage = 0;
#define MAX_PAGES 2

// 触控手势逻辑
int startX = -1, startY = -1;
int scrollOffset = 0;
bool isSwiping = false;
const int SWIPE_MIN_X = 800; // 左右滑动触发阈值 (原始坐标)
const int SWIPE_MIN_Y = 300; // 上下滑动触发阈值 (用于翻页)

uint16_t hexTo565(String hex) {
    if (hex.startsWith("#")) hex = hex.substring(1);
    long rgb = strtol(hex.c_str(), NULL, 16);
    return tft.color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

int getUtf8CharLen(unsigned char firstByte) {
    if ((firstByte & 0x80) == 0) return 1;
    if ((firstByte & 0xE0) == 0xC0) return 2;
    if ((firstByte & 0xF0) == 0xE0) return 3;
    if ((firstByte & 0xF8) == 0xF0) return 4;
    return 1;
}

int drawWrappedTextCentered(String text, int centerX, int startY, int maxW, int lineHeight) {
    int lineCount = 0;
    String currentLine = "";
    int i = 0;
    while (i < (int)text.length()) {
        int charLen = getUtf8CharLen(text[i]);
        String ch = text.substring(i, i + charLen);
        i += charLen;
        String testLine = currentLine + ch;
        if (u8f.getUTF8Width(testLine.c_str()) > maxW && currentLine.length() > 0) {
            u8f.setCursor(centerX - u8f.getUTF8Width(currentLine.c_str()) / 2, startY + lineCount * lineHeight);
            u8f.print(currentLine);
            lineCount++;
            currentLine = ch;
        } else {
            currentLine = testLine;
        }
    }
    if (currentLine.length() > 0) {
        u8f.setCursor(centerX - u8f.getUTF8Width(currentLine.c_str()) / 2, startY + lineCount * lineHeight);
        u8f.print(currentLine);
        lineCount++;
    }
    return lineCount;
}

void drawTaskbar() {
    tft.fillRect(0, 0, SCREEN_WIDTH, 24, tft.color565(40, 40, 40));
    u8f.setFontMode(1);
    u8f.setFont(u8g2_font_wqy12_t_gb2312);
    u8f.setForegroundColor(TFT_WHITE);
    u8f.setCursor(8, 18);
    u8f.print(currentDateStr);
    int timeW = u8f.getUTF8Width(currentTimeStr.c_str());
    u8f.setCursor(SCREEN_WIDTH - timeW - 8, 18);
    u8f.print(currentTimeStr);
}

void drawInfoContent() {
    int curY = 50 + scrollOffset;
    int lineHeight = 25;
    u8f.setFont(u8g2_font_wqy12_t_gb2312);

    auto drawRow = [&](const char* label, String val, uint16_t color) {
        if (curY > 24 && curY < SCREEN_HEIGHT + 20) {
            u8f.setForegroundColor(TFT_CYAN);
            u8f.setCursor(10, curY); u8f.print(label);
            u8f.setForegroundColor(color);
            u8f.setCursor(100, curY); u8f.print(val);
        }
        curY += lineHeight;
    };

    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 网络状态 ---"); curY += lineHeight;
    drawRow("网络名称:", String(WiFi.SSID()), TFT_WHITE);
    drawRow("IP 地址:", WiFi.localIP().toString(), TFT_GREEN);
    drawRow("信号强度:", String(WiFi.RSSI()) + " dBm", TFT_WHITE);
    drawRow("网关地址:", WiFi.gatewayIP().toString(), TFT_WHITE);

    curY += 10;
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 硬件信息 ---"); curY += lineHeight;
    drawRow("CPU 频率:", String(ESP.getCpuFreqMHz()) + " MHz", TFT_WHITE);
    drawRow("可用内存:", String(ESP.getFreeHeap()/1024) + " KB", TFT_GREEN);
    drawRow("Flash大小:", String(ESP.getFlashChipSize()/1024/1024) + " MB", TFT_WHITE);
    drawRow("系统版本:", String(ESP.getSdkVersion()), TFT_WHITE);
}

void drawDisplay() {
    if (currentPage == 0) {
        tft.fillScreen(TFT_BLACK);
        if (lyricActive && currentLyric.length() > 0) {
            u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
            int lyricLines = drawWrappedTextCentered(currentLyric, 160, 100, SCREEN_WIDTH - 20, 22);
            if (currentTranslation.length() > 0) {
                u8f.setFont(u8g2_font_wqy12_t_gb2312); u8f.setForegroundColor(TFT_LIGHTGREY);
                drawWrappedTextCentered(currentTranslation, 160, 100 + lyricLines * 22 + 8, SCREEN_WIDTH - 20, 18);
            }
        } else {
            tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(5);
            tft.setTextDatum(CC_DATUM); tft.drawString(currentTimeStr, 160, 120);
        }
    } else {
        tft.fillScreen(TFT_BLACK);
        drawInfoContent();
        drawTaskbar();
    }
}

void handleLyricPacket() {
    int packetSize = lyricUDP.parsePacket();
    if (packetSize > 0) {
        char buffer[1024]; int len = lyricUDP.read(buffer, 1023);
        if (len > 0) {
            buffer[len] = '\0'; String data = String(buffer);
            int seps[8]; int lastSep = -1; int sc = 0;
            for(int i=0; i<8; i++) { int p = data.indexOf('|', lastSep+1); if(p == -1) break; seps[sc++] = p; lastSep = p; }
            if (sc >= 8) {
                currentLyricColor = hexTo565(data.substring(0, seps[0]));
                String fullText = data.substring(seps[7] + 1);
                int nIdx = fullText.indexOf('\n');
                if (nIdx != -1) { currentLyric = fullText.substring(0, nIdx); currentTranslation = fullText.substring(nIdx+1); }
                else { currentLyric = fullText; currentTranslation = ""; }
                lyricActive = (currentLyric.length() > 0);
                lastLyricTime = millis();
                if (screenHidden) { screenHidden = false; digitalWrite(TFT_BL, HIGH); }
                if (currentPage == 0) drawDisplay();
            }
        }
    }
}

void setup() {
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI);
    touchscreen.begin(); touchscreen.setRotation(1);
    tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK); u8f.begin(tft);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(500);
    timeClient = new NTPClient(ntpUDP, "ntp.aliyun.com", 8 * 3600, 60000);
    timeClient->begin(); lyricUDP.begin(lyric_port);
    drawDisplay();
}

void loop() {
    handleLyricPacket();

    if (millis() - lastTimeUpdate >= 1000) {
        timeClient->update();
        currentTimeStr = timeClient->getFormattedTime();
        unsigned long epochTime = timeClient->getEpochTime();
        struct tm* ptm = gmtime((time_t*)&epochTime);
        char dateBuffer[20]; sprintf(dateBuffer, "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
        currentDateStr = String(dateBuffer);
        if (currentPage == 1 || !lyricActive) drawDisplay();
        lastTimeUpdate = millis();
    }

    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        if (screenHidden) {
            screenHidden = false; digitalWrite(TFT_BL, HIGH); drawDisplay();
        } else {
            if (startX == -1) { startX = p.x; startY = p.y; }
            else {
                int dx = p.x - startX;
                int dy = p.y - startY;

                if (currentPage == 1 && abs(dy) > abs(dx) && abs(dy) > 200) {
                    // 信息页纵向滑动：实时滚动
                    scrollOffset += dy / 20;
                    scrollOffset = constrain(scrollOffset, -200, 0);
                    drawDisplay();
                    startY = p.y; // 更新起始点以平滑滚动
                }
            }
        }
    } else {
        if (startX != -1) {
            TS_Point p = touchscreen.getPoint(); // 实际上此时touched为false，但我们需要最后的坐标，XPT2046通常会保留
            // 处理横向滑动切换页面
            int finalDx = touchscreen.getPoint().x - startX;
            if (finalDx > SWIPE_MIN_X) { // 向右划：上一页
                currentPage = (currentPage - 1 + MAX_PAGES) % MAX_PAGES;
                scrollOffset = 0;
                drawDisplay();
            } else if (finalDx < -SWIPE_MIN_X) { // 向左划：下一页
                currentPage = (currentPage + 1) % MAX_PAGES;
                scrollOffset = 0;
                drawDisplay();
            }
            startX = -1; startY = -1;
        }
    }

    if (lyricActive && millis() - lastLyricTime > 15000) {
        lyricActive = false; drawDisplay();
    }
    delay(10);
}
