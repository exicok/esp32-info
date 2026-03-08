#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include <TFT_eSPI.h>
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

// 全局变量
bool wifiConnected = false;
bool ntpSynced = false;
String currentTimeStr = "--:--:--";
unsigned long lastTimeUpdate = 0;

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
    showConnectionProgress("正在连接WiFi...", 10);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
        int progress = 10 + (attempts * 3);
        if (progress > 50) progress = 50;
        showConnectionProgress("正在连接WiFi...", progress);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        showConnectionProgress("WiFi已连接", 60);
        delay(500);
        return true;
    } else {
        showStatusMessage("WiFi连接失败", TFT_RED);
        delay(2000);
        return false;
    }
}

// 同步NTP时间
bool syncNTPTime() {
    showConnectionProgress("正在同步时间...", 70);
    
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
            showConnectionProgress("正在同步时间...", 75 + retry * 5);
            if (timeClient->update()) {
                ntpSynced = true;
                showConnectionProgress("时间同步成功", 100);
                delay(500);
                return true;
            }
            delay(500);
            retry++;
        }
    }
    
    showStatusMessage("时间同步失败", TFT_RED);
    delay(2000);
    return false;
}

// 初始化WiFi和时间同步
void initWiFiAndTime() {
    if (connectWiFi()) {
        syncNTPTime();
    }
}

// 绘制任务栏
void drawTaskbar() {
    // 绘制任务栏背景
    tft.fillRect(0, 0, SCREEN_WIDTH, TASKBAR_HEIGHT, TFT_DARKGREY);
    tft.drawFastHLine(0, TASKBAR_HEIGHT, SCREEN_WIDTH, TFT_LIGHTGREY);
    
  
    
    // 右侧显示时间
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawString(currentTimeStr, SCREEN_WIDTH - 5, 8);
}

// 更新显示时间
void updateTimeDisplay() {
    if (timeClient != nullptr && ntpSynced) {
        // 每秒更新一次显示
        if (millis() - lastTimeUpdate >= 1000) {
            timeClient->update();
            currentTimeStr = timeClient->getFormattedTime();
            drawTaskbar();
            lastTimeUpdate = millis();
        }
    }
}

// 初始化显示
void initDisplay() {
    // 清屏
    tft.fillScreen(TFT_BLACK);
    
    // 绘制任务栏
    drawTaskbar();
}

void setup() {
    Serial.begin(115200);
    
    // 初始化触摸屏
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSPI);
    touchscreen.setRotation(1);
    
    // 初始化TFT
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    
    // 连接WiFi并同步时间
    initWiFiAndTime();
    
    // 初始化主显示
    initDisplay();
}

void loop() {
    // 更新时间显示
    updateTimeDisplay();
    
    delay(100);
}
