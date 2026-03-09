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

// 第二页滚动相关
int page2ScrollY = 0;
bool isPage2Scrolling = false;
int page2TouchStartY = 0;
#define PAGE2_CONTENT_HEIGHT 280  // 第二页内容总高度
#define PAGE2_VISIBLE_HEIGHT (SCREEN_HEIGHT - TASKBAR_HEIGHT)  // 可见区域高度

// 网络服务器
WebServer server(80);

// 系统数据
String systemFps = "0";

// 第二页的4个数据项
String page2Data[4] = {"0%", "GPU: 0%\nGPU Fan: 0 RPM", "0%", "None"};

// 第三页网络信息
String networkName = "Unknown";
String pcIP = "0.0.0.0";
String wifiDevices = "0";

// 卡片数据结构
struct CardData {
    const char* title;
    String content;
    uint16_t bgColor;
};

// 函数原型
void drawCards();

CardData cards[CARD_COUNT] = {
    {"FPS", "0", TFT_BLACK},
    {"System Info", "", TFT_BLACK},
    {"Network", "", TFT_BLACK},
    {"Card 4", "Content", TFT_BLACK}
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
    if (server.hasArg("fps")) {
        systemFps = server.arg("fps");
        cards[0].content = systemFps;
    }
    
    // 处理第二页的4个数据项
    if (server.hasArg("data1")) {
        page2Data[0] = server.arg("data1");
    }
    if (server.hasArg("data2")) {
        page2Data[1] = server.arg("data2");
    }
    if (server.hasArg("data3")) {
        page2Data[2] = server.arg("data3");
    }
    if (server.hasArg("data4")) {
        page2Data[3] = server.arg("data4");
    }
    
    // 处理第三页网络信息
    if (server.hasArg("network")) {
        networkName = server.arg("network");
    }
    if (server.hasArg("pcip")) {
        pcIP = server.arg("pcip");
    }
    if (server.hasArg("devices")) {
        wifiDevices = server.arg("devices");
    }
    
    server.send(200, "text/plain", "OK");
    
    // 重新绘制当前卡片
    if (currentPage == 0 || currentPage == 1 || currentPage == 2) {
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

// 绘制第二页的4个卡片（支持滚动）
void drawPage2() {
    int cardHeight = 48;
    int cardWidth = SCREEN_WIDTH - 2 * CARD_MARGIN;
    int cardSpacing = 5;
    int startY = TASKBAR_HEIGHT + 10 - page2ScrollY;  // 应用滚动偏移
    
    // 卡片标题（英文）
    const char* cardTitles[4] = {"CPU", "GPU", "Memory", "Window"};
    
    for (int i = 0; i < 4; i++) {
        int y = startY + i * (cardHeight + cardSpacing);
        
        // 只绘制在可见区域内的卡片
        if (y + cardHeight < TASKBAR_HEIGHT || y > SCREEN_HEIGHT) {
            continue;
        }
        
        // 绘制卡片背景
        tft.fillRoundRect(CARD_MARGIN, y, cardWidth, cardHeight, 5, TFT_DARKGREY);
        
        // 绘制卡片标题（靠左对齐）
        tft.setTextColor(TFT_CYAN, TFT_DARKGREY);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(cardTitles[i], CARD_MARGIN + 8, y + 5);
        
        // 绘制卡片内容（靠左对齐）
        tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
        tft.setTextSize(2);
        tft.setTextDatum(TL_DATUM);
        
        // 处理多行内容
        if (i == 1) {
            // GPU卡片：显示GPU和风扇信息
            String gpuLine = page2Data[1].substring(0, page2Data[1].indexOf('\n'));
            String fanLine = page2Data[1].substring(page2Data[1].indexOf('\n') + 1);
            tft.drawString(gpuLine, CARD_MARGIN + 8, y + cardHeight - 25);
            tft.setTextSize(1);
            tft.drawString(fanLine, CARD_MARGIN + 8, y + cardHeight - 10);
        } else if (i == 3) {
            // Window卡片：2行显示，确保不超出屏幕
            String windowText = page2Data[3];
            int textLen = windowText.length();
            int maxCharsPerLine = 14; // 每行最多字符数（根据屏幕宽度调整）
            
            if (textLen <= maxCharsPerLine) {
                // 短文本，单行显示
                tft.drawString(windowText, CARD_MARGIN + 8, y + cardHeight - 18);
            } else {
                // 长文本，分两行显示
                String line1 = windowText.substring(0, maxCharsPerLine);
                String line2 = windowText.substring(maxCharsPerLine);
                
                // 如果第二行太长，截断并添加省略号
                if (line2.length() > maxCharsPerLine) {
                    line2 = line2.substring(0, maxCharsPerLine - 3) + "...";
                }
                
                tft.setTextSize(1);
                tft.drawString(line1, CARD_MARGIN + 8, y + cardHeight - 25);
                tft.drawString(line2, CARD_MARGIN + 8, y + cardHeight - 12);
            }
        } else {
            tft.drawString(page2Data[i], CARD_MARGIN + 8, y + cardHeight - 18);
        }
    }
    
    // 绘制滚动指示器
    if (PAGE2_CONTENT_HEIGHT > PAGE2_VISIBLE_HEIGHT) {
        int scrollbarHeight = (PAGE2_VISIBLE_HEIGHT * PAGE2_VISIBLE_HEIGHT) / PAGE2_CONTENT_HEIGHT;
        int scrollbarY = TASKBAR_HEIGHT + (page2ScrollY * (PAGE2_VISIBLE_HEIGHT - scrollbarHeight)) / (PAGE2_CONTENT_HEIGHT - PAGE2_VISIBLE_HEIGHT);
        tft.fillRect(SCREEN_WIDTH - 4, scrollbarY, 3, scrollbarHeight, TFT_LIGHTGREY);
    }
}

// 绘制第三页网络信息
void drawPage3() {
    int cardHeight = 55;
    int cardWidth = SCREEN_WIDTH - 2 * CARD_MARGIN;
    int cardSpacing = 8;
    int startY = TASKBAR_HEIGHT + 15;
    
    // 网络信息卡片数据
    const char* titles[3] = {"Network Name", "PC IP Address", "WiFi Devices"};
    String values[3] = {networkName, pcIP, wifiDevices};
    
    for (int i = 0; i < 3; i++) {
        int y = startY + i * (cardHeight + cardSpacing);
        
        // 绘制卡片背景
        tft.fillRoundRect(CARD_MARGIN, y, cardWidth, cardHeight, 5, TFT_DARKGREY);
        
        // 绘制卡片标题
        tft.setTextColor(TFT_CYAN, TFT_DARKGREY);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(titles[i], CARD_MARGIN + 8, y + 6);
        
        // 绘制卡片内容
        tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
        tft.setTextSize(2);
        tft.setTextDatum(TL_DATUM);
        
        // 截断文本以适应卡片宽度
        String value = values[i];
        if (value.length() > 16) {
            value = value.substring(0, 13) + "...";
        }
        tft.drawString(value, CARD_MARGIN + 8, y + cardHeight - 20);
    }
}

// 绘制单个卡片
void drawCard(int cardIndex, int offsetX) {
    int cardWidth = SCREEN_WIDTH - 2 * CARD_MARGIN;
    int cardHeight = SCREEN_HEIGHT - TASKBAR_HEIGHT - 2 * CARD_MARGIN;
    int cardX = CARD_MARGIN + offsetX;
    int cardY = TASKBAR_HEIGHT + CARD_MARGIN;
    
    if (cardIndex == 0) {
        // FPS页面：全屏显示
        // 绘制标题
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(2);
        tft.setTextDatum(TC_DATUM);
        tft.drawString(cards[cardIndex].title, SCREEN_WIDTH / 2 + offsetX, 50);
        
        // 根据FPS值设置颜色
        int fps = cards[cardIndex].content.toInt();
        if (fps < 30) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
        } else if (fps < 60) {
            tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        } else {
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
        }
        
        // 绘制FPS数值，使用大字体
        tft.setTextSize(5); // 约50像素大小
        tft.setTextDatum(CC_DATUM);
        tft.drawString(cards[cardIndex].content, SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2);
    } else if (cardIndex == 1) {
        // 第二页：显示4个元素（支持滚动）
        drawPage2();
    } else if (cardIndex == 2) {
        // 第三页：显示网络信息
        drawPage3();
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
            
            // 检查是否在第二页，初始化垂直滚动
            if (currentPage == 1) {
                isPage2Scrolling = true;
                page2TouchStartY = touchY;
            }
        } else {
            // 触摸移动中 - 处理第二页垂直滚动
            if (currentPage == 1 && isPage2Scrolling) {
                int deltaY = page2TouchStartY - touchY;
                if (abs(deltaY) > 10) {  // 最小滚动阈值
                    page2ScrollY += deltaY / 2;  // 滚动速度减半，更平滑
                    // 限制滚动范围
                    if (page2ScrollY < 0) page2ScrollY = 0;
                    if (page2ScrollY > PAGE2_CONTENT_HEIGHT - PAGE2_VISIBLE_HEIGHT) {
                        page2ScrollY = PAGE2_CONTENT_HEIGHT - PAGE2_VISIBLE_HEIGHT;
                    }
                    page2TouchStartY = touchY;
                    drawCards();  // 重绘页面
                }
            }
        }
    } else {
        if (isScrolling) {
            // 触摸结束
            int swipeDistance = touchStartX - map(touchscreen.getPoint().x, 200, 3700, 1, SCREEN_WIDTH);
            
            // 如果不是垂直滚动，处理水平翻页
            if (currentPage != 1 || !isPage2Scrolling || abs(swipeDistance) > 50) {
                // 判断是否切换页面
                if (swipeDistance > 50 && currentPage < CARD_COUNT - 1) {
                    currentPage++;
                    page2ScrollY = 0;  // 重置第二页滚动位置
                } else if (swipeDistance < -50 && currentPage > 0) {
                    currentPage--;
                    page2ScrollY = 0;  // 重置第二页滚动位置
                }
            }
            
            isScrolling = false;
            isPage2Scrolling = false;
            
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
