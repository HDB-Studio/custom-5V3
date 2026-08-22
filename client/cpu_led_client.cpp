// ============================================================
// cpu_led_client.cpp — CPU → LED 灯带 客户端（Windows）
// ------------------------------------------------------------
// 特性：
//   1. 开机自启动（注册 HKCU\...\Run）
//   2. 实时监测 CPU 使用率（GetSystemTimes，开销极低）
//   3. CPU% → 颜色映射（连续分段线性，边界均平滑过渡）：
//        0~10%    浅蓝
//        10~50%   浅蓝 → 绿
//        50~80%   绿 → 黄
//        80~100%  黄 → 红
//   4. 颜色平滑过渡：沿预设颜色序列(浅蓝→绿→黄→红)逐级平滑推进
//   5. 超级轻量：纯 Win32，无任何第三方依赖，常驻内存约 3~8MB
// ------------------------------------------------------------
// 构建：见 build_msvc.bat / build_mingw.bat
// 用法：
//   cpu_led_client.exe                正常启动（显示状态小窗）
//   cpu_led_client.exe --hide         后台启动（不显示窗口，用于开机自启）
//   cpu_led_client.exe --port COM5    指定串口，跳过自动识别
//   cpu_led_client.exe --no-autostart 本次启动不注册开机自启
//   cpu_led_client.exe --help         帮助
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

// ---------- 常量配置 ----------
#define APP_NAME_W      L"CPULedClient"
#define APP_MUTEX_W     L"CPULedClient_SingleInstanceMutex"
#define RUN_KEY_NAME_A  "CPULedClient"
#define RUN_KEY_PATH_A  "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define TRAY_ID         1
#define WM_TRAYICON     (WM_APP + 1)

#define IDT_CPU         1   // CPU 采样周期(ms)
#define IDT_SEND        2   // 颜色平滑与发送周期(ms)
#define IDT_RECONNECT   3   // 串口重连检测周期(ms)

#define CPU_INTERVAL       1000
#define SEND_INTERVAL      20      // 平滑/发送周期(ms)，约 50 FPS
#define RECONNECT_INTERVAL 5000

// 平滑速度：显示 CPU% 每秒向真实值靠拢的百分比量。
// 沿“浅蓝→绿→黄→红”预设序列逐级推进，全量程约 2 秒走完。
#define CPU_SMOOTH_SPEED   50.0f

// 满载快闪告警：CPU 达到该阈值时红灯快速闪烁
#define BLINK_CPU_THRESHOLD 99.0f   // 触发快闪的 CPU%(显示值)
#define BLINK_INTERVAL_MS   100     // 快闪半周期(ms)

#define BAUD_RATE          CBR_115200

// ---------- 颜色关键点 ----------
struct RGBF { float r, g, b; };

static const RGBF C_LIGHT_BLUE = { 135, 206, 235 }; // 浅蓝(天蓝)
static const RGBF C_DEEP_BLUE  = {   0,   0, 255 }; // 深蓝
static const RGBF C_GREEN      = {   0, 255,   0 }; // 绿
static const RGBF C_YELLOW     = { 255, 255,   0 }; // 黄
static const RGBF C_RED        = { 255,   0,   0 }; // 红

// ---------- 全局状态 ----------
static HINSTANCE      g_hinst       = nullptr;
static HWND           g_hMainWnd    = nullptr;
static HWND           g_hStatusWnd  = nullptr;

static ULONGLONG      g_lastIdle    = 0;
static ULONGLONG      g_lastKernel  = 0;
static ULONGLONG      g_lastUser    = 0;
static bool           g_cpuHasBase  = false;   // 是否已有基准采样
static float          g_cpuPercent  = 0.0f;
static float          g_dispCpu     = 0.0f;     // 平滑后的显示 CPU%，驱动颜色

static RGBF           g_curColor    = C_LIGHT_BLUE;
static RGBF           g_targetColor = C_LIGHT_BLUE;
static bool           g_blinkOn     = false;
static ULONGLONG      g_lastBlinkMs = 0;

static HANDLE         g_serial      = INVALID_HANDLE_VALUE;
static std::string    g_port;
static bool           g_connected   = false;

static bool           g_autostartOn = false;
static bool           g_hideOnStart = false;
static bool           g_noAutostart = false;
static std::string    g_forcedPort;   // 空 = 自动识别
static bool           g_statusVisible = false;

// ============================================================
// 串口操作
// ============================================================

// 打开串口并配置 115200 8N1
static bool SerialOpen(const std::string& port, HANDLE& out) {
    std::string path = "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
    dcb.BaudRate     = BAUD_RATE;
    dcb.ByteSize     = 8;
    dcb.Parity       = NOPARITY;
    dcb.StopBits     = ONESTOPBIT;
    dcb.fDtrControl  = DTR_CONTROL_ENABLE;
    dcb.fRtsControl  = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout        = 50;
    t.ReadTotalTimeoutConstant   = 200;
    t.ReadTotalTimeoutMultiplier = 0;
    t.WriteTotalTimeoutConstant  = 50;
    t.WriteTotalTimeoutMultiplier= 0;
    SetCommTimeouts(h, &t);

    out = h;
    return true;
}

// 枚举系统所有串口名（注册表）
static std::vector<std::string> EnumSerialPorts() {
    std::vector<std::string> ports;
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        for (DWORD i = 0; ; ++i) {
            char name[64] = {0};
            char data[64] = {0};
            DWORD nameLen = sizeof(name);
            DWORD dataLen = sizeof(data);
            LONG r = RegEnumValueA(hk, i, name, &nameLen, nullptr, nullptr,
                                   (BYTE*)data, &dataLen);
            if (r != ERROR_SUCCESS) break;
            if (data[0]) ports.emplace_back(data);
        }
        RegCloseKey(hk);
    }
    return ports;
}

// 尝试连接指定串口，通过握手确认对端为 UNO 固件
static bool TryConnectPort(const std::string& port) {
    HANDLE h;
    if (!SerialOpen(port, h)) return false;

    const char* ping = "?\n";
    // 打开串口会触发 UNO 复位，需等待其跳过 bootloader 并运行固件
    // 因此做多次握手尝试
    for (int attempt = 0; attempt < 4; ++attempt) {
        Sleep(500);
        DWORD w = 0;
        WriteFile(h, ping, (DWORD)strlen(ping), &w, nullptr);
        PurgeComm(h, PURGE_RXCLEAR);
        WriteFile(h, ping, (DWORD)strlen(ping), &w, nullptr);

        char buf[32] = {0};
        DWORD n = 0;
        if (ReadFile(h, buf, (DWORD)sizeof(buf) - 1, &n, nullptr) && n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "OK") != nullptr) {
                g_serial    = h;
                g_port      = port;
                g_connected = true;
                return true;
            }
        }
    }
    CloseHandle(h);
    return false;
}

// 自动识别：优先指定端口，否则遍历系统串口做握手
static void TryConnect() {
    if (g_connected) return;

    if (!g_forcedPort.empty()) {
        TryConnectPort(g_forcedPort);
        return;
    }
    for (const auto& p : EnumSerialPorts()) {
        if (TryConnectPort(p)) return;
    }
}

// 关闭串口
static void CloseSerial() {
    if (g_serial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_serial);
        g_serial = INVALID_HANDLE_VALUE;
    }
    g_connected = false;
    g_port.clear();
}

// 向串口发送当前颜色
static void SendColor() {
    if (g_serial == INVALID_HANDLE_VALUE) return;

    int r = (int)(g_curColor.r + 0.5f);
    int g = (int)(g_curColor.g + 0.5f);
    int b = (int)(g_curColor.b + 0.5f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    char msg[32];
    int len = snprintf(msg, sizeof(msg), "%d,%d,%d\n", r, g, b);

    DWORD w = 0;
    if (!WriteFile(g_serial, msg, (DWORD)len, &w, nullptr)) {
        CloseSerial();   // 串口断开，交由重连定时器恢复
    }
}

// ============================================================
// CPU 采样（GetSystemTimes，零依赖、开销极低）
// ============================================================

static void InitCpuSampling() {
    // 采集一次基准值，供首次 SampleCpu 计算差值
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        g_lastIdle   = (((ULONGLONG)idle.dwHighDateTime)   << 32) | idle.dwLowDateTime;
        g_lastKernel = (((ULONGLONG)kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        g_lastUser   = (((ULONGLONG)user.dwHighDateTime)   << 32) | user.dwLowDateTime;
        g_cpuHasBase = true;
    }
}

static void SampleCpu() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return;

    ULONGLONG curIdle   = (((ULONGLONG)idle.dwHighDateTime)   << 32) | idle.dwLowDateTime;
    ULONGLONG curKernel = (((ULONGLONG)kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    ULONGLONG curUser   = (((ULONGLONG)user.dwHighDateTime)   << 32) | user.dwLowDateTime;

    if (g_cpuHasBase) {
        // 本周期系统总时间 = (kernel+user) 增量，空闲增量为 idle
        ULONGLONG total  = (curKernel - g_lastKernel) + (curUser - g_lastUser);
        ULONGLONG active = total - (curIdle - g_lastIdle);
        if (total != 0) {
            g_cpuPercent = (float)((double)active * 100.0 / (double)total);
            if (g_cpuPercent < 0) g_cpuPercent = 0;
            if (g_cpuPercent > 100) g_cpuPercent = 100;
        }
    }

    g_lastIdle   = curIdle;
    g_lastKernel = curKernel;
    g_lastUser   = curUser;
    g_cpuHasBase = true;
}

// ============================================================
// CPU% → 颜色映射（连续分段线性，边界平滑过渡）
// ============================================================
//   0 ~ 10%   浅蓝（恒定）
//   10 ~ 50%  浅蓝 → 绿 线性过渡
//   50 ~ 80%  绿 → 黄 线性过渡
//   80 ~ 100% 黄 → 红 线性过渡
static RGBF Lerp(const RGBF& a, const RGBF& b, float t) {
    RGBF c;
    c.r = a.r + (b.r - a.r) * t;
    c.g = a.g + (b.g - a.g) * t;
    c.b = a.b + (b.b - a.b) * t;
    return c;
}

static RGBF ColorFromCpu(float cpu) {
    // 颜色锚点：按阈值升序排列
    struct Stop { float t; RGBF c; };
    static const Stop stops[] = {
        {   0.0f, C_LIGHT_BLUE },  // 0%    浅蓝
        {  10.0f, C_LIGHT_BLUE },  // 10%   浅蓝（<10% 保持浅蓝）
        {  20.0f, C_DEEP_BLUE  },  // 20%   深蓝
        {  50.0f, C_GREEN      },  // 50%   绿
        {  80.0f, C_YELLOW     },  // 80%   黄
        { 100.0f, C_RED        },  // 100%  红
    };
    const int n = (int)(sizeof(stops) / sizeof(stops[0]));

    if (cpu <= stops[0].t)   return stops[0].c;
    if (cpu >= stops[n-1].t) return stops[n-1].c;

    for (int i = 0; i < n - 1; ++i) {
        if (cpu >= stops[i].t && cpu <= stops[i+1].t) {
            float span = stops[i+1].t - stops[i].t;
            float t = (span > 0.0f) ? (cpu - stops[i].t) / span : 0.0f;
            return Lerp(stops[i].c, stops[i+1].c, t);
        }
    }
    return C_RED; // 理论不可达
}

// 平滑推进：显示 CPU% 沿预设颜色序列向真实 CPU% 逐级靠拢。
// 颜色由 ColorFromCpu(显示值) 得到，天然沿 浅蓝→深蓝→绿→黄→红 顺序过渡，
// 即便真实 CPU 从 100% 骤降到 1%，也会依次经过红、黄、绿、深蓝、浅蓝各档颜色。
static void SmoothStep() {
    float step = CPU_SMOOTH_SPEED * ((float)SEND_INTERVAL / 1000.0f);
    float diff = g_cpuPercent - g_dispCpu;
    if (std::fabs(diff) <= step) {
        g_dispCpu = g_cpuPercent;
    } else {
        g_dispCpu += (diff > 0.0f) ? step : -step;
    }
    g_curColor = ColorFromCpu(g_dispCpu);
}

// 满载快闪：CPU 达到阈值时红灯快速闪烁，作为告警
static void ApplyBlink() {
    bool over = (g_dispCpu >= BLINK_CPU_THRESHOLD);
    if (!over) { g_blinkOn = false; return; }

    ULONGLONG now = GetTickCount64();
    if (now - g_lastBlinkMs >= BLINK_INTERVAL_MS) {
        g_lastBlinkMs = now;
        g_blinkOn = !g_blinkOn;
    }
    g_curColor = g_blinkOn ? C_RED : RGBF{ 0.0f, 0.0f, 0.0f };
}

// ============================================================
// 开机自启动（当前用户 Run 键）
// ============================================================

static bool IsAutostartEnabled() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY_PATH_A, 0,
                      KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    LSTATUS r = RegQueryValueExA(hk, RUN_KEY_NAME_A, nullptr,
                                 &type, nullptr, &size);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

static void SetAutostart(bool on) {
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, RUN_KEY_PATH_A, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;

    if (on) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string v = "\"" + std::string(path) + "\" --hide";
        RegSetValueExA(hk, RUN_KEY_NAME_A, 0, REG_SZ,
                       (const BYTE*)v.c_str(), (DWORD)v.size() + 1);
    } else {
        RegDeleteValueA(hk, RUN_KEY_NAME_A);
    }
    RegCloseKey(hk);
    g_autostartOn = IsAutostartEnabled();
}

// ============================================================
// 状态小窗
// ============================================================

static void StatusPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

    // 颜色色块
    RECT sw = { 24, 24, 124, 124 };
    HBRUSH cb = CreateSolidBrush(RGB((BYTE)(int)g_curColor.r,
                                     (BYTE)(int)g_curColor.g,
                                     (BYTE)(int)g_curColor.b));
    FillRect(hdc, &sw, cb);
    DeleteObject(cb);
    FrameRect(hdc, &sw, (HBRUSH)GetStockObject(GRAY_BRUSH));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(40, 40, 40));

    wchar_t buf[128];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"CPU 使用率: %.1f%%",
                     (double)g_cpuPercent);
    TextOutW(hdc, 148, 24, buf, (int)wcslen(buf));

    StringCchPrintfW(buf, ARRAYSIZE(buf), L"当前颜色: (%d, %d, %d)",
                     (int)g_curColor.r, (int)g_curColor.g, (int)g_curColor.b);
    TextOutW(hdc, 148, 48, buf, (int)wcslen(buf));

    wchar_t portW[64] = L"未连接";
    if (g_connected) {
        MultiByteToWideChar(CP_ACP, 0, g_port.c_str(), -1,
                            portW, ARRAYSIZE(portW));
    }
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"串口: %s", portW);
    TextOutW(hdc, 148, 72, buf, (int)wcslen(buf));

    StringCchPrintfW(buf, ARRAYSIZE(buf), L"目标颜色: (%d, %d, %d)",
                     (int)g_targetColor.r, (int)g_targetColor.g,
                     (int)g_targetColor.b);
    TextOutW(hdc, 148, 96, buf, (int)wcslen(buf));

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK StatusWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        StatusPaint(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hStatusWnd = nullptr;
        g_statusVisible = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowStatusWindow(bool show) {
    if (show && !g_hStatusWnd) {
        g_hStatusWnd = CreateWindowExW(0, L"CPULedStatusWnd",
                                       L"CPU LED 客户端",
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                       CW_USEDEFAULT, CW_USEDEFAULT, 440, 190,
                                       nullptr, nullptr, g_hinst, nullptr);
        if (g_hStatusWnd) {
            ShowWindow(g_hStatusWnd, SW_SHOW);
            UpdateWindow(g_hStatusWnd);
            g_statusVisible = true;
        }
    } else if (!show && g_hStatusWnd) {
        ShowWindow(g_hStatusWnd, SW_HIDE);
        g_statusVisible = false;
    }
}

// ============================================================
// 托盘图标与菜单
// ============================================================

static void AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hMainWnd;
    nid.uID              = TRAY_ID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = LoadIcon(nullptr, IDI_APPLICATION);
    StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), L"CPU LED 客户端");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void UpdateTrayTip() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hMainWnd;
    nid.uID    = TRAY_ID;
    nid.uFlags = NIF_TIP;
    StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip),
                     L"CPU: %.1f%%  %s",
                     (double)g_cpuPercent,
                     g_connected ? L"已连接" : L"未连接");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();

    wchar_t item[64];
    StringCchPrintfW(item, ARRAYSIZE(item), L"CPU 使用率: %.1f%%",
                     (double)g_cpuPercent);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 10, item);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_autostartOn ? MF_CHECKED : 0),
                11, L"开机自启动");
    AppendMenuW(menu, MF_STRING, 12, g_statusVisible ? L"隐藏状态窗" : L"显示状态窗");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 13, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hMainWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_hMainWnd, nullptr);
    DestroyMenu(menu);
}

// ============================================================
// 主窗口
// ============================================================

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == IDT_CPU) {
            SampleCpu();
            g_targetColor = ColorFromCpu(g_cpuPercent);
            UpdateTrayTip();
            if (g_hStatusWnd) InvalidateRect(g_hStatusWnd, nullptr, FALSE);
        } else if (wp == IDT_SEND) {
            SmoothStep();
            ApplyBlink();
            SendColor();
        } else if (wp == IDT_RECONNECT) {
            TryConnect();
        }
        return 0;

    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP) {
            ShowTrayMenu();
        } else if (lp == WM_LBUTTONDBLCLK) {
            ShowStatusWindow(!g_statusVisible);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 11:
            SetAutostart(!g_autostartOn);
            break;
        case 12:
            ShowStatusWindow(!g_statusVisible);
            break;
        case 13:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_CPU);
        KillTimer(hwnd, IDT_SEND);
        KillTimer(hwnd, IDT_RECONNECT);
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = hwnd;
        nid.uID    = TRAY_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        CloseSerial();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// 入口
// ============================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // 单实例保护
    CreateMutexW(nullptr, TRUE, APP_MUTEX_W);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_hinst = hInstance;

    // 解析命令行参数
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--hide") == 0) {
                g_hideOnStart = true;
            } else if (wcscmp(argv[i], L"--no-autostart") == 0) {
                g_noAutostart = true;   // 本次启动不注册开机自启
            } else if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc) {
                char buf[32] = {0};
                WideCharToMultiByte(CP_ACP, 0, argv[++i], -1,
                                    buf, sizeof(buf), nullptr, nullptr);
                g_forcedPort = buf;
            } else if (wcscmp(argv[i], L"--help") == 0 ||
                       wcscmp(argv[i], L"-h") == 0) {
                MessageBoxW(nullptr,
                    L"CPU LED 客户端\n\n"
                    L"用法:\n"
                    L"  cpu_led_client.exe                 启动(显示状态窗)\n"
                    L"  cpu_led_client.exe --hide          后台启动(开机自启使用)\n"
                    L"  cpu_led_client.exe --port COM5     指定串口\n"
                    L"  cpu_led_client.exe --no-autostart  不注册开机自启\n",
                    L"帮助", MB_OK);
                LocalFree(argv);
                return 0;
            }
        }
        LocalFree(argv);
    }

    // 注册窗口类
    WNDCLASSW mwc{};
    mwc.lpfnWndProc   = MainWndProc;
    mwc.hInstance     = hInstance;
    mwc.lpszClassName = L"CPULedMainWnd";
    mwc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassW(&mwc)) return 1;

    WNDCLASSW swc{};
    swc.lpfnWndProc   = StatusWndProc;
    swc.hInstance     = hInstance;
    swc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    swc.lpszClassName = L"CPULedStatusWnd";
    RegisterClassW(&swc);

    // 主窗口（隐藏）
    g_hMainWnd = CreateWindowExW(0, L"CPULedMainWnd", APP_NAME_W,
                                 WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, hInstance, nullptr);
    if (!g_hMainWnd) return 1;

    // 自启动：默认注册开机自启动（--no-autostart 时不注册），
    // 用户可在托盘菜单中随时开关
    if (!g_noAutostart && !IsAutostartEnabled()) {
        SetAutostart(true);
    }
    g_autostartOn = IsAutostartEnabled();

    // 初始化
    AddTrayIcon();
    InitCpuSampling();

    SetTimer(g_hMainWnd, IDT_CPU,       CPU_INTERVAL,       nullptr);
    SetTimer(g_hMainWnd, IDT_SEND,      SEND_INTERVAL,      nullptr);
    SetTimer(g_hMainWnd, IDT_RECONNECT, RECONNECT_INTERVAL, nullptr);

    // 先显示状态窗，再尝试连接（连接过程可能阻塞数秒）
    if (!g_hideOnStart) ShowStatusWindow(true);
    TryConnect();

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
