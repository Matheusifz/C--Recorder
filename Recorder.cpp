// Recorder.cpp  (Full Integration: macro recorder + playback + OpenCV hunt + QuestMarker walk)
//
// Features:
// 1) High-precision REL recording via WM_INPUT (reused buffer, large file buffer, no per-event fflush)
// 2) ABS mode: ALT key OR Cursor.png template match triggers high-rate GetCursorPos poll + EV_MOUSE_POS log
// 3) QuestMarker navigation: finds ALL matches, ignores quest log rect (45,282)-(72,311),
//    picks closest to screen center, holds Shift+W, steers A/D
// 4) Hunt + BattleStart: stops hunt AND questwalk on BattleStart; SHIFT restarts hunt
// 5) Hardcoded default paths: templates\Enemies, templates\BattleStart.png,
//    templates\Cursor.png, templates\QuestMarker.png
// 6) Threads: hunt, cursor-detect, abs-poll, quest-walk (all independent, safe shutdown)
// 7) Clean shutdown: stop all threads, release keys, join safely
// 8) Preserves .rmac file format
//
// Commands:
//   Recorder.exe record        [file.rmac] [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe play          [file.rmac] [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe recordhunt   [file.rmac] [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//                              [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe playhunt     [file.rmac] [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//                              [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe questwalk    [quest_marker.png] [marker_th] [deadzone_px] [tick_ms]
//                              [ignoreL] [ignoreT] [ignoreR] [ignoreB]
//   Recorder.exe hunt         [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//   Recorder.exe full         [file.rmac]   (uses all hardcoded paths, records + hunts + questwalks)
//
// Hardcoded defaults:
//   macro file:    macro.rmac
//   enemy path:    templates\Enemies
//   battle start:  templates\BattleStart.png
//   cursor tmpl:   templates\Cursor.png
//   quest marker:  templates\QuestMarker.png
//   enemy_th=0.75  battle_th=0.88  scan_ms=200  cooldown_ms=900
//   cursor_th=0.88 cursor_scan_ms=33 abs_poll_ms=2
//   marker_th=0.85 deadzone_px=40 tick_ms=50 ignoreRect=45,282,72,311
//
// Build (MSVC x64 Native Tools Prompt):
//   cl /EHsc /O2 /std:c++20 /MD Recorder.cpp user32.lib winmm.lib gdi32.lib ^
//      /I"opencv\build\include" ^
//      /link /MACHINE:X64 ^
//      /LIBPATH:"opencv\build\x64\vc16\lib" opencv_world4120.lib

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
#include <algorithm>

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

using namespace cv;

// ========================= Constants =========================

static const char *kSinkClassName = "RawIO_Sink_Window";
static const char *kOverlayClassName = "RawIO_Overlay_Window";

// Hardcoded default paths
static const char *kDefaultMacroFile = "macro.rmac";
static const char *kDefaultEnemyPath = "templates\\Enemies";
static const char *kDefaultBattlePath = "templates\\BattleStart.png";
static const char *kDefaultCursorPath = "templates\\Cursor.png";
static const char *kDefaultQuestPath = "templates\\QuestMarker.png";

// Quest log icon ignore rectangle (pixels on screen)
static RECT gQuestLogIgnore = {45, 282, 72, 311};

// ========================= Event File Format =========================

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

// ========================= Globals =========================

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

// Reuse buffer for WM_INPUT
static std::vector<BYTE> gRawBuf;

// ========================= ABS mode =========================

static std::atomic<bool> gAbsByAlt{false};
static std::atomic<bool> gAbsByCursor{false};

static std::atomic<bool> gRunAbsPoll{false};
static std::thread gAbsPollThread;

static std::atomic<bool> gRunCursorDetect{false};
static std::thread gCursorDetectThread;

static std::string gAbsCursorTemplatePath = kDefaultCursorPath;
static double gCursorTh = 0.88;
static int gCursorScanMs = 33;
static int gAbsPollMs = 2;

static cv::Mat gAbsCursorTempl;
static bool gCursorMultiScale = true;

// ========================= Hunt config/state =========================

static std::atomic<bool> gAutoHuntRun{false};
static std::thread gAutoHuntThread;
static std::atomic<bool> gBattleStarted{false};

static std::string gEnemyTemplatesPath = kDefaultEnemyPath;
static std::string gBattleStartPath = kDefaultBattlePath;
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

// ========================= QuestWalk state =========================

static std::atomic<bool> gRunQuestWalk{false};
static std::thread gQuestWalkThread;

static std::string gQuestMarkerPath = kDefaultQuestPath;
static double gMarkerTh = 0.85;
static int gDeadzonePx = 40;
static int gQuestTickMs = 50;

// For overlay: last quest marker position
static std::atomic<int> gQuestMarkerX{-1};
static std::atomic<int> gQuestMarkerY{-1};
static std::atomic<double> gQuestMarkerConf{0.0};

// ========================= DPI Awareness =========================

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

// ========================= Timing / IO =========================

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
    // no fflush here (precision)
}

static void flush_events()
{
    if (gOut)
        fflush(gOut);
}

static uint64_t read_exact(FILE *f, void *buf, uint64_t sz)
{
    return (uint64_t)fread(buf, 1, (size_t)sz, f);
}

// ========================= Overlay =========================

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

        bool absMode = gAbsByAlt.load() || gAbsByCursor.load();

        put(gRecording ? "RawIO Overlay [Recording]"
                       : (gPlaying ? "RawIO Overlay [Playing]" : "RawIO Overlay"));

        std::snprintf(line, sizeof(line), "Mode: %s (ALT=%d CursorMatch=%d)",
                      absMode ? "ABS" : "REL",
                      (int)gAbsByAlt.load(), (int)gAbsByCursor.load());
        put(line);

        std::snprintf(line, sizeof(line), "Cursor: %ld, %ld",
                      (long)gCursorPt.x, (long)gCursorPt.y);
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

        std::snprintf(line, sizeof(line), "AutoHunt: %d  Battle: %d  QuestWalk: %d",
                      (int)gAutoHuntRun.load(), (int)gBattleStarted.load(),
                      (int)gRunQuestWalk.load());
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

        int qx = gQuestMarkerX.load(), qy = gQuestMarkerY.load();
        if (qx >= 0)
        {
            std::snprintf(line, sizeof(line), "QuestMarker: (%d,%d) conf=%.2f",
                          qx, qy, gQuestMarkerConf.load());
            put(line);
        }

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

    const int w = 480, h = 320;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP;

    gOverlayHwnd = CreateWindowExA(
        ex, kOverlayClassName, "RawIO Overlay",
        style, 10, 10, w, h,
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

// ========================= SendInput helpers =========================

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

    double relx = std::clamp((double)(x - vsx) / (double)vsw, 0.0, 1.0);
    double rely = std::clamp((double)(y - vsy) / (double)vsh, 0.0, 1.0);

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

static void release_move_keys()
{
    send_key(false, 'W');
    send_key(false, 'A');
    send_key(false, 'S');
    send_key(false, 'D');
    send_key(false, VK_SHIFT);
}

// ========================= Raw Input Sink =========================

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

static bool register_raw(HWND hwnd)
{
    RAWINPUTDEVICE rids[2]{};
    rids[0].usUsagePage = 0x01;
    rids[0].usUsage = 0x02; // mouse
    rids[0].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
    rids[0].hwndTarget = hwnd;

    rids[1].usUsagePage = 0x01;
    rids[1].usUsage = 0x06; // keyboard
    rids[1].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
    rids[1].hwndTarget = hwnd;

    return RegisterRawInputDevices(rids, 2, sizeof(RAWINPUTDEVICE)) == TRUE;
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

        if (gRawBuf.size() < size)
            gRawBuf.resize(size);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, gRawBuf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            break;

        RAWINPUT *raw = reinterpret_cast<RAWINPUT *>(gRawBuf.data());

        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE &m = raw->data.mouse;

            gAbsByAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
            bool absMode = gAbsByAlt.load() || gAbsByCursor.load();

            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
            {
                LONG dx = m.lLastX;
                LONG dy = m.lLastY;
                if (dx != 0 || dy != 0)
                {
                    gLastDx = dx;
                    gLastDy = dy;
                    // Only write REL when NOT in ABS mode
                    if (!absMode)
                        write_event(EV_MOUSE_MOVE, (int32_t)dx, (int32_t)dy, 0);
                    update_overlay_state_on_mouse();
                }
            }

            if (m.usButtonFlags & RI_MOUSE_WHEEL)
            {
                SHORT wd = *reinterpret_cast<const SHORT *>(&m.usButtonData);
                gLastWheel = (int)wd;
                write_event(EV_MOUSE_WHEEL, (int32_t)wd, 0, 0);
                update_overlay_state_on_mouse();
            }

            auto log_btn = [&](int button, bool down)
            {
                if (button >= 1 && button <= 5)
                    gMouseBtn[button] = down;
                write_event(EV_MOUSE_BUTTON, (int32_t)button, down ? 1 : 0, 0);
                update_overlay_state_on_mouse();
            };

            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
                log_btn(1, true);
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
                log_btn(1, false);
            if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
                log_btn(2, true);
            if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
                log_btn(2, false);
            if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
                log_btn(3, true);
            if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
                log_btn(3, false);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)
                log_btn(4, true);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_4_UP)
                log_btn(4, false);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)
                log_btn(5, true);
            if (m.usButtonFlags & RI_MOUSE_BUTTON_5_UP)
                log_btn(5, false);
        }
        else if (raw->header.dwType == RIM_TYPEKEYBOARD)
        {
            const RAWKEYBOARD &kb = raw->data.keyboard;
            bool isBreak = (kb.Flags & RI_KEY_BREAK) != 0;
            UINT vk = kb.VKey;
            if (vk == 255)
                break;

            gAbsByAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

            // ESC stops recording but is NOT written
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
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
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

// ========================= Screen Capture =========================

static cv::Mat capture_screen_full()
{
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

    cv::Mat bgra(vh, vw, CV_8UC4);
    GetDIBits(hDC, hBmp, 0, vh, bgra.data, (BITMAPINFO *)&bi, DIB_RGB_COLORS);

    SelectObject(hDC, old);
    DeleteObject(hBmp);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

static cv::Mat capture_roi_around_cursor(int halfSize)
{
    POINT pt{};
    if (!GetCursorPos(&pt))
        return cv::Mat();

    int w = halfSize * 2, h = halfSize * 2;
    int x0 = pt.x - halfSize, y0 = pt.y - halfSize;

    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    HGDIOBJ old = SelectObject(hDC, hBmp);

    BitBlt(hDC, 0, 0, w, h, hScreen, x0, y0, SRCCOPY | CAPTUREBLT);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    cv::Mat bgra(h, w, CV_8UC4);
    GetDIBits(hDC, hBmp, 0, h, bgra.data, (BITMAPINFO *)&bi, DIB_RGB_COLORS);

    SelectObject(hDC, old);
    DeleteObject(hBmp);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

// ========================= Template Detector =========================

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
            return Point(bestLoc.x + enemies_[bestIdx].cols / 2,
                         bestLoc.y + enemies_[bestIdx].rows / 2);
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
        return (s.size() >= 4 && (s.rfind(".png") == s.size() - 4 ||
                                  s.rfind(".jpg") == s.size() - 4 ||
                                  s.rfind(".bmp") == s.size() - 4)) ||
               (s.size() >= 5 && s.rfind(".jpeg") == s.size() - 5);
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
                std::fprintf(stderr, "Failed: %s\n", full.c_str());
                continue;
            }

            enemies_.push_back(img);
            enemyNames_.push_back(name);
            ++count;
        } while (FindNextFileA(h, &data));

        FindClose(h);
        std::printf("Loaded %d enemy templates from: %s\n", count, folder.c_str());
        return count > 0;
    }

    std::vector<Mat> enemies_;
    std::vector<std::string> enemyNames_;
    Mat battle_;
    double enemyTh_ = 0.75;
    double battleTh_ = 0.88;
};

static TemplateDetector gDet;

// ========================= Cursor Template Detection (ABS mode) =========================

static bool load_abs_cursor_template(const std::string &path)
{
    if (path.empty())
        return false;
    gAbsCursorTempl = cv::imread(path, cv::IMREAD_COLOR);
    if (gAbsCursorTempl.empty())
    {
        std::fprintf(stderr, "[ABS] Failed to load cursor template: %s\n", path.c_str());
        return false;
    }
    std::printf("[ABS] Loaded cursor template: %s (%dx%d)\n",
                path.c_str(), gAbsCursorTempl.cols, gAbsCursorTempl.rows);
    return true;
}

static double best_match_score_multiscale(const cv::Mat &roi, const cv::Mat &templ)
{
    static const double scales[] = {1.00, 0.90, 1.10, 0.80, 1.20};
    double best = -1.0;
    for (double s : scales)
    {
        cv::Mat tScaled;
        if (std::fabs(s - 1.0) < 1e-6)
            tScaled = templ;
        else
            cv::resize(templ, tScaled, cv::Size(), s, s, cv::INTER_LINEAR);

        if (tScaled.empty())
            continue;
        if (tScaled.cols > roi.cols || tScaled.rows > roi.rows)
            continue;

        cv::Mat result;
        cv::matchTemplate(roi, tScaled, result, cv::TM_CCOEFF_NORMED);
        double minVal = 0, maxVal = 0;
        cv::Point minLoc, maxLoc;
        cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
        if (maxVal > best)
            best = maxVal;
    }
    return best;
}

static void start_cursor_detect_thread()
{
    if (gAbsCursorTemplatePath.empty())
        return;
    if (!load_abs_cursor_template(gAbsCursorTemplatePath))
        return;

    gRunCursorDetect = true;
    gCursorDetectThread = std::thread([]()
                                      {
        int half = 80;
        while (gRunCursorDetect)
        {
            if (!gRecording && !gPlaying)
            {
                gAbsByCursor = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            cv::Mat roi = capture_roi_around_cursor(half);
            if (roi.empty() || gAbsCursorTempl.empty())
            {
                gAbsByCursor = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(gCursorScanMs));
                continue;
            }

            double score = -1.0;
            if (!gCursorMultiScale)
            {
                if (gAbsCursorTempl.cols <= roi.cols && gAbsCursorTempl.rows <= roi.rows)
                {
                    cv::Mat result;
                    cv::matchTemplate(roi, gAbsCursorTempl, result, cv::TM_CCOEFF_NORMED);
                    double minVal = 0, maxVal = 0;
                    cv::Point minLoc, maxLoc;
                    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
                    score = maxVal;
                }
            }
            else
            {
                score = best_match_score_multiscale(roi, gAbsCursorTempl);
            }

            gAbsByCursor = (score >= gCursorTh);
            std::this_thread::sleep_for(std::chrono::milliseconds(gCursorScanMs));
        }
        gAbsByCursor = false; });
}

static void stop_cursor_detect_thread()
{
    gRunCursorDetect = false;
    if (gCursorDetectThread.joinable())
        gCursorDetectThread.join();
    gAbsByCursor = false;
}

static void start_abs_poll_thread()
{
    gRunAbsPoll = true;
    gAbsPollThread = std::thread([]()
                                 {
        POINT last{-999999, -999999};
        while (gRunAbsPoll)
        {
            bool absMode = gAbsByAlt.load() || gAbsByCursor.load();

            if (gRecording && absMode)
            {
                POINT pt{};
                if (GetCursorPos(&pt) && (pt.x != last.x || pt.y != last.y))
                {
                    last = pt;
                    gCursorPt = pt;
                    write_event(EV_MOUSE_POS, (int32_t)pt.x, (int32_t)pt.y, 0);
                    overlay_invalidate();
                }
            }
            else
            {
                POINT pt{};
                if (GetCursorPos(&pt)) gCursorPt = pt;
            }

            // Keep ALT state fresh
            gAbsByAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

            std::this_thread::sleep_for(std::chrono::milliseconds(gAbsPollMs));
        } });
}

static void stop_abs_poll_thread()
{
    gRunAbsPoll = false;
    if (gAbsPollThread.joinable())
        gAbsPollThread.join();
}

// ========================= Quest Marker Detection =========================

static bool point_in_rect(int x, int y, const RECT &r)
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

struct MatchHit
{
    cv::Point topLeft;
    double score;
};

static std::vector<MatchHit> find_all_matches(const cv::Mat &screen, const cv::Mat &templ, double th)
{
    std::vector<MatchHit> hits;
    if (screen.empty() || templ.empty())
        return hits;
    if (templ.cols > screen.cols || templ.rows > screen.rows)
        return hits;

    cv::Mat result;
    cv::matchTemplate(screen, templ, result, cv::TM_CCOEFF_NORMED);

    while (true)
    {
        double minV = 0, maxV = 0;
        cv::Point minL, maxL;
        cv::minMaxLoc(result, &minV, &maxV, &minL, &maxL);
        if (maxV < th)
            break;

        hits.push_back({maxL, maxV});

        // Suppress region
        int x0 = std::max(0, maxL.x - templ.cols / 2);
        int y0 = std::max(0, maxL.y - templ.rows / 2);
        int x1 = std::min(result.cols, maxL.x + templ.cols / 2);
        int y1 = std::min(result.rows, maxL.y + templ.rows / 2);
        cv::rectangle(result, cv::Rect(x0, y0, x1 - x0, y1 - y0), cv::Scalar(0), cv::FILLED);
    }
    return hits;
}

static bool pick_world_marker(const cv::Mat &screen, const cv::Mat &questTempl, double th,
                              cv::Point &outCenter, double &outScore)
{
    auto hits = find_all_matches(screen, questTempl, th);
    if (hits.empty())
        return false;

    const int cx0 = screen.cols / 2;
    const int cy0 = screen.rows / 2;
    bool found = false;
    double bestCost = 1e30, bestScore = 0.0;
    cv::Point bestCenter(-1, -1);

    for (auto &h : hits)
    {
        int cx = h.topLeft.x + questTempl.cols / 2;
        int cy = h.topLeft.y + questTempl.rows / 2;

        // Skip quest log icon copy
        if (point_in_rect(cx, cy, gQuestLogIgnore))
            continue;

        double dx = (double)(cx - cx0);
        double dy = (double)(cy - cy0);
        double cost = dx * dx + dy * dy;

        if (!found || cost < bestCost)
        {
            found = true;
            bestCost = cost;
            bestScore = h.score;
            bestCenter = cv::Point(cx, cy);
        }
    }

    if (!found)
        return false;
    outCenter = bestCenter;
    outScore = bestScore;
    return true;
}

// ========================= Quest Walk Thread =========================

static void stop_quest_walk()
{
    gRunQuestWalk = false;
    if (gQuestWalkThread.joinable())
        gQuestWalkThread.join();
}

static void start_quest_walk(const cv::Mat &questTempl, double markerTh, int deadzonePx, int tickMs)
{
    stop_quest_walk();

    gRunQuestWalk = true;
    gQuestWalkThread = std::thread([questTempl, markerTh, deadzonePx, tickMs]()
                                   {
        std::puts("[QUEST] Quest walk thread started. ESC to stop.");
        send_key(true, 'W');
        send_key(true, VK_SHIFT);

        while (gRunQuestWalk)
        {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                std::puts("[QUEST] ESC - stopping quest walk.");
                gRunQuestWalk = false;
                break;
            }

            // Battle detected - stop walking
            if (gBattleStarted.load())
            {
                std::puts("[QUEST] Battle started - pausing quest walk.");
                send_key(false, 'W');
                send_key(false, 'A');
                send_key(false, 'D');
                send_key(false, VK_SHIFT);

                // Wait until battle clears or ESC
                while (gRunQuestWalk && gBattleStarted.load())
                {
                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { gRunQuestWalk = false; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                if (!gRunQuestWalk) break;

                std::puts("[QUEST] Resuming quest walk.");
                send_key(true, 'W');
                send_key(true, VK_SHIFT);
                continue;
            }

            cv::Mat screen = capture_screen_full();
            cv::Point markerCenter;
            double    conf = 0.0;

            bool ok = pick_world_marker(screen, questTempl, markerTh, markerCenter, conf);

            gQuestMarkerX    = ok ? markerCenter.x : -1;
            gQuestMarkerY    = ok ? markerCenter.y : -1;
            gQuestMarkerConf = ok ? conf : 0.0;
            overlay_invalidate();

            if (!ok)
            {
                // Can't see marker: stop steering, keep running forward
                send_key(false, 'A');
                send_key(false, 'D');
                std::this_thread::sleep_for(std::chrono::milliseconds(tickMs));
                continue;
            }

            int centerX = screen.cols / 2;
            int dx = markerCenter.x - centerX;

            if (dx < -deadzonePx)
            {
                send_key(true,  'A');
                send_key(false, 'D');
            }
            else if (dx > deadzonePx)
            {
                send_key(true,  'D');
                send_key(false, 'A');
            }
            else
            {
                send_key(false, 'A');
                send_key(false, 'D');
            }

            GetCursorPos(&gCursorPt);
            overlay_invalidate();

            std::this_thread::sleep_for(std::chrono::milliseconds(tickMs));
        }

        release_move_keys();
        gQuestMarkerX = gQuestMarkerY = -1;
        std::puts("[QUEST] Quest walk stopped."); });
}

// ========================= Hunt control =========================

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
    gBattleStarted = false; // clear so quest walk can resume
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
        std::fprintf(stderr, "[HUNT] Failed to load enemy templates: %s\n", enemyTemplatesPath);
        return;
    }
    if (!gDet.loadBattleStartTemplate(battleStartTemplatePath))
    {
        std::fprintf(stderr, "[HUNT] Failed to load battle-start template: %s\n", battleStartTemplatePath);
        return;
    }

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
        int  tick = 0;

        std::printf("[HUNT] Thread started.\n");

        while (gAutoHuntRun)
        {
            if (!gRecording && !gPlaying && !gRunQuestWalk.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            Mat screen = capture_screen_full();

            double battleConf = 0.0;
            if (gDet.isBattleStart(screen, &battleConf))
            {
                gBattleStarted = true;

                gHuntInfo.lastWasBattle = true;
                gHuntInfo.lastConf      = battleConf;
                gHuntInfo.setLastName("BattleStart");
                gHuntInfo.lastX = gHuntInfo.lastY = -1;
                overlay_invalidate();

                std::printf("[HUNT] Battle Start detected (conf=%.2f). Hunt OFF. Press SHIFT to restart.\n", battleConf);
                gAutoHuntRun = false;
                break;
            }

            double enemyConf = 0.0;
            int    idx = -1;
            Point  p = gDet.findEnemy(screen, &enemyConf, &idx);

            tick++;
            if ((tick % 25) == 0)
            {
                std::printf("[DEBUG] bestEnemyConf=%.3f enemyTh=%.3f best=%s\n",
                            enemyConf, enemyThreshold, gDet.enemyName(idx));
            }

            if (p.x >= 0 && p.y >= 0)
            {
                gHuntInfo.lastWasBattle = false;
                gHuntInfo.lastConf      = enemyConf;
                gHuntInfo.lastX         = p.x;
                gHuntInfo.lastY         = p.y;
                gHuntInfo.setLastName(gDet.enemyName(idx));
                gHuntInfo.detections++;
                overlay_invalidate();

                if (gPlaying || gRunQuestWalk.load())
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
    start_auto_hunt(gEnemyTemplatesPath.c_str(), gBattleStartPath.c_str(),
                    gEnemyTh, gBattleTh, gScanMs, gCooldownMs);
}

// ========================= Global shutdown =========================

static void stop_all_threads()
{
    stop_quest_walk();
    stop_auto_hunt();
    stop_cursor_detect_thread();
    stop_abs_poll_thread();
    release_move_keys();
}

// ========================= Record / Play =========================

static bool record_to_file(const char *path)
{
    ZeroMemory(gMouseBtn, sizeof(gMouseBtn));
    ZeroMemory(gKeyDown, sizeof(gKeyDown));
    gLastDx = gLastDy = 0;
    gLastWheel = 0;
    gCursorPt = POINT{0, 0};
    gAbsByAlt = false;
    gAbsByCursor = false;

    gOut = std::fopen(path, "wb");
    if (!gOut)
    {
        std::fprintf(stderr, "Cannot open output file: %s\n", path);
        return false;
    }

    // 1 MB file buffer for high-precision recording (no per-event fflush)
    static char fileBuf[1 << 20];
    setvbuf(gOut, fileBuf, _IOFBF, sizeof(fileBuf));

    FileHeader hdr{};
    hdr.magic = 0x524D4143; // 'RMAC'
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

    timeBeginPeriod(1);
    start_abs_poll_thread();
    start_cursor_detect_thread();

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

    stop_all_threads();
    flush_events();
    timeEndPeriod(1);

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

    while (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        Sleep(10);

    ZeroMemory(gMouseBtn, sizeof(gMouseBtn));
    ZeroMemory(gKeyDown, sizeof(gKeyDown));
    gLastDx = gLastDy = 0;
    gLastWheel = 0;
    GetCursorPos(&gCursorPt);
    gAbsByAlt = false;
    gAbsByCursor = false;

    bool overlay_ok = create_overlay_window();
    if (overlay_ok)
    {
        overlay_show(true);
        pump_messages_nonblocking();
    }

    gPlaying = true;
    timeBeginPeriod(1);
    start_cursor_detect_thread();

    uint64_t prev_t = 0;
    for (size_t i = 0; i < events.size(); ++i)
    {
        maybe_restart_hunt_on_shift();
        gAbsByAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

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

    gPlaying = false;

    stop_all_threads();
    timeEndPeriod(1);

    if (overlay_ok)
    {
        overlay_show(false);
        destroy_overlay_window();
        pump_messages_nonblocking();
    }

    std::puts("Done.");
    return true;
}

// ========================= Hunt wrappers =========================

static bool record_hunt(const char *file,
                        const char *enemiesPath, const char *battleStartPath,
                        double enemyTh, double battleTh, int scanMs, int cooldownMs)
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
                      const char *enemiesPath, const char *battleStartPath,
                      double enemyTh, double battleTh, int scanMs, int cooldownMs)
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

// ========================= Quest Walk only (main loop) =========================

static void quest_walk_standalone(const cv::Mat &questTempl, double markerTh, int deadzonePx, int tickMs)
{
    bool overlay_ok = create_overlay_window();
    if (overlay_ok)
    {
        overlay_show(true);
        pump_messages_nonblocking();
    }

    timeBeginPeriod(1);

    // Use threaded version
    start_quest_walk(questTempl, markerTh, deadzonePx, tickMs);

    // Wait for thread to finish (ESC inside thread) or external ESC
    while (gRunQuestWalk.load())
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            stop_quest_walk();
            break;
        }
        pump_messages_nonblocking();
        Sleep(50);
    }
    if (gQuestWalkThread.joinable())
        gQuestWalkThread.join();

    timeEndPeriod(1);

    if (overlay_ok)
    {
        overlay_show(false);
        destroy_overlay_window();
        pump_messages_nonblocking();
    }
}

// ========================= Full integrated mode =========================
// Runs: record macro + hunt + quest walk simultaneously.
// Hardcoded paths are used. ESC stops everything.

static bool run_full_integrated(const char *macroFile)
{
    // Setup hunt config
    gEnemyTemplatesPath = kDefaultEnemyPath;
    gBattleStartPath = kDefaultBattlePath;

    // Load quest marker template
    cv::Mat questTempl = cv::imread(kDefaultQuestPath, cv::IMREAD_COLOR);
    if (questTempl.empty())
    {
        std::fprintf(stderr, "[FULL] Failed to load quest marker: %s\n", kDefaultQuestPath);
        return false;
    }

    // Setup abs cursor
    gAbsCursorTemplatePath = kDefaultCursorPath;

    // Open output file
    ZeroMemory(gMouseBtn, sizeof(gMouseBtn));
    ZeroMemory(gKeyDown, sizeof(gKeyDown));
    gLastDx = gLastDy = 0;
    gLastWheel = 0;
    gCursorPt = POINT{0, 0};
    gAbsByAlt = false;
    gAbsByCursor = false;

    gOut = std::fopen(macroFile, "wb");
    if (!gOut)
    {
        std::fprintf(stderr, "[FULL] Cannot open macro file: %s\n", macroFile);
        return false;
    }

    static char fileBuf[1 << 20];
    setvbuf(gOut, fileBuf, _IOFBF, sizeof(fileBuf));

    FileHeader hdr{};
    hdr.magic = 0x524D4143;
    hdr.version = 1;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    hdr.start_utc = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    fwrite(&hdr, sizeof(hdr), 1, gOut);
    fflush(gOut);

    if (!create_sink_window())
    {
        std::fclose(gOut);
        gOut = nullptr;
        return false;
    }
    if (!create_overlay_window())
    {
        destroy_sink_window();
        std::fclose(gOut);
        gOut = nullptr;
        return false;
    }

    QueryPerformanceFrequency(&gFreq);
    QueryPerformanceCounter(&gT0);

    timeBeginPeriod(1);

    start_abs_poll_thread();
    start_cursor_detect_thread();
    start_auto_hunt(kDefaultEnemyPath, kDefaultBattlePath, gEnemyTh, gBattleTh, gScanMs, gCooldownMs);
    start_quest_walk(questTempl, gMarkerTh, gDeadzonePx, gQuestTickMs);

    countdown_3s("[FULL] Recording + Hunt + QuestWalk begin");

    gRecording = true;
    overlay_show(true);
    std::puts("[FULL] Running. ESC to stop all.");

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        maybe_restart_hunt_on_shift();
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gRecording = false;
    overlay_show(false);

    stop_all_threads();
    flush_events();
    timeEndPeriod(1);

    if (gOut)
    {
        std::fclose(gOut);
        gOut = nullptr;
    }

    destroy_overlay_window();
    destroy_sink_window();

    std::puts("[FULL] Stopped.");
    return true;
}

// ========================= CLI parsing =========================

static void parse_abs_args(int argc, char **argv, int startIdx)
{
    if (argc > startIdx)
        gAbsCursorTemplatePath = argv[startIdx];
    if (argc > startIdx + 1)
        gCursorTh = std::atof(argv[startIdx + 1]);
    if (argc > startIdx + 2)
        gCursorScanMs = std::atoi(argv[startIdx + 2]);
    if (argc > startIdx + 3)
        gAbsPollMs = std::atoi(argv[startIdx + 3]);

    gCursorTh = std::clamp(gCursorTh, 0.1, 0.999);
    if (gCursorScanMs < 10)
        gCursorScanMs = 10;
    if (gAbsPollMs < 1)
        gAbsPollMs = 1;
}

static void parse_ignore_rect(int argc, char **argv, int startIdx)
{
    if (argc > startIdx + 3)
    {
        gQuestLogIgnore.left = std::atoi(argv[startIdx + 0]);
        gQuestLogIgnore.top = std::atoi(argv[startIdx + 1]);
        gQuestLogIgnore.right = std::atoi(argv[startIdx + 2]);
        gQuestLogIgnore.bottom = std::atoi(argv[startIdx + 3]);
    }
}

// ========================= main =========================

int main(int argc, char **argv)
{
    enable_dpi_awareness();

    if (argc < 2)
    {
        std::printf(
            "Usage:\n"
            "  %s record       [file.rmac=%s] [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]\n"
            "  %s play         [file.rmac=%s] [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]\n"
            "  %s recordhunt  [file.rmac] [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]\n"
            "                  [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]\n"
            "  %s playhunt    [file.rmac] [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]\n"
            "                  [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]\n"
            "  %s questwalk   [quest_marker.png=%s] [marker_th] [deadzone_px] [tick_ms]\n"
            "                  [ignoreL] [ignoreT] [ignoreR] [ignoreB]\n"
            "  %s hunt        [enemy_path=%s] [battle_start.png=%s] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]\n"
            "  %s full        [file.rmac=%s]   (record + hunt + questwalk, all hardcoded paths)\n"
            "\n"
            "Hardcoded defaults:\n"
            "  Enemy templates: %s\n"
            "  Battle start:    %s\n"
            "  Cursor template: %s\n"
            "  Quest marker:    %s\n"
            "  enemy_th=0.75  battle_th=0.88  scan_ms=200  cooldown_ms=900\n"
            "  cursor_th=0.88 cursor_scan_ms=33 abs_poll_ms=2\n"
            "  marker_th=0.85 deadzone_px=40 tick_ms=50 ignoreRect=45,282,72,311\n",
            argv[0], kDefaultMacroFile,
            argv[0], kDefaultMacroFile,
            argv[0], argv[0],
            argv[0], kDefaultQuestPath,
            argv[0], kDefaultEnemyPath, kDefaultBattlePath,
            argv[0], kDefaultMacroFile,
            kDefaultEnemyPath, kDefaultBattlePath, kDefaultCursorPath, kDefaultQuestPath);
        return 0;
    }

    std::string cmd = argv[1];

    // ---- record ----
    if (cmd == "record")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        parse_abs_args(argc, argv, 3);
        return record_to_file(file) ? 0 : 1;
    }

    // ---- play ----
    if (cmd == "play")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        parse_abs_args(argc, argv, 3);
        return play_file(file) ? 0 : 1;
    }

    // ---- recordhunt / playhunt ----
    if (cmd == "recordhunt" || cmd == "playhunt")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        const char *enemiesPath = (argc >= 4) ? argv[3] : kDefaultEnemyPath;
        const char *battlePath = (argc >= 5) ? argv[4] : kDefaultBattlePath;

        double enemyTh = (argc >= 6) ? std::atof(argv[5]) : 0.75;
        double battleTh = (argc >= 7) ? std::atof(argv[6]) : 0.88;
        int scanMs = (argc >= 8) ? std::atoi(argv[7]) : 200;
        int cooldownMs = (argc >= 9) ? std::atoi(argv[8]) : 900;

        gEnemyTemplatesPath = enemiesPath;
        gBattleStartPath = battlePath;
        gEnemyTh = enemyTh;
        gBattleTh = battleTh;
        gScanMs = scanMs;
        gCooldownMs = cooldownMs;

        parse_abs_args(argc, argv, 9);

        if (cmd == "recordhunt")
            return record_hunt(file, enemiesPath, battlePath, enemyTh, battleTh, scanMs, cooldownMs) ? 0 : 1;
        return play_hunt(file, enemiesPath, battlePath, enemyTh, battleTh, scanMs, cooldownMs) ? 0 : 1;
    }

    // ---- questwalk ----
    if (cmd == "questwalk")
    {
        const char *questPath = (argc >= 3) ? argv[2] : kDefaultQuestPath;
        double markerTh = (argc >= 4) ? std::atof(argv[3]) : 0.85;
        int deadzonePx = (argc >= 5) ? std::atoi(argv[4]) : 40;
        int tickMs = (argc >= 6) ? std::atoi(argv[5]) : 50;
        parse_ignore_rect(argc, argv, 6);

        cv::Mat questTempl = cv::imread(questPath, cv::IMREAD_COLOR);
        if (questTempl.empty())
        {
            std::fprintf(stderr, "Failed to load quest marker template: %s\n", questPath);
            return 1;
        }

        quest_walk_standalone(questTempl, markerTh, deadzonePx, tickMs);
        return 0;
    }

    // ---- hunt (standalone, no macro) ----
    if (cmd == "hunt")
    {
        const char *enemiesPath = (argc >= 3) ? argv[2] : kDefaultEnemyPath;
        const char *battlePath = (argc >= 4) ? argv[3] : kDefaultBattlePath;
        double enemyTh = (argc >= 5) ? std::atof(argv[4]) : 0.75;
        double battleTh = (argc >= 6) ? std::atof(argv[5]) : 0.88;
        int scanMs = (argc >= 7) ? std::atoi(argv[6]) : 200;
        int cooldownMs = (argc >= 8) ? std::atoi(argv[7]) : 900;

        gEnemyTemplatesPath = enemiesPath;
        gBattleStartPath = battlePath;
        gEnemyTh = enemyTh;
        gBattleTh = battleTh;
        gScanMs = scanMs;
        gCooldownMs = cooldownMs;

        // Need gPlaying or gRunQuestWalk=true for hunt to actually attack
        // Set gPlaying=true so hunt thread attacks
        gPlaying = true;

        bool overlay_ok = create_overlay_window();
        if (overlay_ok)
        {
            overlay_show(true);
            pump_messages_nonblocking();
        }

        timeBeginPeriod(1);
        start_auto_hunt(enemiesPath, battlePath, enemyTh, battleTh, scanMs, cooldownMs);
        std::puts("[HUNT] Standalone hunt running. ESC to stop.");

        while (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000))
        {
            maybe_restart_hunt_on_shift();
            pump_messages_nonblocking();
            Sleep(50);
        }

        gPlaying = false;
        stop_all_threads();
        timeEndPeriod(1);

        if (overlay_ok)
        {
            overlay_show(false);
            destroy_overlay_window();
            pump_messages_nonblocking();
        }
        return 0;
    }

    // ---- full (all integrated) ----
    if (cmd == "full")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        return run_full_integrated(file) ? 0 : 1;
    }

    std::fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
    return 1;
}