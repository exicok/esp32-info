#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>

#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// WiFi配置
const char* ssid = "ChinaNet-GWpJ";
const char* password = "12345678";

// 国内NTP服务器
const char* ntpServer1 = "ntp.aliyun.com";
const char* ntpServer2 = "ntp1.aliyun.com";
const char* ntpServer3 = "cn.pool.ntp.org";

// 时区偏移 (北京时间 UTC+8)
const long utcOffsetInSeconds = 8 * 3600;

// TFT和触摸屏实例
TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

// 屏幕尺寸
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define TASKBAR_HEIGHT 30
#define FONT_SIZE 2

// NTP客户端
WiFiUDP ntpUDP;
NTPClient* timeClient = nullptr;

// 歌词广播监听
WiFiUDP lyricUDP;
unsigned int lyric_port = 330;
const unsigned long LYRIC_TIMEOUT = 30000; // 30秒无新歌词恢复时间显示

Preferences preferences;

// 全局变量
bool wifiConnected = false;
bool ntpSynced = false;
String currentTimeStr = "--:--:--";
unsigned long lastTimeUpdate = 0;
bool clockStyleFlip = false; // false: 现代, true: 物理闹钟/翻页样式

// 歌词显示状态
String currentLyric = "";
String currentTranslation = "";
uint16_t currentLyricColor = TFT_YELLOW;
int currentEspFontSize = 16;
float lyricProgress = 0;
float lyricDuration = 0;
bool lyricActive = false;
unsigned long lastLyricTime = 0;

// 屏幕自动隐藏相关
#define TFT_BL 21
bool screenHidden = false;
unsigned long lastActivityTime = 0;
const unsigned long SCREEN_TIMEOUT = 60 * 1000; // 1 分钟无歌词/无操作自动关屏电力保护

// 卡片相关变量
#define CARD_COUNT 6
#define CARD_MARGIN 10
#define CARD_RADIUS 10
int currentPage = 0;

// 滑动相关
int touchStartX = 0;
int touchEndX = 0;
bool isSliding = false;
const int SLIDE_THRESHOLD = 50;
unsigned long touchStartTime = 0;
const unsigned long LONG_PRESS_TIMEOUT = 3000; // 3秒长按熄屏
bool longPressTriggered = false;

// 网络服务器
WebServer server(80);

// 系统数据
String systemFps = "0";

// 卡片数据结构
struct CardData {
    const char* title;
    String content;
    uint16_t bgColor;
};

// 函数原型
void drawCards();

CardData cards[CARD_COUNT] = {
    {"", "--:--:--", TFT_BLACK}
};

// 显示连接状态提示
void showStatusMessage(const char* message, uint16_t color = TFT_WHITE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(message, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}

// 显示连接进度
void showConnectionProgress(const char* step, int progress) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(step, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 20);
    
    // 绘制进度条
    int barWidth = 200;
    int barHeight = 20;
    int barX = (SCREEN_WIDTH - barWidth) / 2;
    int barY = SCREEN_HEIGHT / 2 + 20;
    
    tft.drawRect(barX, barY, barWidth, barHeight, TFT_WHITE);
    int fillWidth = (barWidth * progress) / 100;
    tft.fillRect(barX + 2, barY + 2, fillWidth - 4, barHeight - 4, TFT_BLUE);
}

// 连接WiFi
bool connectWiFi() {
    showConnectionProgress("Connecting WiFi...", 10);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
        int progress = 10 + (attempts * 3);
        if (progress > 50) progress = 50;
        showConnectionProgress("Connecting WiFi...", progress);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        showConnectionProgress("WiFi Connected", 60);
        delay(500);
        return true;
    } else {
        showStatusMessage("WiFi Failed", TFT_RED);
        delay(2000);
        return false;
    }
}

// 同步NTP时间
bool syncNTPTime() {
    showConnectionProgress("Syncing Time...", 70);
    
    // 尝试多个国内NTP服务器
    const char* servers[] = {ntpServer1, ntpServer2, ntpServer3};
    
    for (int i = 0; i < 3; i++) {
        if (timeClient != nullptr) {
            delete timeClient;
        }
        
        timeClient = new NTPClient(ntpUDP, servers[i], utcOffsetInSeconds, 60000);
        timeClient->begin();
        
        int retry = 0;
        while (retry < 3) {
            showConnectionProgress("Syncing Time...", 75 + retry * 5);
            if (timeClient->update()) {
                ntpSynced = true;
                showConnectionProgress("Time Synced", 100);
                delay(500);
                return true;
            }
            delay(500);
            retry++;
        }
    }
    
    showStatusMessage("Time Sync Failed", TFT_RED);
    delay(2000);
    return false;
}

// 处理系统数据更新
void handleSystemData() {
    server.send(200, "text/plain", "OK");
}

// 处理设置歌词端口的请求
void handleSetLyricPort();

// 初始化网络服务器
void initWebServer() {
    server.on("/api/system", handleSystemData);
    server.on("/api/set_port", handleSetLyricPort);
    server.begin();
}

// 获取并更新IP地址
void updateIPAddress() {
    // 不再更新卡片内容，因为我们只有两个页面
    IPAddress ip = WiFi.localIP();
    String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
    Serial.println("IP Address: " + ipStr);
}

// 初始化WiFi和时间同步
void initWiFiAndTime() {
    if (connectWiFi()) {
        syncNTPTime();
        updateIPAddress();
        initWebServer();
    }
}



// 初始化歌词广播监听
void initLyricListener() {
    preferences.begin("lyric", true);
    lyric_port = preferences.getUInt("port", 330);
    preferences.end();

    lyricUDP.stop(); // 确保先停止旧的
    lyricUDP.begin(lyric_port);
    Serial.println("Lyric listener started on port " + String(lyric_port));
}

// 处理设置歌词端口的请求
void handleSetLyricPort() {
    if (server.hasArg("port")) {
        unsigned int newPort = server.arg("port").toInt();
        if (newPort > 0 && newPort < 65535) {
            preferences.begin("lyric", false);
            preferences.putUInt("port", newPort);
            preferences.end();

            lyric_port = newPort;
            initLyricListener();
            server.send(200, "text/plain", "Port set to " + String(newPort));
            return;
        }
    }
    server.send(400, "text/plain", "Invalid port");
}

// 将十六进制颜色字符串转换为 RGB565 (TFT_eSPI 格式)
uint16_t hexToRGB565(String hex) {
    if (hex.startsWith("#")) hex = hex.substring(1);
    long rgb = strtol(hex.c_str(), NULL, 16);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return tft.color565(r, g, b);
}

// 处理接收到的歌词广播
void handleLyricPacket() {
    int packetSize = lyricUDP.parsePacket();
    if (packetSize > 0) {
        char buffer[1024]; // 增大缓冲区
        int len = lyricUDP.read(buffer, sizeof(buffer) - 1);
        if (len >= 0) {
            buffer[len] = '\0';
            String rawData = String(buffer);

            // 数据包格式为: COLOR|FONT_SIZE|PROGRESS|DURATION|IS_PLAYING|HTTP_PORT|TITLE|ARTIST|TEXT
            int seps[8];
            int lastSep = -1;
            bool valid = true;
            for(int i=0; i<8; i++) {
                seps[i] = rawData.indexOf('|', lastSep + 1);
                if(seps[i] == -1) { valid = false; break; }
                lastSep = seps[i];
            }

            if (valid) {
                String colorHex = rawData.substring(0, seps[0]);
                currentEspFontSize = rawData.substring(seps[0] + 1, seps[1]).toInt();
                lyricProgress = rawData.substring(seps[1] + 1, seps[2]).toFloat();
                lyricDuration = rawData.substring(seps[2] + 1, seps[3]).toFloat();
                // IS_PLAYING: rawData.substring(seps[3] + 1, seps[4]);
                // HTTP_PORT: rawData.substring(seps[4] + 1, seps[5]);
                // TITLE: rawData.substring(seps[5] + 1, seps[6]);
                // ARTIST: rawData.substring(seps[6] + 1, seps[7]);
                String fullText = rawData.substring(seps[7] + 1);
                currentLyricColor = hexToRGB565(colorHex);

                int nIndex = fullText.indexOf('\n');
                if (nIndex != -1) {
                    currentLyric = fullText.substring(0, nIndex);
                    currentTranslation = fullText.substring(nIndex + 1);
                } else {
                    currentLyric = fullText;
                    currentTranslation = "";
                }

                // 收到有效数据，重置息屏计时器
                lastActivityTime = millis();
                if (screenHidden) {
                    screenHidden = false;
                    drawCards();
                }
            } else {
                // 降级处理
                currentLyric = rawData;
                currentLyricColor = TFT_YELLOW;
            }

            currentLyric.trim();

            if (currentLyric.length() > 0) {
                lyricActive = true;
                lastLyricTime = millis();
            } else {
                lyricActive = false;
            }

            // 收到数据后立即重绘
            if (currentPage == 0 && !screenHidden) {
                drawCards();
            }
        }
    }
}

// 检查歌词是否超时，超时后恢复时间显示
void checkLyricTimeout() {
    if (lyricActive && millis() - lastLyricTime >= LYRIC_TIMEOUT) {
        lyricActive = false;
        currentLyric = "";
        if (currentPage == 0 && !screenHidden) {
            drawCards();
        }
    }
}

// 更新显示时间
void updateTimeDisplay() {
    if (timeClient != nullptr && ntpSynced && !screenHidden && !lyricActive) {
        // 每秒更新一次显示
        if (millis() - lastTimeUpdate >= 1000) {
            timeClient->update();
            currentTimeStr = timeClient->getFormattedTime();
            
            // 重绘时间页面
            drawCards();
            
            lastTimeUpdate = millis();
        }
    }
}

// 绘制圆角矩形
void drawRoundedRect(int x, int y, int width, int height, int radius, uint16_t color) {
    tft.fillRoundRect(x, y, width, height, radius, color);
}

// 解方程功能
void drawEquationSolver(int offsetX) {
    tft.fillScreen(TFT_BLACK);
    
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Equation Solver", 10 + offsetX, 10);
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextDatum(CC_DATUM);
    tft.drawString("ax + b = c", SCREEN_WIDTH/2 + offsetX, 80);
    
    tft.drawString("Input a, b, c values", SCREEN_WIDTH/2 + offsetX, 120);
    tft.drawString("Touch to input", SCREEN_WIDTH/2 + offsetX, 150);
}



// 获取下一个 UTF-8 字符的字节长度
int utf8CharLen(unsigned char firstByte) {
    if ((firstByte & 0x80) == 0) return 1;
    if ((firstByte & 0xE0) == 0xC0) return 2;
    if ((firstByte & 0xF0) == 0xE0) return 3;
    if ((firstByte & 0xF8) == 0xF0) return 4;
    return 1;
}

// 计算按最大宽度换行后的行数
int countWrappedLines(String text, int maxW) {
    int lineCount = 0;
    String currentLine = "";
    int i = 0;
    while (i < text.length()) {
        int charLen = utf8CharLen(text[i]);
        String ch = text.substring(i, i + charLen);
        i += charLen;

        String testLine = currentLine + ch;
        if (u8f.getUTF8Width(testLine.c_str()) > maxW && currentLine.length() > 0) {
            lineCount++;
            currentLine = ch;
        } else {
            currentLine = testLine;
        }
    }
    if (currentLine.length() > 0) {
        lineCount++;
    }
    return lineCount;
}

// 居中绘制自动换行的 UTF-8 文本，返回实际行数
int drawWrappedTextCentered(String text, int centerX, int startY, int maxW, int lineHeight) {
    int lineCount = 0;
    String currentLine = "";
    int i = 0;
    while (i < text.length()) {
        int charLen = utf8CharLen(text[i]);
        String ch = text.substring(i, i + charLen);
        i += charLen;

        String testLine = currentLine + ch;
        if (u8f.getUTF8Width(testLine.c_str()) > maxW && currentLine.length() > 0) {
            int w = u8f.getUTF8Width(currentLine.c_str());
            u8f.setCursor(centerX - w / 2, startY + lineCount * lineHeight);
            u8f.print(currentLine);
            lineCount++;
            currentLine = ch;
        } else {
            currentLine = testLine;
        }
    }
    if (currentLine.length() > 0) {
        int w = u8f.getUTF8Width(currentLine.c_str());
        u8f.setCursor(centerX - w / 2, startY + lineCount * lineHeight);
        u8f.print(currentLine);
        lineCount++;
    }
    return lineCount;
}

// 绘制歌词原文和翻译（自动换行）
void drawLyricWithTranslation(String original, String translation, uint16_t color, int offsetX) {
    u8f.setFontMode(0);
    u8f.setBackgroundColor(TFT_BLACK);

    int maxW = SCREEN_WIDTH - 20;
    int centerX = SCREEN_WIDTH / 2 + offsetX;

    // 根据电脑端设置选择字号
    const uint8_t* originalFont = u8g2_font_wqy16_t_gb2312a;
    int originalLineHeight = 18;

    if (currentEspFontSize <= 12) { originalFont = u8g2_font_wqy12_t_gb2312a; originalLineHeight = 14; }
    else if (currentEspFontSize <= 14) { originalFont = u8g2_font_wqy14_t_gb2312a; originalLineHeight = 16; }
    else if (currentEspFontSize >= 18) { originalFont = u8g2_font_wqy16_t_gb2312a; originalLineHeight = 20; }

    if (translation.length() > 0) {
        // 有翻译：原文在上，翻译在下
        int translationLineHeight = 14;
        int gap = 8;

        u8f.setFont(originalFont);
        int originalLines = countWrappedLines(original, maxW);
        u8f.setFont(u8g2_font_wqy12_t_gb2312a);
        int translationLines = countWrappedLines(translation, maxW);

        int totalHeight = originalLines * originalLineHeight + gap + translationLines * translationLineHeight;
        int startY = SCREEN_HEIGHT / 2 - totalHeight / 2 + originalLineHeight - 5;

        u8f.setFont(originalFont);
        u8f.setForegroundColor(color);
        drawWrappedTextCentered(original, centerX, startY, maxW, originalLineHeight);

        int translationY = startY + originalLines * originalLineHeight + gap;
        u8f.setFont(u8g2_font_wqy12_t_gb2312a);
        u8f.setForegroundColor(tft.color565(180, 180, 180));
        drawWrappedTextCentered(translation, centerX, translationY, maxW, translationLineHeight);
    } else {
        // 无翻译时，单行或多行大字居中
        u8f.setFont(originalFont);
        int lines = countWrappedLines(original, maxW);
        int totalHeight = lines * originalLineHeight;
        int startY = SCREEN_HEIGHT / 2 - totalHeight / 2 + originalLineHeight;

        u8f.setForegroundColor(color);
        drawWrappedTextCentered(original, centerX, startY, maxW, originalLineHeight);
    }
}

// 绘制物理闹钟/翻页风格的数字
void drawFlipDigit(String val, int x, int y, int w, int h) {
    tft.fillRoundRect(x, y, w, h, 8, tft.color565(40, 40, 40));
    tft.drawFastHLine(x, y + h/2, w, TFT_BLACK); // 中间分界线
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(4);
    tft.setTextDatum(CC_DATUM);
    tft.drawString(val, x + w/2, y + h/2);
}

// 绘制物理闹钟风格时间
void drawFlipClockMinuteAndSecond(int mins, int secs, int startX, int startY, int cardW, int cardH, int gap);

void drawFlipClock(int offsetX) {
    if (timeClient == nullptr) return;
    int hours = timeClient->getHours();
    int mins = timeClient->getMinutes();
    int secs = timeClient->getSeconds();

    int startX = 20 + offsetX;
    int startY = 80;
    int cardW = 60;
    int cardH = 80;
    int gap = 10;

    drawFlipDigit(hours < 10 ? "0" + String(hours) : String(hours), startX, startY, cardW, cardH);
    tft.fillCircle(startX + cardW + gap/2, startY + cardH/3, 3, TFT_DARKGREY);
    tft.fillCircle(startX + cardW + gap/2, startY + 2*cardH/3, 3, TFT_DARKGREY);

    drawFlipClockMinuteAndSecond(mins, secs, startX, startY, cardW, cardH, gap);
}

void drawFlipClockMinuteAndSecond(int mins, int secs, int startX, int startY, int cardW, int cardH, int gap) {
    drawFlipDigit(mins < 10 ? "0" + String(mins) : String(mins), startX + cardW + gap, startY, cardW, cardH);
    tft.fillCircle(startX + 2*cardW + 1.5*gap, startY + cardH/3, 3, TFT_DARKGREY);
    tft.fillCircle(startX + 2*cardW + 1.5*gap, startY + 2*cardH/3, 3, TFT_DARKGREY);

    drawFlipDigit(secs < 10 ? "0" + String(secs) : String(secs), startX + 2*cardW + 2*gap, startY, cardW, cardH);
}

// 绘制单个卡片
void drawCard(int cardIndex, int offsetX) {
    switch (cardIndex) {
        case 0: // 第 0 页：时间显示 / 歌词显示
            if (lyricActive && currentLyric.length() > 0) {
                drawLyricWithTranslation(currentLyric, currentTranslation, currentLyricColor, offsetX);
            } else {
                if (clockStyleFlip) {
                    drawFlipClock(offsetX);
                } else {
                    // 正常时间显示
                    tft.setTextColor(TFT_WHITE, TFT_BLACK);
                    tft.setTextSize(5);
                    tft.setTextDatum(CC_DATUM);
                    tft.drawString(currentTimeStr, SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2);
                }
            }
            break;
            
        case 1: // 第 1 页：信息页
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setTextSize(3);
            tft.setTextDatum(CC_DATUM);
            tft.drawString("Info Page", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 - 30);
            tft.setTextSize(2);
            tft.drawString("ESP32 System", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 + 10);
            tft.drawString("WiFi: " + String(wifiConnected ? "Connected" : "Disconnected"), 
                          SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 + 30);
            tft.drawString("NTP: " + String(ntpSynced ? "Synced" : "Not Synced"), 
                          SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 + 50);
            break;
            
        case 2: // 第 2 页：方程求解器
            drawEquationSolver(offsetX);
            break;
            
        case 3: // 第 3 页：系统状态
            tft.setTextColor(TFT_CYAN, TFT_BLACK);
            tft.setTextSize(3);
            tft.setTextDatum(CC_DATUM);
            tft.drawString("System Status", SCREEN_WIDTH / 2 + offsetX, 60);
            tft.setTextSize(2);
            tft.drawString("FPS: " + systemFps, SCREEN_WIDTH / 2 + offsetX, 100);
            tft.drawString("Uptime: " + String(millis() / 1000) + "s", SCREEN_WIDTH / 2 + offsetX, 130);
            if (wifiConnected) {
                IPAddress ip = WiFi.localIP();
                String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + 
                              String(ip[2]) + "." + String(ip[3]);
                tft.drawString("IP: " + ipStr, SCREEN_WIDTH / 2 + offsetX, 160);
            }
            break;
            
        case 4: // 第 4 页：设置
            tft.setTextColor(TFT_ORANGE, TFT_BLACK);
            tft.setTextSize(3);
            tft.setTextDatum(CC_DATUM);
            tft.drawString("Settings", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 - 20);
            tft.setTextSize(2);
            tft.drawString("Touch to configure", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 + 20);
            break;
            
        case 5: // 第 5 页：关于
            tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
            tft.setTextSize(3);
            tft.setTextDatum(CC_DATUM);
            tft.drawString("About", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 - 20);
            tft.setTextSize(2);
            tft.drawString("ESP32 Info System", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 + 10);
            tft.drawString("Version 1.0", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 + 30);
            break;
    }
}

// 绘制所有卡片（带滑动偏移）
void drawCards() {
    // 清除整个屏幕
    tft.fillScreen(TFT_BLACK);
    
    // 只绘制当前页面
    drawCard(currentPage, 0);
}

// 初始化显示
void initDisplay() {
    // 清屏
    tft.fillScreen(TFT_BLACK);
    
    // 绘制卡片
    drawCards();
    
    // 初始化活动时间
    lastActivityTime = millis();
}

// 处理触摸事件
void handleTouch() {
    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();

        if (screenHidden) {
            // 屏幕隐藏时，任何触摸都唤醒屏幕
            screenHidden = false;
            digitalWrite(TFT_BL, HIGH); // 唤醒背光
            lastActivityTime = millis();
            drawCards();
            // 唤醒后重置长按状态，防止立即又熄屏
            touchStartTime = millis();
            longPressTriggered = true;
            return;
        }

        if (!isSliding) {
            // 开始触摸
            isSliding = true;
            touchStartX = p.x;
            touchEndX = p.x;
            touchStartTime = millis();
            longPressTriggered = false;
        } else {
            // 持续触摸
            touchEndX = p.x;

            // 检查长按熄屏逻辑
            if (!longPressTriggered && (millis() - touchStartTime >= LONG_PRESS_TIMEOUT)) {
                screenHidden = true;
                digitalWrite(TFT_BL, LOW); // 关闭背光
                longPressTriggered = true; // 标记已触发，防止重复
                isSliding = false;
                return;
            }
        }
        
        lastActivityTime = millis(); // 更新活动时间
    } else if (isSliding) {
        // 触摸结束
        if (!longPressTriggered) {
            // 处理点击切换时钟风格 (第0页)
            if (currentPage == 0 && !lyricActive && abs(touchEndX - touchStartX) < 10) {
                 // 这里简化了点击判断
                 static unsigned long lastFlipToggle = 0;
                 if (millis() - lastFlipToggle > 500) {
                     clockStyleFlip = !clockStyleFlip;
                     drawCards();
                     lastFlipToggle = millis();
                 }
            }

            // 处理滑动切换页面
            int diffX = touchEndX - touchStartX;
            if (abs(diffX) > SLIDE_THRESHOLD) {
                if (diffX > 0) {
                    if (currentPage > 0) currentPage--;
                } else {
                    if (currentPage < CARD_COUNT - 1) currentPage++;
                }
                drawCards();
            }
        }
        
        isSliding = false;
        longPressTriggered = false;
    }
}

void setup() {
    Serial.begin(115200);

    // 初始化背光引脚
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // 初始化触摸屏
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSPI);
    touchscreen.setRotation(1);
    
    // 初始化TFT
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    // 初始化中文支持
    u8f.begin(tft);

    // 连接WiFi并同步时间
    initWiFiAndTime();

    // 初始化歌词广播监听
    initLyricListener();

    // 初始化主显示
    initDisplay();
}

void loop() {
    // 处理歌词广播包
    handleLyricPacket();

    // 检查歌词是否超时
    checkLyricTimeout();

    // 更新时间显示（仅在屏幕未隐藏且未显示歌词时）
    if (!screenHidden) {
        updateTimeDisplay();
    }

    // 处理触摸事件
    handleTouch();

    // 处理网络服务器请求
    server.handleClient();

    delay(10);
}
