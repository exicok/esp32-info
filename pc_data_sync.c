#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <pdh.h>
#include <powrprof.h>
#include <shellapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "winhttp.lib")

#define BAUD_RATE 115200
#define SEND_INTERVAL_MS 100
#define PROBE_TIMEOUT_MS 2600
#define RECONNECT_DELAY_MS 2000
#define WEATHER_UPDATE_INTERVAL_MS 1800000
#define WEATHER_RETRY_INTERVAL_MS 60000
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    char name[160];
    char vendor[80];
    char driver_version[64];
    char driver_date[64];
    DWORD memory_mb;
} GpuInfo;

#define MAX_GPUS 4
#define MAX_CPU_CORES_SENT 32
#define MAX_SYSTEM_PROCESSORS 256

typedef struct {
    ULONGLONG idle;
    ULONGLONG kernel;
    ULONGLONG user;
    int valid;
} CpuSample;

typedef struct {
    LARGE_INTEGER idle_time;
    LARGE_INTEGER kernel_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER dpc_time;
    LARGE_INTEGER interrupt_time;
    ULONG interrupt_count;
} ProcessorPerformanceInfo;

typedef LONG (WINAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

typedef struct {
    double usage;
    DWORD dedicated_used_mb;
    DWORD shared_used_mb;
    int fan_percent;
    int fan_rpm;
    double power_watts;
} GpuMetrics;

typedef struct {
    ULONG number;
    ULONG max_mhz;
    ULONG current_mhz;
    ULONG mhz_limit;
    ULONG max_idle_state;
    ULONG current_idle_state;
} CpuPowerInformation;

typedef struct {
    DWORD signature;
    DWORD version;
    DWORD app_entry_size;
    DWORD app_array_offset;
    DWORD app_array_size;
    DWORD osd_entry_size;
    DWORD osd_array_offset;
    DWORD osd_array_size;
    DWORD osd_frame;
} RtssSharedMemory;

typedef struct {
    DWORD process_id;
    char name[MAX_PATH];
    DWORD flags;
    DWORD time0;
    DWORD time1;
    DWORD frames;
    DWORD frame_time_us;
} RtssAppEntryPrefix;

typedef struct {
    DWORD signature;
    DWORD version;
    DWORD header_size;
    DWORD entry_count;
    DWORD entry_size;
    DWORD time;
    DWORD gpu_entry_count;
    DWORD gpu_entry_size;
} MahmHeader;

typedef struct {
    char source_name[MAX_PATH];
    char source_units[MAX_PATH];
    char localized_source_name[MAX_PATH];
    char localized_source_units[MAX_PATH];
    char recommended_format[MAX_PATH];
    float data;
    float min_limit;
    float max_limit;
    DWORD flags;
    DWORD gpu_index;
    DWORD source_id;
} MahmEntry;

typedef struct {
    char gpu_id[MAX_PATH];
    char family[MAX_PATH];
    char device[MAX_PATH];
    char driver[MAX_PATH];
    char bios[MAX_PATH];
    DWORD memory_kb;
} MahmGpuEntry;

static volatile LONG g_running = 1;
static HWND g_tray_window;
static HANDLE g_worker_thread;
static HICON g_tray_icon;
static NOTIFYICONDATAA g_tray_data;
static char g_connected_port[16];
static char g_forced_port[16];
static PDH_HQUERY g_gpu_query;
static PDH_HCOUNTER g_gpu_counter;
static PDH_HCOUNTER g_gpu_dedicated_counter;
static PDH_HCOUNTER g_gpu_shared_counter;
static PDH_HQUERY g_cpu_frequency_query;
static PDH_HCOUNTER g_cpu_frequency_counter;
static PDH_HCOUNTER g_cpu_performance_counter;
static PDH_HQUERY g_cpu_power_query;
static PDH_HCOUNTER g_cpu_power_counter;
static int g_cpu_power_is_array;

#define WM_TRAY_ICON (WM_APP + 1)
#define WM_SYNC_STATUS (WM_APP + 2)
#define WM_ESP32_LOG (WM_APP + 3)
#define TRAY_ICON_ID 1
#define MENU_EXIT_ID 1001
#define MENU_OPEN_ID 1002
#define GUI_STATUS_ID 2001
#define GUI_PORT_ID 2002
#define GUI_SCREEN_ON_ID 2004
#define GUI_SCREEN_OFF_ID 2005
#define GUI_AUTOSTART_ID 2006
#define GUI_SEND_CPU_ID 2007
#define GUI_SEND_MEMORY_ID 2008
#define GUI_SEND_GPU_ID 2009
#define GUI_SEND_FPS_ID 2010
#define GUI_PAGE_COMBO_ID 2011
#define GUI_LOG_ID 2012
#define GUI_LOG_CLEAR_ID 2013
#define LOG_TEXT_LIMIT (256 * 1024)

static HWND g_gui_status;
static HWND g_gui_port;
static HWND g_gui_autostart;
static HWND g_gui_send_cpu;
static HWND g_gui_send_memory;
static HWND g_gui_send_gpu;
static HWND g_gui_send_fps;
static HWND g_gui_page_combo;
static HWND g_gui_log;
static HANDLE g_active_serial = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_serial_lock;
static volatile LONG g_pause_cpu;
static volatile LONG g_pause_memory;
static volatile LONG g_pause_gpu;
static volatile LONG g_pause_fps;

enum SyncStatus {
    SYNC_SCANNING = 1,
    SYNC_CONNECTED,
    SYNC_DISCONNECTED
};

static int write_line(HANDLE serial, const char *line);
static int write_line_unlocked(HANDLE serial, const char *line);

static void report_status(enum SyncStatus status) {
    if (g_tray_window) PostMessageA(g_tray_window, WM_SYNC_STATUS, (WPARAM)status, 0);
}

static void post_esp32_log(const char *text, size_t length) {
    char *copy;
    size_t i, output_length = 0;
    if (!g_tray_window || !text || !length) return;
    copy = (char *)malloc(length * 2 + 3);
    if (!copy) return;
    for (i = 0; i < length; ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) copy[output_length++] = '\r';
        copy[output_length++] = text[i];
    }
    if (output_length < 2 || copy[output_length - 1] != '\n') {
        copy[output_length++] = '\r';
        copy[output_length++] = '\n';
    }
    copy[output_length] = '\0';
    if (!PostMessageA(g_tray_window, WM_ESP32_LOG, 0, (LPARAM)copy)) free(copy);
}

static void append_esp32_log(char *text) {
    LRESULT length;
    size_t incoming_length;
    if (!text) return;
    if (!g_gui_log) {
        free(text);
        return;
    }
    length = GetWindowTextLengthA(g_gui_log);
    incoming_length = strlen(text);
    if ((size_t)length + incoming_length > LOG_TEXT_LIMIT) {
        LRESULT remove_length = max(length - LOG_TEXT_LIMIT / 2, (LRESULT)0);
        SendMessageA(g_gui_log, EM_SETSEL, 0, remove_length);
        SendMessageA(g_gui_log, EM_REPLACESEL, FALSE, (LPARAM)"");
        length = GetWindowTextLengthA(g_gui_log);
    }
    SendMessageA(g_gui_log, EM_SETSEL, length, length);
    SendMessageA(g_gui_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageA(g_gui_log, EM_SCROLLCARET, 0, 0);
    free(text);
}

static ULONGLONG filetime_value(FILETIME value) {
    ULARGE_INTEGER result;
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

static double sample_cpu_usage(CpuSample *previous) {
    FILETIME idle_time, kernel_time, user_time;
    CpuSample current;
    ULONGLONG idle_delta, total_delta;

    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) return 0.0;
    current.idle = filetime_value(idle_time);
    current.kernel = filetime_value(kernel_time);
    current.user = filetime_value(user_time);
    current.valid = 1;
    if (!previous->valid) {
        *previous = current;
        return 0.0;
    }

    idle_delta = current.idle - previous->idle;
    total_delta = (current.kernel - previous->kernel) + (current.user - previous->user);
    *previous = current;
    if (!total_delta || idle_delta > total_delta) return 0.0;
    return 100.0 * (double)(total_delta - idle_delta) / (double)total_delta;
}

static int sample_cpu_core_usages(double usages[MAX_CPU_CORES_SENT], int logical_cores) {
    static ProcessorPerformanceInfo previous[MAX_SYSTEM_PROCESSORS];
    static int previous_count;
    static NtQuerySystemInformationFn query;
    ProcessorPerformanceInfo current[MAX_SYSTEM_PROCESSORS];
    ULONG returned = 0;
    int system_count = min(logical_cores, MAX_SYSTEM_PROCESSORS), count, i;
    if (!query) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) query = (NtQuerySystemInformationFn)GetProcAddress(ntdll, "NtQuerySystemInformation");
    }
    if (!query || system_count <= 0 ||
        query(8, current, (ULONG)sizeof(current), &returned) < 0)
        return 0;
    if (returned >= sizeof(current[0]))
        system_count = min(system_count, (int)(returned / sizeof(current[0])));
    count = min(system_count, MAX_CPU_CORES_SENT);
    for (i = 0; i < count; ++i) {
        ULONGLONG idle = (ULONGLONG)(current[i].idle_time.QuadPart - previous[i].idle_time.QuadPart);
        ULONGLONG kernel = (ULONGLONG)(current[i].kernel_time.QuadPart - previous[i].kernel_time.QuadPart);
        ULONGLONG user = (ULONGLONG)(current[i].user_time.QuadPart - previous[i].user_time.QuadPart);
        ULONGLONG total = kernel + user;
        usages[i] = previous_count == system_count && total && idle <= total
            ? 100.0 * (double)(total - idle) / (double)total : 0.0;
        previous[i] = current[i];
    }
    for (; i < system_count; ++i) previous[i] = current[i];
    previous_count = system_count;
    return count;
}

static DWORD sample_current_cpu_mhz(DWORD logical_cores, DWORD fallback_mhz) {
    CpuPowerInformation *information;
    PDH_FMT_COUNTERVALUE frequency, performance;
    ULONG size;
    ULONGLONG total = 0;
    DWORD count = 0, i;

    if (!g_cpu_frequency_query) {
        if (PdhOpenQueryA(NULL, 0, &g_cpu_frequency_query) == ERROR_SUCCESS) {
            PdhAddEnglishCounterA(g_cpu_frequency_query,
                "\\Processor Information(_Total)\\Processor Frequency",
                0, &g_cpu_frequency_counter);
            PdhAddEnglishCounterA(g_cpu_frequency_query,
                "\\Processor Information(_Total)\\% Processor Performance",
                0, &g_cpu_performance_counter);
            if (!g_cpu_frequency_counter && !g_cpu_performance_counter) {
                PdhCloseQuery(g_cpu_frequency_query);
                g_cpu_frequency_query = NULL;
            } else {
                PdhCollectQueryData(g_cpu_frequency_query);
            }
        } else if (g_cpu_frequency_query) {
            PdhCloseQuery(g_cpu_frequency_query);
            g_cpu_frequency_query = NULL;
        }
    } else if (PdhCollectQueryData(g_cpu_frequency_query) == ERROR_SUCCESS) {
        if (g_cpu_performance_counter &&
            PdhGetFormattedCounterValue(g_cpu_performance_counter, PDH_FMT_DOUBLE,
                                        NULL, &performance) == ERROR_SUCCESS &&
            performance.CStatus == ERROR_SUCCESS &&
            performance.doubleValue > 0.0 && performance.doubleValue < 1000.0)
            return (DWORD)(fallback_mhz * performance.doubleValue / 100.0);
        if (g_cpu_frequency_counter &&
            PdhGetFormattedCounterValue(g_cpu_frequency_counter, PDH_FMT_DOUBLE,
                                        NULL, &frequency) == ERROR_SUCCESS &&
            frequency.CStatus == ERROR_SUCCESS && frequency.doubleValue > 0.0)
            return (DWORD)frequency.doubleValue;
    }

    if (!logical_cores || logical_cores > 256) return fallback_mhz;
    size = logical_cores * sizeof(CpuPowerInformation);
    information = (CpuPowerInformation *)malloc(size);
    if (!information) return fallback_mhz;
    if (CallNtPowerInformation(ProcessorInformation, NULL, 0, information, size) == 0) {
        for (i = 0; i < logical_cores; ++i) {
            if (!information[i].current_mhz) continue;
            total += information[i].current_mhz;
            ++count;
        }
    }
    free(information);
    return count ? (DWORD)(total / count) : fallback_mhz;
}

static double sample_cpu_power_watts(void) {
    DWORD buffer_size = 0, item_count = 0, i;
    PPDH_FMT_COUNTERVALUE_ITEM_A items = NULL;
    double total = 0.0;

    if (!g_cpu_power_query) {
        if (PdhOpenQueryA(NULL, 0, &g_cpu_power_query) != ERROR_SUCCESS) return -1.0;
        if (PdhAddEnglishCounterA(g_cpu_power_query, "\\Power Meter(*)\\Power",
                                  0, &g_cpu_power_counter) == ERROR_SUCCESS) {
            g_cpu_power_is_array = 1;
        } else if (PdhAddEnglishCounterA(g_cpu_power_query,
                                         "\\Processor Energy(_Total)\\Energy",
                                         0, &g_cpu_power_counter) == ERROR_SUCCESS) {
            g_cpu_power_is_array = 0;
        } else {
            PdhCloseQuery(g_cpu_power_query);
            g_cpu_power_query = NULL;
            g_cpu_power_counter = NULL;
            return -1.0;
        }
        PdhCollectQueryData(g_cpu_power_query);
        return -1.0;
    }
    if (PdhCollectQueryData(g_cpu_power_query) != ERROR_SUCCESS) return -1.0;
    if (!g_cpu_power_is_array) {
        PDH_FMT_COUNTERVALUE value;
        if (PdhGetFormattedCounterValue(g_cpu_power_counter, PDH_FMT_DOUBLE,
                                        NULL, &value) == ERROR_SUCCESS &&
            value.CStatus == ERROR_SUCCESS && value.doubleValue > 0.0 &&
            value.doubleValue <= 2000.0)
            return value.doubleValue;
        return -1.0;
    }
    PdhGetFormattedCounterArrayA(g_cpu_power_counter, PDH_FMT_DOUBLE,
                                 &buffer_size, &item_count, NULL);
    if (!buffer_size) return -1.0;
    items = (PPDH_FMT_COUNTERVALUE_ITEM_A)malloc(buffer_size);
    if (!items) return -1.0;
    if (PdhGetFormattedCounterArrayA(g_cpu_power_counter, PDH_FMT_DOUBLE,
                                     &buffer_size, &item_count, items) == ERROR_SUCCESS) {
        for (i = 0; i < item_count; ++i) {
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS &&
                items[i].FmtValue.doubleValue >= 0.0)
                total += items[i].FmtValue.doubleValue;
        }
    }
    free(items);
    return total > 0.0 && total <= 2000.0 ? total : -1.0;
}

static DWORD run_hidden_command(char *command, char *output, DWORD capacity) {
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE read_pipe = NULL, write_pipe = NULL;
    DWORD total = 0, received = 0;

    if (!capacity || !CreatePipe(&read_pipe, &write_pipe, &security, 0)) return 0;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        output[0] = '\0';
        return 0;
    }
    CloseHandle(write_pipe);
    while (total + 1 < capacity &&
           ReadFile(read_pipe, output + total, capacity - total - 1, &received, NULL) && received)
        total += received;
    output[total] = '\0';
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    return total;
}

static int name_contains(const char *name, const char *part) {
    size_t length = strlen(part);
    while (*name) {
        if (_strnicmp(name, part, length) == 0) return 1;
        ++name;
    }
    return 0;
}

static void sample_msi_afterburner_metrics(int fan_percent[MAX_GPUS],
                                           int fan_rpm[MAX_GPUS],
                                           double gpu_power_watts[MAX_GPUS],
                                           double *cpu_power_watts) {
    HANDLE mapping;
    const BYTE *view;
    const MahmHeader *header;
    const BYTE *gpu_array;
    int rx_gpu_indices[MAX_GPUS] = {-1, -1, -1, -1};
    int rx_count = 0;
    DWORD i;

    mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "MAHMSharedMemory");
    if (!mapping) return;
    view = (const BYTE *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        return;
    }
    header = (const MahmHeader *)view;
    if ((header->signature == 0x4D41484D || header->signature == 0x4D48414D) &&
        header->header_size >= sizeof(MahmHeader) &&
        header->entry_size >= sizeof(MahmEntry) &&
        header->gpu_entry_size >= sizeof(MahmGpuEntry) &&
        header->entry_count <= 512 && header->gpu_entry_count <= 64) {
        gpu_array = view + header->header_size +
                    (size_t)header->entry_count * header->entry_size;
        for (i = 0; i < header->gpu_entry_count && rx_count < MAX_GPUS; ++i) {
            const MahmGpuEntry *gpu = (const MahmGpuEntry *)(
                gpu_array + (size_t)i * header->gpu_entry_size);
            if (name_contains(gpu->device, "RX")) rx_gpu_indices[rx_count++] = (int)i;
        }
        for (i = 0; i < header->entry_count; ++i) {
            const MahmEntry *entry = (const MahmEntry *)(
                view + header->header_size + (size_t)i * header->entry_size);
            int output_index;
            if (entry->data == FLT_MAX) continue;
            if (entry->source_id == 0x00000100 && entry->data >= 0.0f &&
                entry->data <= 2000.0f) {
                if (entry->data > *cpu_power_watts) *cpu_power_watts = entry->data;
                continue;
            }
            for (output_index = 0; output_index < rx_count; ++output_index)
                if ((DWORD)rx_gpu_indices[output_index] == entry->gpu_index) break;
            if (output_index >= rx_count) continue;
            if ((entry->source_id == 0x10 || entry->source_id == 0x12 ||
                 entry->source_id == 0x14) && entry->data >= 0.0f && entry->data <= 100.0f) {
                fan_percent[output_index] = (int)(entry->data + 0.5f);
            } else if ((entry->source_id == 0x11 || entry->source_id == 0x13 ||
                        entry->source_id == 0x15) && entry->data >= 0.0f) {
                fan_rpm[output_index] = (int)(entry->data + 0.5f);
            } else if (entry->source_id == 0x00000061 && entry->data >= 0.0f &&
                       entry->data <= 2000.0f) {
                gpu_power_watts[output_index] = entry->data;
            }
        }
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
}

static void sample_nvidia_gpu_fans(int fan_percent[MAX_GPUS]) {
    static ULONGLONG last_sample_ms;
    static int cached[MAX_GPUS] = {-1, -1, -1, -1};
    ULONGLONG now = GetTickCount64();
    char command[] = "nvidia-smi --query-gpu=fan.speed --format=csv,noheader,nounits";
    char output[512], *line, *context = NULL;
    int index = 0, i;

    if (last_sample_ms && now - last_sample_ms < 1500) {
        for (i = 0; i < MAX_GPUS; ++i) fan_percent[i] = cached[i];
        return;
    }
    for (i = 0; i < MAX_GPUS; ++i) cached[i] = -1;
    if (run_hidden_command(command, output, sizeof(output))) {
        line = strtok_s(output, "\r\n", &context);
        while (index < MAX_GPUS && line) {
            char *end;
            long value = strtol(line, &end, 10);
            if (end != line && value >= 0 && value <= 100) cached[index] = (int)value;
            ++index;
            line = strtok_s(NULL, "\r\n", &context);
        }
    }
    last_sample_ms = now;
    for (i = 0; i < MAX_GPUS; ++i) fan_percent[i] = cached[i];
}

static void collect_gpu_counter(PDH_HCOUNTER counter, double scale, int sum,
                                double values[MAX_GPUS]) {
    DWORD buffer_size = 0, item_count = 0;
    PPDH_FMT_COUNTERVALUE_ITEM_A items = NULL;
    DWORD i;
    if (!counter) return;
    PdhGetFormattedCounterArrayA(counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, NULL);
    if (!buffer_size) return;
    items = (PPDH_FMT_COUNTERVALUE_ITEM_A)malloc(buffer_size);
    if (!items) return;
    if (PdhGetFormattedCounterArrayA(counter, PDH_FMT_DOUBLE,
                                     &buffer_size, &item_count, items) == ERROR_SUCCESS) {
        for (i = 0; i < item_count; ++i) {
            char *physical;
            unsigned long adapter_index;
            double value;
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS ||
                (physical = strstr(items[i].szName, "phys_")) == NULL) continue;
            adapter_index = strtoul(physical + 5, NULL, 10);
            if (adapter_index >= MAX_GPUS) continue;
            value = items[i].FmtValue.doubleValue / scale;
            if (sum) values[adapter_index] += value;
            else if (value > values[adapter_index]) values[adapter_index] = value;
        }
    }
    free(items);
}

static void sample_gpu_metrics(GpuMetrics metrics[MAX_GPUS],
                               const GpuInfo *gpus, int gpu_count,
                               double *msi_cpu_power_watts) {
    double usage[MAX_GPUS] = {0}, dedicated[MAX_GPUS] = {0}, shared[MAX_GPUS] = {0};
    int nvidia_fans[MAX_GPUS] = {-1, -1, -1, -1};
    int amd_fans[MAX_GPUS] = {-1, -1, -1, -1};
    int amd_rpms[MAX_GPUS] = {-1, -1, -1, -1};
    double amd_power_watts[MAX_GPUS] = {-1.0, -1.0, -1.0, -1.0};
    int nvidia_index = 0, amd_index = 0;
    DWORD i;
    ZeroMemory(metrics, sizeof(GpuMetrics) * MAX_GPUS);
    for (i = 0; i < MAX_GPUS; ++i) {
        metrics[i].fan_percent = -1;
        metrics[i].fan_rpm = -1;
        metrics[i].power_watts = -1.0;
    }
    sample_nvidia_gpu_fans(nvidia_fans);
    sample_msi_afterburner_metrics(amd_fans, amd_rpms, amd_power_watts,
                                   msi_cpu_power_watts);
    for (i = 0; i < (DWORD)gpu_count && i < MAX_GPUS; ++i) {
        if (name_contains(gpus[i].name, "RX")) {
            if (amd_index < MAX_GPUS) {
                metrics[i].fan_percent = amd_fans[amd_index];
                metrics[i].fan_rpm = amd_rpms[amd_index];
                metrics[i].power_watts = amd_power_watts[amd_index++];
            }
        } else if (name_contains(gpus[i].name, "NVIDIA") ||
                   name_contains(gpus[i].name, "GeForce")) {
            if (nvidia_index < MAX_GPUS)
                metrics[i].fan_percent = nvidia_fans[nvidia_index++];
        }
    }

    if (!g_gpu_query) {
        if (PdhOpenQueryA(NULL, 0, &g_gpu_query) != ERROR_SUCCESS) return;
        if (PdhAddEnglishCounterA(g_gpu_query,
                "\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpu_counter) != ERROR_SUCCESS) {
            PdhCloseQuery(g_gpu_query);
            g_gpu_query = NULL;
            return;
        }
        if (PdhAddEnglishCounterA(g_gpu_query, "\\GPU Adapter Memory(*)\\Dedicated Usage",
                                  0, &g_gpu_dedicated_counter) != ERROR_SUCCESS)
            PdhAddEnglishCounterA(g_gpu_query, "\\GPU Process Memory(*)\\Dedicated Usage",
                                  0, &g_gpu_dedicated_counter);
        if (PdhAddEnglishCounterA(g_gpu_query, "\\GPU Adapter Memory(*)\\Shared Usage",
                                  0, &g_gpu_shared_counter) != ERROR_SUCCESS)
            PdhAddEnglishCounterA(g_gpu_query, "\\GPU Process Memory(*)\\Shared Usage",
                                  0, &g_gpu_shared_counter);
        PdhCollectQueryData(g_gpu_query);
        return;
    }
    if (PdhCollectQueryData(g_gpu_query) != ERROR_SUCCESS) return;
    collect_gpu_counter(g_gpu_counter, 1.0, 1, usage);
    collect_gpu_counter(g_gpu_dedicated_counter, 1048576.0, 1, dedicated);
    collect_gpu_counter(g_gpu_shared_counter, 1048576.0, 1, shared);
    for (i = 0; i < MAX_GPUS; ++i) {
        if (usage[i] < 0.0) usage[i] = 0.0;
        if (usage[i] > 100.0) usage[i] = 100.0;
        metrics[i].usage = usage[i];
        metrics[i].dedicated_used_mb = (DWORD)dedicated[i];
        metrics[i].shared_used_mb = (DWORD)shared[i];
    }
}

static ULONGLONG unix_epoch_ms(void) {
    FILETIME now;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&now);
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    return (value.QuadPart - 116444736000000000ULL) / 10000ULL;
}

static double sample_rtss_fps(double *frame_time_ms, char *graphics_api, size_t api_size) {
    HANDLE mapping;
    const BYTE *view;
    const RtssSharedMemory *shared;
    double selected_fps = -1.0;
    DWORD selected_time = 0, now = GetTickCount(), i;

    *frame_time_ms = -1.0;
    lstrcpynA(graphics_api, "--", (int)api_size);
    mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!mapping) return -1.0;
    view = (const BYTE *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        return -1.0;
    }
    shared = (const RtssSharedMemory *)view;
    if (shared->signature == 0x52545353 &&
        shared->app_entry_size >= sizeof(RtssAppEntryPrefix) &&
        shared->app_array_size <= 256) {
        for (i = 0; i < shared->app_array_size; ++i) {
            const RtssAppEntryPrefix *entry = (const RtssAppEntryPrefix *)(
                view + shared->app_array_offset + (size_t)i * shared->app_entry_size);
            double fps = -1.0;
            if (!entry->process_id) continue;
            if (entry->time1 && now - entry->time1 > 3000) continue;
            if (entry->frame_time_us > 0)
                fps = 1000000.0 / (double)entry->frame_time_us;
            else if (entry->time1 > entry->time0 && entry->frames > 0)
                fps = 1000.0 * (double)entry->frames / (double)(entry->time1 - entry->time0);
            if (fps > 0.0 && fps <= 2000.0 && entry->time1 >= selected_time) {
                selected_time = entry->time1;
                selected_fps = fps;
                *frame_time_ms = entry->frame_time_us > 0
                    ? entry->frame_time_us / 1000.0
                    : 1000.0 / fps;
                switch (entry->flags & 0x0000FFFF) {
                    case 0x0001: lstrcpynA(graphics_api, "OpenGL", (int)api_size); break;
                    case 0x0002: lstrcpynA(graphics_api, "DirectDraw", (int)api_size); break;
                    case 0x0003: lstrcpynA(graphics_api, "Direct3D 8", (int)api_size); break;
                    case 0x0004: lstrcpynA(graphics_api, "Direct3D 9", (int)api_size); break;
                    case 0x0005: lstrcpynA(graphics_api, "Direct3D 9Ex", (int)api_size); break;
                    case 0x0006: lstrcpynA(graphics_api, "Direct3D 10", (int)api_size); break;
                    case 0x0007: lstrcpynA(graphics_api, "Direct3D 11", (int)api_size); break;
                    case 0x0008: lstrcpynA(graphics_api, "Direct3D 12", (int)api_size); break;
                    case 0x0009: lstrcpynA(graphics_api, "D3D12 AFR", (int)api_size); break;
                    case 0x000A: lstrcpynA(graphics_api, "Vulkan", (int)api_size); break;
                    default: lstrcpynA(graphics_api, "--", (int)api_size); break;
                }
            }
        }
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return selected_fps;
}

static void registry_string(HKEY root, const char *path, const char *name,
                            char *output, DWORD output_size) {
    HKEY key;
    DWORD type = 0;
    DWORD size = output_size;
    output[0] = '\0';
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)output, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) output[0] = '\0';
    output[output_size - 1] = '\0';
    RegCloseKey(key);
}

static DWORD registry_dword(HKEY root, const char *path, const char *name) {
    HKEY key;
    DWORD value = 0, type = 0, size = sizeof(value);
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS ||
        type != REG_DWORD) value = 0;
    RegCloseKey(key);
    return value;
}

static void json_escape(const char *input, char *output, size_t output_size) {
    size_t used = 0;
    while (*input && used + 2 < output_size) {
        unsigned char c = (unsigned char)*input++;
        if (c == '"' || c == '\\') {
            output[used++] = '\\';
            output[used++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            output[used++] = '\\';
            output[used++] = c == '\n' ? 'n' : (c == '\r' ? 'r' : 't');
        } else if (c >= 0x20) {
            output[used++] = (char)c;
        }
    }
    output[used] = '\0';
}

static const char *json_value(const char *json, const char *key) {
    const char *value = strstr(json, key);
    if (!value) return NULL;
    value += strlen(key);
    while (*value == ' ' || *value == '\t') ++value;
    return value;
}

static int json_number(const char *json, const char *key, double *result) {
    char *end;
    const char *value = json_value(json, key);
    if (!value) return 0;
    *result = strtod(value, &end);
    return end != value;
}

static int json_array_number(const char *json, const char *key, int index, double *result) {
    const char *value = json_value(json, key);
    int i;
    if (!value || *value != '[') return 0;
    ++value;
    for (i = 0; i <= index; ++i) {
        char *end;
        while (*value == ' ' || *value == '\t') ++value;
        *result = strtod(value, &end);
        if (end == value) return 0;
        if (i == index) return 1;
        value = strchr(end, ',');
        if (!value) return 0;
        ++value;
    }
    return 0;
}

static int json_array_string(const char *json, const char *key, int index,
                             char *output, size_t output_size) {
    const char *value = json_value(json, key);
    const char *end;
    int i;
    size_t length;
    if (!value || *value != '[' || !output_size) return 0;
    ++value;
    for (i = 0; i <= index; ++i) {
        value = strchr(value, '"');
        if (!value) return 0;
        ++value;
        end = strchr(value, '"');
        if (!end) return 0;
        if (i == index) {
            length = min((size_t)(end - value), output_size - 1);
            memcpy(output, value, length);
            output[length] = '\0';
            return 1;
        }
        value = end + 1;
    }
    return 0;
}

static char *download_weather_json(void) {
    static const wchar_t path[] = L"/v1/forecast?latitude=25.4800&longitude=100.5600&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=Asia%2FShanghai&forecast_days=7";
    HINTERNET session = NULL, connection = NULL, request = NULL;
    char *response = NULL;
    size_t used = 0, capacity = 8192;
    DWORD status_code = 0, status_size = sizeof(status_code);
    session = WinHttpOpen(L"ESP32HubPcSync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto cleanup;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    connection = WinHttpConnect(session, L"api.open-meteo.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) goto cleanup;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) goto cleanup;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                             WINHTTP_NO_HEADER_INDEX) || status_code != 200) goto cleanup;
    response = (char *)malloc(capacity);
    if (!response) goto cleanup;
    for (;;) {
        DWORD available = 0, received = 0;
        char *expanded;
        if (!WinHttpQueryDataAvailable(request, &available)) goto error;
        if (!available) break;
        if (used + available + 1 > capacity) {
            while (used + available + 1 > capacity) capacity *= 2;
            if (capacity > 65536) goto error;
            expanded = (char *)realloc(response, capacity);
            if (!expanded) goto error;
            response = expanded;
        }
        if (!WinHttpReadData(request, response + used, available, &received)) goto error;
        used += received;
    }
    response[used] = '\0';
    goto cleanup;
error:
    free(response);
    response = NULL;
cleanup:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return response;
}

static int fetch_and_send_weather(HANDLE serial) {
    char *json = download_weather_json();
    const char *current, *daily;
    char line[2048], daily_json[1200] = "[";
    double temperature, humidity, weather_code, wind_speed, wind_direction;
    int i;
    if (!json) return 0;
    current = strstr(json, "\"current\":{");
    daily = strstr(json, "\"daily\":{");
    if (!current || !daily ||
        !json_number(current, "\"temperature_2m\":", &temperature) ||
        !json_number(current, "\"relative_humidity_2m\":", &humidity) ||
        !json_number(current, "\"weather_code\":", &weather_code) ||
        !json_number(current, "\"wind_speed_10m\":", &wind_speed) ||
        !json_number(current, "\"wind_direction_10m\":", &wind_direction)) {
        free(json);
        return 0;
    }
    for (i = 0; i < 7; ++i) {
        char date[24], item[160];
        double code, high, low;
        if (!json_array_string(daily, "\"time\":", i, date, sizeof(date)) ||
            !json_array_number(daily, "\"weather_code\":", i, &code) ||
            !json_array_number(daily, "\"temperature_2m_max\":", i, &high) ||
            !json_array_number(daily, "\"temperature_2m_min\":", i, &low)) {
            free(json);
            return 0;
        }
        snprintf(item, sizeof(item), "%s{\"date\":\"%s\",\"weatherCode\":%.0f,\"high\":%.1f,\"low\":%.1f}",
                 i ? "," : "", date, code, high, low);
        strncat(daily_json, item, sizeof(daily_json) - strlen(daily_json) - 1);
    }
    strncat(daily_json, "]", sizeof(daily_json) - strlen(daily_json) - 1);
    snprintf(line, sizeof(line),
             "{\"cmd\":\"weather_update\",\"source\":\"pc\",\"city\":\"云南大理祥云\","
             "\"latitude\":25.48,\"longitude\":100.56,\"current\":{"
             "\"temperature\":%.1f,\"humidity\":%.0f,\"weatherCode\":%.0f,"
             "\"windSpeed\":%.1f,\"windDirection\":%.1f},\"daily\":%s}\n",
             temperature, humidity, weather_code, wind_speed, wind_direction, daily_json);
    free(json);
    return write_line(serial, line);
}

static int physical_core_count(void) {
    DWORD length = 0;
    int count = 0;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info, item;
    BYTE *end;

    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &length);
    if (!length) return 0;
    info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)malloc(length);
    if (!info || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        free(info);
        return 0;
    }
    item = info;
    end = (BYTE *)info + length;
    while ((BYTE *)item < end && item->Size) {
        if (item->Relationship == RelationProcessorCore) ++count;
        item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((BYTE *)item + item->Size);
    }
    free(info);
    return count;
}

static int get_gpu_info(GpuInfo *results, int capacity) {
    HDEVINFO devices = SetupDiGetClassDevsA(&GUID_DEVCLASS_DISPLAY, NULL, NULL, DIGCF_PRESENT);
    SP_DEVINFO_DATA data;
    DWORD index;
    int count = 0;

    if (devices == INVALID_HANDLE_VALUE) return 0;
    data.cbSize = sizeof(data);
    for (index = 0; SetupDiEnumDeviceInfo(devices, index, &data); ++index) {
        char name[sizeof(results[0].name)] = "";
        HKEY key;
        DWORD type = 0, size = sizeof(ULONGLONG);
        ULONGLONG memory = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(devices, &data, SPDRP_FRIENDLYNAME, NULL,
                                               (BYTE *)name, sizeof(name), NULL)) {
            SetupDiGetDeviceRegistryPropertyA(devices, &data, SPDRP_DEVICEDESC, NULL,
                                              (BYTE *)name, sizeof(name), NULL);
        }
        if (!name[0] || strstr(name, "Remote") || strstr(name, "Basic Display") || count >= capacity) continue;
        strncpy(results[count].name, name, sizeof(results[count].name) - 1);
        results[count].name[sizeof(results[count].name) - 1] = '\0';
        SetupDiGetDeviceRegistryPropertyA(devices, &data, SPDRP_MFG, NULL,
                                          (BYTE *)results[count].vendor,
                                          sizeof(results[count].vendor), NULL);
        key = SetupDiOpenDevRegKey(devices, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            if (RegQueryValueExA(key, "HardwareInformation.qwMemorySize", NULL, &type,
                                 (BYTE *)&memory, &size) != ERROR_SUCCESS) {
                size = sizeof(DWORD);
                RegQueryValueExA(key, "HardwareInformation.MemorySize", NULL, &type,
                                 (BYTE *)&memory, &size);
            }
            results[count].memory_mb = (DWORD)(memory / (1024 * 1024));
            RegCloseKey(key);
        }
        key = SetupDiOpenDevRegKey(devices, &data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            DWORD value_size = sizeof(results[count].driver_version);
            RegQueryValueExA(key, "DriverVersion", NULL, NULL,
                             (BYTE *)results[count].driver_version, &value_size);
            value_size = sizeof(results[count].driver_date);
            RegQueryValueExA(key, "DriverDate", NULL, NULL,
                             (BYTE *)results[count].driver_date, &value_size);
            results[count].driver_version[sizeof(results[count].driver_version) - 1] = '\0';
            results[count].driver_date[sizeof(results[count].driver_date) - 1] = '\0';
            RegCloseKey(key);
        }
        ++count;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return count;
}

static HANDLE open_serial(const char *port) {
    char path[32];
    HANDLE handle;
    DCB dcb = {0};
    COMMTIMEOUTS timeouts = {0};

    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) goto error;
    dcb.BaudRate = BAUD_RATE;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(handle, &dcb)) goto error;
    timeouts.ReadIntervalTimeout = 30;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(handle, &timeouts)) goto error;
    SetupComm(handle, 4096, 4096);
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return handle;
error:
    CloseHandle(handle);
    return INVALID_HANDLE_VALUE;
}

static int write_line_unlocked(HANDLE serial, const char *line) {
    DWORD written = 0;
    size_t length = strlen(line);
    return WriteFile(serial, line, (DWORD)length, &written, NULL) && written == length;
}

static int write_line(HANDLE serial, const char *line) {
    int result;
    EnterCriticalSection(&g_serial_lock);
    result = write_line_unlocked(serial, line);
    LeaveCriticalSection(&g_serial_lock);
    return result;
}

static int read_for_identity(HANDLE serial, DWORD timeout_ms) {
    char buffer[1024] = "";
    size_t used = 0;
    DWORD start = GetTickCount(), received;
    while (GetTickCount() - start < timeout_ms && InterlockedCompareExchange(&g_running, 0, 0)) {
        if (ReadFile(serial, buffer + used, (DWORD)(sizeof(buffer) - used - 1), &received, NULL) && received) {
            used += received;
            buffer[used] = '\0';
            if ((strstr(buffer, "\"type\":\"ack\"") && strstr(buffer, "\"cmd\":\"hello\"")) ||
                strstr(buffer, "\"type\":\"telemetry\"")) {
                post_esp32_log(buffer, used);
                return 1;
            }
            if (used > sizeof(buffer) / 2) {
                memmove(buffer, buffer + used / 2, used - used / 2);
                used -= used / 2;
                buffer[used] = '\0';
            }
        }
        Sleep(20);
    }
    return 0;
}

static HANDLE find_esp32(char *selected_port, size_t selected_size, const char *forced_port) {
    unsigned int index, begin = 1, end = 256;
    if (forced_port && forced_port[0]) {
        strncpy(selected_port, forced_port, selected_size - 1);
        begin = end = 0;
    }
    for (index = begin; index <= end && InterlockedCompareExchange(&g_running, 0, 0); ++index) {
        char port[16], target[1024];
        HANDLE serial;
        if (forced_port && forced_port[0]) strncpy(port, forced_port, sizeof(port) - 1);
        else snprintf(port, sizeof(port), "COM%u", index);
        port[sizeof(port) - 1] = '\0';
        if (!forced_port && !QueryDosDeviceA(port, target, (DWORD)sizeof(target))) continue;
        serial = open_serial(port);
        if (serial == INVALID_HANDLE_VALUE) continue;
        printf("Probing %s...\n", port);
        Sleep(350);
        write_line(serial, "{\"cmd\":\"hello\",\"client\":\"pc_data_sync\"}\n");
        if (read_for_identity(serial, PROBE_TIMEOUT_MS)) {
            strncpy(selected_port, port, selected_size - 1);
            selected_port[selected_size - 1] = '\0';
            return serial;
        }
        CloseHandle(serial);
        if (forced_port) break;
    }
    return INVALID_HANDLE_VALUE;
}

static void send_time_sync(HANDLE serial) {
    TIME_ZONE_INFORMATION zone;
    DWORD zone_id;
    LONG offset_minutes;
    char line[256];
    zone_id = GetTimeZoneInformation(&zone);
    if (zone_id == TIME_ZONE_ID_INVALID) offset_minutes = 0;
    else if (zone_id == TIME_ZONE_ID_DAYLIGHT) offset_minutes = -(zone.Bias + zone.DaylightBias);
    else offset_minutes = -(zone.Bias + zone.StandardBias);
    snprintf(line, sizeof(line),
             "{\"cmd\":\"time_sync\",\"epochMs\":%llu,\"utcOffsetMinutes\":%ld,\"source\":\"pc\"}\n",
             (unsigned long long)unix_epoch_ms(), offset_minutes);
    write_line(serial, line);
}

static HWND window_under_cursor(void) {
    POINT cursor;
    HWND window;
    if (!GetCursorPos(&cursor)) return NULL;
    window = WindowFromPoint(cursor);
    return window ? GetAncestor(window, GA_ROOT) : NULL;
}

static void enable_shutdown_privilege(void) {
    HANDLE token;
    TOKEN_PRIVILEGES privileges;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    ZeroMemory(&privileges, sizeof(privileges));
    privileges.PrivilegeCount = 1;
    if (LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid)) {
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
    }
    CloseHandle(token);
}

static void execute_pc_control(const char *action) {
    HWND target;
    if (strcmp(action, "media-play-pause") == 0) {
        INPUT input[2];
        ZeroMemory(input, sizeof(input));
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wVk = VK_MEDIA_PLAY_PAUSE;
        input[1] = input[0];
        input[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, input, sizeof(INPUT));
        return;
    }
    if (strcmp(action, "shutdown") == 0 || strcmp(action, "restart") == 0) {
        enable_shutdown_privilege();
        ExitWindowsEx(strcmp(action, "restart") == 0 ? EWX_REBOOT | EWX_FORCEIFHUNG
                                                       : EWX_POWEROFF | EWX_FORCEIFHUNG,
                       SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED);
        return;
    }
    target = window_under_cursor();
    if (!target || target == GetDesktopWindow() || target == GetShellWindow()) return;
    if (strcmp(action, "minimize") == 0) ShowWindowAsync(target, SW_MINIMIZE);
    else if (strcmp(action, "maximize") == 0) ShowWindowAsync(target, SW_MAXIMIZE);
    else if (strcmp(action, "close") == 0) PostMessageA(target, WM_CLOSE, 0, 0);
}

static void handle_device_commands(HANDLE serial, COMSTAT *status) {
    static char buffer[2048];
    static size_t used;
    char chunk[512];
    DWORD received = 0, i;
    if (!status->cbInQue) return;
    if (!ReadFile(serial, chunk, min((DWORD)sizeof(chunk), status->cbInQue), &received, NULL) || !received) return;
    for (i = 0; i < received; ++i) {
        if (chunk[i] == '\n') {
            const char *action = NULL;
            buffer[used] = '\0';
            post_esp32_log(buffer, used);
            if (strstr(buffer, "\"type\":\"pc_control\"")) {
                if (strstr(buffer, "\"action\":\"media-play-pause\"")) action = "media-play-pause";
                else if (strstr(buffer, "\"action\":\"minimize\"")) action = "minimize";
                else if (strstr(buffer, "\"action\":\"maximize\"")) action = "maximize";
                else if (strstr(buffer, "\"action\":\"close\"")) action = "close";
                else if (strstr(buffer, "\"action\":\"restart\"")) action = "restart";
                else if (strstr(buffer, "\"action\":\"shutdown\"")) action = "shutdown";
                if (action) execute_pc_control(action);
            }
            used = 0;
        } else if (chunk[i] != '\r' && used + 1 < sizeof(buffer)) {
            buffer[used++] = chunk[i];
        }
    }
}

static int send_pc_status(HANDLE serial, CpuSample *cpu, const char *cpu_name,
                          int logical_cores, int physical_cores, DWORD cpu_mhz,
                          const GpuInfo *gpus, int gpu_count) {
    MEMORYSTATUSEX memory;
    ULARGE_INTEGER free_bytes, total_bytes, total_free;
    char windows_dir[MAX_PATH], root[4] = "C:\\";
    char cpu_json[320], cpu_cores_json[320] = "[", gpu_json[320], driver_json[160];
    char gpus_json[2200] = "[", line[5200];
    char cpu_part[1100] = "", memory_part[420] = "", gpu_part[2700] = "", fps_part[160] = "";
    double cpu_usage = 0.0, memory_usage, cpu_power_watts = -1.0;
    double msi_cpu_power_watts = -1.0;
    double rtss_fps = -1.0, frame_time_ms = -1.0;
    char graphics_api[16] = "--";
    GpuMetrics gpu_metrics[MAX_GPUS] = {0};
    DWORD cpu_current_mhz = cpu_mhz;
    DWORD memory_used_mb, memory_total_mb;
    int pause_cpu = (int)InterlockedCompareExchange(&g_pause_cpu, 0, 0);
    int pause_memory = (int)InterlockedCompareExchange(&g_pause_memory, 0, 0);
    int pause_gpu = (int)InterlockedCompareExchange(&g_pause_gpu, 0, 0);
    int pause_fps = (int)InterlockedCompareExchange(&g_pause_fps, 0, 0);
    int primary_gpu_index = 0, gpu_index;
    double cpu_core_usages[MAX_CPU_CORES_SENT] = {0};
    int cpu_core_count = 0;

    ZeroMemory(&memory, sizeof(memory));
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return 0;
    if (!pause_cpu) {
        cpu_usage = sample_cpu_usage(cpu);
        cpu_core_count = sample_cpu_core_usages(cpu_core_usages, logical_cores);
        cpu_current_mhz = sample_current_cpu_mhz((DWORD)logical_cores, cpu_mhz);
        cpu_power_watts = sample_cpu_power_watts();
    }
    if (!pause_fps)
        rtss_fps = sample_rtss_fps(&frame_time_ms, graphics_api, sizeof(graphics_api));
    if (!pause_gpu || !pause_cpu)
        sample_gpu_metrics(gpu_metrics, gpus, gpu_count, &msi_cpu_power_watts);
    if (!pause_cpu && msi_cpu_power_watts >= 0.0)
        cpu_power_watts = msi_cpu_power_watts;
    memory_total_mb = (DWORD)(memory.ullTotalPhys / 1048576ULL);
    memory_used_mb = (DWORD)((memory.ullTotalPhys - memory.ullAvailPhys) / 1048576ULL);
    memory_usage = memory.ullTotalPhys ? 100.0 * (double)(memory.ullTotalPhys - memory.ullAvailPhys) /
                                        (double)memory.ullTotalPhys : 0.0;
    if (GetWindowsDirectoryA(windows_dir, sizeof(windows_dir)) && windows_dir[1] == ':')
        root[0] = windows_dir[0];
    if (!GetDiskFreeSpaceExA(root, &free_bytes, &total_bytes, &total_free)) {
        free_bytes.QuadPart = total_bytes.QuadPart = 0;
    }
    for (gpu_index = 0; gpu_index < gpu_count; ++gpu_index) {
        if (name_contains(gpus[gpu_index].name, "RX") ||
            name_contains(gpus[gpu_index].name, "NVIDIA") ||
            name_contains(gpus[gpu_index].name, "GeForce")) {
            primary_gpu_index = gpu_index;
            break;
        }
    }
    json_escape(cpu_name, cpu_json, sizeof(cpu_json));
    if (gpu_count > 0) json_escape(gpus[primary_gpu_index].name, gpu_json, sizeof(gpu_json));
    else lstrcpyA(gpu_json, "Unknown GPU");
    if (gpu_count > 0) json_escape(gpus[primary_gpu_index].driver_version, driver_json, sizeof(driver_json));
    else driver_json[0] = '\0';
    if (!pause_gpu) {
        int i;
        size_t used = 1;
        for (i = 0; i < gpu_count; ++i) {
            char name[320], vendor[160], driver[128], date[128], item[760];
            int length;
            json_escape(gpus[i].name, name, sizeof(name));
            json_escape(gpus[i].vendor, vendor, sizeof(vendor));
            json_escape(gpus[i].driver_version, driver, sizeof(driver));
            json_escape(gpus[i].driver_date, date, sizeof(date));
            length = snprintf(item, sizeof(item),
                "%s{\"name\":\"%s\",\"vendor\":\"%s\",\"memoryMB\":%lu,"
                "\"dedicatedUsedMB\":%lu,\"sharedUsedMB\":%lu,\"usage\":%.1f,"
                "\"fanPercent\":%d,\"fanRpm\":%d,\"powerWatts\":%.1f,"
                "\"driverVersion\":\"%s\",\"driverDate\":\"%s\"}",
                i ? "," : "", name, vendor, (unsigned long)gpus[i].memory_mb,
                (unsigned long)gpu_metrics[i].dedicated_used_mb,
                (unsigned long)gpu_metrics[i].shared_used_mb, gpu_metrics[i].usage,
                gpu_metrics[i].fan_percent, gpu_metrics[i].fan_rpm,
                gpu_metrics[i].power_watts, driver, date);
            if (length <= 0 || used + (size_t)length + 2 >= sizeof(gpus_json)) break;
            memcpy(gpus_json + used, item, (size_t)length);
            used += (size_t)length;
            gpus_json[used] = '\0';
        }
        strncat(gpus_json, "]", sizeof(gpus_json) - strlen(gpus_json) - 1);
    }
    if (!pause_cpu)
    {
        int i;
        size_t used = 1;
        for (i = 0; i < cpu_core_count; ++i) {
            int length = snprintf(cpu_cores_json + used, sizeof(cpu_cores_json) - used,
                                  "%s%.1f", i ? "," : "", cpu_core_usages[i]);
            if (length <= 0 || used + (size_t)length + 2 >= sizeof(cpu_cores_json)) break;
            used += (size_t)length;
        }
        strncat(cpu_cores_json, "]", sizeof(cpu_cores_json) - strlen(cpu_cores_json) - 1);
        snprintf(cpu_part, sizeof(cpu_part),
            "\"cpuName\":\"%s\",\"cpuUsage\":%.1f,\"cpuCores\":%d,"
            "\"cpuPhysicalCores\":%d,\"cpuMaxMHz\":%lu,\"cpuCurrentMHz\":%lu,"
            "\"cpuPowerWatts\":%.1f,\"cpuCoreUsages\":%s,",
            cpu_json, cpu_usage, logical_cores, physical_cores, (unsigned long)cpu_mhz,
            (unsigned long)cpu_current_mhz, cpu_power_watts, cpu_cores_json);
    }
    if (!pause_memory)
        snprintf(memory_part, sizeof(memory_part),
            "\"memoryUsedMB\":%lu,\"memoryTotalMB\":%lu,\"memoryUsage\":%.1f,"
            "\"disks\":[{\"name\":\"%c:\",\"totalMB\":%llu,\"freeMB\":%llu}],",
            (unsigned long)memory_used_mb, (unsigned long)memory_total_mb, memory_usage, root[0],
            (unsigned long long)(total_bytes.QuadPart / 1048576ULL),
            (unsigned long long)(free_bytes.QuadPart / 1048576ULL));
    if (!pause_gpu)
        snprintf(gpu_part, sizeof(gpu_part),
            "\"gpuName\":\"%s\",\"gpuMemoryMB\":%lu,\"gpuMemoryUsedMB\":%lu,"
            "\"gpuUsage\":%.1f,\"gpuFanPercent\":%d,\"gpuFanRpm\":%d,\"gpuPowerWatts\":%.1f,"
            "\"gpuDriver\":\"%s\",\"gpus\":%s,",
            gpu_json, (unsigned long)(gpu_count > 0 ? gpus[primary_gpu_index].memory_mb : 0),
            (unsigned long)gpu_metrics[primary_gpu_index].dedicated_used_mb,
            gpu_metrics[primary_gpu_index].usage,
            gpu_metrics[primary_gpu_index].fan_percent,
            gpu_metrics[primary_gpu_index].fan_rpm,
            gpu_metrics[primary_gpu_index].power_watts, driver_json, gpus_json);
    if (!pause_fps)
        snprintf(fps_part, sizeof(fps_part),
            "\"fps\":%.1f,\"frameTimeMs\":%.2f,\"graphicsApi\":\"%s\",",
            rtss_fps, frame_time_ms, graphics_api);
    snprintf(line, sizeof(line),
        "{\"cmd\":\"pc_status\",%s%s%s%s"
        "\"pauseCpu\":%d,\"pauseMemory\":%d,\"pauseGpu\":%d,\"pauseFps\":%d,"
        "\"sentEpochMs\":%llu,\"hostUptimeSeconds\":%llu,\"processId\":%lu}\n",
        cpu_part, memory_part, gpu_part, fps_part,
        pause_cpu, pause_memory, pause_gpu, pause_fps,
        (unsigned long long)unix_epoch_ms(),
        (unsigned long long)(GetTickCount64() / 1000ULL), (unsigned long)GetCurrentProcessId());
    return write_line(serial, line);
}

static DWORD WINAPI sync_worker(LPVOID parameter) {
    const char *forced_port = g_forced_port[0] ? g_forced_port : NULL;
    char cpu_name[256] = "Unknown CPU", port[16] = "";
    DWORD cpu_mhz;
    SYSTEM_INFO system_info;
    int physical_cores;
    GpuInfo gpus[MAX_GPUS] = {0};
    int gpu_count;

    (void)parameter;
    registry_string(HKEY_LOCAL_MACHINE,
                    "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                    "ProcessorNameString", cpu_name, sizeof(cpu_name));
    cpu_mhz = registry_dword(HKEY_LOCAL_MACHINE,
                             "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz");
    GetNativeSystemInfo(&system_info);
    physical_cores = physical_core_count();
    gpu_count = get_gpu_info(gpus, MAX_GPUS);

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        report_status(SYNC_SCANNING);
        HANDLE serial = find_esp32(port, sizeof(port), forced_port);
        CpuSample cpu = {0};
        DWORD last_time_sync, last_weather_update = 0, last_weather_attempt = 0;
        if (serial == INVALID_HANDLE_VALUE) {
            report_status(SYNC_DISCONNECTED);
            Sleep(RECONNECT_DELAY_MS);
            continue;
        }
        lstrcpynA(g_connected_port, port, (int)sizeof(g_connected_port));
        EnterCriticalSection(&g_serial_lock);
        g_active_serial = serial;
        LeaveCriticalSection(&g_serial_lock);
        report_status(SYNC_CONNECTED);
        send_time_sync(serial);
        last_time_sync = GetTickCount();
        sample_cpu_usage(&cpu);

        while (InterlockedCompareExchange(&g_running, 0, 0)) {
            DWORD errors = 0;
            COMSTAT status;
            if (!ClearCommError(serial, &errors, &status)) break;
            handle_device_commands(serial, &status);
            if (!send_pc_status(serial, &cpu, cpu_name,
                                (int)system_info.dwNumberOfProcessors, physical_cores,
                                cpu_mhz, gpus, gpu_count)) break;
            if (GetTickCount() - last_time_sync >= 60000) {
                send_time_sync(serial);
                last_time_sync = GetTickCount();
            }
            if ((!last_weather_update && (!last_weather_attempt ||
                    GetTickCount() - last_weather_attempt >= WEATHER_RETRY_INTERVAL_MS)) ||
                (last_weather_update &&
                    GetTickCount() - last_weather_update >= WEATHER_UPDATE_INTERVAL_MS)) {
                last_weather_attempt = GetTickCount();
                if (fetch_and_send_weather(serial)) last_weather_update = GetTickCount();
            }
            Sleep(SEND_INTERVAL_MS);
        }
        EnterCriticalSection(&g_serial_lock);
        if (g_active_serial == serial) g_active_serial = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&g_serial_lock);
        CloseHandle(serial);
        if (InterlockedCompareExchange(&g_running, 0, 0)) {
            report_status(SYNC_DISCONNECTED);
            Sleep(RECONNECT_DELAY_MS);
        }
    }
    return 0;
}

static HICON create_e_icon(void) {
    HDC screen = GetDC(NULL);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, 32, 32);
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    HGDIOBJ old_bitmap = SelectObject(memory, color);
    HBRUSH background = CreateSolidBrush(RGB(0, 110, 190));
    HFONT font = CreateFontA(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ old_font;
    ICONINFO info;
    HICON icon;

    FillRect(memory, &(RECT){0, 0, 32, 32}, background);
    old_font = SelectObject(memory, font);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(255, 255, 255));
    TextOutA(memory, 7, 0, "E", 1);
    SelectObject(memory, old_font);
    SelectObject(memory, old_bitmap);
    DeleteObject(font);
    DeleteObject(background);
    DeleteDC(memory);
    ReleaseDC(NULL, screen);

    ZeroMemory(&info, sizeof(info));
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = color;
    icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void set_tray_text(const char *text) {
    lstrcpynA(g_tray_data.szTip, text, (int)sizeof(g_tray_data.szTip));
    g_tray_data.uFlags = NIF_TIP;
    Shell_NotifyIconA(NIM_MODIFY, &g_tray_data);
}

static void show_control_panel(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

static int send_gui_command(const char *line) {
    int result = 0;
    EnterCriticalSection(&g_serial_lock);
    if (g_active_serial != INVALID_HANDLE_VALUE) result = write_line_unlocked(g_active_serial, line);
    LeaveCriticalSection(&g_serial_lock);
    return result;
}

static int autostart_enabled(void) {
    HKEY key;
    char value[MAX_PATH * 2];
    DWORD type = 0, size = sizeof(value);
    int enabled = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        enabled = RegQueryValueExA(key, "ESP32HubPcSync", NULL, &type,
                                   (BYTE *)value, &size) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    return enabled;
}

static void set_autostart(int enabled) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    if (enabled) {
        char executable[MAX_PATH], quoted[MAX_PATH + 4];
        GetModuleFileNameA(NULL, executable, sizeof(executable));
        snprintf(quoted, sizeof(quoted), "\"%s\"", executable);
        RegSetValueExA(key, "ESP32HubPcSync", 0, REG_SZ,
                       (const BYTE *)quoted, (DWORD)strlen(quoted) + 1);
    } else {
        RegDeleteValueA(key, "ESP32HubPcSync");
    }
    RegCloseKey(key);
}

static void send_selected_page(void) {
    int page = (int)SendMessageA(g_gui_page_combo, CB_GETCURSEL, 0, 0);
    char command[64];
    if (page < 0 || page >= 8) return;
    snprintf(command, sizeof(command), "{\"cmd\":\"set_page\",\"page\":%d}\n", page);
    send_gui_command(command);
}

static BOOL CALLBACK set_child_font(HWND child, LPARAM font) {
    SendMessageA(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static LRESULT CALLBACK tray_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_CREATE:
            ZeroMemory(&g_tray_data, sizeof(g_tray_data));
            g_tray_data.cbSize = sizeof(g_tray_data);
            g_tray_data.hWnd = window;
            g_tray_data.uID = TRAY_ICON_ID;
            g_tray_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            g_tray_data.uCallbackMessage = WM_TRAY_ICON;
            g_tray_data.hIcon = g_tray_icon;
            lstrcpynA(g_tray_data.szTip, "ESP32 Sync - Starting", (int)sizeof(g_tray_data.szTip));
            Shell_NotifyIconA(NIM_ADD, &g_tray_data);
            g_tray_data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconA(NIM_SETVERSION, &g_tray_data);
            g_gui_status = CreateWindowA("STATIC", "同步状态：正在扫描 ESP32",
                WS_CHILD | WS_VISIBLE, 24, 28, 680, 28, window,
                (HMENU)GUI_STATUS_ID, NULL, NULL);
            g_gui_port = CreateWindowA("STATIC", "串口：等待连接",
                WS_CHILD | WS_VISIBLE, 24, 68, 680, 28, window,
                (HMENU)GUI_PORT_ID, NULL, NULL);
            CreateWindowA("STATIC",
                "设备控制",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 110, 680, 24, window,
                NULL, NULL, NULL);
            CreateWindowA("BUTTON", "开启屏幕", WS_CHILD | WS_VISIBLE,
                24, 140, 110, 32, window, (HMENU)GUI_SCREEN_ON_ID, NULL, NULL);
            CreateWindowA("BUTTON", "关闭屏幕", WS_CHILD | WS_VISIBLE,
                144, 140, 110, 32, window, (HMENU)GUI_SCREEN_OFF_ID, NULL, NULL);
            g_gui_autostart = CreateWindowA("BUTTON", "开机自动启动", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                280, 145, 150, 24, window, (HMENU)GUI_AUTOSTART_ID, NULL, NULL);
            SendMessageA(g_gui_autostart, BM_SETCHECK,
                         autostart_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);

            CreateWindowA("STATIC", "数据发送（取消勾选即暂停该类数据）", WS_CHILD | WS_VISIBLE,
                24, 195, 500, 24, window, NULL, NULL, NULL);
            g_gui_send_cpu = CreateWindowA("BUTTON", "CPU", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                24, 225, 100, 24, window, (HMENU)GUI_SEND_CPU_ID, NULL, NULL);
            g_gui_send_memory = CreateWindowA("BUTTON", "内存", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                134, 225, 100, 24, window, (HMENU)GUI_SEND_MEMORY_ID, NULL, NULL);
            g_gui_send_gpu = CreateWindowA("BUTTON", "显卡", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                244, 225, 100, 24, window, (HMENU)GUI_SEND_GPU_ID, NULL, NULL);
            g_gui_send_fps = CreateWindowA("BUTTON", "FPS", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                354, 225, 100, 24, window, (HMENU)GUI_SEND_FPS_ID, NULL, NULL);
            SendMessageA(g_gui_send_cpu, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageA(g_gui_send_memory, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageA(g_gui_send_gpu, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageA(g_gui_send_fps, BM_SETCHECK, BST_CHECKED, 0);

            CreateWindowA("STATIC", "切换 ESP32 页面", WS_CHILD | WS_VISIBLE,
                24, 280, 150, 24, window, NULL, NULL, NULL);
            g_gui_page_combo = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                174, 276, 386, 220, window, (HMENU)GUI_PAGE_COMBO_ID, NULL, NULL);
            {
                const char *pages[] = {"0 时间/音乐", "1 系统信息", "2 天气", "3 电脑控制",
                    "4 日历", "5 计时", "6 电脑状态", "7 世界时间"};
                int i;
                for (i = 0; i < 8; ++i) SendMessageA(g_gui_page_combo, CB_ADDSTRING, 0, (LPARAM)pages[i]);
                SendMessageA(g_gui_page_combo, CB_SETCURSEL, 0, 0);
            }
            CreateWindowA("STATIC", "ESP32 日志", WS_CHILD | WS_VISIBLE,
                24, 326, 150, 24, window, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "清空日志", WS_CHILD | WS_VISIBLE,
                624, 320, 96, 30, window, (HMENU)GUI_LOG_CLEAR_ID, NULL, NULL);
            g_gui_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
                24, 356, 696, 264, window, (HMENU)GUI_LOG_ID, NULL, NULL);
            SendMessageA(g_gui_log, EM_SETLIMITTEXT, LOG_TEXT_LIMIT, 0);
            {
                HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                EnumChildWindows(window, (WNDENUMPROC)set_child_font, (LPARAM)font);
            }
            return 0;

        case WM_ESP32_LOG:
            append_esp32_log((char *)l_param);
            return 0;

        case WM_SYNC_STATUS:
            if (w_param == SYNC_CONNECTED) {
                char text[64];
                snprintf(text, sizeof(text), "ESP32 Sync - Connected (%s)", g_connected_port);
                set_tray_text(text);
                if (g_gui_status) SetWindowTextA(g_gui_status, "同步状态：设备已连接");
                if (g_gui_port) {
                    char port_text[64];
                    snprintf(port_text, sizeof(port_text), "串口：%s", g_connected_port);
                    SetWindowTextA(g_gui_port, port_text);
                }
            } else if (w_param == SYNC_SCANNING) {
                set_tray_text("ESP32 Sync - Scanning COM ports");
                if (g_gui_status) SetWindowTextA(g_gui_status, "同步状态：正在扫描 ESP32");
                if (g_gui_port) SetWindowTextA(g_gui_port, "串口：等待连接");
            } else {
                set_tray_text("ESP32 Sync - Disconnected, retrying");
                if (g_gui_status) SetWindowTextA(g_gui_status, "同步状态：连接断开，正在重试");
                if (g_gui_port) SetWindowTextA(g_gui_port, "串口：未连接");
            }
            return 0;

        case WM_TRAY_ICON:
            if (LOWORD(l_param) == WM_LBUTTONDBLCLK || LOWORD(l_param) == NIN_SELECT) {
                show_control_panel(window);
                return 0;
            }
            if (LOWORD(l_param) == WM_CONTEXTMENU || LOWORD(l_param) == WM_RBUTTONUP) {
                POINT cursor;
                HMENU menu = CreatePopupMenu();
                AppendMenuA(menu, MF_STRING | MF_DEFAULT, MENU_OPEN_ID, "打开控制面板");
                AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(menu, MF_STRING, MENU_EXIT_ID, "Exit");
                GetCursorPos(&cursor);
                SetForegroundWindow(window);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                               cursor.x, cursor.y, 0, window, NULL);
                DestroyMenu(menu);
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(w_param) == MENU_OPEN_ID) show_control_panel(window);
            else if (LOWORD(w_param) == MENU_EXIT_ID) DestroyWindow(window);
            else if (LOWORD(w_param) == GUI_SCREEN_ON_ID)
                send_gui_command("{\"cmd\":\"screen_on\"}\n");
            else if (LOWORD(w_param) == GUI_SCREEN_OFF_ID)
                send_gui_command("{\"cmd\":\"screen_off\"}\n");
            else if (LOWORD(w_param) == GUI_AUTOSTART_ID)
                set_autostart(SendMessageA(g_gui_autostart, BM_GETCHECK, 0, 0) == BST_CHECKED);
            else if (LOWORD(w_param) == GUI_SEND_CPU_ID)
                InterlockedExchange(&g_pause_cpu,
                    SendMessageA(g_gui_send_cpu, BM_GETCHECK, 0, 0) != BST_CHECKED);
            else if (LOWORD(w_param) == GUI_SEND_MEMORY_ID)
                InterlockedExchange(&g_pause_memory,
                    SendMessageA(g_gui_send_memory, BM_GETCHECK, 0, 0) != BST_CHECKED);
            else if (LOWORD(w_param) == GUI_SEND_GPU_ID)
                InterlockedExchange(&g_pause_gpu,
                    SendMessageA(g_gui_send_gpu, BM_GETCHECK, 0, 0) != BST_CHECKED);
            else if (LOWORD(w_param) == GUI_SEND_FPS_ID)
                InterlockedExchange(&g_pause_fps,
                    SendMessageA(g_gui_send_fps, BM_GETCHECK, 0, 0) != BST_CHECKED);
            else if (LOWORD(w_param) == GUI_PAGE_COMBO_ID && HIWORD(w_param) == CBN_SELCHANGE)
                send_selected_page();
            else if (LOWORD(w_param) == GUI_LOG_CLEAR_ID && g_gui_log)
                SetWindowTextA(g_gui_log, "");
            return 0;

        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;

        case WM_SIZE:
            if (w_param == SIZE_MINIMIZED) ShowWindow(window, SW_HIDE);
            else if (g_gui_log) {
                int width = LOWORD(l_param);
                int height = HIWORD(l_param);
                MoveWindow(g_gui_log, 24, 356, max(200, width - 48), max(100, height - 380), TRUE);
            }
            return 0;

        case WM_DESTROY:
            InterlockedExchange(&g_running, 0);
            Shell_NotifyIconA(NIM_DELETE, &g_tray_data);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(window, message, w_param, l_param);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_command) {
    const char class_name[] = "Esp32PcDataSyncTray";
    WNDCLASSEXA window_class;
    MSG message;
    HANDLE single_instance;
    char *argument = command_line;

    (void)previous;
    (void)show_command;
    while (*argument == ' ' || *argument == '\t') ++argument;
    if (*argument == '"') {
        char *end;
        ++argument;
        end = strchr(argument, '"');
        if (end) *end = '\0';
    } else {
        char *end = strpbrk(argument, " \t");
        if (end) *end = '\0';
    }
    if (*argument) lstrcpynA(g_forced_port, argument, (int)sizeof(g_forced_port));

    single_instance = CreateMutexA(NULL, TRUE, "Local\\ESP32HubPcDataSyncBackground");
    if (!single_instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single_instance) CloseHandle(single_instance);
        return 0;
    }

    InitializeCriticalSection(&g_serial_lock);

    g_tray_icon = create_e_icon();
    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = tray_window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = g_tray_icon;
    window_class.lpszClassName = class_name;
    if (!RegisterClassExA(&window_class)) {
        CloseHandle(single_instance);
        return 1;
    }

    /* 创建默认隐藏的控制面板，用户可从托盘打开。 */
    g_tray_window = CreateWindowExA(0, class_name, "ESP32 Hub 后台同步",
                                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 700,
                                    NULL, NULL, instance, NULL);
    if (!g_tray_window) {
        DestroyIcon(g_tray_icon);
        CloseHandle(single_instance);
        return 1;
    }
    g_worker_thread = CreateThread(NULL, 0, sync_worker, NULL, 0, NULL);
    if (!g_worker_thread) DestroyWindow(g_tray_window);

    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    if (g_worker_thread) {
        WaitForSingleObject(g_worker_thread, 5000);
        CloseHandle(g_worker_thread);
    }

    if (g_gpu_query) PdhCloseQuery(g_gpu_query);
    if (g_cpu_frequency_query) PdhCloseQuery(g_cpu_frequency_query);
    if (g_cpu_power_query) PdhCloseQuery(g_cpu_power_query);
    DestroyIcon(g_tray_icon);
    ReleaseMutex(single_instance);
    CloseHandle(single_instance);
    DeleteCriticalSection(&g_serial_lock);
    return 0;
}
