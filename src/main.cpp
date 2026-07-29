#include <Arduino.h>
#include <SPI.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ============ 用户配置区（可自定义修改） ============

// 天气配置
const char* WEATHER_CITY = "云南大理祥云";  // 显示名称

const long LOCAL_UTC_OFFSET_SECONDS = 8 * 3600;

// ============ 配置区结束 ============

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite pcGraphSprite = TFT_eSprite(&tft);
bool pcGraphSpriteReady = false;
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
String desktopCommandBuffer = "";

enum class TimeSource { NONE, DESKTOP };
TimeSource activeTimeSource = TimeSource::NONE;


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
int pcCpuCurrentMHz = 0;
float pcCpuPowerWatts = -1;
float pcMemoryUsage = 0;
String pcGpuDriver = "";
String pcGpuNames[4];
String pcGpuVendors[4];
String pcGpuDriverVersions[4];
String pcGpuDriverDates[4];
int pcGpuMemoryMBs[4] = {0};
int pcGpuDedicatedUsedMBs[4] = {0};
int pcGpuSharedUsedMBs[4] = {0};
float pcGpuUsages[4] = {0};
int pcGpuFanPercents[4] = {-1, -1, -1, -1};
int pcGpuFanRpms[4] = {-1, -1, -1, -1};
float pcGpuPowerWattValues[4] = {-1, -1, -1, -1};
int pcGpuCount = 0;
String pcDiskSummary = "等待磁盘数据";
int pcScrollOffset = 0;
bool pcStatusDirty = false;
unsigned long lastPcStatusDraw = 0;
unsigned long lastPcScrollDraw = 0;
float pcCpuHistory[36] = {0};
float pcGpuHistory[36] = {0};
float pcFpsHistory[36] = {0};
float pcGpuHistories[4][36] = {{0}};
float pcCpuCoreUsages[32] = {0};
int pcCpuCoreCount = 0;
int pcCpuCoreRendered[32] = {-1};
bool pcFpsFullscreen = false;
float pcGpuUsage = 0;
float pcGpuPowerWatts = -1;
float pcFps = -1;
float pcFrameTimeMs = -1;
String pcGraphicsApi = "--";
unsigned long pcSyncLatencyMs = 0;
String pcRenderedValues[10];
String pcSummaryRenderedValues[8];
String pcGraphicsRenderedValues[2];
const int PC_DETAIL_OFFSET = 416;

int currentPage = 0;
#define MAX_PAGES 8

struct WorldClockRow {
    const char* name;
    int utcOffsetHours;
    uint16_t color;
};
const WorldClockRow WORLD_CLOCK_ROWS[] = {
    {"中国北京 UTC+8", 8, TFT_YELLOW},
    {"世界标准 UTC+0", 0, TFT_WHITE},
    {"英国伦敦 UTC+0", 0, TFT_GREEN},
    {"美国纽约 UTC-5", -5, TFT_CYAN},
    {"日本东京 UTC+9", 9, TFT_MAGENTA},
    {"澳洲悉尼 UTC+10", 10, TFT_ORANGE}
};
String worldClockRenderedValues[6];
bool worldClockLayoutReady = false;

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
unsigned long lastWindAnimation = 0;
uint8_t windAnimationPhase = 0;
String weatherCity = WEATHER_CITY; // 使用配置区的城市名称

bool timerCountdownMode = false;
bool timerRunning = false;
unsigned long timerStartedAt = 0;
unsigned long timerElapsedMs = 0;
unsigned long countdownDurationMs = 300000UL;
unsigned long lastTimerDraw = 0;
bool timerAlarmActive = false;
bool timerAlarmRed = false;
unsigned long lastTimerAlarmToggle = 0;
unsigned long lastTimerMemorySave = 0;
String lastTimerValueStr = "";
uint16_t lastTimerBackground = 0xFFFF;
Preferences timerPreferences;
String pendingPcControlAction = "";
unsigned long pendingPcControlUntil = 0;

int drawWrappedTextCentered(String text, int centerX, int startY, int maxW, int lineHeight);

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
    // 系统状态
    drawCard(curY - 18, 126);
    u8f.setForegroundColor(TFT_YELLOW);
    u8f.setCursor(10, curY); u8f.print("--- 系统状态 ---"); curY += lineHeight;
    char cpuBuf[16];
    sprintf(cpuBuf, "%.1f%%", cpuUsagePercent);
    drawRow("CPU占有率:", cpuBuf, TFT_CYAN);
    drawRow("开机时间:", formatUptime(millis()), TFT_WHITE);
    drawRow("同步延迟:", String(pcSyncLatencyMs) + " ms", pcSyncLatencyMs < 100 ? TFT_GREEN : TFT_YELLOW);
}

void drawWeatherPage();
void drawPcStatusPage();
void drawFpsFullscreen();

String windDirectionDesc(float degrees) {
    static const char* directions[] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
    int index = static_cast<int>((degrees + 22.5f) / 45.0f) % 8;
    return directions[index];
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

    if (!currentWeather.isValid) {
        u8f.setForegroundColor(TFT_YELLOW);
        u8f.setCursor(20, 60);
        u8f.print("等待电脑版天气数据");
        u8f.setForegroundColor(TFT_WHITE);
        u8f.setCursor(20, 90);
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

bool sendPcControl(const char* action) {
    StaticJsonDocument<96> doc;
    doc["type"] = "pc_control";
    doc["action"] = action;
    serializeJson(doc, Serial);
    Serial.println();
    return true;
}

void drawPcControlPage() {
    const char* labels[] = {"播放/暂停", "最小化", "最大化", "关闭窗口", "重启电脑", "关闭电脑"};
    const uint16_t colors[] = {TFT_CYAN, TFT_GREEN, TFT_GREEN, TFT_YELLOW, TFT_ORANGE, TFT_RED};
    tft.fillScreen(TFT_BLACK);
    u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
    u8f.setFont(u8g2_font_wqy14_t_gb2312); u8f.setForegroundColor(TFT_CYAN);
    u8f.setCursor(12, 24); u8f.print("电脑控制");
    u8f.setFont(u8g2_font_wqy12_t_gb2312); u8f.setForegroundColor(TFT_LIGHTGREY);
    if (pendingPcControlAction.length() && millis() < pendingPcControlUntil) {
        u8f.setForegroundColor(TFT_YELLOW); u8f.setCursor(105, 24); u8f.print("再次点击确认");
    } else {
        pendingPcControlAction = "";
        u8f.setCursor(88, 24); u8f.print("窗口操作作用于鼠标所在窗口");
    }
    for (int i = 0; i < 6; ++i) {
        int x = 8 + (i % 2) * 156;
        int y = 38 + (i / 2) * 64;
        tft.drawRoundRect(x, y, 148, 54, 5, colors[i]);
        u8f.setForegroundColor(colors[i]);
        int textWidth = u8f.getUTF8Width(labels[i]);
        u8f.setCursor(x + (148 - textWidth) / 2, y + 33); u8f.print(labels[i]);
    }
}

bool handlePcControlTap(int screenX, int screenY) {
    const char* actions[] = {"media-play-pause", "minimize", "maximize", "close", "restart", "shutdown"};
    if (screenY < 38 || screenY > 220) return false;
    int column = screenX < 160 ? 0 : 1;
    int row = (screenY - 38) / 64;
    if (row < 0 || row > 2) return false;
    if ((screenY - 38) % 64 >= 54) return false;
    int index = row * 2 + column;
    String action = actions[index];
    if (index >= 4) {
        if (pendingPcControlAction == action && millis() < pendingPcControlUntil) {
            sendPcControl(actions[index]);
            pendingPcControlAction = "";
            pendingPcControlUntil = 0;
        } else {
            pendingPcControlAction = action;
            pendingPcControlUntil = millis() + 4000;
        }
        drawPcControlPage();
        return true;
    }
    pendingPcControlAction = "";
    pendingPcControlUntil = 0;
    return sendPcControl(actions[index]);
}

bool handlePcStatusTap(int screenX, int screenY) {
    if (pcFpsFullscreen) {
        pcFpsFullscreen = false;
        pcScrollOffset = 0;
        drawPcStatusPage();
        return true;
    }
    int contentY = screenY - pcScrollOffset;
    if (screenX >= 10 && screenX <= 78 && contentY >= 30 && contentY <= 76) {
        pcFpsFullscreen = true;
        drawFpsFullscreen();
        return true;
    }
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

String formatWorldTime(time_t utcNow, int utcOffsetHours) {
    time_t localTime = utcNow + static_cast<time_t>(utcOffsetHours) * 3600;
    struct tm value;
    gmtime_r(&localTime, &value);
    char buffer[24];
    strftime(buffer, sizeof(buffer), "%m-%d %H:%M:%S", &value);
    return String(buffer);
}

void drawWorldTimePage();

void refreshWorldTimePage() {
    if (!desktopTimeSynced) return;
    if (!worldClockLayoutReady) {
        drawWorldTimePage();
        return;
    }

    time_t utcNow = time(nullptr);
    u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
    u8f.setFont(u8g2_font_wqy12_t_gb2312); u8f.setForegroundColor(TFT_WHITE);
    for (int i = 0; i < 6; ++i) {
        String value = formatWorldTime(utcNow, WORLD_CLOCK_ROWS[i].utcOffsetHours);
        if (value == worldClockRenderedValues[i]) continue;
        int y = 52 + i * 30;
        tft.fillRect(186, y - 16, 132, 20, TFT_BLACK);
        u8f.setCursor(188, y); u8f.print(value);
        worldClockRenderedValues[i] = value;
    }
}

void drawWorldTimePage() {
    tft.fillScreen(TFT_BLACK);
    worldClockLayoutReady = false;
    for (int i = 0; i < 6; ++i) worldClockRenderedValues[i] = "";
    u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
    u8f.setFont(u8g2_font_wqy14_t_gb2312); u8f.setForegroundColor(TFT_CYAN);
    u8f.setCursor(12, 22); u8f.print("世界时间（北京时间换算）");

    if (!desktopTimeSynced) {
        u8f.setFont(u8g2_font_wqy12_t_gb2312); u8f.setForegroundColor(TFT_YELLOW);
        u8f.setCursor(68, 125); u8f.print("等待北京时间同步...");
        return;
    }

    u8f.setFont(u8g2_font_wqy12_t_gb2312);
    for (int i = 0; i < 6; ++i) {
        int y = 52 + i * 30;
        u8f.setForegroundColor(WORLD_CLOCK_ROWS[i].color);
        u8f.setCursor(10, y); u8f.print(WORLD_CLOCK_ROWS[i].name);
    }
    worldClockLayoutReady = true;
    refreshWorldTimePage();
}

unsigned long currentTimerValue() {
    unsigned long elapsed = timerElapsedMs + (timerRunning ? millis() - timerStartedAt : 0);
    return timerCountdownMode ? (elapsed >= countdownDurationMs ? 0 : countdownDurationMs - elapsed) : elapsed;
}

void saveTimerMemory() {
    unsigned long savedElapsed = timerElapsedMs + (timerRunning ? millis() - timerStartedAt : 0);
    if (timerCountdownMode && savedElapsed > countdownDurationMs) savedElapsed = countdownDurationMs;
    timerPreferences.putBool("countdown", timerCountdownMode);
    timerPreferences.putULong("duration", countdownDurationMs);
    timerPreferences.putULong("elapsed", savedElapsed);
    lastTimerMemorySave = millis();
}

void loadTimerMemory() {
    timerCountdownMode = timerPreferences.getBool("countdown", false);
    countdownDurationMs = timerPreferences.getULong("duration", 300000UL);
    timerElapsedMs = timerPreferences.getULong("elapsed", 0);
    if (countdownDurationMs == 0) countdownDurationMs = 300000UL;
    if (timerCountdownMode && timerElapsedMs > countdownDurationMs)
        timerElapsedMs = countdownDurationMs;
    timerRunning = false;
    timerAlarmActive = false;
}

void drawTimerValue() {
    unsigned long value = currentTimerValue() / 1000;
    char buf[16]; sprintf(buf, "%02lu:%02lu:%02lu", value / 3600, (value / 60) % 60, value % 60);
    String currentValue = String(buf);
    uint16_t background = timerAlarmActive && timerAlarmRed ? TFT_RED : TFT_BLACK;
    tft.setTextDatum(MC_DATUM); tft.setTextSize(4); tft.setTextColor(TFT_WHITE, background);
    int charWidth = tft.textWidth("0");
    int startX = 160 - charWidth * 4;

    if (lastTimerValueStr.length() != currentValue.length() || lastTimerBackground != background) {
        tft.fillRect(20, 65, 280, 70, background);
        for (int i = 0; i < currentValue.length(); ++i) {
            int charX = startX + i * charWidth + charWidth / 2;
            tft.drawString(String(currentValue[i]), charX, 100);
        }
    } else {
        for (int i = 0; i < currentValue.length(); ++i) {
            if (currentValue[i] == lastTimerValueStr[i]) continue;
            int charX = startX + i * charWidth + charWidth / 2;
            tft.fillRect(charX - charWidth / 2, 70, charWidth, 60, background);
            tft.drawString(String(currentValue[i]), charX, 100);
        }
    }
    lastTimerValueStr = currentValue;
    lastTimerBackground = background;
    tft.setTextSize(1);
}

void drawTimerPage() {
    tft.fillScreen(TFT_BLACK); u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK);
    lastTimerValueStr = "";
    lastTimerBackground = 0xFFFF;
    u8f.setFont(u8g2_font_wqy14_t_gb2312); u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(15, 25);
    u8f.print(timerCountdownMode ? "倒计时" : "秒表计时");
    drawTimerValue(); u8f.setFont(u8g2_font_wqy12_t_gb2312);
    const char* labels[] = {timerRunning ? "暂停" : "开始", "复位", timerCountdownMode ? "秒表" : "倒计时"};
    for (int i = 0; i < 3; ++i) { int x = 15 + i * 103; tft.drawRoundRect(x, 155, 85, 40, 4, TFT_CYAN); u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(x + 25, 181); u8f.print(labels[i]); }
    if (timerCountdownMode) {
        const char* presets[] = {"5分", "15分", "30分", "60分"};
        for (int i = 0; i < 4; ++i) {
            int x = 5 + i * 79; tft.drawRoundRect(x, 205, 70, 30, 3, TFT_DARKGREY);
            u8f.setCursor(x + 20, 226); u8f.print(presets[i]);
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

void drawScaledPcGraph(int x, int y, int width, int height, const float* values,
                       float maximum, uint16_t color) {
    tft.drawRect(x, y, width, height, tft.color565(55, 65, 78));
    if (maximum <= 0) maximum = 100.0f;
    for (int i = 1; i < 36; ++i) {
        int x1 = x + 1 + (i - 1) * (width - 3) / 35;
        int x2 = x + 1 + i * (width - 3) / 35;
        int y1 = y + height - 2 - constrain(values[i - 1], 0.0f, maximum) * (height - 3) / maximum;
        int y2 = y + height - 2 - constrain(values[i], 0.0f, maximum) * (height - 3) / maximum;
        tft.drawLine(x1, y1, x2, y2, color);
    }
}

void drawBufferedPcGraph(int x, int y, int width, int height, const float* values,
                         float maximum, uint16_t color) {
    if (!pcGraphSpriteReady || width > 320 || height > 105) {
        if (maximum == 100.0f) drawPcGraph(x, y, width, height, values, color);
        else drawScaledPcGraph(x, y, width, height, values, maximum, color);
        return;
    }
    pcGraphSprite.fillSprite(TFT_BLACK);
    pcGraphSprite.drawRect(0, 0, width, height, tft.color565(55, 65, 78));
    if (maximum <= 0) maximum = 100.0f;
    for (int i = 1; i < 36; ++i) {
        int x1 = 1 + (i - 1) * (width - 3) / 35;
        int x2 = 1 + i * (width - 3) / 35;
        int y1 = height - 2 - constrain(values[i - 1], 0.0f, maximum) * (height - 3) / maximum;
        int y2 = height - 2 - constrain(values[i], 0.0f, maximum) * (height - 3) / maximum;
        pcGraphSprite.drawLine(x1, y1, x2, y2, color);
    }
    pcGraphSprite.pushSprite(x, y, 0, 0, width, height);
}

bool pcRegionVisible(int top, int height) {
    return top < SCREEN_HEIGHT && top + height > 0;
}

void drawCpuCorePanel(int o, bool force) {
    const int top = 212 + o;
    if (force) {
        tft.drawRoundRect(4, top, 312, 198, 6, tft.color565(55, 65, 78));
        u8f.setFont(u8g2_font_wqy12_t_gb2312);
        u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(12, top + 20); u8f.print("逻辑处理器核心占用");
        for (int i = 0; i < 32; ++i) pcCpuCoreRendered[i] = -1;
    }
    int count = min(pcCpuCoreCount, 32);
    for (int i = 0; i < count; ++i) {
        int value = constrain(static_cast<int>(pcCpuCoreUsages[i] + 0.5f), 0, 100);
        if (!force && value == pcCpuCoreRendered[i]) continue;
        int column = i % 4, row = i / 4;
        int x = 9 + column * 77, y = top + 28 + row * 20;
        tft.fillRect(x, y, 73, 18, TFT_BLACK);
        u8f.setForegroundColor(value >= 85 ? TFT_RED : (value >= 60 ? TFT_YELLOW : TFT_GREEN));
        u8f.setCursor(x + 2, y + 13); u8f.print(String(i) + " " + String(value) + "%");
        int barWidth = 20 * value / 100;
        tft.drawRect(x + 50, y + 4, 22, 8, tft.color565(55, 65, 78));
        if (barWidth > 0) tft.fillRect(x + 51, y + 5, barWidth, 6,
            value >= 85 ? TFT_RED : (value >= 60 ? TFT_YELLOW : TFT_GREEN));
        pcCpuCoreRendered[i] = value;
    }
}

void refreshFpsFullscreen() {
    String fpsText = pcFps >= 0 ? String(pcFps, 0) : String("--");
    if (pcGraphSpriteReady) {
        pcGraphSprite.fillSprite(TFT_BLACK);
        pcGraphSprite.setTextDatum(MC_DATUM);
        pcGraphSprite.setTextColor(pcFps >= 0 ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
        pcGraphSprite.setTextSize(6);
        pcGraphSprite.drawString(fpsText, SCREEN_WIDTH / 2, 49);
        pcGraphSprite.pushSprite(0, 42, 0, 0, SCREEN_WIDTH, 105);
    } else {
        tft.fillRect(0, 42, SCREEN_WIDTH, 105, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(pcFps >= 0 ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
        tft.setTextSize(6);
        tft.drawString(fpsText, SCREEN_WIDTH / 2, 91);
    }
    tft.setTextSize(1);
    tft.fillRect(0, 148, SCREEN_WIDTH, 28, TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString((pcFrameTimeMs >= 0 ? String(pcFrameTimeMs, 2) : "--") + " ms  " + pcGraphicsApi,
                   SCREEN_WIDTH / 2, 160);
    drawBufferedPcGraph(10, 181, 300, 50, pcFpsHistory, 240.0f, TFT_YELLOW);
}

void drawFpsFullscreen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2); tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("FPS", SCREEN_WIDTH / 2, 22);
    refreshFpsFullscreen();
}

String pcSummaryValue(int index) {
    switch (index) {
        case 0: return pcFps >= 0 ? String(pcFps, 1) : "--";
        case 1: return String(pcCpuUsage, 1) + "%";
        case 2: return String(pcMemoryUsage, 1) + "%";
        case 3: return String(pcGpuUsage, 1) + "%";
        case 4: return String(pcCpuCurrentMHz) + "M";
        case 5: return (pcCpuPowerWatts >= 0 ? String(pcCpuPowerWatts, 0) : "--")
            + "/" + (pcGpuPowerWatts >= 0 ? String(pcGpuPowerWatts, 0) : "--") + "W";
        case 6: return String(pcGpuDedicatedUsedMBs[0]) + "M";
        case 7: return pcGpuFanRpms[0] >= 0 ? String(pcGpuFanRpms[0]) + "R"
            : (pcGpuFanPercents[0] >= 0 ? String(pcGpuFanPercents[0]) + "%" : "--");
        default: return "--";
    }
}

void drawPcSummaryCards(int o) {
    const char* labels[] = {"FPS", "CPU", "内存", "GPU", "CPU频率", "CPU/GPU功率", "显存", "风扇"};
    const uint16_t colors[] = {TFT_YELLOW, TFT_GREEN, TFT_MAGENTA, TFT_CYAN,
        TFT_GREEN, TFT_YELLOW, TFT_MAGENTA, TFT_CYAN};
    u8f.setFont(u8g2_font_wqy12_t_gb2312);
    tft.drawRoundRect(4, 5 + o, 154, 130, 6, tft.color565(55, 65, 78));
    tft.drawRoundRect(162, 5 + o, 154, 130, 6, tft.color565(55, 65, 78));
    u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(10, 23 + o); u8f.print("实时状态");
    u8f.setCursor(168, 23 + o); u8f.print("硬件状态");
    for (int i = 0; i < 8; ++i) {
        int groupX = i < 4 ? 4 : 162;
        int local = i % 4;
        int x = groupX + 6 + (local % 2) * 72;
        int y = 30 + (local / 2) * 50 + o;
        tft.drawRoundRect(x, y, 68, 46, 4, tft.color565(42, 52, 65));
        u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(x + 5, y + 15); u8f.print(labels[i]);
        u8f.setForegroundColor(colors[i]); u8f.setCursor(x + 5, y + 38);
        pcSummaryRenderedValues[i] = pcSummaryValue(i);
        u8f.print(pcSummaryRenderedValues[i]);
    }
    tft.drawRoundRect(4, 140 + o, 312, 66, 6, tft.color565(55, 65, 78));
    const char* graphicsLabels[] = {"图形 API", "帧延迟"};
    pcGraphicsRenderedValues[0] = pcGraphicsApi;
    pcGraphicsRenderedValues[1] = pcFrameTimeMs >= 0 ? String(pcFrameTimeMs, 2) + " ms" : "-- ms";
    for (int i = 0; i < 2; ++i) {
        int x = 10 + i * 154;
        tft.drawRoundRect(x, 147 + o, 146, 52, 4, tft.color565(42, 52, 65));
        u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(x + 7, 164 + o); u8f.print(graphicsLabels[i]);
        u8f.setForegroundColor(i == 0 ? TFT_CYAN : TFT_YELLOW);
        u8f.setCursor(x + 7, 190 + o); u8f.print(pcGraphicsRenderedValues[i]);
    }
}

void refreshPcSummaryCards(int o) {
    const uint16_t colors[] = {TFT_YELLOW, TFT_GREEN, TFT_MAGENTA, TFT_CYAN,
        TFT_GREEN, TFT_YELLOW, TFT_MAGENTA, TFT_CYAN};
    for (int i = 0; i < 8; ++i) {
        String value = pcSummaryValue(i);
        if (value == pcSummaryRenderedValues[i]) continue;
        int groupX = i < 4 ? 4 : 162;
        int local = i % 4;
        int x = groupX + 6 + (local % 2) * 72;
        int y = 30 + (local / 2) * 50 + o;
        tft.fillRect(x + 2, y + 19, 64, 25, TFT_BLACK);
        u8f.setForegroundColor(colors[i]); u8f.setCursor(x + 5, y + 38); u8f.print(value);
        pcSummaryRenderedValues[i] = value;
    }
    String graphicsValues[] = {pcGraphicsApi,
        pcFrameTimeMs >= 0 ? String(pcFrameTimeMs, 2) + " ms" : "-- ms"};
    for (int i = 0; i < 2; ++i) {
        if (graphicsValues[i] == pcGraphicsRenderedValues[i]) continue;
        int x = 10 + i * 154;
        tft.fillRect(x + 2, 168 + o, 142, 29, TFT_BLACK);
        u8f.setForegroundColor(i == 0 ? TFT_CYAN : TFT_YELLOW);
        u8f.setCursor(x + 7, 190 + o); u8f.print(graphicsValues[i]);
        pcGraphicsRenderedValues[i] = graphicsValues[i];
    }
}

void cachePcRenderedValues() {
    pcRenderedValues[0] = pcCpuName.substring(0, 28);
    pcRenderedValues[1] = String(pcCpuUsage, 1) + "%";
    pcRenderedValues[2] = String(pcCpuPhysicalCores) + "核/" + String(pcCpuCores) + "线程";
    pcRenderedValues[3] = String(pcCpuCurrentMHz) + " MHz  "
        + (pcCpuPowerWatts >= 0 ? String(pcCpuPowerWatts, 1) + " W" : "-- W");
    pcRenderedValues[4] = pcGpuName.substring(0, 31);
    pcRenderedValues[5] = String(pcGpuUsage, 1) + "%  "
        + (pcFps >= 0 ? String(pcFps, 1) + " FPS" : "FPS --");
    pcRenderedValues[6] = "显存已用 " + String(pcGpuDedicatedUsedMBs[0]) + " MB";
    pcRenderedValues[7] = pcGpuFanRpms[0] >= 0
        ? "风扇 " + String(pcGpuFanRpms[0]) + " RPM"
        : (pcGpuFanPercents[0] >= 0 ? "风扇 " + String(pcGpuFanPercents[0]) + "%" : "风扇不可用");
    pcRenderedValues[8] = "内存 " + String(pcMemoryUsedMB) + "/" + String(pcMemoryTotalMB) + " MB";
    pcRenderedValues[9] = "占用 " + String(pcMemoryUsage, 1) + "%";
}

void refreshPcStatusPage() {
    if (pcFpsFullscreen) {
        refreshFpsFullscreen();
        pcStatusDirty = false;
        lastPcStatusDraw = millis();
        return;
    }
    int o = pcScrollOffset;
    String values[10] = {
        pcCpuName.substring(0, 28), String(pcCpuUsage, 1) + "%",
        String(pcCpuPhysicalCores) + "核/" + String(pcCpuCores) + "线程",
        String(pcCpuCurrentMHz) + " MHz  "
            + (pcCpuPowerWatts >= 0 ? String(pcCpuPowerWatts, 1) + " W" : "-- W"), pcGpuName.substring(0, 31),
        String(pcGpuUsage, 1) + "%  " + (pcFps >= 0 ? String(pcFps, 1) + " FPS" : "FPS --"),
        "显存已用 " + String(pcGpuDedicatedUsedMBs[0]) + " MB",
        pcGpuFanRpms[0] >= 0 ? "风扇 " + String(pcGpuFanRpms[0]) + " RPM"
            : (pcGpuFanPercents[0] >= 0 ? "风扇 " + String(pcGpuFanPercents[0]) + "%" : "风扇不可用"),
        "内存 " + String(pcMemoryUsedMB) + "/" + String(pcMemoryTotalMB) + " MB",
        "占用 " + String(pcMemoryUsage, 1) + "%"
    };
    const int x[10] = {70, 14, 14, 14, 55, 14, 14, 14, 14, 14};
    const int y[10] = {24 + PC_DETAIL_OFFSET, 57 + PC_DETAIL_OFFSET,
        82 + PC_DETAIL_OFFSET, 103 + PC_DETAIL_OFFSET, 142 + PC_DETAIL_OFFSET,
        176 + PC_DETAIL_OFFSET, 202 + PC_DETAIL_OFFSET, 222 + PC_DETAIL_OFFSET,
        292 + PC_DETAIL_OFFSET, 316 + PC_DETAIL_OFFSET};
    const int width[10] = {238, 105, 105, 105, 253, 105, 105, 105, 292, 292};
    const uint16_t color[10] = {TFT_WHITE, TFT_GREEN, TFT_LIGHTGREY, TFT_LIGHTGREY,
        TFT_WHITE, TFT_CYAN, TFT_LIGHTGREY, TFT_LIGHTGREY, TFT_WHITE, TFT_CYAN};

    u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK); u8f.setFont(u8g2_font_wqy12_t_gb2312);
    if (pcRegionVisible(o, 206)) refreshPcSummaryCards(o);
    if (pcRegionVisible(212 + o, 198)) drawCpuCorePanel(o, false);
    for (int i = 0; i < 10; ++i) {
        if (!pcRegionVisible(y[i] - 18 + o, 22)) continue;
        if (values[i] == pcRenderedValues[i]) continue;
        tft.fillRect(x[i] - 2, y[i] - 16 + o, width[i], 19, TFT_BLACK);
        u8f.setForegroundColor(color[i]);
        u8f.setCursor(x[i], y[i] + o); u8f.print(values[i]);
        pcRenderedValues[i] = values[i];
    }
    if (pcRegionVisible(42 + PC_DETAIL_OFFSET + o, 58))
        drawBufferedPcGraph(125, 42 + PC_DETAIL_OFFSET + o, 178, 58, pcCpuHistory, 100.0f, TFT_GREEN);
    if (pcRegionVisible(159 + PC_DETAIL_OFFSET + o, 60))
        drawBufferedPcGraph(125, 159 + PC_DETAIL_OFFSET + o, 178, 60, pcGpuHistory, 100.0f, TFT_CYAN);

    static String renderedDisk;
    if (renderedDisk != pcDiskSummary) {
        tft.fillRect(12, 322 + PC_DETAIL_OFFSET + o, 296, 19, TFT_BLACK);
        u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(14, 338 + PC_DETAIL_OFFSET + o); u8f.print(pcDiskSummary);
        renderedDisk = pcDiskSummary;
    }
    static String renderedGpuIdentity[4];
    static String renderedGpuMetrics[4];
    if (pcGpuCount > 0) {
        for (int i = 0; i < pcGpuCount; ++i) {
            String identity = pcGpuNames[i] + pcGpuDriverVersions[i] + pcGpuDriverDates[i];
            String metrics = pcGpuVendors[i] + String(pcGpuUsages[i], 1)
                + String(pcGpuDedicatedUsedMBs[i]) + String(pcGpuSharedUsedMBs[i])
                + String(pcGpuFanPercents[i]) + String(pcGpuFanRpms[i]);
            int top = 382 + PC_DETAIL_OFFSET + i * 180 + o;
            if (!pcRegionVisible(top, 170)) continue;
            if (identity != renderedGpuIdentity[i]) {
                tft.fillRect(10, top + 3, 300, 20, TFT_BLACK);
                u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(12, top + 20);
                u8f.print(String(i + 1) + ". " + pcGpuNames[i].substring(0, 28));
                tft.fillRect(10, top + 90, 300, 20, TFT_BLACK);
                u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(12, top + 107);
                u8f.print("驱动 " + pcGpuDriverVersions[i].substring(0, 18) + " " + pcGpuDriverDates[i]);
                renderedGpuIdentity[i] = identity;
            }
            if (metrics != renderedGpuMetrics[i]) {
                tft.fillRect(10, top + 25, 300, 64, TFT_BLACK);
                u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(12, top + 42);
                u8f.print(pcGpuVendors[i].substring(0, 15) + " 核心 " + String(pcGpuUsages[i], 1) + "% "
                    + (pcGpuFanRpms[i] >= 0 ? "风扇 " + String(pcGpuFanRpms[i]) + "RPM"
                        : (pcGpuFanPercents[i] >= 0 ? "风扇 " + String(pcGpuFanPercents[i]) + "%" : "风扇不可用")));
                u8f.setCursor(12, top + 64); u8f.print("独显已用 " + String(pcGpuDedicatedUsedMBs[i]) + " MB");
                u8f.setCursor(12, top + 86); u8f.print("共享 " + String(pcGpuSharedUsedMBs[i]) + " MB");
                renderedGpuMetrics[i] = metrics;
            }
            drawBufferedPcGraph(12, top + 114, 296, 50, pcGpuHistories[i], 100.0f, TFT_CYAN);
        }
    }
    pcStatusDirty = false;
    lastPcStatusDraw = millis();
}

void drawPcStatusPage() {
    if (pcFpsFullscreen) {
        drawFpsFullscreen();
        return;
    }
    tft.fillScreen(TFT_BLACK);
    u8f.setFontMode(1); u8f.setBackgroundColor(TFT_BLACK); u8f.setFont(u8g2_font_wqy12_t_gb2312);
    int o = pcScrollOffset;
    int d = PC_DETAIL_OFFSET;

    drawPcSummaryCards(o);
    drawCpuCorePanel(o, true);

    tft.drawRoundRect(4, 5 + d + o, 312, 110, 6, tft.color565(55, 65, 78));
    u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(12, 24 + d + o); u8f.print("处理器");
    u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(70, 24 + d + o); u8f.print(pcCpuName.substring(0, 28));
    u8f.setForegroundColor(TFT_GREEN); u8f.setCursor(14, 57 + d + o); u8f.print(String(pcCpuUsage, 1) + "%");
    u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(14, 82 + d + o);
    u8f.print(String(pcCpuPhysicalCores) + "核/" + String(pcCpuCores) + "线程");
    u8f.setCursor(14, 103 + d + o); u8f.print(String(pcCpuCurrentMHz) + " MHz  "
        + (pcCpuPowerWatts >= 0 ? String(pcCpuPowerWatts, 1) + " W" : "-- W"));
    drawPcGraph(125, 42 + d + o, 178, 58, pcCpuHistory, TFT_GREEN);

    tft.drawRoundRect(4, 122 + d + o, 312, 113, 6, tft.color565(55, 65, 78));
    u8f.setForegroundColor(TFT_YELLOW); u8f.setCursor(12, 142 + d + o); u8f.print("显卡");
    u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(55, 142 + d + o); u8f.print(pcGpuName.substring(0, 31));
    u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(14, 176 + d + o);
    u8f.print(String(pcGpuUsage, 1) + "%  " + (pcFps >= 0 ? String(pcFps, 1) + " FPS" : "FPS --"));
    u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(14, 202 + d + o);
    u8f.print("显存已用 " + String(pcGpuDedicatedUsedMBs[0]) + " MB");
    u8f.setCursor(14, 222 + d + o); u8f.print(pcGpuFanRpms[0] >= 0
        ? "风扇 " + String(pcGpuFanRpms[0]) + " RPM"
        : (pcGpuFanPercents[0] >= 0 ? "风扇 " + String(pcGpuFanPercents[0]) + "%" : "风扇不可用"));
    drawPcGraph(125, 159 + d + o, 178, 60, pcGpuHistory, TFT_CYAN);

    tft.drawRoundRect(4, 242 + d + o, 312, 105, 6, tft.color565(55, 65, 78));
    u8f.setForegroundColor(TFT_MAGENTA); u8f.setCursor(12, 264 + d + o); u8f.print("内存与磁盘");
    u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(14, 292 + d + o); u8f.print("内存 " + String(pcMemoryUsedMB) + "/" + String(pcMemoryTotalMB) + " MB");
    u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(14, 316 + d + o); u8f.print("占用 " + String(pcMemoryUsage, 1) + "%");
    u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(14, 338 + d + o); u8f.print(pcDiskSummary);
    if (pcGpuCount > 0) {
        int cardHeight = 35 + pcGpuCount * 180;
        tft.drawRoundRect(4, 354 + d + o, 312, cardHeight, 6, tft.color565(55, 65, 78));
        u8f.setForegroundColor(TFT_YELLOW); u8f.setCursor(12, 375 + d + o); u8f.print("显卡详细状态");
        for (int i = 0; i < pcGpuCount; ++i) {
            int top = 382 + d + i * 180 + o;
            if (!pcRegionVisible(top, 170)) continue;
            u8f.setForegroundColor(TFT_CYAN); u8f.setCursor(12, top + 20);
            u8f.print(String(i + 1) + ". " + pcGpuNames[i].substring(0, 28));
            u8f.setForegroundColor(TFT_WHITE); u8f.setCursor(12, top + 42);
            u8f.print(pcGpuVendors[i].substring(0, 15) + " 核心 " + String(pcGpuUsages[i], 1) + "% "
                + (pcGpuFanRpms[i] >= 0 ? "风扇 " + String(pcGpuFanRpms[i]) + "RPM"
                    : (pcGpuFanPercents[i] >= 0 ? "风扇 " + String(pcGpuFanPercents[i]) + "%" : "风扇不可用")));
            u8f.setCursor(12, top + 64); u8f.print("独显已用 " + String(pcGpuDedicatedUsedMBs[i]) + " MB");
            u8f.setCursor(12, top + 86); u8f.print("共享 " + String(pcGpuSharedUsedMBs[i]) + " MB");
            u8f.setForegroundColor(TFT_LIGHTGREY); u8f.setCursor(12, top + 107);
            u8f.print("驱动 " + pcGpuDriverVersions[i].substring(0, 18) + " " + pcGpuDriverDates[i]);
            drawPcGraph(12, top + 114, 296, 50, pcGpuHistories[i], TFT_CYAN);
        }
    }
    cachePcRenderedValues();
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
        drawPcControlPage();
    } else if (currentPage == 4) {
        drawCalendarPage();
    } else if (currentPage == 5) {
        drawTimerPage();
    } else if (currentPage == 6) {
        drawPcStatusPage();
    } else if (currentPage == 7) {
        drawWorldTimePage();
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
    doc["timeSynced"] = desktopTimeSynced;
    doc["timeSource"] = activeTimeSource == TimeSource::DESKTOP ? "pc-via-com" : "unsynced";
    doc["date"] = currentDateStr;
    doc["time"] = currentTimeStr;
    serializeJson(doc, Serial);
    Serial.println();
}

void handleDesktopCommand(const String& line) {
    DynamicJsonDocument doc(8192);
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
    } else if (strcmp(cmd, "screen_on") == 0) {
        screenHidden = false;
        lastInteractionMillis = millis();
        digitalWrite(TFT_BL, HIGH);
        drawDisplay();
        sendDesktopAck(cmd, "屏幕已开启");
    } else if (strcmp(cmd, "set_page") == 0) {
        int requestedPage = constrain(doc["page"] | 0, 0, MAX_PAGES - 1);
        if (requestedPage != 6) pcFpsFullscreen = false;
        if (requestedPage != 0) {
            lyricActive = false;
            currentLyric = "";
            currentTranslation = "";
        }
        currentPage = requestedPage;
        scrollOffset = 0;
        weatherScrollOffset = 0;
        pcScrollOffset = 0;
        lastTimeStr = "";
        screenHidden = false;
        lastInteractionMillis = millis();
        digitalWrite(TFT_BL, HIGH);
        drawDisplay();
        sendDesktopAck(cmd, "页面已切换");
    } else if (strcmp(cmd, "weather_update") == 0) {
        JsonObject current = doc["current"];
        JsonArray daily = doc["daily"];
        if (current.isNull() || daily.size() < 7) {
            sendDesktopAck(cmd, "电脑版天气数据无效");
            return;
        }
        weatherCity = doc["city"] | WEATHER_CITY;
        currentWeather.city = weatherCity;
        currentWeather.temp = String(current["temperature"].as<float>(), 1) + "°C";
        currentWeather.humidity = String(current["humidity"].as<int>()) + "%";
        currentWeather.windSpeedKmh = current["windSpeed"] | 0.0f;
        currentWeather.windSpeed = String(currentWeather.windSpeedKmh, 1) + " km/h";
        currentWeather.windDirectionDegrees = current["windDirection"] | 0.0f;
        currentWeather.windDir = windDirectionDesc(currentWeather.windDirectionDegrees);
        currentWeather.weather = wmoWeatherDesc(current["weatherCode"] | -1);
        for (int i = 0; i < 7; ++i) {
            forecast[i].date = daily[i]["date"].as<String>();
            forecast[i].high = String(daily[i]["high"].as<float>(), 0) + "°";
            forecast[i].low = String(daily[i]["low"].as<float>(), 0) + "°";
            forecast[i].weather = wmoWeatherDesc(daily[i]["weatherCode"] | -1);
        }
        currentWeather.isValid = true;
        if (currentPage == 2 && !screenHidden) drawWeatherPage();
        sendDesktopAck(cmd, "电脑版天气已更新");
    } else if (strcmp(cmd, "pc_status") == 0) {
        bool pauseCpu = doc["pauseCpu"] | false;
        bool pauseMemory = doc["pauseMemory"] | false;
        bool pauseGpu = doc["pauseGpu"] | false;
        bool pauseFps = doc["pauseFps"] | false;
        if (!pauseCpu) {
            pcCpuName = doc["cpuName"].as<String>();
            pcCpuUsage = doc["cpuUsage"] | 0.0f;
            pcCpuCores = doc["cpuCores"] | 0;
            pcCpuPhysicalCores = doc["cpuPhysicalCores"] | 0;
            pcCpuMaxMHz = doc["cpuMaxMHz"] | 0;
            pcCpuCurrentMHz = doc["cpuCurrentMHz"] | pcCpuMaxMHz;
            pcCpuPowerWatts = doc["cpuPowerWatts"] | -1.0f;
            JsonArray coreUsages = doc["cpuCoreUsages"].as<JsonArray>();
            if (!coreUsages.isNull()) {
                pcCpuCoreCount = 0;
                for (JsonVariant usage : coreUsages) {
                    if (pcCpuCoreCount >= 32) break;
                    pcCpuCoreUsages[pcCpuCoreCount++] = usage.as<float>();
                }
            }
        }
        if (!pauseMemory) {
            pcMemoryUsedMB = doc["memoryUsedMB"] | 0;
            pcMemoryTotalMB = doc["memoryTotalMB"] | 0;
            pcMemoryUsage = doc["memoryUsage"] | 0.0f;
        }
        if (!pauseGpu) {
            pcGpuName = doc["gpuName"].as<String>();
            pcGpuUsage = doc["gpuUsage"] | 0.0f;
            pcGpuMemoryMB = doc["gpuMemoryMB"] | 0;
            pcGpuFanPercents[0] = doc["gpuFanPercent"] | -1;
            pcGpuFanRpms[0] = doc["gpuFanRpm"] | -1;
            pcGpuPowerWatts = doc["gpuPowerWatts"] | -1.0f;
            pcGpuDriver = doc["gpuDriver"].as<String>();
        }
        if (!pauseFps) {
            pcFps = doc["fps"] | -1.0f;
            pcFrameTimeMs = doc["frameTimeMs"] | -1.0f;
            pcGraphicsApi = doc["graphicsApi"] | "--";
            for (int i = 0; i < 35; ++i) pcFpsHistory[i] = pcFpsHistory[i + 1];
            pcFpsHistory[35] = max(pcFps, 0.0f);
        }
        JsonArray gpuArray = doc["gpus"].as<JsonArray>();
        if (!pauseGpu && !gpuArray.isNull()) {
            pcGpuCount = 0;
            for (JsonObject gpu : gpuArray) {
                if (pcGpuCount >= 4) break;
                pcGpuNames[pcGpuCount] = gpu["name"].as<String>();
                pcGpuVendors[pcGpuCount] = gpu["vendor"].as<String>();
                pcGpuDriverVersions[pcGpuCount] = gpu["driverVersion"].as<String>();
                pcGpuDriverDates[pcGpuCount] = gpu["driverDate"].as<String>();
                pcGpuMemoryMBs[pcGpuCount] = gpu["memoryMB"] | 0;
                pcGpuDedicatedUsedMBs[pcGpuCount] = gpu["dedicatedUsedMB"] | 0;
                pcGpuSharedUsedMBs[pcGpuCount] = gpu["sharedUsedMB"] | 0;
                pcGpuUsages[pcGpuCount] = gpu["usage"] | 0.0f;
                pcGpuFanPercents[pcGpuCount] = gpu["fanPercent"] | -1;
                pcGpuFanRpms[pcGpuCount] = gpu["fanRpm"] | -1;
                pcGpuPowerWattValues[pcGpuCount] = gpu["powerWatts"] | -1.0f;
                ++pcGpuCount;
            }
        }
        uint64_t sentEpochMs = doc["sentEpochMs"] | 0ULL;
        if (sentEpochMs > 0) {
            struct timeval now;
            gettimeofday(&now, nullptr);
            uint64_t nowEpochMs = static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_usec / 1000ULL;
            uint64_t latencyMs = nowEpochMs >= sentEpochMs ? nowEpochMs - sentEpochMs : 0;
            pcSyncLatencyMs = static_cast<unsigned long>(latencyMs > 9999ULL ? 9999ULL : latencyMs);
        }
        JsonArray disks = doc["disks"].as<JsonArray>();
        if (!pauseMemory && !disks.isNull() && disks.size() > 0) {
            int freeMB = disks[0]["freeMB"] | 0;
            int totalMB = disks[0]["totalMB"] | 0;
            pcDiskSummary = disks[0]["name"].as<String>() + " " + String(freeMB / 1024.0f, 1) + "/" + String(totalMB / 1024.0f, 1) + " GB 可用";
        }
        if (!pauseCpu) {
            for (int i = 0; i < 35; ++i) pcCpuHistory[i] = pcCpuHistory[i + 1];
            pcCpuHistory[35] = pcCpuUsage;
        }
        if (!pauseGpu) {
            for (int i = 0; i < 35; ++i) pcGpuHistory[i] = pcGpuHistory[i + 1];
            pcGpuHistory[35] = pcGpuUsage;
            for (int gpu = 0; gpu < pcGpuCount; ++gpu) {
                for (int i = 0; i < 35; ++i) pcGpuHistories[gpu][i] = pcGpuHistories[gpu][i + 1];
                pcGpuHistories[gpu][35] = pcGpuUsages[gpu];
            }
        }
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
        } else if (c != '\r' && desktopCommandBuffer.length() < 6144) {
            desktopCommandBuffer += c;
        }
    }
}

void updateTimeFromSystemClock() {
    if (!desktopTimeSynced) return;
    time_t utcNow = time(nullptr);
    time_t localNow = utcNow + desktopUtcOffsetSeconds;
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
    timerPreferences.begin("timer", false);
    loadTimerMemory();
    desktopCommandBuffer.reserve(6144);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI);
    touchscreen.begin(); touchscreen.setRotation(1);
    tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK); u8f.begin(tft);
    pcGraphSprite.setColorDepth(8);
    pcGraphSpriteReady = pcGraphSprite.createSprite(320, 105) != nullptr;
    lastInteractionMillis = millis();
    drawDisplay();
    sendDesktopTelemetry();
}

void loop() {
    unsigned long loopStart = micros();

    readDesktopCommands();
    if (millis() - lastTimeUpdate >= 1000) {
        if (!lyricActive) updateTimeFromSystemClock();
        if (currentPage == 0 && !lyricActive) {
            drawDisplay();
        } else if (currentPage == 7 && !screenHidden) {
            refreshWorldTimePage();
        }
        lastTimeUpdate = millis();
    }

    if (currentPage == 1 && millis() - lastInfoUpdate >= 1000) {
        refreshInfoPage();
        lastInfoUpdate = millis();
    }

    unsigned long pcRefreshInterval = pcFpsFullscreen ? 100UL : 250UL;
    if (currentPage == 6 && pcStatusDirty && !screenHidden
        && millis() - lastPcStatusDraw >= pcRefreshInterval) {
        refreshPcStatusPage();
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
            saveTimerMemory();
        }
        if (timerFinished) drawTimerPage(); else drawTimerValue();
        lastTimerDraw = millis();
    }

    if (currentPage == 5 && timerAlarmActive && millis() - lastTimerAlarmToggle >= 500) {
        timerAlarmRed = !timerAlarmRed;
        lastTimerAlarmToggle = millis();
        drawTimerValue();
    }

    if (timerRunning && millis() - lastTimerMemorySave >= 30000UL) {
        saveTimerMemory();
    }

    if (currentPage == 3 && pendingPcControlAction.length()
        && millis() >= pendingPcControlUntil) {
        pendingPcControlAction = "";
        pendingPcControlUntil = 0;
        drawPcControlPage();
    }

    if (currentPage == 2 && currentWeather.isValid) {
        unsigned long animationInterval = static_cast<unsigned long>(constrain(260.0f - currentWeather.windSpeedKmh * 8.0f, 70.0f, 260.0f));
        if (millis() - lastWindAnimation >= animationInterval) {
            windAnimationPhase = (windAnimationPhase + 3) % 27;
            lastWindAnimation = millis();
            drawWindAnimation();
        }
    }

    if (currentPage != 5 && currentPage != 6 && !screenHidden
        && millis() - lastInteractionMillis >= SCREEN_IDLE_TIMEOUT) {
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
                } else if (currentPage == 6 && !pcFpsFullscreen
                           && abs(dy) > abs(dx) && abs(dy) > 200) {
                    pcScrollOffset += dy / 20;
                    pcScrollOffset = constrain(pcScrollOffset,
                        pcGpuCount > 0 ? -(pcGpuCount * 180 + 570) : -530, 0);
                    if (millis() - lastPcScrollDraw >= 80) {
                        drawDisplay();
                        lastPcScrollDraw = millis();
                    }
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
                            timerAlarmActive = false; timerAlarmRed = false;
                        } else if (screenX < 210) {
                            timerRunning = false; timerElapsedMs = 0;
                            timerAlarmActive = false; timerAlarmRed = false;
                        } else {
                            timerCountdownMode = !timerCountdownMode; timerRunning = false; timerElapsedMs = 0;
                            if (timerCountdownMode && countdownDurationMs == 0) countdownDurationMs = 300000UL;
                        }
                        saveTimerMemory();
                    } else if (timerCountdownMode && screenY > 205) {
                        const unsigned long presetMinutes[] = {5, 15, 30, 60};
                        int presetIndex = constrain(screenX / 80, 0, 3);
                        countdownDurationMs = presetMinutes[presetIndex] * 60000UL;
                        timerElapsedMs = 0;
                        timerStartedAt = millis();
                        timerRunning = true;
                        timerAlarmActive = false; timerAlarmRed = false;
                        saveTimerMemory();
                    }
                    drawDisplay();
                } else if (currentPage == 6 && abs(finalDx) < 200 && abs(finalDy) < 200) {
                    int screenX = constrain(map(finalX, 200, 3700, 0, SCREEN_WIDTH), 0, SCREEN_WIDTH - 1);
                    int screenY = constrain(map(finalY, 240, 3800, 0, SCREEN_HEIGHT), 0, SCREEN_HEIGHT - 1);
                    handlePcStatusTap(screenX, screenY);
                } else if (currentPage == 3 && abs(finalDx) < 200 && abs(finalDy) < 200) {
                    int screenX = constrain(map(finalX, 200, 3700, 0, SCREEN_WIDTH), 0, SCREEN_WIDTH - 1);
                    int screenY = constrain(map(finalY, 240, 3800, 0, SCREEN_HEIGHT), 0, SCREEN_HEIGHT - 1);
                    handlePcControlTap(screenX, screenY);
                } else if (abs(finalDx) < 200 && abs(finalDy) < 200 && handleMusicControlTap(finalX, finalY)) {
                    // 音乐控制点击已处理
                } else if (finalDx > SWIPE_MIN_X) { // 向右划：上一页
                    currentPage = (currentPage - 1 + MAX_PAGES) % MAX_PAGES;
                    pcFpsFullscreen = false;
                    scrollOffset = 0;
                    weatherScrollOffset = 0;
                    pcScrollOffset = 0;
                    lastTimeStr = ""; // 重置时间显示状态
                    drawDisplay();
                } else if (finalDx < -SWIPE_MIN_X) { // 向左划：下一页
                    currentPage = (currentPage + 1) % MAX_PAGES;
                    pcFpsFullscreen = false;
                    scrollOffset = 0;
                    weatherScrollOffset = 0;
                    pcScrollOffset = 0;
                    lastTimeStr = ""; // 重置时间显示状态
                    drawDisplay();
                }
            } else if (currentPage == 6) {
                drawDisplay();
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
