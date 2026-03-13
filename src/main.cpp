#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WebServer.h>

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

// 屏幕自动隐藏相关
bool screenHidden = false;
unsigned long lastActivityTime = 0;
const unsigned long SCREEN_TIMEOUT = 5 * 60 * 1000; // 5分钟

// 计算器相关
bool inCalculator = false;
double calculatorValue = 0;
double calculatorOperand = 0;
char calculatorOperator = ' ';
bool calculatorNewNumber = true;

// 卡片相关变量
#define CARD_COUNT 1
#define CARD_MARGIN 10
#define CARD_RADIUS 10
int currentPage = 0;

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

// 初始化网络服务器
void initWebServer() {
    server.on("/api/system", handleSystemData);
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



// 更新显示时间
void updateTimeDisplay() {
    if (timeClient != nullptr && ntpSynced && !screenHidden) {
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



// 绘制单个卡片
void drawCard(int cardIndex, int offsetX) {
    if (cardIndex == 0) {
        // 时间页面：全屏显示
        // 绘制时间，使用大字体
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(5); // 约50像素大小
        tft.setTextDatum(CC_DATUM);
        tft.drawString(currentTimeStr, SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2);
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
    if (screenHidden) {
        // 屏幕隐藏时，任何触摸都唤醒屏幕
        screenHidden = false;
        lastActivityTime = millis();
        drawCards();
        return;
    }
    
    // 触摸事件不再处理页面切换，因为只有一个时间页面
    lastActivityTime = millis(); // 更新活动时间
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
    // 检查屏幕是否需要自动隐藏
    if (!screenHidden && millis() - lastActivityTime >= SCREEN_TIMEOUT) {
        screenHidden = true;
        tft.fillScreen(TFT_BLACK); // 黑屏隐藏
    }
    
    // 更新时间显示（仅在屏幕未隐藏时）
    if (!screenHidden) {
        updateTimeDisplay();
    }
    
    // 处理触摸事件
    handleTouch();
    
    // 处理网络服务器请求
    server.handleClient();
    
    delay(10);
}
