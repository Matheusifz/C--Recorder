// macro_recplay.cpp
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

static const char *kClassName = "RawIO_Sink_Window";

enum EventType : uint32_t
{
    EV_MOUSE_MOVE = 0,
    EV_MOUSE_WHEEL = 1,
    EV_KEY_DOWN = 2,
    EV_KEY_UP = 3,
    EV_MOUSE_BUTTON = 4, // NEW: mouse button down/up
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

static LARGE_INTEGER gFreq{};
static LARGE_INTEGER gT0{};
static FILE *gOut = nullptr;
static bool gRecording = false;
static bool gPlaying = false;
static HWND gHwnd = nullptr;

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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
                    write_event(EV_MOUSE_MOVE, (int32_t)dx, (int32_t)dy, 0);
                }
            }

            // Wheel
            if (m.usButtonFlags & RI_MOUSE_WHEEL)
            {
                SHORT wheelDelta = *reinterpret_cast<const SHORT *>(&m.usButtonData);
                write_event(EV_MOUSE_WHEEL, (int32_t)wheelDelta, 0, 0);
            }

            // NEW: mouse button presses / releases
            auto log_button = [](int button, bool down)
            {
                // a = button id, b = 1 for down, 0 for up
                write_event(EV_MOUSE_BUTTON, (int32_t)button, down ? 1 : 0, 0);
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
            }
            else
            {
                write_event(EV_KEY_DOWN, (int32_t)vk, 0, 0);
            }

            // ESC ends recording session quickly (and is still logged)
            if (!isBreak && vk == VK_ESCAPE)
            {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

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

static bool create_sink_window()
{
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kClassName;
    if (!RegisterClassA(&wc))
    {
        std::fprintf(stderr, "RegisterClassA failed (%lu)\n", GetLastError());
        return false;
    }
    gHwnd = CreateWindowExA(0, kClassName, "RawIO_Sink",
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!gHwnd)
    {
        std::fprintf(stderr, "CreateWindowExA failed (%lu)\n", GetLastError());
        return false;
    }
    ShowWindow(gHwnd, SW_HIDE);
    if (!register_raw(gHwnd))
    {
        std::fprintf(stderr, "RegisterRawInputDevices failed (%lu)\n", GetLastError());
        return false;
    }
    return true;
}

static uint64_t read_exact(FILE *f, void *buf, uint64_t sz)
{
    return (uint64_t)fread(buf, 1, (size_t)sz, f);
}

static void send_mouse_move(int dx, int dy)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE; // relative move
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

static void send_key(bool down, UINT vk)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

static void send_mouse_button(int button, bool down)
{
    INPUT in{};
    in.type = INPUT_MOUSE;

    switch (button)
    {
    case 1: // left
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 2: // right
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case 3: // middle
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 4: // X1
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON1;
        break;
    case 5: // X2
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON2;
        break;
    default:
        return; // unknown button, ignore
    }

    SendInput(1, &in, sizeof(INPUT));
}

static bool record_to_file(const char *path)
{
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

    QueryPerformanceFrequency(&gFreq);
    QueryPerformanceCounter(&gT0);

    // 5s countdown before starting
    countdown_5s("Recording will begin");

    gRecording = true;

    std::puts("Recording... (ESC to stop)");
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gRecording = false;
    if (gOut)
    {
        std::fclose(gOut);
        gOut = nullptr;
    }
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

    // Read all events
    std::vector<Event> events;
    Event ev{};
    while (read_exact(in, &ev, sizeof(ev)) == sizeof(ev))
    {
        events.push_back(ev);
    }
    std::fclose(in);

    // 5s countdown before playback
    countdown_5s("Playback will begin");
    std::puts("Playing...");

    gPlaying = true;

    // Improve timer granularity (optional)
    TIMECAPS tc;
    bool setPeriod = (timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR);
    if (setPeriod)
        timeBeginPeriod(tc.wPeriodMin);

    uint64_t prev_t = 0;
    for (size_t i = 0; i < events.size(); ++i)
    {
        const Event &e = events[i];
        if (e.t_us > prev_t)
        {
            const uint64_t dt = e.t_us - prev_t;
            std::this_thread::sleep_for(std::chrono::microseconds(dt));
        }
        prev_t = e.t_us;

        switch (e.type)
        {
        case EV_MOUSE_MOVE:
            send_mouse_move(e.a, e.b);
            break;
        case EV_MOUSE_WHEEL:
            send_mouse_wheel(e.a);
            break;
        case EV_MOUSE_BUTTON:
            // a = button id, b = 1 (down) / 0 (up)
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

    // NOTE: requires C++11 for <thread>/<chrono> anyway
    if (std::string(argv[1]) == "record")
    {
        return record_to_file(argv[2]) ? 0 : 1;
    }
    else if (std::string(argv[1]) == "play")
    {
        return play_file(argv[2]) ? 0 : 1;
    }
    else
    {
        std::fprintf(stderr, "Unknown command: %s (use 'record' or 'play')\n", argv[1]);
        return 1;
    }
}
