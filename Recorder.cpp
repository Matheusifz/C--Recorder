// Recorder.cpp  (macro recorder + playback + OpenCV template hunt)
//
// Commands:
//   Recorder.exe record        <file.rmac>
//   Recorder.exe play          <file.rmac>
//   Recorder.exe recordhunt    <file.rmac> <enemy_path> <battle_start.png> [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//   Recorder.exe playhunt      <file.rmac> <enemy_path> <battle_start.png> [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//
// Behavior:
// - recordhunt: hunter runs SCAN-ONLY (no clicks) during recording; stops when BattleStart detected; SHIFT restarts hunting
// - playhunt  : hunter runs SCAN+ATTACK during playback; stops when BattleStart detected; SHIFT restarts hunting
//
// Build (MSVC, x64 tools prompt):
//   cl /EHsc /O2 /MD Recorder.cpp user32.lib winmm.lib gdi32.lib ^
//      /I"opencv\build\include" ^
//      /link /MACHINE:X64 ^
//      /LIBPATH:"opencv\build\x64\vc16\lib" opencv_world4120.lib
//
// Runtime (DLL path) if not in system PATH:
//   set "PATH=%PATH%;C:\Users\T-GAMER\Desktop\Codigo\Codigo\CppRecorder\opencv\build\x64\vc16\bin"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

using namespace cv;

static const char *kSinkClassName = "RawIO_Sink_Window";
static const char *kOverlayClassName = "RawIO_Overlay_Window";

enum EventType : uint32_t
{
    EV_MOUSE_MOVE = 0,
    EV_MOUSE_WHEEL = 1,
    EV_KEY_DOWN = 2,
    EV_KEY_UP = 3,
    EV_MOUSE_BUTTON = 4,
    EV_MOUSE_POS = 5,
};

#pragma pack(push, 1)
struct FileHeader
{
    uint32_t magic;     // 'RMAC' = 0x524D4143
    uint32_t version;   // 1
    uint64_t start_utc; // FILETIME informational
};
struct Event
{
    uint32_t type;
    uint64_t t_us;
    int32_t a;
    int32_t b;
    int32_t c;
};
#pragma pack(pop)

// --------------------------- Globals ---------------------------

static LARGE_INTEGER gFreq{};
static LARGE_INTEGER gT0{};

static FILE *gOut = nullptr;
static bool gRecording = false;
static bool gPlaying = false;

static HWND gSinkHwnd = nullptr;
static HWND gOverlayHwnd = nullptr;

// Overlay state
static LONG gLastDx = 0, gLastDy = 0;
static int gLastWheel = 0;
static bool gMouseBtn[6] = {};
static bool gKeyDown[256] = {};
static POINT gCursorPt{};

// --------------------------- Hunt config/state ---------------------------

static std::atomic<bool> gAutoHuntRun{false};
static std::thread gAutoHuntThread;
static std::atomic<bool> gBattleStarted{false};

static std::string gEnemyTemplatesPath;
static std::string gBattleStartPath;
static double gEnemyTh = 0.75;
static double gBattleTh = 0.88;
static int gScanMs = 200;
static int gCooldownMs = 900;

struct HuntInfo
{
    std::atomic<int> detections{0};
    std::atomic<int> attacks{0};
    std::atomic<int> lastX{-1};
    std::atomic<int> lastY{-1};
    std::atomic<double> lastConf{0.0};
    std::atomic<bool> lastWasBattle{false};

    mutable std::mutex nameMu;
    char lastName[256]{};

    void setLastName(const char *s)
    {
        std::lock_guard<std::mutex> lk(nameMu);
        std::snprintf(lastName, sizeof(lastName), "%s", (s && s[0]) ? s : "(unknown)");
    }
    void getLastName(char *out, size_t outSz) const
    {
        std::lock_guard<std::mutex> lk(nameMu);
        std::snprintf(out, outSz, "%s", lastName[0] ? lastName : "(none)");
    }
};
static HuntInfo gHuntInfo;

// --------------------------- DPI Awareness ---------------------------

static void enable_dpi_awareness()
{
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (user32)
    {
        using SetDpiCtxFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
        auto pSetCtx = (SetDpiCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (pSetCtx)
        {
            pSetCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            FreeLibrary(user32);
            return;
        }
        FreeLibrary(user32);
    }
    SetProcessDPIAware();
}

// --------------------------- Timing / IO ---------------------------

static uint64_t now_us_since_start()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const long double dt =
        (long double)(t.QuadPart - gT0.QuadPart) / (long double)gFreq.QuadPart;
    return (uint64_t)(dt * 1000000.0L);
}

static void countdown_3s(const char *msg)
{
    std::cout << msg << " in 3 seconds...\n";
    for (int i = 3; i > 0; --i)
    {
        std::cout << i << "...\n";
        Sleep(1000);
    }
}

static void write_event(uint32_t type, int32_t a = 0, int32_t b = 0, int32_t c = 0)
{
    if (!gOut)
        return;
    Event ev{};
    ev.type = type;
    ev.t_us = now_us_since_start();
    ev.a = a;
    ev.b = b;
    ev.c = c;
    fwrite(&ev, sizeof(ev), 1, gOut);
    fflush(gOut);
}

static uint64_t read_exact(FILE *f, void *buf, uint64_t sz)
{
    return (uint64_t)fread(buf, 1, (size_t)sz, f);
}

// --------------------------- Overlay ---------------------------

static void overlay_invalidate()
{
    if (gOverlayHwnd)
        InvalidateRect(gOverlayHwnd, nullptr, FALSE);
}

static void overlay_show(bool on)
{
    if (!gOverlayHwnd)
        return;

    if (on)
    {
        ShowWindow(gOverlayHwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(gOverlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(gOverlayHwnd);
        overlay_invalidate();
    }
    else
    {
        ShowWindow(gOverlayHwnd, SW_HIDE);
    }
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(10, 10, 10));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(230, 230, 230));

        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT old = (HFONT)SelectObject(hdc, font);

        char line[512];
        int y = 8;
        auto put = [&](const char *s)
        {
            TextOutA(hdc, 8, y, s, (int)std::strlen(s));
            y += 18;
        };

        put(gRecording ? "RawIO Overlay (Recording)" : (gPlaying ? "RawIO Overlay (Playing)" : "RawIO Overlay"));

        std::snprintf(line, sizeof(line), "Cursor: %ld, %ld", (long)gCursorPt.x, (long)gCursorPt.y);
        put(line);

        std::snprintf(line, sizeof(line), "Mouse dx/dy: %ld / %ld", gLastDx, gLastDy);
        put(line);

        std::snprintf(line, sizeof(line), "Wheel: %d", gLastWheel);
        put(line);

        std::snprintf(line, sizeof(line), "Buttons: L=%d R=%d M=%d X1=%d X2=%d",
                      gMouseBtn[1], gMouseBtn[2], gMouseBtn[3], gMouseBtn[4], gMouseBtn[5]);
        put(line);

        std::snprintf(line, sizeof(line),
                      "Keys: W=%d A=%d S=%d D=%d Shift=%d Ctrl=%d Alt=%d Space=%d",
                      gKeyDown['W'], gKeyDown['A'], gKeyDown['S'], gKeyDown['D'],
                      gKeyDown[VK_SHIFT], gKeyDown[VK_CONTROL], gKeyDown[VK_MENU], gKeyDown[VK_SPACE]);
        put(line);

        std::snprintf(line, sizeof(line), "AutoHunt: %d  BattleStarted: %d",
                      (int)gAutoHuntRun.load(), (int)gBattleStarted.load());
        put(line);

        char nm[256];
        gHuntInfo.getLastName(nm, sizeof(nm));
        std::snprintf(line, sizeof(line), "Hunt: det=%d atk=%d",
                      gHuntInfo.detections.load(), gHuntInfo.attacks.load());
        put(line);

        std::snprintf(line, sizeof(line), "Last: %s conf=%.2f pos=(%d,%d)%s",
                      nm,
                      gHuntInfo.lastConf.load(),
                      gHuntInfo.lastX.load(), gHuntInfo.lastY.load(),
                      gHuntInfo.lastWasBattle.load() ? " [BATTLE]" : "");
        put(line);

        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static bool create_overlay_window()
{
    WNDCLASSA wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kOverlayClassName;

    if (!RegisterClassA(&wc))
    {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::fprintf(stderr, "RegisterClassA overlay failed (%lu)\n", e);
            return false;
        }
    }

    const int w = 360, h = 260;
    const int x = 10, y = 10;

    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP;

    gOverlayHwnd = CreateWindowExA(
        ex, kOverlayClassName, "RawIO Overlay",
        style, x, y, w, h,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!gOverlayHwnd)
    {
        std::fprintf(stderr, "CreateWindowExA overlay failed (%lu)\n", GetLastError());
        return false;
    }

    SetLayeredWindowAttributes(gOverlayHwnd, 0, 210, LWA_ALPHA);
    ShowWindow(gOverlayHwnd, SW_HIDE);
    return true;
}

static void destroy_overlay_window()
{
    if (gOverlayHwnd)
    {
        DestroyWindow(gOverlayHwnd);
        gOverlayHwnd = nullptr;
    }
}

static void pump_messages_nonblocking()
{
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

// --------------------------- Raw Input Sink ---------------------------

static bool register_raw(HWND hwnd)
{
    RAWINPUTDEVICE rids[2]{};

    rids[0].usUsagePage = 0x01;
    rids[0].usUsage = 0x02;
    rids[0].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
    rids[0].hwndTarget = hwnd;

    rids[1].usUsagePage = 0x01;
    rids[1].usUsage = 0x06;
    rids[1].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
    rids[1].hwndTarget = hwnd;

    return RegisterRawInputDevices(rids, 2, sizeof(RAWINPUTDEVICE)) == TRUE;
}

static void update_overlay_state_on_mouse()
{
    GetCursorPos(&gCursorPt);
    overlay_invalidate();
}

static void update_overlay_state_on_key(UINT vk, bool down)
{
    if (vk < 256)
        gKeyDown[vk] = down;
    overlay_invalidate();
}

static LRESULT CALLBACK SinkProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INPUT:
    {
        if (!gRecording)
            break;

        UINT size = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (!size)
            break;

        std::vector<BYTE> buf(size);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            break;

        RAWINPUT *raw = reinterpret_cast<RAWINPUT *>(buf.data());

        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE &m = raw->data.mouse;

            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
            {
                LONG dx = m.lLastX;
                LONG dy = m.lLastY;

                if (dx != 0 || dy != 0)
                {
                    gLastDx = dx;
                    gLastDy = dy;

                    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                    if (!altDown)
                        write_event(EV_MOUSE_MOVE, (int32_t)dx, (int32_t)dy, 0);
                    else
                    {
                        POINT pt{};
                        if (GetCursorPos(&pt))
                            write_event(EV_MOUSE_POS, (int32_t)pt.x, (int32_t)pt.y, 0);
                    }
                    update_overlay_state_on_mouse();
                }
            }

            if (m.usButtonFlags & RI_MOUSE_WHEEL)
            {
                SHORT wheelDelta = *reinterpret_cast<const SHORT *>(&m.usButtonData);
                gLastWheel = (int)wheelDelta;
                write_event(EV_MOUSE_WHEEL, (int32_t)wheelDelta, 0, 0);
                update_overlay_state_on_mouse();
            }

            auto log_button = [&](int button, bool down)
            {
                if (button >= 1 && button <= 5)
                    gMouseBtn[button] = down;
                write_event(EV_MOUSE_BUTTON, (int32_t)button, down ? 1 : 0, 0);
                update_overlay_state_on_mouse();
            };

            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
                log_button(1, true);
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
                log_button(1, false);
            if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
                log_button(2, true);
            if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
                log_button(2, false);
            if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
                log_button(3, true);
            if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
                log_button(3, false);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)
                log_button(4, true);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_4_UP)
                log_button(4, false);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)
                log_button(5, true);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_5_UP)
                log_button(5, false);
        }
        else if (raw->header.dwType == RIM_TYPEKEYBOARD)
        {
            const RAWKEYBOARD &kb = raw->data.keyboard;

            bool isBreak = (kb.Flags & RI_KEY_BREAK) != 0;
            UINT vk = kb.VKey;
            if (vk == 255)
                break;

            // FIX: ESC stops recording but is NOT written to the macro file.
            if (!isBreak && vk == VK_ESCAPE)
            {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                break;
            }

            if (isBreak)
            {
                write_event(EV_KEY_UP, (int32_t)vk, 0, 0);
                update_overlay_state_on_key(vk, false);
            }
            else
            {
                write_event(EV_KEY_DOWN, (int32_t)vk, 0, 0);
                update_overlay_state_on_key(vk, true);
            }
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static bool create_sink_window()
{
    WNDCLASSA wc{};
    wc.lpfnWndProc = SinkProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kSinkClassName;

    if (!RegisterClassA(&wc))
    {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::fprintf(stderr, "RegisterClassA sink failed (%lu)\n", e);
            return false;
        }
    }

    gSinkHwnd = CreateWindowExA(
        0, kSinkClassName, "RawIO_Sink",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!gSinkHwnd)
    {
        std::fprintf(stderr, "CreateWindowExA sink failed (%lu)\n", GetLastError());
        return false;
    }

    ShowWindow(gSinkHwnd, SW_HIDE);

    if (!register_raw(gSinkHwnd))
    {
        std::fprintf(stderr, "RegisterRawInputDevices failed (%lu)\n", GetLastError());
        return false;
    }
    return true;
}

static void destroy_sink_window()
{
    if (gSinkHwnd)
    {
        DestroyWindow(gSinkHwnd);
        gSinkHwnd = nullptr;
    }
}

// --------------------------- SendInput helpers (playback) ---------------------------

static void send_mouse_move_rel(int dx, int dy)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

static void send_mouse_move_abs(int x, int y)
{
    int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsw <= 0 || vsh <= 0)
        return;

    double relx = (double)(x - vsx) / (double)vsw;
    double rely = (double)(y - vsy) / (double)vsh;

    if (relx < 0.0)
        relx = 0.0;
    if (relx > 1.0)
        relx = 1.0;
    if (rely < 0.0)
        rely = 0.0;
    if (rely > 1.0)
        rely = 1.0;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = (LONG)(relx * 65535.0 + 0.5);
    in.mi.dy = (LONG)(rely * 65535.0 + 0.5);
    SendInput(1, &in, sizeof(INPUT));
}

static void send_mouse_wheel(int delta)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}

static void send_mouse_button(int button, bool down)
{
    INPUT in{};
    in.type = INPUT_MOUSE;

    switch (button)
    {
    case 1:
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 2:
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case 3:
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 4:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON1;
        break;
    case 5:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON2;
        break;
    default:
        return;
    }
    SendInput(1, &in, sizeof(INPUT));
}

static void send_key(bool down, UINT vk)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// --------------------------- Template Detector ---------------------------

class TemplateDetector
{
public:
    bool loadEnemyTemplates(const std::string &path)
    {
        enemies_.clear();
        enemyNames_.clear();

        DWORD attr = GetFileAttributesA(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            std::fprintf(stderr, "Enemy templates path not found: %s\n", path.c_str());
            return false;
        }

        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            return loadFolder(path);

        Mat img = imread(path, IMREAD_COLOR);
        if (img.empty())
        {
            std::fprintf(stderr, "Failed to load enemy template: %s\n", path.c_str());
            return false;
        }
        enemies_.push_back(img);
        enemyNames_.push_back(path);
        std::printf("Loaded enemy template: %s (%dx%d)\n", path.c_str(), img.cols, img.rows);
        return true;
    }

    bool loadBattleStartTemplate(const std::string &file)
    {
        battle_ = imread(file, IMREAD_COLOR);
        if (battle_.empty())
        {
            std::fprintf(stderr, "Failed to load battle start template: %s\n", file.c_str());
            return false;
        }
        std::printf("Loaded battle-start template: %dx%d\n", battle_.cols, battle_.rows);
        return true;
    }

    void setEnemyThreshold(double t) { enemyTh_ = t; }
    void setBattleThreshold(double t) { battleTh_ = t; }

    Mat captureScreen() const
    {
        // Virtual screen for multi-monitor + works with windowed/borderless
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        HDC hScreen = GetDC(NULL);
        HDC hDC = CreateCompatibleDC(hScreen);
        HBITMAP hBmp = CreateCompatibleBitmap(hScreen, vw, vh);
        HGDIOBJ old = SelectObject(hDC, hBmp);

        BitBlt(hDC, 0, 0, vw, vh, hScreen, vx, vy, SRCCOPY | CAPTUREBLT);

        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = vw;
        bi.biHeight = -vh;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        Mat bgra(vh, vw, CV_8UC4);
        GetDIBits(hDC, hBmp, 0, vh, bgra.data, (BITMAPINFO *)&bi, DIB_RGB_COLORS);

        SelectObject(hDC, old);
        DeleteObject(hBmp);
        DeleteDC(hDC);
        ReleaseDC(NULL, hScreen);

        Mat bgr;
        cvtColor(bgra, bgr, COLOR_BGRA2BGR);
        return bgr;
    }

    bool isBattleStart(const Mat &screen, double *outConf = nullptr) const
    {
        if (battle_.empty())
            return false;
        if (battle_.cols > screen.cols || battle_.rows > screen.rows)
            return false;

        Mat result;
        matchTemplate(screen, battle_, result, TM_CCOEFF_NORMED);

        double minVal = 0, maxVal = 0;
        Point minLoc, maxLoc;
        minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

        if (outConf)
            *outConf = maxVal;
        return maxVal >= battleTh_;
    }

    Point findEnemy(const Mat &screen, double *outConf = nullptr, int *outIdx = nullptr) const
    {
        if (enemies_.empty())
        {
            if (outConf)
                *outConf = 0.0;
            if (outIdx)
                *outIdx = -1;
            return Point(-1, -1);
        }

        double bestScore = -1.0;
        Point bestLoc(-1, -1);
        int bestIdx = -1;

        for (int i = 0; i < (int)enemies_.size(); ++i)
        {
            const Mat &templ = enemies_[i];
            if (templ.cols > screen.cols || templ.rows > screen.rows)
                continue;

            Mat result;
            matchTemplate(screen, templ, result, TM_CCOEFF_NORMED);

            double minVal = 0, maxVal = 0;
            Point minLoc, maxLoc;
            minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

            if (maxVal > bestScore)
            {
                bestScore = maxVal;
                bestLoc = maxLoc;
                bestIdx = i;
            }
        }

        if (outConf)
            *outConf = bestScore;
        if (outIdx)
            *outIdx = bestIdx;

        if (bestIdx >= 0 && bestScore >= enemyTh_)
        {
            Point center(bestLoc.x + enemies_[bestIdx].cols / 2,
                         bestLoc.y + enemies_[bestIdx].rows / 2);
            return center;
        }

        return Point(-1, -1);
    }

    const char *enemyName(int idx) const
    {
        if (idx < 0 || idx >= (int)enemyNames_.size())
            return "";
        return enemyNames_[idx].c_str();
    }

    static void moveCursorTowards(const Point &target, int steps = 18, int stepMs = 6)
    {
        POINT pt{};
        GetCursorPos(&pt);
        Point cur(pt.x, pt.y);

        for (int i = 1; i <= steps; ++i)
        {
            double t = (double)i / steps;
            int x = (int)(cur.x + (target.x - cur.x) * t);
            int y = (int)(cur.y + (target.y - cur.y) * t);
            SetCursorPos(x, y);
            std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
        }
    }

private:
    static bool has_image_ext(const std::string &name)
    {
        std::string s = name;
        for (char &c : s)
            c = (char)tolower((unsigned char)c);
        if (s.size() >= 4)
        {
            if (s.rfind(".png") == s.size() - 4)
                return true;
            if (s.rfind(".jpg") == s.size() - 4)
                return true;
            if (s.rfind(".bmp") == s.size() - 4)
                return true;
        }
        if (s.size() >= 5 && s.rfind(".jpeg") == s.size() - 5)
            return true;
        return false;
    }

    bool loadFolder(const std::string &folder)
    {
        std::string search = folder + "\\*.*";
        WIN32_FIND_DATAA data{};
        HANDLE h = FindFirstFileA(search.c_str(), &data);
        if (h == INVALID_HANDLE_VALUE)
        {
            std::fprintf(stderr, "Cannot open enemy folder: %s\n", folder.c_str());
            return false;
        }

        int count = 0;
        do
        {
            std::string name = data.cFileName;
            if (name == "." || name == "..")
                continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (!has_image_ext(name))
                continue;

            std::string full = folder + "\\" + name;
            Mat img = imread(full, IMREAD_COLOR);
            if (img.empty())
            {
                std::fprintf(stderr, "Failed to load enemy image: %s\n", full.c_str());
                continue;
            }

            enemies_.push_back(img);
            enemyNames_.push_back(name);
            ++count;
        } while (FindNextFileA(h, &data));

        FindClose(h);

        std::printf("Loaded %d enemy templates from folder: %s\n", count, folder.c_str());
        return count > 0;
    }

private:
    std::vector<Mat> enemies_;
    std::vector<std::string> enemyNames_;
    Mat battle_;
    double enemyTh_ = 0.75;
    double battleTh_ = 0.88;
};

static TemplateDetector gDet;

static void debug_save_capture_once()
{
    cv::Mat s = gDet.captureScreen();
    if (s.empty())
    {
        std::printf("[DEBUG] captureScreen returned empty Mat\n");
        return;
    }
    cv::imwrite("debug_capture.png", s);
    std::printf("[DEBUG] Wrote debug_capture.png (%dx%d)\n", s.cols, s.rows);
}

// --------------------------- Hunt control ---------------------------

static void stop_auto_hunt()
{
    gAutoHuntRun = false;
    if (gAutoHuntThread.joinable())
        gAutoHuntThread.join();
}

static void start_auto_hunt_with_saved_config(); // forward

static void maybe_restart_hunt_on_shift()
{
    if ((GetAsyncKeyState(VK_SHIFT) & 1) == 0)
        return;
    if (gAutoHuntRun.load())
        return;
    if (!gBattleStarted.load())
        return;

    std::printf("[HUNT] SHIFT pressed -> restarting hunt...\n");
    start_auto_hunt_with_saved_config();
}

static void start_auto_hunt(const char *enemyTemplatesPath,
                            const char *battleStartTemplatePath,
                            double enemyThreshold,
                            double battleThreshold,
                            int scanMs,
                            int attackCooldownMs)
{
    stop_auto_hunt();

    gBattleStarted = false;

    gHuntInfo.detections = 0;
    gHuntInfo.attacks = 0;
    gHuntInfo.lastX = -1;
    gHuntInfo.lastY = -1;
    gHuntInfo.lastConf = 0.0;
    gHuntInfo.lastWasBattle = false;
    gHuntInfo.setLastName("(none)");
    overlay_invalidate();

    if (!gDet.loadEnemyTemplates(enemyTemplatesPath))
    {
        std::fprintf(stderr, "Auto-hunt: failed to load enemy templates: %s\n", enemyTemplatesPath);
        return;
    }
    if (!gDet.loadBattleStartTemplate(battleStartTemplatePath))
    {
        std::fprintf(stderr, "Auto-hunt: failed to load battle-start template: %s\n", battleStartTemplatePath);
        return;
    }

    debug_save_capture_once();

    gDet.setEnemyThreshold(enemyThreshold);
    gDet.setBattleThreshold(battleThreshold);

    if (scanMs < 20)
        scanMs = 20;
    if (attackCooldownMs < 100)
        attackCooldownMs = 100;

    gAutoHuntRun = true;

    gAutoHuntThread = std::thread([=]()
                                  {
        auto lastAttack = std::chrono::steady_clock::now() - std::chrono::milliseconds(attackCooldownMs);
        int tick = 0;

        std::printf("[HUNT] Thread started.\n");

        while (gAutoHuntRun)
        {
            // Wait until record/play becomes active
            if (!gRecording && !gPlaying)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            Mat screen = gDet.captureScreen();

            // 1) battle start stops hunting
            double battleConf = 0.0;
            if (gDet.isBattleStart(screen, &battleConf))
            {
                gBattleStarted = true;

                gHuntInfo.lastWasBattle = true;
                gHuntInfo.lastConf = battleConf;
                gHuntInfo.setLastName("BattleStart");
                gHuntInfo.lastX = -1;
                gHuntInfo.lastY = -1;
                overlay_invalidate();

                std::printf("[HUNT] Battle Start detected (conf=%.2f). Hunt OFF. Press SHIFT to restart.\n", battleConf);
                gAutoHuntRun = false;
                break;
            }

            // 2) enemy match
            double enemyConf = 0.0;
            int idx = -1;
            Point p = gDet.findEnemy(screen, &enemyConf, &idx);

            tick++;
            if ((tick % 25) == 0)
            {
                std::printf("[DEBUG] bestEnemyConf=%.3f enemyTh=%.3f best=%s\n",
                            enemyConf, enemyThreshold, gDet.enemyName(idx));
            }

            if (p.x >= 0 && p.y >= 0)
            {
                gHuntInfo.lastWasBattle = false;
                gHuntInfo.lastConf = enemyConf;
                gHuntInfo.lastX = p.x;
                gHuntInfo.lastY = p.y;
                gHuntInfo.setLastName(gDet.enemyName(idx));
                gHuntInfo.detections++;
                overlay_invalidate();

                if (gPlaying)
                {
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastAttack >= std::chrono::milliseconds(attackCooldownMs))
                    {
                        TemplateDetector::moveCursorTowards(p, 18, 6);
                        send_mouse_button(1, true);
                        std::this_thread::sleep_for(std::chrono::milliseconds(35));
                        send_mouse_button(1, false);

                        gHuntInfo.attacks++;
                        overlay_invalidate();

                        std::printf("[HUNT] Attacked: %s conf=%.2f at=(%d,%d)\n",
                                    gDet.enemyName(idx), enemyConf, p.x, p.y);

                        lastAttack = now;
                    }
                }
                else
                {
                    std::printf("[SCAN] Enemy detected: %s conf=%.2f at=(%d,%d)\n",
                                gDet.enemyName(idx), enemyConf, p.x, p.y);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(scanMs));
        } });
}

static void start_auto_hunt_with_saved_config()
{
    if (gEnemyTemplatesPath.empty() || gBattleStartPath.empty())
    {
        std::fprintf(stderr, "[HUNT] No saved config to restart.\n");
        return;
    }
    start_auto_hunt(gEnemyTemplatesPath.c_str(),
                    gBattleStartPath.c_str(),
                    gEnemyTh, gBattleTh,
                    gScanMs, gCooldownMs);
}

// --------------------------- Record / Play ---------------------------

static bool record_to_file(const char *path)
{
    ZeroMemory(gMouseBtn, sizeof(gMouseBtn));
    ZeroMemory(gKeyDown, sizeof(gKeyDown));
    gLastDx = gLastDy = 0;
    gLastWheel = 0;
    gCursorPt = POINT{0, 0};

    gOut = std::fopen(path, "wb");
    if (!gOut)
    {
        std::fprintf(stderr, "Cannot open output file: %s\n", path);
        return false;
    }

    FileHeader hdr{};
    hdr.magic = 0x524D4143;
    hdr.version = 1;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    hdr.start_utc = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    fwrite(&hdr, sizeof(hdr), 1, gOut);
    fflush(gOut);

    if (!create_sink_window())
        return false;
    if (!create_overlay_window())
        return false;

    QueryPerformanceFrequency(&gFreq);
    QueryPerformanceCounter(&gT0);

    countdown_3s("Recording will begin");

    gRecording = true;
    overlay_show(true);

    std::puts("Recording... (ESC to stop)");

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        maybe_restart_hunt_on_shift();
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gRecording = false;
    overlay_show(false);

    stop_auto_hunt();

    if (gOut)
    {
        std::fclose(gOut);
        gOut = nullptr;
    }

    destroy_overlay_window();
    destroy_sink_window();

    std::puts("Recording stopped.");
    return true;
}

static bool play_file(const char *path)
{
    FILE *in = std::fopen(path, "rb");
    if (!in)
    {
        std::fprintf(stderr, "Cannot open input file: %s\n", path);
        return false;
    }

    FileHeader hdr{};
    if (read_exact(in, &hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != 0x524D4143)
    {
        std::fprintf(stderr, "Invalid file format.\n");
        std::fclose(in);
        return false;
    }

    std::vector<Event> events;
    Event ev{};
    while (read_exact(in, &ev, sizeof(ev)) == sizeof(ev))
        events.push_back(ev);
    std::fclose(in);

    countdown_3s("Playback will begin");
    std::puts("Playing... (ESC to stop)");

    // Debounce physical ESC so playback doesn't instantly stop
    while (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        Sleep(10);

    ZeroMemory(gMouseBtn, sizeof(gMouseBtn));
    ZeroMemory(gKeyDown, sizeof(gKeyDown));
    gLastDx = gLastDy = 0;
    gLastWheel = 0;
    GetCursorPos(&gCursorPt);

    bool overlay_ok = create_overlay_window();
    if (overlay_ok)
    {
        overlay_show(true);
        pump_messages_nonblocking();
    }

    gPlaying = true;

    TIMECAPS tc{};
    bool setPeriod = (timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR);
    if (setPeriod)
        timeBeginPeriod(tc.wPeriodMin);

    uint64_t prev_t = 0;
    for (size_t i = 0; i < events.size(); ++i)
    {
        maybe_restart_hunt_on_shift();

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            std::puts("\nPlayback stopped by ESC.");
            break;
        }

        const Event &e = events[i];

        if (e.t_us > prev_t)
        {
            uint64_t dt = e.t_us - prev_t;
            std::this_thread::sleep_for(std::chrono::microseconds(dt));
        }
        prev_t = e.t_us;

        switch (e.type)
        {
        case EV_MOUSE_MOVE:
            send_mouse_move_rel(e.a, e.b);
            GetCursorPos(&gCursorPt);
            gLastDx = e.a;
            gLastDy = e.b;
            overlay_invalidate();
            pump_messages_nonblocking();
            break;

        case EV_MOUSE_POS:
            send_mouse_move_abs(e.a, e.b);
            gCursorPt.x = e.a;
            gCursorPt.y = e.b;
            overlay_invalidate();
            pump_messages_nonblocking();
            break;

        case EV_MOUSE_WHEEL:
            send_mouse_wheel(e.a);
            gLastWheel = (int)e.a;
            overlay_invalidate();
            pump_messages_nonblocking();
            break;

        case EV_MOUSE_BUTTON:
            send_mouse_button(e.a, e.b != 0);
            if (e.a >= 1 && e.a <= 5)
                gMouseBtn[e.a] = (e.b != 0);
            overlay_invalidate();
            pump_messages_nonblocking();
            break;

        case EV_KEY_DOWN:
            send_key(true, (UINT)e.a);
            update_overlay_state_on_key((UINT)e.a, true);
            pump_messages_nonblocking();
            break;

        case EV_KEY_UP:
            send_key(false, (UINT)e.a);
            update_overlay_state_on_key((UINT)e.a, false);
            pump_messages_nonblocking();
            break;

        default:
            break;
        }
    }

    if (setPeriod)
        timeEndPeriod(tc.wPeriodMin);

    gPlaying = false;

    stop_auto_hunt();

    if (overlay_ok)
    {
        overlay_show(false);
        destroy_overlay_window();
        pump_messages_nonblocking();
    }

    std::puts("Done.");
    return true;
}

// --------------------------- Commands (hunt wrappers) ---------------------------

static bool record_hunt(const char *file,
                        const char *enemiesPath,
                        const char *battleStartPath,
                        double enemyTh, double battleTh,
                        int scanMs, int cooldownMs)
{
    gEnemyTemplatesPath = enemiesPath;
    gBattleStartPath = battleStartPath;
    gEnemyTh = enemyTh;
    gBattleTh = battleTh;
    gScanMs = scanMs;
    gCooldownMs = cooldownMs;

    start_auto_hunt(enemiesPath, battleStartPath, enemyTh, battleTh, scanMs, cooldownMs);
    return record_to_file(file);
}

static bool play_hunt(const char *file,
                      const char *enemiesPath,
                      const char *battleStartPath,
                      double enemyTh, double battleTh,
                      int scanMs, int cooldownMs)
{
    gEnemyTemplatesPath = enemiesPath;
    gBattleStartPath = battleStartPath;
    gEnemyTh = enemyTh;
    gBattleTh = battleTh;
    gScanMs = scanMs;
    gCooldownMs = cooldownMs;

    start_auto_hunt(enemiesPath, battleStartPath, enemyTh, battleTh, scanMs, cooldownMs);
    return play_file(file);
}

// --------------------------- main ---------------------------

int main(int argc, char **argv)
{
    enable_dpi_awareness();

    if (argc < 3)
    {
        std::printf(
            "Usage:\n"
            "  %s record      <file.rmac>\n"
            "  %s play        <file.rmac>\n"
            "  %s recordhunt  <file.rmac> <enemy_path> <battle_start.png> [enemy_th] [battle_th] [scan_ms] [cooldown_ms]\n"
            "  %s playhunt    <file.rmac> <enemy_path> <battle_start.png> [enemy_th] [battle_th] [scan_ms] [cooldown_ms]\n"
            "\n"
            "Defaults:\n"
            "  enemy_th=0.75  battle_th=0.88  scan_ms=200  cooldown_ms=900\n"
            "\n"
            "After BattleStart is detected, hunting stops. Press SHIFT to start hunting again.\n"
            "\n"
            "Examples:\n"
            "  %s recordhunt run.rmac templates\\enemies templates\\BattleStart.png 0.70 0.88 200 900\n"
            "  %s playhunt   run.rmac templates\\enemies templates\\BattleStart.png 0.70 0.88 200 900\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "record")
        return record_to_file(argv[2]) ? 0 : 1;

    if (cmd == "play")
        return play_file(argv[2]) ? 0 : 1;

    if (cmd == "recordhunt" || cmd == "playhunt")
    {
        if (argc < 5)
        {
            std::fprintf(stderr, "%s needs: <file.rmac> <enemy_path> <battle_start.png>\n", cmd.c_str());
            return 1;
        }

        const char *file = argv[2];
        const char *enemiesPath = argv[3];
        const char *battlePath = argv[4];

        double enemyTh = (argc >= 6) ? std::atof(argv[5]) : 0.75;
        double battleTh = (argc >= 7) ? std::atof(argv[6]) : 0.88;
        int scanMs = (argc >= 8) ? std::atoi(argv[7]) : 200;
        int cooldownMs = (argc >= 9) ? std::atoi(argv[8]) : 900;

        if (cmd == "recordhunt")
            return record_hunt(file, enemiesPath, battlePath, enemyTh, battleTh, scanMs, cooldownMs) ? 0 : 1;

        return play_hunt(file, enemiesPath, battlePath, enemyTh, battleTh, scanMs, cooldownMs) ? 0 : 1;
    }

    std::fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
    return 1;
}
