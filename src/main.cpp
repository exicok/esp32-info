#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>

// ============ 用户配置区（可自定义修改） ============

// WiFi配置
const char* ssid = "ChinaNet-GWpJ";
const char* password = "12345678";

// 天气配置
const char* WEATHER_CITY = "祥云县";        // 城市名称
const float WEATHER_LAT = 25.48;            // 纬度
const float WEATHER_LON = 100.56;           // 经度

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

WiFiUDP ntpUDP;
NTPClient* timeClient = nullptr;
WiFiUDP lyricUDP;
unsigned int lyric_port = 330;

// 状态变量
String currentLyric = "";
String currentTranslation = "";
uint16_t currentLyricColor = TFT_YELLOW;
String currentTimeStr = "00:00:00";
String lastTimeStr = ""; // 用于局部刷新的上一次时间
String currentDateStr = "2024-01-01";
bool lyricActive = false;
bool screenHidden = false;
unsigned long lastLyricTime = 0;
unsigned long lastTimeUpdate = 0;

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
const unsigned long WEATHER_UPDATE_INTERVAL = 1800000; // 30分钟更新一次
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

// 天气API配置（使用Open-Meteo开源API，无需API key）
const char* WEATHER_API_BASE = "https://api.open-meteo.com/v1/forecast";

void fetchWeatherData() {
    if (weatherLoading) return;
    weatherLoading = true;
    weatherError = "";

    HTTPClient http;
    // Open-Meteo API: 使用坐标获取天气
    String url = String(WEATHER_API_BASE) +
        "?latitude=" + String(weatherLat, 4) +
        "&longitude=" + String(weatherLon, 4) +
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m" +
        "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
        "&timezone=Asia/Shanghai" +
        "&forecast_days=7";

    Serial.println("Fetching weather: " + url);
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(8192);
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            // 解析当前天气
            JsonObject current = doc["current"];
            currentWeather.city = weatherCity;
            currentWeather.temp = String(current["temperature_2m"].as<float>(), 1) + "°C";
            currentWeather.feelsLike = String(current["apparent_temperature"].as<float>(), 1) + "°C";
            currentWeather.humidity = String(current["relative_humidity_2m"].as<int>()) + "%";
            currentWeather.windSpeed = String(current["wind_speed_10m"].as<float>(), 1) + " km/h";

            int windDir = current["wind_direction_10m"].as<int>();
            if (windDir < 22 || windDir >= 337) currentWeather.windDir = "北";
            else if (windDir < 67) currentWeather.windDir = "东北";
            else if (windDir < 112) currentWeather.windDir = "东";
            else if (windDir < 157) currentWeather.windDir = "东南";
            else if (windDir < 202) currentWeather.windDir = "南";
            else if (windDir < 247) currentWeather.windDir = "西南";
            else if (windDir < 292) currentWeather.windDir = "西";
            else currentWeather.windDir = "西北";

            int weatherCode = current["weather_code"].as<int>();
            currentWeather.weather = wmoWeatherDesc(weatherCode);
            currentWeather.icon = currentWeather.weather;
            currentWeather.updateTime = currentDateStr + " " + currentTimeStr;
            currentWeather.isValid = true;

            // 解析未来7天预报
            JsonObject daily = doc["daily"];
            JsonArray timeArr = daily["time"];
            JsonArray maxTempArr = daily["temperature_2m_max"];
            JsonArray minTempArr = daily["temperature_2m_min"];
            JsonArray weatherCodeArr = daily["weather_code"];

            for (int i = 0; i < min(7, (int)timeArr.size()); i++) {
                forecast[i].date = timeArr[i].as<String>();
                forecast[i].high = String(maxTempArr[i].as<float>(), 1) + "°";
                forecast[i].low = String(minTempArr[i].as<float>(), 1) + "°";
                forecast[i].weather = wmoWeatherDesc(weatherCodeArr[i].as<int>());
                forecast[i].icon = forecast[i].weather;
            }

            Serial.println("Weather updated successfully");
        } else {
            weatherError = "JSON解析失败";
            currentWeather.isValid = false;
        }
    } else {
        weatherError = "HTTP错误: " + String(httpCode);
        currentWeather.isValid = false;
    }

    http.end();
    weatherLoading = false;
    // 无论成功失败都更新时间戳，避免频繁重试
    lastWeatherUpdate = millis();
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

    // 网络状态
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 网络状态 ---"); curY += lineHeight;
    drawRow("网络名称:", WiFi.SSID(), TFT_WHITE);
    drawRow("IP 地址:", WiFi.localIP().toString(), TFT_GREEN);
    drawRow("信号强度:", String(WiFi.RSSI()) + " dBm", TFT_WHITE);
    drawRow("网关地址:", WiFi.gatewayIP().toString(), TFT_WHITE);
    drawRow("MAC地址:", WiFi.macAddress(), TFT_WHITE);
    drawRow("子网掩码:", WiFi.subnetMask().toString(), TFT_WHITE);
    drawRow("DNS:", WiFi.dnsIP().toString(), TFT_WHITE);
    drawRow("信道:", String(WiFi.channel()), TFT_WHITE);
    drawRow("BSSID:", WiFi.BSSIDstr(), TFT_WHITE);

    curY += 10;
    // 硬件信息
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 硬件信息 ---"); curY += lineHeight;
    drawRow("CPU 频率:", String(ESP.getCpuFreqMHz()) + " MHz", TFT_WHITE);
    drawRow("芯片型号:", ESP.getChipModel(), TFT_WHITE);
    drawRow("芯片版本:", String(ESP.getChipRevision()), TFT_WHITE);
    drawRow("核心数:", String(ESP.getChipCores()), TFT_WHITE);
    drawRow("系统版本:", String(ESP.getSdkVersion()), TFT_WHITE);

    curY += 10;
    // 内存信息
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
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- ROM/Flash ---"); curY += lineHeight;
    drawRow("Flash大小:", String(ESP.getFlashChipSize() / 1024 / 1024) + " MB", TFT_WHITE);
    drawRow("Flash速度:", String(ESP.getFlashChipSpeed() / 1000000) + " MHz", TFT_WHITE);

    curY += 10;
    // 系统状态
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 系统状态 ---"); curY += lineHeight;
    char cpuBuf[16];
    sprintf(cpuBuf, "%.1f%%", cpuUsagePercent);
    drawRow("CPU占有率:", cpuBuf, TFT_CYAN);
    drawRow("开机时间:", formatUptime(millis()), TFT_WHITE);
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
        String tempNum = currentWeather.feelsLike;
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

void drawDisplay() {
    if (currentPage == 0) {
        if (lyricActive && currentLyric.length() > 0) {
            tft.fillScreen(TFT_BLACK);
            u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
            int lyricLines = drawWrappedTextCentered(currentLyric, 160, 100, SCREEN_WIDTH - 20, 22);
            if (currentTranslation.length() > 0) {
                u8f.setFont(u8g2_font_wqy12_t_gb2312); u8f.setForegroundColor(TFT_LIGHTGREY);
                drawWrappedTextCentered(currentTranslation, 160, 100 + lyricLines * 22 + 8, SCREEN_WIDTH - 20, 18);
            }
            lastTimeStr = ""; // 重置，下次显示时间时全量绘制
        } else {
            // 时间显示 - 局部刷新
            tft.setTextSize(5);
            tft.setTextDatum(CC_DATUM);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            
            // 计算时间字符串的宽度和每个字符的位置
            int charWidth = tft.textWidth("0"); // 单个数字的宽度
            int totalWidth = charWidth * 8; // "HH:MM:SS" 共8个字符
            int startX = (SCREEN_WIDTH - totalWidth) / 2;
            int y = 120;
            
            if (lastTimeStr.length() != currentTimeStr.length()) {
                // 首次显示或格式变化，全量绘制
                tft.fillScreen(TFT_BLACK);
                tft.drawString(currentTimeStr, 160, y);
            } else {
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
        drawInfoContent();
        drawTaskbar();
    } else if (currentPage == 2) {
        drawWeatherPage();
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

    // 首次获取天气数据
    fetchWeatherData();
    drawDisplay();
}

void loop() {
    unsigned long loopStart = micros();

    handleLyricPacket();

    if (millis() - lastTimeUpdate >= 1000) {
        timeClient->update();
        currentTimeStr = timeClient->getFormattedTime();
        unsigned long epochTime = timeClient->getEpochTime();
        struct tm* ptm = gmtime((time_t*)&epochTime);
        char dateBuffer[20]; sprintf(dateBuffer, "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
        currentDateStr = String(dateBuffer);
        // 天气页（currentPage == 2）不参与时间刷新
        // 时间页（currentPage == 0）非歌词模式时刷新，或信息页（currentPage == 1）时刷新
        if ((currentPage == 0 && !lyricActive) || currentPage == 1) drawDisplay();
        lastTimeUpdate = millis();
    }

    // 定期更新天气数据
    if (millis() - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL) {
        fetchWeatherData();
        if (currentPage == 2) drawDisplay();
    }

    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
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
                }
                else if (currentPage == 2 && abs(dy) > abs(dx) && abs(dy) > 200) {
                    // 天气页纵向滑动：实时滚动
                    weatherScrollOffset += dy / 20;
                    weatherScrollOffset = constrain(weatherScrollOffset, -300, 0);
                    drawDisplay();
                    startY = p.y; // 更新起始点以平滑滚动
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
                // 其他页面的横向滑动切换
                if (finalDx > SWIPE_MIN_X) { // 向右划：上一页
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
