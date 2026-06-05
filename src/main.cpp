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
const unsigned long SCREEN_TIMEOUT = 5 * 60 * 1000; // 5 分钟

// 计算器相关
bool inCalculator = false;
double calculatorValue = 0;
double calculatorOperand = 0;
char calculatorOperator = ' ';
bool calculatorNewNumber = true;
String calculatorDisplay = "0";
String equationResult = "";
bool inEquationMode = false;

// 卡片相关变量
#define CARD_COUNT 7
#define CARD_MARGIN 10
#define CARD_RADIUS 10
int currentPage = 0;

// 滑动相关
int touchStartX = 0;
int touchEndX = 0;
bool isSliding = false;
const int SLIDE_THRESHOLD = 50;

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

// 计算器按钮定义
#define CALC_BUTTON_COLS 4
#define CALC_BUTTON_ROWS 5
#define CALC_BUTTON_WIDTH 60
#define CALC_BUTTON_HEIGHT 35
#define CALC_BUTTON_SPACING 5
#define CALC_START_X 20
#define CALC_START_Y 80

const char* calcButtons[CALC_BUTTON_ROWS][CALC_BUTTON_COLS] = {
    {"7", "8", "9", "/"},
    {"4", "5", "6", "*"},
    {"1", "2", "3", "-"},
    {"0", ".", "=", "+"},
    {"C", "EQ", "←", ""}
};

// 绘制计算器界面
void drawCalculator(int offsetX) {
    tft.fillScreen(TFT_BLACK);
    
    // 绘制标题
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Calculator", 10 + offsetX, 10);
    
    // 绘制显示屏
    drawRoundedRect(10 + offsetX, 40, 300, 35, 5, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(3);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(calculatorDisplay, 300 + offsetX, 58);
    
    // 绘制方程结果
    if (equationResult != "") {
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(2);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(equationResult, 10 + offsetX, 80);
    }
    
    // 绘制按钮
    for (int row = 0; row < CALC_BUTTON_ROWS; row++) {
        for (int col = 0; col < CALC_BUTTON_COLS; col++) {
            if (calcButtons[row][col][0] != '\0') {
                int x = CALC_START_X + offsetX + col * (CALC_BUTTON_WIDTH + CALC_BUTTON_SPACING);
                int y = CALC_START_Y + row * (CALC_BUTTON_HEIGHT + CALC_BUTTON_SPACING);
                
                uint16_t btnColor = TFT_BLUE;
                if (row == 4 && col == 0) btnColor = TFT_RED;      // C
                else if (row == 4 && col == 1) btnColor = TFT_GREEN; // EQ
                else if (row == 4 && col == 2) btnColor = TFT_ORANGE; // ←
                else if (col == 3) btnColor = TFT_PURPLE;          // 运算符
                
                drawRoundedRect(x, y, CALC_BUTTON_WIDTH, CALC_BUTTON_HEIGHT, 5, btnColor);
                
                tft.setTextColor(TFT_WHITE, btnColor);
                tft.setTextSize(2);
                tft.setTextDatum(CC_DATUM);
                tft.drawString(calcButtons[row][col], x + CALC_BUTTON_WIDTH/2, y + CALC_BUTTON_HEIGHT/2);
            }
        }
    }
}

// 处理计算器触摸
void handleCalculatorTouch(int touchX, int touchY) {
    // 检查是否点击了按钮
    for (int row = 0; row < CALC_BUTTON_ROWS; row++) {
        for (int col = 0; col < CALC_BUTTON_COLS; col++) {
            if (calcButtons[row][col][0] != '\0') {
                int x = CALC_START_X + col * (CALC_BUTTON_WIDTH + CALC_BUTTON_SPACING);
                int y = CALC_START_Y + row * (CALC_BUTTON_HEIGHT + CALC_BUTTON_SPACING);
                
                if (touchX >= x && touchX <= x + CALC_BUTTON_WIDTH &&
                    touchY >= y && touchY <= y + CALC_BUTTON_HEIGHT) {
                    
                    String button = calcButtons[row][col];
                    
                    if (button == "C") {
                        // 清空
                        calculatorValue = 0;
                        calculatorOperand = 0;
                        calculatorOperator = ' ';
                        calculatorNewNumber = true;
                        calculatorDisplay = "0";
                        equationResult = "";
                    } else if (button == "=" || button == "EQ") {
                        // 计算结果
                        if (calculatorOperator != ' ') {
                            // 从显示读取操作数并计算
                            calculatorOperand = calculatorDisplay.toDouble();
                            double result = 0;
                            switch (calculatorOperator) {
                                case '+': result = calculatorValue + calculatorOperand; break;
                                case '-': result = calculatorValue - calculatorOperand; break;
                                case '*': result = calculatorValue * calculatorOperand; break;
                                case '/': 
                                    if (calculatorOperand != 0) {
                                        result = calculatorValue / calculatorOperand;
                                    } else {
                                        calculatorDisplay = "Error";
                                        calculatorNewNumber = true;
                                        calculatorOperator = ' ';
                                        drawCalculator(0);
                                        return;
                                    }
                                    break;
                            }
                            equationResult = String(calculatorValue) + String(calculatorOperator) + 
                                             String(calculatorOperand) + "=" + String(result);
                            calculatorValue = result;
                            calculatorDisplay = String(result);
                            calculatorOperator = ' ';
                            calculatorNewNumber = true;
                        }
                    } else if (button == "←") {
                        // 退格
                        if (calculatorDisplay.length() > 1) {
                            calculatorDisplay.remove(calculatorDisplay.length() - 1);
                        } else {
                            calculatorDisplay = "0";
                        }
                    } else if (button == "+" || button == "-" || button == "*" || button == "/") {
                        // 运算符
                        if (calculatorOperator != ' ' && !calculatorNewNumber) {
                            // 如果已经有运算符且用户输入了新数字，使用当前显示作为操作数并先计算
                            calculatorOperand = calculatorDisplay.toDouble();
                            double result = 0;
                            switch (calculatorOperator) {
                                case '+': result = calculatorValue + calculatorOperand; break;
                                case '-': result = calculatorValue - calculatorOperand; break;
                                case '*': result = calculatorValue * calculatorOperand; break;
                                case '/': 
                                    if (calculatorOperand != 0) result = calculatorValue / calculatorOperand;
                                    else {
                                        calculatorDisplay = "Error";
                                        calculatorNewNumber = true;
                                        calculatorOperator = ' ';
                                        drawCalculator(0);
                                        return;
                                    }
                                    break;
                            }
                            calculatorValue = result;
                            calculatorDisplay = String(result);
                        } else {
                            // 将当前显示设置为累积值
                            calculatorValue = calculatorDisplay.toDouble();
                        }
                        calculatorOperator = button.charAt(0);
                        calculatorNewNumber = true;
                    } else if (button == ".") {
                        // 小数点
                        // 仅当显示中还没有小数点时追加
                        if (calculatorDisplay.indexOf('.') < 0) {
                            if (calculatorNewNumber) {
                                // 从新数字开始，显示以 0. 开头更友好
                                calculatorDisplay = "0.";
                                calculatorNewNumber = false;
                            } else {
                                calculatorDisplay += ".";
                            }
                        }
                    } else {
                        // 数字
                        if (calculatorNewNumber) {
                            calculatorDisplay = button;
                            calculatorNewNumber = false;
                        } else {
                            if (calculatorDisplay == "0") {
                                calculatorDisplay = button;
                            } else {
                                calculatorDisplay += button;
                            }
                        }
                    }
                    
                    drawCalculator(0);
                    return;
                }
            }
        }
    }
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



// 绘制单个卡片
void drawCard(int cardIndex, int offsetX) {
    switch (cardIndex) {
        case 0: // 第 0 页：时间显示
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.setTextSize(5);
            tft.setTextDatum(CC_DATUM);
            tft.drawString(currentTimeStr, SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2);
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
            
        case 2: // 第 2 页：计算器
            drawCalculator(offsetX);
            break;
            
        case 3: // 第 3 页：方程求解器
            drawEquationSolver(offsetX);
            break;
            
        case 4: // 第 4 页：系统状态
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
            
        case 5: // 第 5 页：设置
            tft.setTextColor(TFT_ORANGE, TFT_BLACK);
            tft.setTextSize(3);
            tft.setTextDatum(CC_DATUM);
            tft.drawString("Settings", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 - 20);
            tft.setTextSize(2);
            tft.drawString("Touch to configure", SCREEN_WIDTH / 2 + offsetX, SCREEN_HEIGHT / 2 + 20);
            break;
            
        case 6: // 第 6 页：关于
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
    if (screenHidden) {
        // 屏幕隐藏时，任何触摸都唤醒屏幕
        screenHidden = false;
        lastActivityTime = millis();
        drawCards();
        return;
    }
    
    // 特殊处理计算器页面（第 2 页）
    if (currentPage == 2) {
        if (touchscreen.touched()) {
            TS_Point p = touchscreen.getPoint();
            handleCalculatorTouch(p.x, p.y);
            lastActivityTime = millis();
            return;
        }
    }
    
    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        
        if (!isSliding) {
            // 开始触摸
            isSliding = true;
            touchStartX = p.x;
            touchEndX = p.x;
        } else {
            // 持续触摸，更新位置
            touchEndX = p.x;
        }
        
        lastActivityTime = millis(); // 更新活动时间
    } else if (isSliding) {
        // 触摸结束，判断滑动方向
        int diffX = touchEndX - touchStartX;
        
        // 滑动阈值
        if (abs(diffX) > SLIDE_THRESHOLD) {
            if (diffX > 0) {
                // 向右滑动：上一页
                if (currentPage > 0) {
                    currentPage--;
                }
            } else {
                // 向左滑动：下一页
                if (currentPage < CARD_COUNT - 1) {
                    currentPage++;
                }
            }
            drawCards();
        }
        
        isSliding = false;
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
