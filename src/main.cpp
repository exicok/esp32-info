#include <Arduino.h>
#include <SPI.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_sntp.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>

// ============ 用户配置区（可自定义修改） ============

// 天气配置
const char* WEATHER_CITY = "云南大理祥云";  // 显示名称
const float WEATHER_LAT = 25.48;            // 祥云县纬度
const float WEATHER_LON = 100.56;           // 祥云县经度

// WiFi 与网络校时配置。SSID 留空时禁用 WiFi 校时。
const char* WIFI_SSID = "ChinaNet-GWpJ";
const char* WIFI_PASSWORD = "12345678";
const char* NTP_SERVER_1 = "ntp.aliyun.com";
const char* NTP_SERVER_2 = "pool.ntp.org";
const long LOCAL_UTC_OFFSET_SECONDS = 8 * 3600;

// ============ 配置区结束 ============

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

// 状态变量
String currentLyric = "";
String currentTranslation = "";
uint16_t currentLyricColor = TFT_YELLOW;
uint8_t currentLyricFontSize = 16;
bool musicIsPlaying = false;
float musicProgressSeconds = 0;
float musicDurationSeconds = 0;
unsigned long musicProgressUpdatedAt = 0;
unsigned long lastMusicProgressDraw = 0;
String currentTimeStr = "00:00:00";
String lastTimeStr = ""; // 用于局部刷新的上一次时间
String currentDateStr = "2024-01-01";
bool lyricActive = false;
bool screenHidden = false;
unsigned long lastInteractionMillis = 0;
const unsigned long SCREEN_IDLE_TIMEOUT = 300000UL; // 5分钟
unsigned long lastLyricTime = 0;
unsigned long lastTimeUpdate = 0;
unsigned long lastInfoUpdate = 0;
unsigned long lastTelemetryUpdate = 0;
long desktopUtcOffsetSeconds = LOCAL_UTC_OFFSET_SECONDS;
bool desktopTimeSynced = false;
bool wifiTimeSynced = false;
bool wifiNtpStarted = false;
volatile bool wifiNtpSyncPending = false;
unsigned long lastWifiConnectAttempt = 0;
String desktopCommandBuffer = "";

enum class TimeSource { NONE, DESKTOP, WIFI_NTP };
TimeSource activeTimeSource = TimeSource::NONE;

const unsigned long WIFI_RECONNECT_INTERVAL = 30000;

// CPU使用率计算
unsigned long lastCpuUpdate = 0;
float cpuUsagePercent = 0;
unsigned long activeTimeUs = 0;
unsigned long lastLoopStart = 0;

String pcCpuName = "等待电脑数据";
String pcGpuName = "等待电脑数据";
float pcCpuUsage = 0;
int pcCpuCores = 0;
int pcMemoryUsedMB = 0;
int pcMemoryTotalMB = 0;
int pcGpuMemoryMB = 0;
int pcCpuPhysicalCores = 0;
int pcCpuMaxMHz = 0;
float pcMemoryUsage = 0;
String pcGpuDriver = "";
String pcDiskSummary = "等待磁盘数据";
int pcScrollOffset = 0;
bool pcStatusDirty = false;
unsigned long lastPcStatusDraw = 0;
float pcCpuHistory[36] = {0};
float pcGpuHistory[36] = {0};
float pcGpuUsage = 0;

int currentPage = 0;
#define MAX_PAGES 7

// 天气数据结构
struct WeatherData {
    String city;
    String temp;
    String humidity;
    String windSpeed;
    String windDir;
    String weather;
    float windSpeedKmh;
    float windDirectionDegrees;
    bool isValid;
};

struct ForecastData {
    String date;
    String high;
    String low;
    String weather;
};

WeatherData currentWeather = {"", "", "", "", "", "", 0.0f, 0.0f, false};
ForecastData forecast[7];
unsigned long lastWeatherUpdate = 0;
unsigned long lastWeatherAttempt = 0;
unsigned long lastWindAnimation = 0;
uint8_t windAnimationPhase = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 1800000; // 30分钟更新一次
const unsigned long WEATHER_RETRY_INTERVAL = 60000;     // 失败后1分钟重试
bool weatherLoading = false;
String weatherError = "";
String weatherCity = WEATHER_CITY; // 使用配置区的城市名称

struct NewsItem { String title; String body; String source; };
NewsItem newsItems[30];
int newsCount = 0;
bool newsValid = false;
bool newsDetail = false;
int newsDetailIndex = -1;
int newsScrollOffset = 0;
int newsDetailScrollOffset = 0;
bool timerCountdownMode = false;
bool timerRunning = false;
unsigned long timerStartedAt = 0;
unsigned long timerElapsedMs = 0;
unsigned long countdownDurationMs = 300000UL;
unsigned long lastTimerDraw = 0;
bool timerAlarmActive = false;
bool timerAlarmRed = false;
unsigned long lastTimerAlarmToggle = 0;
unsigned long lastNewsUpdate = 0;

int drawWrappedTextCentered(String text, int centerX, int startY, int maxW, int lineHeight);

String xmlValue(const String& xml, const char* tag, int from) {
    String open = String("<") + tag + ">", close = String("</") + tag + ">";
    int a = xml.indexOf(open, from), b;
    if (a < 0) return "";
    a += open.length(); b = xml.indexOf(close, a);
    if (b < 0) return "";
    String value = xml.substring(a, b); value.replace("&amp;", "&"); value.replace("&quot;", "\"");
    return value;
}

bool fetchNews() {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClientSecure client; client.setInsecure(); HTTPClient http;
    const char* feeds[] = {
        "https://www.chinanews.com.cn/rss/scroll-news.xml",
        "https://www.chinanews.com.cn/rss/china.xml",
        "https://www.chinanews.com.cn/rss/world.xml"
    };
    newsCount = 0;
    for (const char* feed : feeds) {
        if (newsCount >= 30 || !http.begin(client, feed)) continue;
        http.setTimeout(10000);
        if (http.GET() == HTTP_CODE_OK) {
            String xml = http.getString(); int pos = 0;
            while (newsCount < 30) {
                int item = xml.indexOf("<item>", pos); if (item < 0) break;
                String title = xmlValue(xml, "title", item);
                if (title.length()) {
                    String body = xmlValue(xml, "description", item);
                    if (body.length() == 0) body = xmlValue(xml, "content:encoded", item);
                    body.replace("<![CDATA[", ""); body.replace("]]>", "");
                    newsItems[newsCount].title = title;
                    newsItems[newsCount].body = body;
                    newsItems[newsCount].source = feed;
                    newsCount++;
                }
                pos = item + 6;
            }
        }
        http.end();
    }
    newsValid = newsCount > 0; lastNewsUpdate = millis();
    return newsValid;
}

void drawNewsPage() {
    tft.fillScreen(TFT_BLACK); u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
    if (newsDetail && newsDetailIndex >= 0 && newsDetailIndex < newsCount) {
        u8f.setFont(u8g2_font_wqy14_t_gb2312); u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(8, 22); u8f.print("< 返回");
        u8f.setFont(u8g2_font_wqy12_t_gb2312); u8f.setForegroundColor(TFT_WHITE);
        int titleLines = drawWrappedTextCentered(newsItems[newsDetailIndex].title, 160, 52, 300, 18);
        u8f.setForegroundColor(TFT_LIGHTGREY);
        String body = newsItems[newsDetailIndex].body;
        if (body.length() == 0) body = "暂无正文摘要";
        drawWrappedTextCentered(body, 160, 58 + titleLines * 18 + newsDetailScrollOffset, 300, 16);
        u8f.setForegroundColor(TFT_DARKGREY); u8f.setCursor(10, 220); u8f.print("来源: " + newsItems[newsDetailIndex].source);
        return;
    }
    u8f.setFont(u8g2_font_wqy14_t_gb2312); u8f.setForegroundColor(TFT_CYAN);
    u8f.setCursor(12, 24); u8f.print("新闻");
    u8f.setFont(u8g2_font_wqy12_t_gb2312);
    if (!newsValid) { u8f.setForegroundColor(TFT_YELLOW); u8f.setCursor(20, 70); u8f.print("新闻加载中..."); return; }
    for (int i = 0; i < newsCount; ++i) {
        int y = 52 + i * 30 + newsScrollOffset;
        if (y < 30 || y > SCREEN_HEIGHT + 20) continue;
        u8f.setForegroundColor(TFT_YELLOW); u8f.setCursor(10, y); u8f.print(String(i + 1) + ".");
        u8f.setForegroundColor(TFT_WHITE); drawWrappedTextCentered(newsItems[i].title, 175, y, 275, 16);
    }
}

// 祥云县坐标（使用配置区）
float weatherLat = WEATHER_LAT;
float weatherLon = WEATHER_LON;

// WMO天气代码转中文
String wmoWeatherDesc(int code) {
    switch (code) {
        case 0: return "晴";
        case 1: return "大部晴";
        case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: return "冻毛毛雨";
        case 61: case 63: case 65: return "雨";
        case 66: case 67: return "冻雨";
        case 71: case 73: case 75: return "雪";
        case 77: return "雪粒";
        case 80: case 81: case 82: return "阵雨";
        case 85: case 86: return "阵雪";
        case 95: return "雷暴";
        case 96: case 99: return "雷暴冰雹";
        default: return "未知";
    }
}

// 触控手势逻辑
int startX = -1, startY = -1;
int lastTouchX = -1, lastTouchY = -1; // 保存最后的有效触摸坐标
bool isSwiping = false; // 标记是否为滑动操作
int scrollOffset = 0;
int weatherScrollOffset = 0; // 天气页面专用滚动偏移
const int SWIPE_MIN_X = 800; // 左右滑动触发阈值 (原始坐标)

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

String formatUptime(unsigned long ms) {
    unsigned long seconds = ms / 1000;
    unsigned long days = seconds / 86400;
    seconds %= 86400;
    unsigned long hours = seconds / 3600;
    seconds %= 3600;
    unsigned long minutes = seconds / 60;
    seconds %= 60;
    char buf[32];
    sprintf(buf, "%lud %02lu:%02lu:%02lu", days, hours, minutes, seconds);
    return String(buf);
}

void drawInfoContent(bool forceRefresh) {
    static String rowCache[24];
    int rowIndex = 0;
    int curY = 50 + scrollOffset;
    int lineHeight = 25;
    u8f.setFont(u8g2_font_wqy12_t_gb2312);

    auto drawRow = [&](const char* label, String val, uint16_t color) {
        bool changed = forceRefresh || rowCache[rowIndex] != val;
        if (changed && curY > 24 && curY < SCREEN_HEIGHT + 20) {
            tft.fillRect(0, curY - 16, SCREEN_WIDTH, 20, TFT_BLACK);
            u8f.setForegroundColor(TFT_CYAN);
            u8f.setCursor(10, curY); u8f.print(label);
            u8f.setForegroundColor(color);
            u8f.setCursor(100, curY); u8f.print(val);
        }
        rowCache[rowIndex++] = val;
        curY += lineHeight;
    };

    auto drawCard = [&](int top, int height) {
        if (top < SCREEN_HEIGHT && top + height > 24) {
            tft.drawRoundRect(4, top, SCREEN_WIDTH - 8, height, 6, tft.color565(55, 65, 78));
        }
    };

    // 硬件信息
    drawCard(curY - 18, 151);
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 硬件信息 ---"); curY += lineHeight;
    drawRow("CPU 频率:", String(ESP.getCpuFreqMHz()) + " MHz", TFT_WHITE);
    drawRow("芯片型号:", ESP.getChipModel(), TFT_WHITE);
    drawRow("芯片版本:", String(ESP.getChipRevision()), TFT_WHITE);
    drawRow("核心数:", String(ESP.getChipCores()), TFT_WHITE);
    drawRow("系统版本:", String(ESP.getSdkVersion()), TFT_WHITE);

    curY += 10;
    // 内存信息
    drawCard(curY - 18, psramFound() ? 151 : 101);
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 内存信息 ---"); curY += lineHeight;
    drawRow("可用堆内存:", String(ESP.getFreeHeap() / 1024) + " KB", TFT_GREEN);
    drawRow("最小可用堆:", String(ESP.getMinFreeHeap() / 1024) + " KB", TFT_WHITE);
    drawRow("总堆内存:", String(ESP.getHeapSize() / 1024) + " KB", TFT_WHITE);
    if (psramFound()) {
        drawRow("PSRAM总量:", String(ESP.getPsramSize() / 1024) + " KB", TFT_WHITE);
        drawRow("PSRAM可用:", String(ESP.getFreePsram() / 1024) + " KB", TFT_GREEN);
    }

    curY += 10;
    // ROM/Flash信息
    drawCard(curY - 18, 76);
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- ROM/Flash ---"); curY += lineHeight;
    drawRow("Flash大小:", String(ESP.getFlashChipSize() / 1024 / 1024) + " MB", TFT_WHITE);
    drawRow("Flash速度:", String(ESP.getFlashChipSpeed() / 1000000) + " MHz", TFT_WHITE);

    curY += 10;
    // WiFi 信息
    drawCard(curY - 18, 151);
    bool wifiConnected = WiFi.status() == WL_CONNECTED;
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- WiFi 信息 ---"); curY += lineHeight;
    drawRow("连接状态:", wifiConnected ? "已连接" : "未连接", wifiConnected ? TFT_GREEN : TFT_RED);
    String wifiSsid = wifiConnected ? WiFi.SSID() : String(WIFI_SSID);
    if (wifiSsid.length() == 0) wifiSsid = "未配置";
    if (wifiSsid.length() > 28) wifiSsid = wifiSsid.substring(0, 25) + "...";
    drawRow("SSID:", wifiSsid, TFT_WHITE);
    drawRow("IP地址:", wifiConnected ? WiFi.localIP().toString() : "--", TFT_WHITE);
    drawRow("信号强度:", wifiConnected ? String(WiFi.RSSI()) + " dBm" : "--", wifiConnected ? TFT_CYAN : TFT_WHITE);
    drawRow("MAC地址:", WiFi.macAddress(), TFT_WHITE);

    curY += 10;
    // 系统状态
    drawCard(curY - 18, 101);
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 系统状态 ---"); curY += lineHeight;
    char cpuBuf[16];
    sprintf(cpuBuf, "%.1f%%", cpuUsagePercent);
    drawRow("CPU占有率:", cpuBuf, TFT_CYAN);
    drawRow("开机时间:", formatUptime(millis()), TFT_WHITE);
}

void drawWeatherPage();

String windDirectionDesc(float degrees) {
    static const char* directions[] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
    int index = static_cast<int>((degrees + 22.5f) / 45.0f) % 8;
    return directions[index];
}

bool fetchWeather() {
    if (WiFi.status() != WL_CONNECTED || weatherLoading) return false;

    weatherLoading = true;
    weatherError = "";
    lastWeatherAttempt = millis();
    if (currentPage == 2) drawWeatherPage();

    // Open-Meteo 国际免费天气接口，无需 API Key。
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(weatherLat, 4)
        + "&longitude=" + String(weatherLon, 4)
        + "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m"
        + "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        + "&timezone=Asia%2FShanghai&forecast_days=7";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, url)) {
        weatherError = "无法连接天气服务";
        weatherLoading = false;
        return false;
    }

    int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
        weatherError = "请求失败: " + String(statusCode);
        http.end();
        weatherLoading = false;
        return false;
    }

    String responseBody = http.getString();
    http.end();
    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, responseBody);
    if (error) {
        weatherError = "天气数据解析失败: " + String(error.c_str());
        weatherLoading = false;
        return false;
    }

    JsonObject current = doc["current"];
    currentWeather.city = WEATHER_CITY;
    currentWeather.temp = String(current["temperature_2m"].as<float>(), 1) + "°C";
    currentWeather.humidity = String(current["relative_humidity_2m"].as<int>()) + "%";
    currentWeather.windSpeed = String(current["wind_speed_10m"].as<float>(), 1) + " km/h";
    currentWeather.windSpeedKmh = current["wind_speed_10m"].as<float>();
    currentWeather.windDirectionDegrees = current["wind_direction_10m"].as<float>();
    currentWeather.windDir = windDirectionDesc(currentWeather.windDirectionDegrees);
    currentWeather.weather = wmoWeatherDesc(current["weather_code"].as<int>());

    JsonObject daily = doc["daily"];
    if (daily.isNull()) {
        weatherError = "天气接口未返回预报";
        weatherLoading = false;
        return false;
    }

    for (int i = 0; i < 7; ++i) {
        if (i >= daily.size()) {
            forecast[i] = {"", "", "", ""};
            continue;
        }
        forecast[i].date = daily["time"][i].as<String>();
        forecast[i].high = String(daily["temperature_2m_max"][i].as<float>(), 0) + "°";
        forecast[i].low = String(daily["temperature_2m_min"][i].as<float>(), 0) + "°";
        forecast[i].weather = wmoWeatherDesc(daily["weather_code"][i].as<int>());
    }

    currentWeather.isValid = true;
    weatherLoading = false;
    lastWeatherUpdate = millis();
    return true;
}

void refreshInfoPage() {
    drawInfoContent(false);
    drawTaskbar();
}

void drawWindAnimation() {
    if (!currentWeather.isValid) return;
    int cy = 117 + weatherScrollOffset;
    if (cy < 24 || cy >= SCREEN_HEIGHT) return;
    tft.fillRect(252, cy - 20, 68, 44, TFT_BLACK);
    int cx = 300, radius = 14;
    tft.drawCircle(cx, cy, radius, tft.color565(55, 65, 78));
    float radians = (currentWeather.windDirectionDegrees - 90.0f) * PI / 180.0f;
    int tipX = cx + static_cast<int>(cos(radians) * radius);
    int tipY = cy + static_cast<int>(sin(radians) * radius);
    tft.drawLine(cx, cy, tipX, tipY, TFT_CYAN);
    float left = radians + 2.65f, right = radians - 2.65f;
    tft.drawLine(tipX, tipY, tipX + static_cast<int>(cos(left) * 6), tipY + static_cast<int>(sin(left) * 6), TFT_CYAN);
    tft.drawLine(tipX, tipY, tipX + static_cast<int>(cos(right) * 6), tipY + static_cast<int>(sin(right) * 6), TFT_CYAN);
    for (int i = 0; i < 3; ++i) {
        int offset = (windAnimationPhase + i * 9) % 27;
        tft.drawFastHLine(258 + offset, cy - 10 + i * 9, 7, tft.color565(80, 180, 220));
    }
}

void drawWeatherPage() {
    tft.fillScreen(TFT_BLACK);
    // 天气页不显示任务栏

    u8f.setFontMode(1);
    u8f.setBackgroundColor(TFT_BLACK);
    u8f.setFont(u8g2_font_wqy12_t_gb2312);

    if (weatherLoading) {
        u8f.setForegroundColor(TFT_YELLOW);
        u8f.setCursor(SCREEN_WIDTH / 2 - 40, SCREEN_HEIGHT / 2);
        u8f.print("加载中...");
        return;
    }

    if (!currentWeather.isValid) {
        u8f.setForegroundColor(TFT_RED);
        u8f.setCursor(20, 60);
        u8f.print("天气数据获取失败");
        u8f.setForegroundColor(TFT_WHITE);
        u8f.setCursor(20, 90);
        u8f.print(weatherError);
        u8f.setCursor(20, 120);
        u8f.print("城市: " + weatherCity);
        return;
    }

    int y = 15 + weatherScrollOffset; // 无任务栏，起始位置更靠上

    // 城市名称（居中显示）
    if (y > 0 && y < SCREEN_HEIGHT) {
        u8f.setForegroundColor(TFT_CYAN);
        u8f.setFont(u8g2_font_wqy12_t_gb2312);
        int cityWidth = u8f.getUTF8Width(currentWeather.city.c_str());
        u8f.setCursor((SCREEN_WIDTH - cityWidth) / 2, y);
        u8f.print(currentWeather.city);
    }
    y += 35;

    // 当前温度（大号字体，居中）- 只显示数字部分
    if (y > 24 && y < SCREEN_HEIGHT) {
        u8f.setForegroundColor(TFT_WHITE);
        u8f.setFont(u8g2_font_logisoso32_tn);
        // 提取温度数字部分（去掉°C）
        String tempNum = currentWeather.temp;
        int degreeIdx = tempNum.indexOf("°");
        if (degreeIdx > 0) tempNum = tempNum.substring(0, degreeIdx);
        int tempWidth = u8f.getUTF8Width(tempNum.c_str());
        // 温度数字居中
        int tempX = (SCREEN_WIDTH - tempWidth) / 2;
        u8f.setCursor(tempX, y);
        u8f.print(tempNum);
        // 在温度后显示单位（用普通字体）
        u8f.setFont(u8g2_font_wqy12_t_gb2312);
        u8f.setCursor(tempX + tempWidth + 5, y + 12);
        u8f.print("°C");
    }
    y += 50;

    // 天气状况（居中）
    if (y > 24 && y < SCREEN_HEIGHT) {
        u8f.setForegroundColor(TFT_YELLOW);
        u8f.setFont(u8g2_font_wqy12_t_gb2312);
        int weatherWidth = u8f.getUTF8Width(currentWeather.weather.c_str());
        u8f.setCursor((SCREEN_WIDTH - weatherWidth) / 2, y);
        u8f.print(currentWeather.weather);
    }
    y += 25;

    // 详细信息（两列布局）
    if (y > 24 && y < SCREEN_HEIGHT) {
        u8f.setForegroundColor(TFT_LIGHTGREY);
        u8f.setCursor(30, y);
        u8f.print("湿度: " + currentWeather.humidity);
        u8f.setCursor(180, y);
        u8f.print("风向: " + currentWeather.windDir);
    }
    drawWindAnimation();
    y += 18;
    if (y > 24 && y < SCREEN_HEIGHT) {
        u8f.setCursor(30, y);
        u8f.print("风速: " + currentWeather.windSpeed);
    }
    y += 25;

    // 分隔线
    if (y > 24 && y < SCREEN_HEIGHT) {
        tft.drawFastHLine(20, y, SCREEN_WIDTH - 40, tft.color565(60, 60, 60));
    }
    y += 12;

    // 未来7天预报标题
    if (y > 24 && y < SCREEN_HEIGHT) {
        u8f.setForegroundColor(TFT_YELLOW);
        u8f.setCursor(20, y);
        u8f.print("未来7天预报");
    }
    y += 22;

    // 7天预报（详细布局，每行显示更多信息）
    u8f.setFont(u8g2_font_wqy12_t_gb2312);
    for (int i = 0; i < 7; i++) {
        if (y > SCREEN_HEIGHT + 20) break;

        String dateStr = forecast[i].date.substring(5); // 只显示月-日

        // 日期（青色）
        if (y > 24 && y < SCREEN_HEIGHT) {
            u8f.setForegroundColor(TFT_CYAN);
            u8f.setCursor(20, y);
            u8f.print(dateStr);

            // 天气（白色）
            u8f.setForegroundColor(TFT_WHITE);
            u8f.setCursor(75, y);
            u8f.print(forecast[i].weather);

            // 最高温（红色）
            u8f.setForegroundColor(TFT_RED);
            u8f.setCursor(150, y);
            u8f.print(forecast[i].high);

            // 最低温（蓝色）
            u8f.setForegroundColor(TFT_BLUE);
            u8f.setCursor(210, y);
            u8f.print(forecast[i].low);
        }
        y += 22;
    }

    // 绘制滚动条指示器
    if (weatherScrollOffset < 0) {
        int scrollBarHeight = 40;
        int scrollBarY = map(weatherScrollOffset, -300, 0, 30, SCREEN_HEIGHT - scrollBarHeight - 30);
        tft.fillRect(SCREEN_WIDTH - 4, scrollBarY, 3, scrollBarHeight, tft.color565(100, 100, 100));
    }
}

void drawMusicControls() {
    struct ControlButton { int x; const char* label; };
    const ControlButton buttons[] = {{55, "|<"}, {135, musicIsPlaying ? "||" : ">"}, {215, ">|"}};
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    for (const auto& button : buttons) {
        tft.fillRoundRect(button.x, 194, 50, 36, 4, tft.color565(38, 45, 54));
        tft.drawRoundRect(button.x, 194, 50, 36, 4, tft.color565(90, 105, 120));
        tft.setTextColor(TFT_WHITE, tft.color565(38, 45, 54));
        tft.drawString(button.label, button.x + 25, 212);
    }
}

void drawMusicProgress() {
    if (!lyricActive || musicDurationSeconds <= 0) return;
    float progress = musicProgressSeconds;
    if (musicIsPlaying) progress += (millis() - musicProgressUpdatedAt) / 1000.0f;
    progress = constrain(progress / musicDurationSeconds, 0.0f, 1.0f);
    const int x = 20, y = 178, width = 280, height = 4;
    tft.fillRect(x, y, width, height, tft.color565(55, 60, 68));
    tft.fillRect(x, y, static_cast<int>(width * progress), height, TFT_CYAN);
}

bool sendMusicControl(const char* action) {
    StaticJsonDocument<96> doc;
    doc["type"] = "music_control";
    doc["action"] = action;
    serializeJson(doc, Serial);
    Serial.println();
    return true;
}

bool handleMusicControlTap(int rawX, int rawY) {
    if (!lyricActive || currentPage != 0) return false;
    int screenX = constrain(map(rawX, 200, 3700, 0, SCREEN_WIDTH), 0, SCREEN_WIDTH - 1);
    int screenY = constrain(map(rawY, 240, 3800, 0, SCREEN_HEIGHT), 0, SCREEN_HEIGHT - 1);
    if (screenY < 188 || screenY > 239) return false;

    if (screenX >= 45 && screenX <= 115) return sendMusicControl("previous");
    if (screenX >= 125 && screenX <= 195) return sendMusicControl("play-pause");
    if (screenX >= 205 && screenX <= 275) return sendMusicControl("next");
    return false;
}

String holidayName(int month, int day) {
    if (month == 1 && day == 1) return "元旦";
    if (month == 5 && day == 1) return "劳动节";
    if (month == 10 && day == 1) return "国庆节";
    // 2026 年主要农历节日。
    if (month == 2 && day == 17) return "春节";
    if (month == 6 && day == 19) return "端午";
    if (month == 9 && day == 25) return "中秋";
    return "";
}

bool isHolidayBreak2026(int month, int day) {
    return (month == 1 && day >= 1 && day <= 3)
        || (month == 2 && day >= 15 && day <= 23)
        || (month == 4 && day >= 4 && day <= 6)
        || (month == 5 && day >= 1 && day <= 5)
        || (month == 6 && day >= 19 && day <= 21)
        || (month == 9 && day >= 25 && day <= 27)
        || (month == 10 && day >= 1 && day <= 7);
}

bool isMakeupWorkday2026(int month, int day) {
    return (month == 1 && day == 4) || (month == 2 && (day == 14 || day == 28))
        || (month == 4 && day == 26) || (month == 5 && day == 9)
        || (month == 9 && day == 20) || (month == 10 && day == 10);
}

void drawCalendarPage() {
    tft.fillScreen(TFT_BLACK); u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
    int year = currentDateStr.substring(0, 4).toInt();
    int month = currentDateStr.substring(5, 7).toInt();
    int today = currentDateStr.substring(8, 10).toInt();
    if (year < 2024 || month < 1) { year = 2026; month = 1; today = 1; }
    u8f.setFont(u8g2_font_wqy14_t_gb2312); u8f.setForegroundColor(TFT_CYAN);
    String title = String(year) + " 年 " + String(month) + " 月";
    u8f.setCursor(160 - u8f.getUTF8Width(title.c_str()) / 2, 22); u8f.print(title);
    const char* weeks[] = {"日", "一", "二", "三", "四", "五", "六"};
    u8f.setFont(u8g2_font_wqy12_t_gb2312);
    for (int i = 0; i < 7; ++i) { u8f.setForegroundColor(i == 0 || i == 6 ? TFT_RED : TFT_LIGHTGREY); u8f.setCursor(18 + i * 44, 48); u8f.print(weeks[i]); }
    struct tm first = {}; first.tm_year = year - 1900; first.tm_mon = month - 1; first.tm_mday = 1; mktime(&first);
    int days = month == 2 ? (((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 29 : 28) : ((month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31);
    for (int day = 1; day <= days; ++day) {
        int cell = first.tm_wday + day - 1, x = 12 + (cell % 7) * 44, y = 75 + (cell / 7) * 28;
        String holiday = holidayName(month, day);
        bool holidayBreak = year == 2026 && isHolidayBreak2026(month, day);
        bool makeupWorkday = year == 2026 && isMakeupWorkday2026(month, day);
        if (day == today) tft.fillRoundRect(x - 5, y - 16, 35, 22, 3, tft.color565(20, 85, 115));
        u8f.setForegroundColor(makeupWorkday ? TFT_CYAN : (holidayBreak || holiday.length() ? TFT_YELLOW : ((cell % 7 == 0 || cell % 7 == 6) ? TFT_RED : TFT_WHITE)));
        u8f.setCursor(x, y); u8f.print(day);
        String marker = makeupWorkday ? "班" : (holiday.length() ? holiday : (holidayBreak ? "休" : ""));
        if (marker.length()) { u8f.setCursor(x - 5, y + 13); u8f.print(marker); }
    }
}

unsigned long currentTimerValue() {
    unsigned long elapsed = timerElapsedMs + (timerRunning ? millis() - timerStartedAt : 0);
    return timerCountdownMode ? (elapsed >= countdownDurationMs ? 0 : countdownDurationMs - elapsed) : elapsed;
}

void drawTimerValue() {
    unsigned long value = currentTimerValue() / 1000;
    char buf[16]; sprintf(buf, "%02lu:%02lu:%02lu", value / 3600, (value / 60) % 60, value % 60);
    uint16_t background = timerAlarmActive && timerAlarmRed ? TFT_RED : TFT_BLACK;
    tft.fillRect(20, 65, 280, 70, background);
    tft.setTextDatum(MC_DATUM); tft.setTextSize(4); tft.setTextColor(TFT_WHITE, background);
    tft.drawString(buf, 160, 100); tft.setTextSize(1);
}

void drawTimerPage() {
    tft.fillScreen(TFT_BLACK); u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
    u8f.setFont(u8g2_font_wqy14_t_gb2312); u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(15, 25);
    u8f.print(timerCountdownMode ? "倒计时" : "秒表计时");
    drawTimerValue(); u8f.setFont(u8g2_font_wqy12_t_gb2312);
    const char* labels[] = {timerRunning ? "暂停" : "开始", "复位", timerCountdownMode ? "秒表" : "倒计时"};
    for (int i = 0; i < 3; ++i) { int x = 15 + i * 103; tft.drawRoundRect(x, 155, 85, 40, 4, TFT_CYAN); u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(x + 25, 181); u8f.print(labels[i]); }
    if (timerCountdownMode) {
        const char* adjust[] = {"-10", "-1", "+1", "+10"};
        for (int i = 0; i < 4; ++i) {
            int x = 5 + i * 79; tft.drawRoundRect(x, 205, 70, 30, 3, TFT_DARKGREY);
            u8f.setCursor(x + 23, 226); u8f.print(adjust[i]);
        }
    }
}

void drawPcGraph(int x, int y, int width, int height, const float* values, uint16_t color) {
    tft.drawRect(x, y, width, height, tft.color565(55, 65, 78));
    for (int i = 1; i < 36; ++i) {
        int x1 = x + 1 + (i - 1) * (width - 3) / 35;
        int x2 = x + 1 + i * (width - 3) / 35;
        int y1 = y + height - 2 - constrain(values[i - 1], 0.0f, 100.0f) * (height - 3) / 100.0f;
        int y2 = y + height - 2 - constrain(values[i], 0.0f, 100.0f) * (height - 3) / 100.0f;
        tft.drawLine(x1, y1, x2, y2, color);
    }
}

void drawPcStatusPage() {
    tft.fillScreen(TFT_BLACK);
    u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK); u8f.setFont(u8g2_font_wqy12_t_gb2312);
    int o = pcScrollOffset;

    tft.drawRoundRect(4, 5 + o, 312, 110, 6, tft.color565(55, 65, 78));
    u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(12, 24 + o); u8f.print("处理器");
    u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(70, 24 + o); u8f.print(pcCpuName.substring(0, 28));
    u8f.setForegroundColor(TFT_GREEN); u8f.setCursor(14, 57 + o); u8f.print(String(pcCpuUsage, 1) + "%");
    u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(14, 82 + o); u8f.print(String(pcCpuPhysicalCores) + "核/" + String(pcCpuCores) + "线程");
    u8f.setCursor(14, 103 + o); u8f.print(String(pcCpuMaxMHz) + " MHz");
    drawPcGraph(125, 42 + o, 178, 58, pcCpuHistory, TFT_GREEN);

    tft.drawRoundRect(4, 122 + o, 312, 113, 6, tft.color565(55, 65, 78));
    u8f.setForegroundColor(TFT_YELLOW); u8f.setCursor(12, 142 + o); u8f.print("显卡");
    u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(55, 142 + o); u8f.print(pcGpuName.substring(0, 31));
    u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(14, 176 + o); u8f.print(String(pcGpuUsage, 1) + "%");
    u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(14, 202 + o); u8f.print("显存 " + String(pcGpuMemoryMB / 1024.0f, 1) + " GB");
    u8f.setCursor(14, 222 + o); u8f.print("驱动 " + pcGpuDriver);
    drawPcGraph(125, 159 + o, 178, 60, pcGpuHistory, TFT_CYAN);

    tft.drawRoundRect(4, 242 + o, 312, 105, 6, tft.color565(55, 65, 78));
    u8f.setForegroundColor(TFT_MAGENTA); u8f.setCursor(12, 264 + o); u8f.print("内存与磁盘");
    u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(14, 292 + o); u8f.print("内存 " + String(pcMemoryUsedMB) + "/" + String(pcMemoryTotalMB) + " MB");
    u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(14, 316 + o); u8f.print("占用 " + String(pcMemoryUsage, 1) + "%");
    u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(14, 338 + o); u8f.print(pcDiskSummary);
    pcStatusDirty = false;
    lastPcStatusDraw = millis();
}

void drawDisplay() {
    if (currentPage == 0) {
        if (lyricActive && currentLyric.length() > 0) {
            tft.fillScreen(TFT_BLACK);
            u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
            switch (currentLyricFontSize) {
                case 12: u8f.setFont(u8g2_font_wqy12_t_gb2312); break;
                case 13: u8f.setFont(u8g2_font_wqy13_t_gb2312); break;
                case 14: u8f.setFont(u8g2_font_wqy14_t_gb2312); break;
                case 15: u8f.setFont(u8g2_font_wqy15_t_gb2312); break;
                default: u8f.setFont(u8g2_font_wqy16_t_gb2312); break;
            }
            u8f.setForegroundColor(currentLyricColor);
            int lyricLines = drawWrappedTextCentered(currentLyric, 160, 70, SCREEN_WIDTH - 20, 20);
            if (currentTranslation.length() > 0) {
                u8f.setFont(u8g2_font_wqy12_t_gb2312); u8f.setForegroundColor(TFT_LIGHTGREY);
                drawWrappedTextCentered(currentTranslation, 160, 70 + lyricLines * 20 + 8, SCREEN_WIDTH - 20, 18);
            }
            drawMusicProgress();
            drawMusicControls();
            lastTimeStr = ""; // 重置，下次显示时间时全量绘制
        } else {
            // 时间显示 - 局部刷新
            tft.setTextSize(5);
            tft.setTextDatum(CC_DATUM);
            tft.setTextColor(TFT_WHITE);
            
            // 计算时间字符串的宽度和每个字符的位置
            int charWidth = tft.textWidth("0"); // 单个数字的宽度
            int totalWidth = charWidth * 8; // "HH:MM:SS" 共8个字符
            int startX = (SCREEN_WIDTH - totalWidth) / 2;
            int y = 120;
            
            if (lastTimeStr.length() != currentTimeStr.length()) {
                tft.fillScreen(TFT_BLACK);
                tft.setTextColor(TFT_WHITE, TFT_BLACK);
                tft.drawString(currentTimeStr, 160, y);
            } else {
                tft.setTextColor(TFT_WHITE, TFT_BLACK);
                // 逐字符比较，只重绘变化的字符
                for (int i = 0; i < currentTimeStr.length(); i++) {
                    if (currentTimeStr[i] != lastTimeStr[i]) {
                        // 计算该字符的位置
                        int charX = startX + i * charWidth + charWidth / 2;
                        // 清除该字符区域
                        tft.fillRect(charX - charWidth / 2, y - 30, charWidth, 60, TFT_BLACK);
                        // 重绘该字符
                        String ch = String(currentTimeStr[i]);
                        tft.drawString(ch, charX, y);
                    }
                }
            }
            lastTimeStr = currentTimeStr;
        }
    } else if (currentPage == 1) {
        tft.fillScreen(TFT_BLACK);
        drawInfoContent(true);
        drawTaskbar();
    } else if (currentPage == 2) {
        drawWeatherPage();
    } else if (currentPage == 3) {
        drawNewsPage();
    } else if (currentPage == 4) {
        drawCalendarPage();
    } else if (currentPage == 5) {
        drawTimerPage();
    } else if (currentPage == 6) {
        drawPcStatusPage();
    }
}

void applyDesktopLyricPacket(const String& data) {
    if (data.length() == 0 || data.length() > 1800) return;
    int seps[8]; int lastSep = -1; int sc = 0;
    for(int i=0; i<8; i++) { int p = data.indexOf('|', lastSep+1); if(p == -1) break; seps[sc++] = p; lastSep = p; }
    if (sc >= 8) {
        currentLyricColor = hexTo565(data.substring(0, seps[0]));
        currentLyricFontSize = constrain(data.substring(seps[0] + 1, seps[1]).toInt(), 12, 16);
        musicProgressSeconds = data.substring(seps[1] + 1, seps[2]).toFloat();
        musicDurationSeconds = data.substring(seps[2] + 1, seps[3]).toFloat();
        musicIsPlaying = data.substring(seps[3] + 1, seps[4]).toInt() == 1;
        musicProgressUpdatedAt = millis();
        String fullText = data.substring(seps[7] + 1);
        int nIdx = fullText.indexOf('\n');
        if (nIdx != -1) { currentLyric = fullText.substring(0, nIdx); currentTranslation = fullText.substring(nIdx+1); }
        else { currentLyric = fullText; currentTranslation = ""; }
        lyricActive = (currentLyric.length() > 0);
        lastLyricTime = millis();
        lastInteractionMillis = millis();
        if (lyricActive && screenHidden) {
            screenHidden = false;
            digitalWrite(TFT_BL, HIGH);
        }
        if (currentPage == 0) drawDisplay();
    }
}

void sendDesktopAck(const char* cmd, const char* message) {
    StaticJsonDocument<192> doc;
    doc["type"] = "ack";
    doc["cmd"] = cmd;
    doc["message"] = message;
    serializeJson(doc, Serial);
    Serial.println();
}

void sendDesktopTelemetry() {
    DynamicJsonDocument doc(1536);
    doc["type"] = "telemetry";
    doc["chipModel"] = ESP.getChipModel();
    doc["chipRevision"] = ESP.getChipRevision();
    doc["chipCores"] = ESP.getChipCores();
    doc["cpuMHz"] = ESP.getCpuFreqMHz();
    doc["sdkVersion"] = ESP.getSdkVersion();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["heapSize"] = ESP.getHeapSize();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["psramSize"] = ESP.getPsramSize();
    doc["freePsram"] = ESP.getFreePsram();
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["flashSpeed"] = ESP.getFlashChipSpeed();
    doc["temperature"] = temperatureRead();
    doc["uptimeSeconds"] = millis() / 1000;
    doc["timeSynced"] = desktopTimeSynced || wifiTimeSynced;
    doc["timeSource"] = activeTimeSource == TimeSource::WIFI_NTP
        ? "wifi-ntp"
        : (activeTimeSource == TimeSource::DESKTOP ? "ntp-via-com" : "unsynced");
    doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    doc["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    doc["date"] = currentDateStr;
    doc["time"] = currentTimeStr;
    serializeJson(doc, Serial);
    Serial.println();
}

void handleDesktopCommand(const String& line) {
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, line);
    if (error) return;

    const char* cmd = doc["cmd"] | "";
    if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "snapshot") == 0) {
        sendDesktopAck(cmd, "ESP32 已响应");
        sendDesktopTelemetry();
    } else if (strcmp(cmd, "lyric_packet") == 0) {
        applyDesktopLyricPacket(doc["payload"].as<String>());
    } else if (strcmp(cmd, "time_sync") == 0) {
        uint64_t epochMs = doc["epochMs"] | 0ULL;
        desktopUtcOffsetSeconds = (doc["utcOffsetMinutes"] | 0) * 60L;
        struct timeval tv = { static_cast<time_t>(epochMs / 1000ULL), static_cast<suseconds_t>((epochMs % 1000ULL) * 1000ULL) };
        settimeofday(&tv, nullptr);
        desktopTimeSynced = epochMs > 0;
        if (desktopTimeSynced) activeTimeSource = TimeSource::DESKTOP;
        sendDesktopAck(cmd, doc["source"] == "ntp" ? "NTP 时间经 COM 同步完成" : "电脑时间同步完成");
    } else if (strcmp(cmd, "screen_off") == 0) {
        screenHidden = true;
        digitalWrite(TFT_BL, LOW);
        sendDesktopAck(cmd, "屏幕已关闭，点击屏幕唤醒");
    } else if (strcmp(cmd, "pc_status") == 0) {
        pcCpuName = doc["cpuName"].as<String>();
        pcGpuName = doc["gpuName"].as<String>();
        pcCpuUsage = doc["cpuUsage"] | 0.0f;
        pcGpuUsage = doc["gpuUsage"] | 0.0f;
        pcCpuCores = doc["cpuCores"] | 0;
        pcCpuPhysicalCores = doc["cpuPhysicalCores"] | 0;
        pcCpuMaxMHz = doc["cpuMaxMHz"] | 0;
        pcMemoryUsedMB = doc["memoryUsedMB"] | 0;
        pcMemoryTotalMB = doc["memoryTotalMB"] | 0;
        pcMemoryUsage = doc["memoryUsage"] | 0.0f;
        pcGpuMemoryMB = doc["gpuMemoryMB"] | 0;
        pcGpuDriver = doc["gpuDriver"].as<String>();
        JsonArray disks = doc["disks"].as<JsonArray>();
        if (!disks.isNull() && disks.size() > 0) {
            int freeMB = disks[0]["freeMB"] | 0;
            int totalMB = disks[0]["totalMB"] | 0;
            pcDiskSummary = disks[0]["name"].as<String>() + " " + String(freeMB / 1024.0f, 1) + "/" + String(totalMB / 1024.0f, 1) + " GB 可用";
        }
        for (int i = 0; i < 35; ++i) {
            pcCpuHistory[i] = pcCpuHistory[i + 1];
            pcGpuHistory[i] = pcGpuHistory[i + 1];
        }
        pcCpuHistory[35] = pcCpuUsage;
        pcGpuHistory[35] = pcGpuUsage;
        pcStatusDirty = true;
    } else if (strcmp(cmd, "reboot") == 0) {
        sendDesktopAck(cmd, "设备正在重启");
        delay(150);
        ESP.restart();
    }
}

void readDesktopCommands() {
    while (Serial.available()) {
        char c = static_cast<char>(Serial.read());
        if (c == '\n') {
            desktopCommandBuffer.trim();
            if (desktopCommandBuffer.length() > 0) handleDesktopCommand(desktopCommandBuffer);
            desktopCommandBuffer = "";
        } else if (c != '\r' && desktopCommandBuffer.length() < 2048) {
            desktopCommandBuffer += c;
        }
    }
}

void onWifiNtpSync(struct timeval*) {
    wifiNtpSyncPending = true;
}

void startWifiConnection() {
    if (strlen(WIFI_SSID) == 0) return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiConnectAttempt = millis();
}

void maintainWifiTimeSync() {
    if (strlen(WIFI_SSID) == 0) return;

    if (WiFi.status() != WL_CONNECTED) {
        wifiNtpStarted = false;
        if (millis() - lastWifiConnectAttempt >= WIFI_RECONNECT_INTERVAL) {
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            lastWifiConnectAttempt = millis();
        }
        return;
    }

    if (!wifiNtpStarted) {
        sntp_set_time_sync_notification_cb(onWifiNtpSync);
        configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
        wifiNtpStarted = true;
    }

    if (wifiNtpSyncPending) {
        wifiNtpSyncPending = false;
        wifiTimeSynced = true;
        activeTimeSource = TimeSource::WIFI_NTP;
    }
}

void updateTimeFromSystemClock() {
    if (!desktopTimeSynced && !wifiTimeSynced) return;
    time_t utcNow = time(nullptr);
    long utcOffsetSeconds = activeTimeSource == TimeSource::DESKTOP
        ? desktopUtcOffsetSeconds
        : LOCAL_UTC_OFFSET_SECONDS;
    time_t localNow = utcNow + utcOffsetSeconds;
    struct tm current;
    gmtime_r(&localNow, &current);
    char timeBuffer[12];
    char dateBuffer[20];
    strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", &current);
    strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", &current);
    currentTimeStr = String(timeBuffer);
    currentDateStr = String(dateBuffer);
}

void setup() {
    Serial.begin(115200);
    desktopCommandBuffer.reserve(2048);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI);
    touchscreen.begin(); touchscreen.setRotation(1);
    tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK); u8f.begin(tft);
    lastInteractionMillis = millis();
    startWifiConnection();
    drawDisplay();
    sendDesktopTelemetry();
}

void loop() {
    unsigned long loopStart = micros();

    readDesktopCommands();
    maintainWifiTimeSync();
    if (WiFi.status() == WL_CONNECTED) {
        bool updateDue = currentWeather.isValid
            ? millis() - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL
            : millis() - lastWeatherAttempt >= WEATHER_RETRY_INTERVAL || lastWeatherAttempt == 0;
        if (updateDue) {
            fetchWeather();
            if (currentPage == 2) drawWeatherPage();
        }
        if (millis() - lastNewsUpdate >= 900000UL || (!newsValid && lastNewsUpdate == 0)) {
            fetchNews();
            if (currentPage == 3) drawNewsPage();
        }
    }
    if (millis() - lastTimeUpdate >= 1000) {
        if (!lyricActive) updateTimeFromSystemClock();
        if (currentPage == 0 && !lyricActive) {
            drawDisplay();
        }
        lastTimeUpdate = millis();
    }

    if (currentPage == 1 && millis() - lastInfoUpdate >= 1000) {
        refreshInfoPage();
        lastInfoUpdate = millis();
    }

    if (currentPage == 6 && pcStatusDirty && !screenHidden
        && millis() - lastPcStatusDraw >= 1000) {
        drawPcStatusPage();
        pcStatusDirty = false;
        lastPcStatusDraw = millis();
    }

    if (millis() - lastTelemetryUpdate >= 1000) {
        sendDesktopTelemetry();
        lastTelemetryUpdate = millis();
    }

    if (currentPage == 5 && timerRunning && millis() - lastTimerDraw >= 250) {
        bool timerFinished = false;
        if (timerCountdownMode && currentTimerValue() == 0) {
            timerElapsedMs = countdownDurationMs;
            timerRunning = false;
            timerFinished = true;
            timerAlarmActive = true;
            timerAlarmRed = true;
            lastTimerAlarmToggle = millis();
        }
        if (timerFinished) drawTimerPage(); else drawTimerValue();
        lastTimerDraw = millis();
    }

    if (currentPage == 5 && timerAlarmActive && millis() - lastTimerAlarmToggle >= 500) {
        timerAlarmRed = !timerAlarmRed;
        lastTimerAlarmToggle = millis();
        drawTimerValue();
    }

    if (currentPage == 2 && currentWeather.isValid && !weatherLoading) {
        unsigned long animationInterval = static_cast<unsigned long>(constrain(260.0f - currentWeather.windSpeedKmh * 8.0f, 70.0f, 260.0f));
        if (millis() - lastWindAnimation >= animationInterval) {
            windAnimationPhase = (windAnimationPhase + 3) % 27;
            lastWindAnimation = millis();
            drawWindAnimation();
        }
    }

    if (!screenHidden && millis() - lastInteractionMillis >= SCREEN_IDLE_TIMEOUT) {
        screenHidden = true;
        digitalWrite(TFT_BL, LOW);
    }

    if (currentPage == 0 && lyricActive && musicDurationSeconds > 0
        && millis() - lastMusicProgressDraw >= 500) {
        drawMusicProgress();
        lastMusicProgressDraw = millis();
    }

    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        lastInteractionMillis = millis();
        // 保存有效的触摸坐标
        lastTouchX = p.x;
        lastTouchY = p.y;
        if (screenHidden) {
            screenHidden = false; digitalWrite(TFT_BL, HIGH); drawDisplay();
        } else {
            if (startX == -1) { 
                startX = p.x; 
                startY = p.y; 
                isSwiping = false; // 重置滑动标记
            }
            else {
                int dx = p.x - startX;
                int dy = p.y - startY;

                if (currentPage == 1 && abs(dy) > abs(dx) && abs(dy) > 200) {
                    // 信息页纵向滑动：实时滚动
                    scrollOffset += dy / 20;
                    scrollOffset = constrain(scrollOffset, -500, 0);
                    drawDisplay();
                    startY = p.y; // 更新起始点以平滑滚动
                    isSwiping = true; // 标记为滑动操作
                } else if (currentPage == 2 && abs(dy) > abs(dx) && abs(dy) > 200) {
                    weatherScrollOffset += dy / 20;
                    weatherScrollOffset = constrain(weatherScrollOffset, -300, 0);
                    drawDisplay();
                    startY = p.y;
                    isSwiping = true;
                } else if (currentPage == 3 && abs(dy) > abs(dx) && abs(dy) > 200) {
                    if (newsDetail) {
                        newsDetailScrollOffset += dy / 20;
                        newsDetailScrollOffset = constrain(newsDetailScrollOffset, -100, 0);
                    } else {
                    newsScrollOffset += dy / 20;
                    int maxScroll = max(0, newsCount * 30 - 180);
                    newsScrollOffset = constrain(newsScrollOffset, -maxScroll, 0);
                    }
                    drawDisplay();
                    startY = p.y;
                    isSwiping = true;
                } else if (currentPage == 6 && abs(dy) > abs(dx) && abs(dy) > 200) {
                    pcScrollOffset += dy / 20;
                    pcScrollOffset = constrain(pcScrollOffset, -115, 0);
                    drawDisplay();
                    startY = p.y;
                    isSwiping = true;
                }
            }
        }
    } else {
        if (startX != -1) {
            // 使用保存的最后有效触摸坐标
            int finalX = lastTouchX;
            int finalY = lastTouchY;
            int finalDx = finalX - startX;
            int finalDy = finalY - startY;

            // 只有当不是滑动操作时才处理点击
            if (!isSwiping) {
                if (currentPage == 5 && abs(finalDx) < 200 && abs(finalDy) < 200) {
                    int screenX = constrain(map(finalX, 200, 3700, 0, SCREEN_WIDTH), 0, SCREEN_WIDTH - 1);
                    int screenY = constrain(map(finalY, 240, 3800, 0, SCREEN_HEIGHT), 0, SCREEN_HEIGHT - 1);
                    if (screenY >= 145 && screenY <= 205) {
                        if (screenX < 105) {
                            if (timerRunning) timerElapsedMs += millis() - timerStartedAt;
                            else timerStartedAt = millis();
                            timerRunning = !timerRunning;
                        } else if (screenX < 210) {
                            timerRunning = false; timerElapsedMs = 0;
                            if (timerCountdownMode) countdownDurationMs = 0;
                            timerAlarmActive = false; timerAlarmRed = false;
                        } else {
                            timerCountdownMode = !timerCountdownMode; timerRunning = false; timerElapsedMs = 0;
                        }
                    } else if (timerCountdownMode && screenY > 205) {
                        long changeMinutes = screenX < 80 ? -10 : (screenX < 160 ? -1 : (screenX < 240 ? 1 : 10));
                        long currentMinutes = countdownDurationMs / 60000UL;
                        currentMinutes = constrain(currentMinutes + changeMinutes, 1L, 5999L);
                        countdownDurationMs = static_cast<unsigned long>(currentMinutes) * 60000UL;
                        timerElapsedMs = 0;
                    }
                    drawDisplay();
                } else if (currentPage == 3 && abs(finalDx) < 200 && abs(finalDy) < 200) {
                    int screenX = constrain(map(finalX, 200, 3700, 0, SCREEN_WIDTH), 0, SCREEN_WIDTH - 1);
                    int screenY = constrain(map(finalY, 240, 3800, 0, SCREEN_HEIGHT), 0, SCREEN_HEIGHT - 1);
                    if (newsDetail && screenX < 90 && screenY < 45) {
                        newsDetail = false; newsDetailIndex = -1; newsDetailScrollOffset = 0; drawDisplay();
                    } else if (!newsDetail && screenY >= 30) {
                        int index = (screenY - 52 - newsScrollOffset) / 30;
                        if (index >= 0 && index < newsCount) { newsDetail = true; newsDetailIndex = index; newsDetailScrollOffset = 0; drawDisplay(); }
                    }
                } else if (abs(finalDx) < 200 && abs(finalDy) < 200 && handleMusicControlTap(finalX, finalY)) {
                    // 音乐控制点击已处理
                } else if (finalDx > SWIPE_MIN_X) { // 向右划：上一页
                    currentPage = (currentPage - 1 + MAX_PAGES) % MAX_PAGES;
                    scrollOffset = 0;
                    weatherScrollOffset = 0;
                    newsScrollOffset = 0;
                    pcScrollOffset = 0;
                    lastTimeStr = ""; // 重置时间显示状态
                    drawDisplay();
                } else if (finalDx < -SWIPE_MIN_X) { // 向左划：下一页
                    currentPage = (currentPage + 1) % MAX_PAGES;
                    scrollOffset = 0;
                    weatherScrollOffset = 0;
                    newsScrollOffset = 0;
                    pcScrollOffset = 0;
                    lastTimeStr = ""; // 重置时间显示状态
                    drawDisplay();
                }
            }
            
            // 重置触摸状态
            startX = -1; 
            startY = -1;
            isSwiping = false;
        }
    }

    if (lyricActive && millis() - lastLyricTime > 15000) {
        lyricActive = false; drawDisplay();
    }

    // 计算CPU使用率（每秒更新一次）
    unsigned long loopEnd = micros();
    activeTimeUs += (loopEnd - loopStart);
    if (millis() - lastCpuUpdate >= 1000) {
        cpuUsagePercent = (activeTimeUs / 10000.0); // 1秒=1000ms=1000000us, 百分比=active/10000
        if (cpuUsagePercent > 100.0) cpuUsagePercent = 100.0;
        activeTimeUs = 0;
        lastCpuUpdate = millis();
    }

    delay(10);
}
