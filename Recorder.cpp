// Recorder.cpp  (Full Integration: macro recorder + playback + OpenCV hunt + QuestMarker walk)
//
// Features:
// 1) High-precision REL recording via WM_INPUT (reused buffer, large file buffer, no per-event fflush)
// 2) ABS mode: ALT key OR Cursor.png template match triggers high-rate GetCursorPos poll + EV_MOUSE_POS log
// 3) QuestMarker navigation with Tesseract OCR distance reading:
//    - Finds ALL marker matches, ignores quest log rect (45,282)-(72,311)
//    - Captures ROI below marker, OCRs the distance number (e.g. "76m")
//    - distance > 5m  : W + Shift (fast) + A/D steering
//    - distance 3-5m  : W only (slow approach) + A/D steering
//    - distance <= 3m : STOP all movement and wait
//    - distance >= 5m while stopped: resume walking
//    - overshot guard : if stopped and distance grows, press S briefly to correct
// 4) Hunt + BattleStart: stops hunt AND questwalk on BattleStart; SHIFT restarts hunt
// 5) Hardcoded default paths: templates\Enemies, templates\BattleStart.png,
//    templates\Cursor.png, templates\QuestMarker.png
// 6) Threads: hunt, cursor-detect, abs-poll, quest-walk (all independent, safe shutdown)
// 7) Clean shutdown: stop all threads, release keys, join safely
// 8) Preserves .rmac file format
//
// Commands:
//   Recorder.exe record       [file.rmac] [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe play         [file.rmac] [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe recordhunt  [file.rmac] [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//                             [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe playhunt    [file.rmac] [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//                             [cursor.png] [cursor_th] [cursor_scan_ms] [abs_poll_ms]
//   Recorder.exe questwalk   [quest_marker.png] [marker_th] [deadzone_px] [tick_ms]
//                             [ignoreL] [ignoreT] [ignoreR] [ignoreB]
//   Recorder.exe hunt        [enemy_path] [battle_start.png] [enemy_th] [battle_th] [scan_ms] [cooldown_ms]
//   Recorder.exe full        [file.rmac]   (record + hunt + questwalk, all hardcoded paths)
//
// Defaults:
//   macro.rmac | templates\Enemies | templates\BattleStart.png
//   templates\Cursor.png | templates\QuestMarker.png
//   enemy_th=0.75  battle_th=0.88  scan_ms=200  cooldown_ms=900
//   cursor_th=0.88 cursor_scan_ms=33 abs_poll_ms=2
//   marker_th=0.85 deadzone_px=40 tick_ms=50 ignoreRect=45,282,72,311
//   arrival_m=3    resume_m=5
//
// Build (MSVC x64 Native Tools Prompt):
//   cl /EHsc /O2 /std:c++20 /MD Recorder.cpp user32.lib winmm.lib gdi32.lib ^
//      /I"opencv\build\include" ^
//      /I"tesseract\include" ^
//      /link /MACHINE:X64 ^
//      /LIBPATH:"opencv\build\x64\vc16\lib" opencv_world4120.lib ^
//      /LIBPATH:"tesseract\lib" tesseract53.lib leptonica-1.82.0.lib
//
// Runtime files needed next to Recorder.exe:
//   tesseract53.dll, leptonica-1.82.0.dll, tessdata\eng.traineddata

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
#include <cctype>

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

using namespace cv;

// ========================= Constants =========================

static const char *kSinkClassName = "RawIO_Sink_Window";
static const char *kOverlayClassName = "RawIO_Overlay_Window";

static const char *kDefaultMacroFile = "macro.rmac";
static const char *kDefaultEnemyPath = "templates\\Enemies";
static const char *kDefaultBattlePath = "templates\\BattleStart.png";
static const char *kDefaultCursorPath = "templates\\Cursor.png";
static const char *kDefaultQuestPath = "templates\\QuestMarker.png";

// Quest log icon ignore rectangle (screen pixels)
static RECT gQuestLogIgnore = {45, 282, 72, 311};

// Distance thresholds (meters)
static const int kArrivalMeters = 3; // stop when dist <= this
static const int kResumeMeters = 5;  // resume when dist >= this after stopping

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
    uint32_t magic;   // 'RMAC' = 0x524D4143
    uint32_t version; // 1
    uint64_t start_utc;
};
struct Event
{
    uint32_t type;
    uint64_t t_us;
    int32_t a, b, c;
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

static LONG gLastDx = 0, gLastDy = 0;
static int gLastWheel = 0;
static bool gMouseBtn[6] = {};
static bool gKeyDown[256] = {};
static POINT gCursorPt{};

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

// ========================= Hunt state =========================

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
    void getLastName(char *out, size_t sz) const
    {
        std::lock_guard<std::mutex> lk(nameMu);
        std::snprintf(out, sz, "%s", lastName[0] ? lastName : "(none)");
    }
};
static HuntInfo gHuntInfo;

// ========================= Quest walk state =========================

static std::atomic<bool> gRunQuestWalk{false};
static std::thread gQuestWalkThread;
static std::string gQuestMarkerPath = kDefaultQuestPath;
static double gMarkerTh = 0.85;
static int gDeadzonePx = 40;
static int gQuestTickMs = 50;

// Overlay feedback
static std::atomic<int> gQuestMarkerX{-1};
static std::atomic<int> gQuestMarkerY{-1};
static std::atomic<double> gQuestMarkerConf{0.0};
static std::atomic<int> gQuestDistanceM{-1};

// ========================= Tesseract OCR =========================

class TesseractOCR
{
public:
    bool init(const char *dataPath = "tessdata", const char *lang = "eng")
    {
        api_ = new tesseract::TessBaseAPI();
        if (api_->Init(dataPath, lang) != 0)
        {
            std::fprintf(stderr, "[OCR] Tesseract init failed. tessdata path: %s\n", dataPath);
            delete api_;
            api_ = nullptr;
            return false;
        }
        // Only allow digits and 'm' - fastest, cleanest output
        api_->SetVariable("tessedit_char_whitelist", "0123456789m");
        api_->SetPageSegMode(tesseract::PSM_SINGLE_LINE);
        std::puts("[OCR] Tesseract ready.");
        return true;
    }

    ~TesseractOCR()
    {
        if (api_)
        {
            api_->End();
            delete api_;
            api_ = nullptr;
        }
    }

    // Returns distance in meters, -1 on failure
    int readDistance(const cv::Mat &roiBGR)
    {
        if (!api_ || roiBGR.empty())
            return -1;

        // Preprocess: grayscale -> threshold white-on-dark -> upscale 3x for accuracy
        cv::Mat gray, thresh, scaled;
        cv::cvtColor(roiBGR, gray, cv::COLOR_BGR2GRAY);
        cv::threshold(gray, thresh, 160, 255, cv::THRESH_BINARY);
        cv::resize(thresh, scaled, cv::Size(), 3.0, 3.0, cv::INTER_LINEAR);

        // Convert cv::Mat to Leptonica Pix
        Pix *pix = pixCreate(scaled.cols, scaled.rows, 8);
        for (int y = 0; y < scaled.rows; ++y)
            for (int x = 0; x < scaled.cols; ++x)
                pixSetPixel(pix, x, y, scaled.at<uchar>(y, x));

        api_->SetImage(pix);
        char *raw = api_->GetUTF8Text();
        pixDestroy(&pix);

        int dist = -1;
        if (raw)
        {
            std::string s(raw);
            delete[] raw;
            // Strip whitespace
            s.erase(std::remove_if(s.begin(), s.end(),
                                   [](unsigned char c)
                                   { return std::isspace(c); }),
                    s.end());
            // Strip trailing 'm'
            if (!s.empty() && (s.back() == 'm' || s.back() == 'M'))
                s.pop_back();
            if (!s.empty())
            {
                try
                {
                    dist = std::stoi(s);
                }
                catch (...)
                {
                    dist = -1;
                }
            }
        }
        return dist;
    }

private:
    tesseract::TessBaseAPI *api_ = nullptr;
};

// ========================= DPI Awareness =========================

static void enable_dpi_awareness()
{
    HMODULE u32 = LoadLibraryA("user32.dll");
    if (u32)
    {
        using Fn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
        auto f = (Fn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (f)
        {
            f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            FreeLibrary(u32);
            return;
        }
        FreeLibrary(u32);
    }
    SetProcessDPIAware();
}

// ========================= Timing =========================

static uint64_t now_us_since_start()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    long double dt = (long double)(t.QuadPart - gT0.QuadPart) / (long double)gFreq.QuadPart;
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
    fwrite(&ev, sizeof(ev), 1, gOut); // no fflush - precision
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
        ShowWindow(gOverlayHwnd, SW_HIDE);
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
        { TextOutA(hdc, 8, y, s, (int)strlen(s)); y += 18; };

        bool absMode = gAbsByAlt.load() || gAbsByCursor.load();
        put(gRecording ? "RawIO [Recording]" : (gPlaying ? "RawIO [Playing]" : "RawIO Overlay"));

        snprintf(line, sizeof(line), "Mode: %s  ALT=%d CursorMatch=%d",
                 absMode ? "ABS" : "REL", (int)gAbsByAlt.load(), (int)gAbsByCursor.load());
        put(line);

        snprintf(line, sizeof(line), "Cursor: %ld, %ld", (long)gCursorPt.x, (long)gCursorPt.y);
        put(line);

        snprintf(line, sizeof(line), "dx/dy: %ld / %ld  Wheel: %d", gLastDx, gLastDy, gLastWheel);
        put(line);

        snprintf(line, sizeof(line), "Btns: L=%d R=%d M=%d X1=%d X2=%d",
                 gMouseBtn[1], gMouseBtn[2], gMouseBtn[3], gMouseBtn[4], gMouseBtn[5]);
        put(line);

        snprintf(line, sizeof(line), "Keys: W=%d A=%d S=%d D=%d Shift=%d Ctrl=%d Alt=%d",
                 gKeyDown['W'], gKeyDown['A'], gKeyDown['S'], gKeyDown['D'],
                 gKeyDown[VK_SHIFT], gKeyDown[VK_CONTROL], gKeyDown[VK_MENU]);
        put(line);

        snprintf(line, sizeof(line), "Hunt=%d  Battle=%d  QuestWalk=%d",
                 (int)gAutoHuntRun.load(), (int)gBattleStarted.load(), (int)gRunQuestWalk.load());
        put(line);

        char nm[256];
        gHuntInfo.getLastName(nm, sizeof(nm));
        snprintf(line, sizeof(line), "HuntDet=%d Atk=%d  Last=%s conf=%.2f",
                 gHuntInfo.detections.load(), gHuntInfo.attacks.load(),
                 nm, gHuntInfo.lastConf.load());
        put(line);

        int qx = gQuestMarkerX.load(), qy = gQuestMarkerY.load(), qd = gQuestDistanceM.load();
        if (qx >= 0)
            snprintf(line, sizeof(line), "Quest: (%d,%d) conf=%.2f  dist=%dm",
                     qx, qy, gQuestMarkerConf.load(), qd);
        else
            snprintf(line, sizeof(line), "Quest: marker not found");
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
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    gOverlayHwnd = CreateWindowExA(ex, kOverlayClassName, "RawIO Overlay",
                                   WS_POPUP, 10, 10, 500, 340,
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
    int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN), vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsw <= 0 || vsh <= 0)
        return;
    double relx = std::clamp((double)(x - vsx) / vsw, 0.0, 1.0);
    double rely = std::clamp((double)(y - vsy) / vsh, 0.0, 1.0);
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
    rids[0].usUsage = 0x02;
    rids[0].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
    rids[0].hwndTarget = hwnd;
    rids[1].usUsagePage = 0x01;
    rids[1].usUsage = 0x06;
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
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT,
                            gRawBuf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            break;

        RAWINPUT *raw = reinterpret_cast<RAWINPUT *>(gRawBuf.data());

        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE &m = raw->data.mouse;
            gAbsByAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
            bool absMode = gAbsByAlt.load() || gAbsByCursor.load();

            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
            {
                LONG dx = m.lLastX, dy = m.lLastY;
                if (dx != 0 || dy != 0)
                {
                    gLastDx = dx;
                    gLastDy = dy;
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
            auto log_btn = [&](int btn, bool down)
            {
                if (btn >= 1 && btn <= 5)
                    gMouseBtn[btn] = down;
                write_event(EV_MOUSE_BUTTON, (int32_t)btn, down ? 1 : 0, 0);
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
    gSinkHwnd = CreateWindowExA(0, kSinkClassName, "RawIO_Sink",
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
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC hScreen = GetDC(NULL), hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, vw, vh);
    HGDIOBJ old = SelectObject(hDC, hBmp);
    BitBlt(hDC, 0, 0, vw, vh, hScreen, vx, vy, SRCCOPY | CAPTUREBLT);
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
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
    int w = halfSize * 2, h = halfSize * 2, x0 = pt.x - halfSize, y0 = pt.y - halfSize;
    HDC hScreen = GetDC(NULL), hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    HGDIOBJ old = SelectObject(hDC, hBmp);
    BitBlt(hDC, 0, 0, w, h, hScreen, x0, y0, SRCCOPY | CAPTUREBLT);
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
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

// Crop the distance label area below a detected quest marker
// markerCenter = center pixel of the matched template on screen
// templH       = height of the quest marker template (pixels)
static cv::Mat crop_distance_label(const cv::Mat &screen, cv::Point markerCenter,
                                   int templH, int roiW = 80, int roiH = 28)
{
    int x = markerCenter.x - roiW / 2;
    int y = markerCenter.y + templH / 2 + 2; // just below the marker bottom edge
    x = std::max(0, std::min(screen.cols - roiW, x));
    y = std::max(0, std::min(screen.rows - roiH, y));
    return screen(cv::Rect(x, y, roiW, roiH)).clone();
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
            std::fprintf(stderr, "Enemy path not found: %s\n", path.c_str());
            return false;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            return loadFolder(path);
        Mat img = imread(path, IMREAD_COLOR);
        if (img.empty())
        {
            std::fprintf(stderr, "Failed to load: %s\n", path.c_str());
            return false;
        }
        enemies_.push_back(img);
        enemyNames_.push_back(path);
        std::printf("Loaded enemy: %s (%dx%d)\n", path.c_str(), img.cols, img.rows);
        return true;
    }

    bool loadBattleStartTemplate(const std::string &file)
    {
        battle_ = imread(file, IMREAD_COLOR);
        if (battle_.empty())
        {
            std::fprintf(stderr, "Failed to load battle: %s\n", file.c_str());
            return false;
        }
        std::printf("Loaded battle-start: %dx%d\n", battle_.cols, battle_.rows);
        return true;
    }

    void setEnemyThreshold(double t) { enemyTh_ = t; }
    void setBattleThreshold(double t) { battleTh_ = t; }

    bool isBattleStart(const Mat &screen, double *outConf = nullptr) const
    {
        if (battle_.empty() || battle_.cols > screen.cols || battle_.rows > screen.rows)
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
                *outConf = 0;
            if (outIdx)
                *outIdx = -1;
            return Point(-1, -1);
        }
        double bestScore = -1;
        Point bestLoc(-1, -1);
        int bestIdx = -1;
        for (int i = 0; i < (int)enemies_.size(); ++i)
        {
            const Mat &t = enemies_[i];
            if (t.cols > screen.cols || t.rows > screen.rows)
                continue;
            Mat result;
            matchTemplate(screen, t, result, TM_CCOEFF_NORMED);
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
            return Point(bestLoc.x + enemies_[bestIdx].cols / 2, bestLoc.y + enemies_[bestIdx].rows / 2);
        return Point(-1, -1);
    }

    const char *enemyName(int idx) const
    {
        return (idx >= 0 && idx < (int)enemyNames_.size()) ? enemyNames_[idx].c_str() : "";
    }

    static void moveCursorTowards(const Point &target, int steps = 18, int stepMs = 6)
    {
        POINT pt{};
        GetCursorPos(&pt);
        Point cur(pt.x, pt.y);
        for (int i = 1; i <= steps; ++i)
        {
            double t = (double)i / steps;
            SetCursorPos((int)(cur.x + (target.x - cur.x) * t), (int)(cur.y + (target.y - cur.y) * t));
            std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
        }
    }

private:
    static bool has_image_ext(const std::string &name)
    {
        std::string s = name;
        for (char &c : s)
            c = (char)tolower((unsigned char)c);
        return (s.size() >= 4 && (s.rfind(".png") == s.size() - 4 || s.rfind(".jpg") == s.size() - 4 ||
                                  s.rfind(".bmp") == s.size() - 4)) ||
               (s.size() >= 5 && s.rfind(".jpeg") == s.size() - 5);
    }

    bool loadFolder(const std::string &folder)
    {
        WIN32_FIND_DATAA data{};
        std::string search = folder + "\\*.*";
        HANDLE h = FindFirstFileA(search.c_str(), &data);
        if (h == INVALID_HANDLE_VALUE)
        {
            std::fprintf(stderr, "Cannot open folder: %s\n", folder.c_str());
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

// ========================= Cursor Template Detection =========================

static bool load_abs_cursor_template(const std::string &path)
{
    if (path.empty())
        return false;
    gAbsCursorTempl = cv::imread(path, cv::IMREAD_COLOR);
    if (gAbsCursorTempl.empty())
    {
        std::fprintf(stderr, "[ABS] Failed cursor template: %s\n", path.c_str());
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
        if (tScaled.empty() || tScaled.cols > roi.cols || tScaled.rows > roi.rows)
            continue;
        cv::Mat result;
        cv::matchTemplate(roi, tScaled, result, cv::TM_CCOEFF_NORMED);
        double minV = 0, maxV = 0;
        cv::Point mL, xL;
        cv::minMaxLoc(result, &minV, &maxV, &mL, &xL);
        if (maxV > best)
            best = maxV;
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
        while (gRunCursorDetect)
        {
            if (!gRecording && !gPlaying) { gAbsByCursor = false; Sleep(50); continue; }
            cv::Mat roi = capture_roi_around_cursor(80);
            if (roi.empty() || gAbsCursorTempl.empty())
            { gAbsByCursor = false; Sleep(gCursorScanMs); continue; }
            double score = gCursorMultiScale
                ? best_match_score_multiscale(roi, gAbsCursorTempl)
                : [&]()->double{
                      if (gAbsCursorTempl.cols>roi.cols||gAbsCursorTempl.rows>roi.rows) return -1.0;
                      cv::Mat r; cv::matchTemplate(roi,gAbsCursorTempl,r,cv::TM_CCOEFF_NORMED);
                      double mn=0,mx=0; cv::Point mL,xL; cv::minMaxLoc(r,&mn,&mx,&mL,&xL); return mx;
                  }();
            gAbsByCursor = (score >= gCursorTh);
            Sleep(gCursorScanMs);
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
                if (GetCursorPos(&pt) && (pt.x!=last.x || pt.y!=last.y))
                {
                    last=pt; gCursorPt=pt;
                    write_event(EV_MOUSE_POS,(int32_t)pt.x,(int32_t)pt.y,0);
                    overlay_invalidate();
                }
            }
            else { POINT pt{}; if(GetCursorPos(&pt)) gCursorPt=pt; }
            gAbsByAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
            Sleep(gAbsPollMs);
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
    if (screen.empty() || templ.empty() || templ.cols > screen.cols || templ.rows > screen.rows)
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
        int x0 = std::max(0, maxL.x - templ.cols / 2), y0 = std::max(0, maxL.y - templ.rows / 2);
        int x1 = std::min(result.cols, maxL.x + templ.cols / 2), y1 = std::min(result.rows, maxL.y + templ.rows / 2);
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
    const int cx0 = screen.cols / 2, cy0 = screen.rows / 2;
    bool found = false;
    double bestCost = 1e30, bestScore = 0;
    cv::Point bestCenter(-1, -1);
    for (auto &h : hits)
    {
        int cx = h.topLeft.x + questTempl.cols / 2, cy = h.topLeft.y + questTempl.rows / 2;
        if (point_in_rect(cx, cy, gQuestLogIgnore))
            continue;
        double dx = cx - cx0, dy = cy - cy0, cost = dx * dx + dy * dy;
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
        std::puts("[QUEST] Thread started. ESC to stop.");

        // Tesseract lives on this thread only
        TesseractOCR ocr;
        bool ocrOk = ocr.init("tessdata", "eng");
        if (!ocrOk) std::puts("[QUEST] WARNING: Tesseract unavailable - no distance OCR.");

        bool arrived = false;   // true when we stopped at <= kArrivalMeters
        int  prevDist = -1;     // last valid OCR distance

        // Begin walking
        send_key(true, 'W');
        send_key(true, VK_SHIFT);

        while (gRunQuestWalk)
        {
            // --- ESC ---
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            { std::puts("[QUEST] ESC."); gRunQuestWalk = false; break; }

            // --- Battle pause ---
            if (gBattleStarted.load())
            {
                std::puts("[QUEST] Battle detected - pausing.");
                release_move_keys();
                while (gRunQuestWalk && gBattleStarted.load())
                {
                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { gRunQuestWalk=false; break; }
                    Sleep(200);
                }
                if (!gRunQuestWalk) break;
                std::puts("[QUEST] Resuming after battle.");
                arrived = false;
                send_key(true, 'W');
                send_key(true, VK_SHIFT);
                continue;
            }

            // --- Capture + find marker ---
            cv::Mat screen = capture_screen_full();
            cv::Point markerCenter; double conf = 0.0;
            bool markerFound = pick_world_marker(screen, questTempl, markerTh, markerCenter, conf);

            if (!markerFound)
            {
                gQuestMarkerX = gQuestMarkerY = -1; gQuestMarkerConf = 0.0;
                // Marker not visible: keep going forward, release steering
                if (!arrived)
                {
                    send_key(false, 'A'); send_key(false, 'D'); send_key(false, 'S');
                }
                overlay_invalidate();
                Sleep(tickMs);
                continue;
            }

            gQuestMarkerX    = markerCenter.x;
            gQuestMarkerY    = markerCenter.y;
            gQuestMarkerConf = conf;

            // --- OCR distance number below marker ---
            int distM = -1;
            if (ocrOk)
            {
                cv::Mat distRoi = crop_distance_label(screen, markerCenter, questTempl.rows, 80, 28);
                if (!distRoi.empty())
                    distM = ocr.readDistance(distRoi);
            }

            // Keep last valid reading if OCR failed this frame
            if (distM >= 0) prevDist = distM;
            else            distM    = prevDist;

            gQuestDistanceM = distM;
            overlay_invalidate();

            std::printf("[QUEST] pos=(%d,%d) conf=%.2f dist=%dm arrived=%d\n",
                        markerCenter.x, markerCenter.y, conf, distM, (int)arrived);

            // ============================================================
            // Distance-based movement control
            // ============================================================

            if (!arrived)
            {
                // Check if we've arrived
                if (distM >= 0 && distM <= kArrivalMeters)
                {
                    std::printf("[QUEST] ARRIVED - dist=%dm <= %dm. Stopping.\n", distM, kArrivalMeters);
                    release_move_keys();
                    arrived = true;
                    Sleep(tickMs);
                    continue;
                }

                // Speed: fast (W+Shift) when >5m, slow (W only) when 3-5m
                if (distM < 0 || distM > 5)
                {
                    send_key(true, 'W'); send_key(true, VK_SHIFT);
                }
                else
                {
                    send_key(true,  'W'); send_key(false, VK_SHIFT);
                }
                send_key(false, 'S');
            }
            else
            {
                // We are stopped at destination - check resume conditions
                if (distM >= 0 && distM >= kResumeMeters)
                {
                    std::printf("[QUEST] Resuming - dist=%dm >= %dm.\n", distM, kResumeMeters);
                    arrived = false;
                    send_key(true, 'W'); send_key(true, VK_SHIFT);
                    send_key(false, 'S');
                }
                else if (distM > kArrivalMeters && distM < kResumeMeters)
                {
                    // Just slightly too far - nudge forward slowly without Shift
                    send_key(true,  'W'); send_key(false, VK_SHIFT); send_key(false, 'S');
                }
                else
                {
                    // Still at destination.
                    // Overshoot guard: if distance is growing we've drifted past - press S to correct
                    if (prevDist >= 0 && distM >= 0 && distM > prevDist + 2)
                    {
                        std::printf("[QUEST] Overshot! %d->%dm - pressing S.\n", prevDist, distM);
                        send_key(false, 'W'); send_key(false, VK_SHIFT);
                        send_key(true,  'S');
                        Sleep(300);
                        send_key(false, 'S');
                    }
                    else
                    {
                        // Fully stopped
                        send_key(false, 'W'); send_key(false, 'S'); send_key(false, VK_SHIFT);
                    }
                    Sleep(tickMs);
                    continue;
                }
            }

            // ============================================================
            // Horizontal steering: A / D based on marker X vs screen center
            // ============================================================

            int centerX = screen.cols / 2;
            int dx = markerCenter.x - centerX;

            if (dx < -deadzonePx)
            {
                send_key(true,  'A'); send_key(false, 'D'); send_key(false, 'S');
            }
            else if (dx > deadzonePx)
            {
                send_key(true,  'D'); send_key(false, 'A'); send_key(false, 'S');
            }
            else
            {
                send_key(false, 'A'); send_key(false, 'D'); send_key(false, 'S');
            }

            GetCursorPos(&gCursorPt);
            overlay_invalidate();
            Sleep(tickMs);
        }

        release_move_keys();
        gQuestMarkerX = gQuestMarkerY = -1; gQuestDistanceM = -1;
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
    std::printf("[HUNT] SHIFT -> restart hunt, clear battle flag.\n");
    gBattleStarted = false;
    start_auto_hunt_with_saved_config();
}

static void start_auto_hunt(const char *enemyTemplatesPath, const char *battleStartTemplatePath,
                            double enemyThreshold, double battleThreshold,
                            int scanMs, int attackCooldownMs)
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
        std::fprintf(stderr, "[HUNT] Failed enemy templates: %s\n", enemyTemplatesPath);
        return;
    }
    if (!gDet.loadBattleStartTemplate(battleStartTemplatePath))
    {
        std::fprintf(stderr, "[HUNT] Failed battle template: %s\n", battleStartTemplatePath);
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
        int tick = 0;
        std::printf("[HUNT] Thread started.\n");

        while (gAutoHuntRun)
        {
            if (!gRecording && !gPlaying && !gRunQuestWalk.load()) { Sleep(50); continue; }

            Mat screen = capture_screen_full();

            double battleConf = 0.0;
            if (gDet.isBattleStart(screen, &battleConf))
            {
                gBattleStarted=true;
                gHuntInfo.lastWasBattle=true; gHuntInfo.lastConf=battleConf;
                gHuntInfo.setLastName("BattleStart"); gHuntInfo.lastX=gHuntInfo.lastY=-1;
                overlay_invalidate();
                std::printf("[HUNT] Battle Start conf=%.2f. Hunt OFF. SHIFT to restart.\n", battleConf);
                gAutoHuntRun = false; break;
            }

            double enemyConf=0.0; int idx=-1;
            Point p = gDet.findEnemy(screen, &enemyConf, &idx);
            tick++;
            if ((tick%25)==0)
                std::printf("[DEBUG] conf=%.3f th=%.3f best=%s\n",
                            enemyConf, enemyThreshold, gDet.enemyName(idx));

            if (p.x >= 0 && p.y >= 0)
            {
                gHuntInfo.lastWasBattle=false; gHuntInfo.lastConf=enemyConf;
                gHuntInfo.lastX=p.x; gHuntInfo.lastY=p.y;
                gHuntInfo.setLastName(gDet.enemyName(idx));
                gHuntInfo.detections++;
                overlay_invalidate();

                if (gPlaying || gRunQuestWalk.load())
                {
                    auto now = std::chrono::steady_clock::now();
                    if (now-lastAttack >= std::chrono::milliseconds(attackCooldownMs))
                    {
                        TemplateDetector::moveCursorTowards(p, 18, 6);
                        send_mouse_button(1, true); Sleep(35); send_mouse_button(1, false);
                        gHuntInfo.attacks++; overlay_invalidate();
                        std::printf("[HUNT] Attacked: %s conf=%.2f at=(%d,%d)\n",
                                    gDet.enemyName(idx), enemyConf, p.x, p.y);
                        lastAttack = now;
                    }
                }
                else
                    std::printf("[SCAN] Enemy: %s conf=%.2f at=(%d,%d)\n",
                                gDet.enemyName(idx), enemyConf, p.x, p.y);
            }
            Sleep(scanMs);
        } });
}

static void start_auto_hunt_with_saved_config()
{
    if (gEnemyTemplatesPath.empty() || gBattleStartPath.empty())
    {
        std::fprintf(stderr, "[HUNT] No saved config.\n");
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
        std::fprintf(stderr, "Cannot open: %s\n", path);
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
        std::fprintf(stderr, "Cannot open: %s\n", path);
        return false;
    }
    FileHeader hdr{};
    if (read_exact(in, &hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != 0x524D4143)
    {
        std::fprintf(stderr, "Invalid format.\n");
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
            std::puts("\nStopped by ESC.");
            break;
        }

        const Event &e = events[i];
        if (e.t_us > prev_t)
            std::this_thread::sleep_for(std::chrono::microseconds(e.t_us - prev_t));
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

static bool record_hunt(const char *file, const char *ep, const char *bp,
                        double et, double bt, int sm, int cm)
{
    gEnemyTemplatesPath = ep;
    gBattleStartPath = bp;
    gEnemyTh = et;
    gBattleTh = bt;
    gScanMs = sm;
    gCooldownMs = cm;
    start_auto_hunt(ep, bp, et, bt, sm, cm);
    return record_to_file(file);
}

static bool play_hunt(const char *file, const char *ep, const char *bp,
                      double et, double bt, int sm, int cm)
{
    gEnemyTemplatesPath = ep;
    gBattleStartPath = bp;
    gEnemyTh = et;
    gBattleTh = bt;
    gScanMs = sm;
    gCooldownMs = cm;
    start_auto_hunt(ep, bp, et, bt, sm, cm);
    return play_file(file);
}

// ========================= Standalone quest walk =========================

static void quest_walk_standalone(const cv::Mat &questTempl, double markerTh, int deadzonePx, int tickMs)
{
    bool overlay_ok = create_overlay_window();
    if (overlay_ok)
    {
        overlay_show(true);
        pump_messages_nonblocking();
    }
    timeBeginPeriod(1);
    start_quest_walk(questTempl, markerTh, deadzonePx, tickMs);
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

static bool run_full_integrated(const char *macroFile)
{
    gEnemyTemplatesPath = kDefaultEnemyPath;
    gBattleStartPath = kDefaultBattlePath;
    gAbsCursorTemplatePath = kDefaultCursorPath;

    cv::Mat questTempl = cv::imread(kDefaultQuestPath, cv::IMREAD_COLOR);
    if (questTempl.empty())
    {
        std::fprintf(stderr, "[FULL] Failed quest marker: %s\n", kDefaultQuestPath);
        return false;
    }

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
        std::fprintf(stderr, "[FULL] Cannot open: %s\n", macroFile);
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

    countdown_3s("[FULL] Recording + Hunt + QuestWalk");
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

static void parse_abs_args(int argc, char **argv, int i)
{
    if (argc > i)
        gAbsCursorTemplatePath = argv[i];
    if (argc > i + 1)
        gCursorTh = std::atof(argv[i + 1]);
    if (argc > i + 2)
        gCursorScanMs = std::atoi(argv[i + 2]);
    if (argc > i + 3)
        gAbsPollMs = std::atoi(argv[i + 3]);
    gCursorTh = std::clamp(gCursorTh, 0.1, 0.999);
    if (gCursorScanMs < 10)
        gCursorScanMs = 10;
    if (gAbsPollMs < 1)
        gAbsPollMs = 1;
}

static void parse_ignore_rect(int argc, char **argv, int i)
{
    if (argc > i + 3)
    {
        gQuestLogIgnore.left = std::atoi(argv[i + 0]);
        gQuestLogIgnore.top = std::atoi(argv[i + 1]);
        gQuestLogIgnore.right = std::atoi(argv[i + 2]);
        gQuestLogIgnore.bottom = std::atoi(argv[i + 3]);
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
            "  %s record       [file=%s]\n"
            "  %s play         [file=%s]\n"
            "  %s recordhunt  [file] [enemies] [battle] [eTh] [bTh] [scan] [cool]\n"
            "  %s playhunt    [file] [enemies] [battle] [eTh] [bTh] [scan] [cool]\n"
            "  %s questwalk   [marker=%s] [th] [deadzone] [tick] [iL iT iR iB]\n"
            "  %s hunt        [enemies=%s] [battle=%s] [eTh] [bTh] [scan] [cool]\n"
            "  %s full        [file=%s]\n"
            "\nDistance: stop at %dm, resume at %dm. OCR uses tessdata\\eng.traineddata\n",
            argv[0], kDefaultMacroFile, argv[0], kDefaultMacroFile,
            argv[0], argv[0],
            argv[0], kDefaultQuestPath,
            argv[0], kDefaultEnemyPath, kDefaultBattlePath,
            argv[0], kDefaultMacroFile,
            kArrivalMeters, kResumeMeters);
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "record")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        parse_abs_args(argc, argv, 3);
        return record_to_file(file) ? 0 : 1;
    }

    if (cmd == "play")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        parse_abs_args(argc, argv, 3);
        return play_file(file) ? 0 : 1;
    }

    if (cmd == "recordhunt" || cmd == "playhunt")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        const char *ep = (argc >= 4) ? argv[3] : kDefaultEnemyPath;
        const char *bp = (argc >= 5) ? argv[4] : kDefaultBattlePath;
        double et = (argc >= 6) ? std::atof(argv[5]) : 0.75;
        double bt = (argc >= 7) ? std::atof(argv[6]) : 0.88;
        int sm = (argc >= 8) ? std::atoi(argv[7]) : 200;
        int cm = (argc >= 9) ? std::atoi(argv[8]) : 900;
        gEnemyTemplatesPath = ep;
        gBattleStartPath = bp;
        gEnemyTh = et;
        gBattleTh = bt;
        gScanMs = sm;
        gCooldownMs = cm;
        parse_abs_args(argc, argv, 9);
        if (cmd == "recordhunt")
            return record_hunt(file, ep, bp, et, bt, sm, cm) ? 0 : 1;
        return play_hunt(file, ep, bp, et, bt, sm, cm) ? 0 : 1;
    }

    if (cmd == "questwalk")
    {
        const char *qp = (argc >= 3) ? argv[2] : kDefaultQuestPath;
        double th = (argc >= 4) ? std::atof(argv[3]) : 0.85;
        int dz = (argc >= 5) ? std::atoi(argv[4]) : 40;
        int tick = (argc >= 6) ? std::atoi(argv[5]) : 50;
        parse_ignore_rect(argc, argv, 6);
        cv::Mat qt = cv::imread(qp, cv::IMREAD_COLOR);
        if (qt.empty())
        {
            std::fprintf(stderr, "Failed to load: %s\n", qp);
            return 1;
        }
        quest_walk_standalone(qt, th, dz, tick);
        return 0;
    }

    if (cmd == "hunt")
    {
        const char *ep = (argc >= 3) ? argv[2] : kDefaultEnemyPath;
        const char *bp = (argc >= 4) ? argv[3] : kDefaultBattlePath;
        double et = (argc >= 5) ? std::atof(argv[4]) : 0.75;
        double bt = (argc >= 6) ? std::atof(argv[5]) : 0.88;
        int sm = (argc >= 7) ? std::atoi(argv[6]) : 200;
        int cm = (argc >= 8) ? std::atoi(argv[7]) : 900;
        gEnemyTemplatesPath = ep;
        gBattleStartPath = bp;
        gEnemyTh = et;
        gBattleTh = bt;
        gScanMs = sm;
        gCooldownMs = cm;
        gPlaying = true;
        bool overlay_ok = create_overlay_window();
        if (overlay_ok)
        {
            overlay_show(true);
            pump_messages_nonblocking();
        }
        timeBeginPeriod(1);
        start_auto_hunt(ep, bp, et, bt, sm, cm);
        std::puts("[HUNT] Standalone. ESC to stop.");
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

    if (cmd == "full")
    {
        const char *file = (argc >= 3) ? argv[2] : kDefaultMacroFile;
        return run_full_integrated(file) ? 0 : 1;
    }

    std::fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
    return 1;
}