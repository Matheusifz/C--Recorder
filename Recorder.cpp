// macro_recplay.cpp
// Build (MSVC):
//   cl /EHsc /O2 macro_recplay.cpp user32.lib winmm.lib
//
// Notes:
// - Overlay works over normal windows and borderless fullscreen.
// - Overlay may NOT appear over "exclusive fullscreen" apps/games (by design of how exclusive fullscreen works).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h> // timeGetDevCaps / timeBeginPeriod / timeEndPeriod (link winmm.lib)
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>

static const char *kSinkClassName = "RawIO_Sink_Window";
static const char *kOverlayClassName = "RawIO_Overlay_Window";

enum EventType : uint32_t
{
    EV_MOUSE_MOVE = 0, // relative dx, dy
    EV_MOUSE_WHEEL = 1,
    EV_KEY_DOWN = 2,
    EV_KEY_UP = 3,
    EV_MOUSE_BUTTON = 4, // mouse button press/release
    EV_MOUSE_POS = 5,    // absolute x, y when Alt is held
};

#pragma pack(push, 1)
struct FileHeader
{
    uint32_t magic;     // 'RMAC' = 0x524D4143
    uint32_t version;   // 1
    uint64_t start_utc; // FILETIME; informational
};
struct Event
{
    uint32_t type; // EventType
    uint64_t t_us; // microseconds since start
    int32_t a;     // payload
    int32_t b;     // payload
    int32_t c;     // payload
};
#pragma pack(pop)

// --------------------------- Globals ---------------------------

static LARGE_INTEGER gFreq{};
static LARGE_INTEGER gT0{};

static FILE *gOut = nullptr;
static bool gRecording = false;
static bool gPlaying = false;

static HWND gSinkHwnd = nullptr;    // hidden raw input sink window
static HWND gOverlayHwnd = nullptr; // small always-on-top overlay window

// Overlay state
static LONG gLastDx = 0, gLastDy = 0;
static int gLastWheel = 0;
static bool gMouseBtn[6] = {};  // 1..5 used
static bool gKeyDown[256] = {}; // VK state
static POINT gCursorPt{};

// --------------------------- Timing / IO ---------------------------

static uint64_t now_us_since_start()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const long double dt = (long double)(t.QuadPart - gT0.QuadPart) / (long double)gFreq.QuadPart;
    return (uint64_t)(dt * 1000000.0L);
}

static void countdown_5s(const char *msg)
{
    std::cout << msg << " in 5 seconds...\n";
    for (int i = 5; i > 0; --i)
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

// --------------------------- Overlay Window ---------------------------

static void overlay_invalidate()
{
    if (gOverlayHwnd)
        InvalidateRect(gOverlayHwnd, nullptr, FALSE);
}

static void overlay_show(bool on)
{
    if (!gOverlayHwnd)
        return;
    ShowWindow(gOverlayHwnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (on)
        overlay_invalidate();
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // never steal focus

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);

        // Background fill (with layered alpha)
        HBRUSH bg = CreateSolidBrush(RGB(10, 10, 10));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(230, 230, 230));

        // A simple fixed font can look nicer than the default.
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT old = (HFONT)SelectObject(hdc, font);

        char line[512];
        int y = 8;

        auto put = [&](const char *s)
        {
            TextOutA(hdc, 8, y, s, (int)strlen(s));
            y += 18;
        };

        put("RawIO Overlay (Recording)");

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
                      "Keys: W=%d A=%d S=%d D=%d  Shift=%d Ctrl=%d Alt=%d Space=%d",
                      gKeyDown['W'], gKeyDown['A'], gKeyDown['S'], gKeyDown['D'],
                      gKeyDown[VK_SHIFT], gKeyDown[VK_CONTROL], gKeyDown[VK_MENU], gKeyDown[VK_SPACE]);
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

    const int w = 300, h = 200;
    const int x = 10, y = 10;

    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP;

    gOverlayHwnd = CreateWindowExA(
        ex, kOverlayClassName, "RawIO Overlay",
        style,
        x, y, w, h,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!gOverlayHwnd)
    {
        std::fprintf(stderr, "CreateWindowExA overlay failed (%lu)\n", GetLastError());
        return false;
    }

    // Global opacity (0..255). Whole window becomes translucent.
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

// --------------------------- Raw Input Sink Window ---------------------------

static bool register_raw(HWND hwnd)
{
    RAWINPUTDEVICE rids[2]{};

    // Mouse
    rids[0].usUsagePage = 0x01; // Generic Desktop
    rids[0].usUsage = 0x02;     // Mouse
    rids[0].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
    rids[0].hwndTarget = hwnd;

    // Keyboard
    rids[1].usUsagePage = 0x01; // Generic Desktop
    rids[1].usUsage = 0x06;     // Keyboard
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

            // Raw relative movement
            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
            {
                LONG dx = m.lLastX;
                LONG dy = m.lLastY;

                if (dx != 0 || dy != 0)
                {
                    gLastDx = dx;
                    gLastDy = dy;

                    // Alt held? If yes, record absolute position instead of delta
                    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

                    if (!altDown)
                    {
                        write_event(EV_MOUSE_MOVE, (int32_t)dx, (int32_t)dy, 0);
                    }
                    else
                    {
                        POINT pt{};
                        if (GetCursorPos(&pt))
                            write_event(EV_MOUSE_POS, (int32_t)pt.x, (int32_t)pt.y, 0);
                    }

                    update_overlay_state_on_mouse();
                }
            }

            // Wheel
            if (m.usButtonFlags & RI_MOUSE_WHEEL)
            {
                SHORT wheelDelta = *reinterpret_cast<const SHORT *>(&m.usButtonData);
                gLastWheel = (int)wheelDelta;
                write_event(EV_MOUSE_WHEEL, (int32_t)wheelDelta, 0, 0);
                update_overlay_state_on_mouse();
            }

            // Mouse buttons
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
                break; // filter bogus VK

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

            // ESC ends recording session quickly (and is still logged)
            if (!isBreak && vk == VK_ESCAPE)
            {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
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

    // Hidden; just exists to receive WM_INPUT with INPUTSINK
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

// --------------------------- Playback SendInput helpers ---------------------------

// Relative move
static void send_mouse_move_rel(int dx, int dy)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

// Absolute move (screen coords)
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

// --------------------------- Record / Play ---------------------------

static bool record_to_file(const char *path)
{
    // Reset overlay state
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

    countdown_5s("Recording will begin");

    gRecording = true;
    overlay_show(true);

    std::puts("Recording... (ESC to stop)");

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gRecording = false;
    overlay_show(false);

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

    countdown_5s("Playback will begin");
    std::puts("Playing...");

    gPlaying = true;

    // Improve timer granularity (optional)
    TIMECAPS tc{};
    bool setPeriod = (timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR);
    if (setPeriod)
        timeBeginPeriod(tc.wPeriodMin);

    uint64_t prev_t = 0;
    for (size_t i = 0; i < events.size(); ++i)
    {
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
            break;
        case EV_MOUSE_POS:
            send_mouse_move_abs(e.a, e.b);
            break;
        case EV_MOUSE_WHEEL:
            send_mouse_wheel(e.a);
            break;
        case EV_MOUSE_BUTTON:
            send_mouse_button(e.a, e.b != 0);
            break;
        case EV_KEY_DOWN:
            send_key(true, (UINT)e.a);
            break;
        case EV_KEY_UP:
            send_key(false, (UINT)e.a);
            break;
        default:
            break;
        }
    }

    if (setPeriod)
        timeEndPeriod(tc.wPeriodMin);

    gPlaying = false;
    std::puts("Done.");
    return true;
}

// --------------------------- main ---------------------------

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::printf(
            "Usage:\n"
            "  %s record <file.rmac>\n"
            "  %s play   <file.rmac>\n",
            argv[0], argv[0]);
        return 0;
    }

    if (std::string(argv[1]) == "record")
        return record_to_file(argv[2]) ? 0 : 1;

    if (std::string(argv[1]) == "play")
        return play_file(argv[2]) ? 0 : 1;

    std::fprintf(stderr, "Unknown command: %s (use 'record' or 'play')\n", argv[1]);
    return 1;
}
