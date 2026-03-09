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

// 卡片相关变量
#define CARD_COUNT 4
#define CARD_MARGIN 10
#define CARD_RADIUS 10
int currentPage = 0;
bool isScrolling = false;
int touchStartX = 0;
int touchStartY = 0;

// 网络服务器
WebServer server(80);

// 系统数据
String systemFps = "0 FPS";
String systemCpu = "CPU: 0%";
String systemGpu = "GPU: 0%";
String systemMem = "内存: 0%";

// 卡片数据结构
struct CardData {
    const char* title;
    String content;
    uint16_t bgColor;
};

// 函数原型
void drawCards();

CardData cards[CARD_COUNT] = {
    {"FPS", "0 FPS", TFT_BLACK},
    {"系统资源", "CPU: 0%\nGPU: 0%\n内存: 0%", TFT_BLACK},
    {"IP地址", "", TFT_BLACK},
    {"卡片4", "这是第四个卡片的内容", TFT_BLACK}
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

// 处理系统数据更新
void handleSystemData() {
    if (server.hasArg("fps")) {
        systemFps = server.arg("fps");
        cards[0].content = systemFps;
    }
    if (server.hasArg("cpu")) {
        systemCpu = "CPU: " + server.arg("cpu") + "%";
    }
    if (server.hasArg("gpu")) {
        systemGpu = "GPU: " + server.arg("gpu") + "%";
    }
    if (server.hasArg("mem")) {
        systemMem = "内存: " + server.arg("mem") + "%";
    }
    cards[1].content = systemCpu + "\n" + systemGpu + "\n" + systemMem;
    
    server.send(200, "text/plain", "OK");
    
    // 重新绘制当前卡片
    if (currentPage == 0 || currentPage == 1) {
        drawCards();
    }
}

// 初始化网络服务器
void initWebServer() {
    server.on("/api/system", handleSystemData);
    server.begin();
}

// 获取并更新IP地址
void updateIPAddress() {
    IPAddress ip = WiFi.localIP();
    String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
    cards[2].content = ipStr;
    if (currentPage == 2) {
        drawCards();
    }
}

// 初始化WiFi和时间同步
void initWiFiAndTime() {
    if (connectWiFi()) {
        syncNTPTime();
        updateIPAddress();
        initWebServer();
    }
}

// 绘制任务栏
void drawTaskbar() {
    // 只有在非FPS页面时显示任务栏
    if (currentPage != 0) {
        // 绘制任务栏背景
        tft.fillRect(0, 0, SCREEN_WIDTH, TASKBAR_HEIGHT, TFT_DARKGREY);
        tft.drawFastHLine(0, TASKBAR_HEIGHT, SCREEN_WIDTH, TFT_LIGHTGREY);
        
        // 右侧显示时间
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
        tft.drawString(currentTimeStr, SCREEN_WIDTH - 5, 8);
    }
}

// 更新显示时间
void updateTimeDisplay() {
    if (timeClient != nullptr && ntpSynced && currentPage != 0) {
        // 每秒更新一次显示
        if (millis() - lastTimeUpdate >= 1000) {
            timeClient->update();
            currentTimeStr = timeClient->getFormattedTime();
            
            // 只重绘任务栏区域
            tft.fillRect(0, 0, SCREEN_WIDTH, TASKBAR_HEIGHT, TFT_DARKGREY);
            tft.drawFastHLine(0, TASKBAR_HEIGHT, SCREEN_WIDTH, TFT_LIGHTGREY);
            tft.setTextDatum(TR_DATUM);
            tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
            tft.drawString(currentTimeStr, SCREEN_WIDTH - 5, 8);
            
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
    int cardWidth = SCREEN_WIDTH - 2 * CARD_MARGIN;
    int cardHeight = SCREEN_HEIGHT - TASKBAR_HEIGHT - 2 * CARD_MARGIN;
    int cardX = CARD_MARGIN + offsetX;
    int cardY = TASKBAR_HEIGHT + CARD_MARGIN;
    
    if (cardIndex == 0) {
        // FPS页面：全屏显示
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        // 绘制标题
        tft.setTextSize(2);
        tft.setTextDatum(TC_DATUM);
        tft.drawString(cards[cardIndex].title, SCREEN_WIDTH / 2 + offsetX, 50);
        // 绘制FPS数值，使用大字体
        tft.setTextSize(5); // 约50像素大小
        tft.setTextDatum(CC_DATUM);
        tft.drawString(cards[cardIndex].content, SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2);
    } else {
        // 其他页面：带任务栏
        // 绘制卡片标题
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(2);
        tft.setTextDatum(TC_DATUM);
        tft.drawString(cards[cardIndex].title, SCREEN_WIDTH / 2 + offsetX, cardY + 30);
        
        // 绘制卡片内容
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.drawString(cards[cardIndex].content, SCREEN_WIDTH / 2 + offsetX, cardY + 70);
    }
}

// 绘制所有卡片（带滑动偏移）
void drawCards() {
    if (currentPage == 0) {
        // FPS页面：清除整个屏幕
        tft.fillScreen(TFT_BLACK);
    } else {
        // 其他页面：清除内容区域（任务栏以下）
        tft.fillRect(0, TASKBAR_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - TASKBAR_HEIGHT, TFT_BLACK);
        // 绘制任务栏
        drawTaskbar();
    }
    
    // 只绘制当前页面
    drawCard(currentPage, 0);
}

// 初始化显示
void initDisplay() {
    // 清屏
    tft.fillScreen(TFT_BLACK);
    
    // 绘制任务栏
    drawTaskbar();
    
    // 绘制卡片
    drawCards();
}

// 处理触摸事件
void handleTouch() {
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        int touchX = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
        int touchY = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
        
        if (!isScrolling) {
            // 开始触摸
            isScrolling = true;
            touchStartX = touchX;
            touchStartY = touchY;
        }
    } else {
        if (isScrolling) {
            // 触摸结束
            isScrolling = false;
            
            // 计算滑动距离
            int swipeDistance = touchStartX - map(touchscreen.getPoint().x, 200, 3700, 1, SCREEN_WIDTH);
            
            // 判断是否切换页面
            if (swipeDistance > 50 && currentPage < CARD_COUNT - 1) {
                currentPage++;
            } else if (swipeDistance < -50 && currentPage > 0) {
                currentPage--;
            }
            
            // 重绘卡片
            drawCards();
        }
    }
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
    
    // 处理触摸事件
    handleTouch();
    
    // 处理网络服务器请求
    server.handleClient();
    
    delay(10);
}
