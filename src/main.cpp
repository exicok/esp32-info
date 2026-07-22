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

int currentPage = 0;
#define MAX_PAGES 3

// 天气数据结构
struct WeatherData {
    String city;
    String temp;
    String feelsLike;
    String humidity;
    String windSpeed;
    String windDir;
    String weather;
    String icon;
    String updateTime;
    bool isValid;
};

struct ForecastData {
    String date;
    String high;
    String low;
    String weather;
    String icon;
};

WeatherData currentWeather = {"", "", "", "", "", "", "", "", "", false};
ForecastData forecast[7];
unsigned long lastWeatherUpdate = 0;
unsigned long lastWeatherAttempt = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 1800000; // 30分钟更新一次
const unsigned long WEATHER_RETRY_INTERVAL = 60000;     // 失败后1分钟重试
bool weatherLoading = false;
String weatherError = "";
String weatherCity = WEATHER_CITY; // 使用配置区的城市名称

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
        + "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m"
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
    currentWeather.feelsLike = String(current["apparent_temperature"].as<float>(), 1) + "°C";
    currentWeather.humidity = String(current["relative_humidity_2m"].as<int>()) + "%";
    currentWeather.windSpeed = String(current["wind_speed_10m"].as<float>(), 1) + " km/h";
    currentWeather.windDir = windDirectionDesc(current["wind_direction_10m"].as<float>());
    currentWeather.weather = wmoWeatherDesc(current["weather_code"].as<int>());
    currentWeather.updateTime = current["time"].as<String>();

    JsonObject daily = doc["daily"];
    if (daily.isNull()) {
        weatherError = "天气接口未返回预报";
        weatherLoading = false;
        return false;
    }

    for (int i = 0; i < 7; ++i) {
        if (i >= daily.size()) {
            forecast[i] = {"", "", "", "", ""};
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
    }
    if (millis() - lastTimeUpdate >= 1000) {
        if (!lyricActive) updateTimeFromSystemClock();
        if (currentPage == 0 && !lyricActive) {
            drawDisplay();
        } else if (currentPage == 1) {
            refreshInfoPage();
        }
        lastTimeUpdate = millis();
    }

    if (millis() - lastTelemetryUpdate >= 1000) {
        sendDesktopTelemetry();
        lastTelemetryUpdate = millis();
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
                if (abs(finalDx) < 200 && abs(finalDy) < 200 && handleMusicControlTap(finalX, finalY)) {
                    // 音乐控制点击已处理
                } else if (finalDx > SWIPE_MIN_X) { // 向右划：上一页
                    currentPage = (currentPage - 1 + MAX_PAGES) % MAX_PAGES;
                    scrollOffset = 0;
                    weatherScrollOffset = 0;
                    lastTimeStr = ""; // 重置时间显示状态
                    drawDisplay();
                } else if (finalDx < -SWIPE_MIN_X) { // 向左划：下一页
                    currentPage = (currentPage + 1) % MAX_PAGES;
                    scrollOffset = 0;
                    weatherScrollOffset = 0;
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
