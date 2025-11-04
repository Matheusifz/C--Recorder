#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

static const char *kClassName = "RawMouseSinkClass";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INPUT:
    {
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (!size)
            break;

        std::vector<BYTE> buffer(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            break;

        RAWINPUT *raw = reinterpret_cast<RAWINPUT *>(buffer.data());
        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE &m = raw->data.mouse;

            // If absolute, many gaming mice still report relative; handle both.
            LONG dx = m.lLastX;
            LONG dy = m.lLastY;

            // Buttons/wheel (optional)
            bool wheel = (m.usButtonFlags & RI_MOUSE_WHEEL) != 0;
            SHORT wheelDelta = 0;
            if (wheel)
            {
                wheelDelta = *reinterpret_cast<const SHORT *>(&m.usButtonData);
            }

            // Movement mode info (optional)
            bool isRelative = (m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;

            // Print raw deltas
            std::printf("dx=%ld dy=%ld%s%s\n",
                        dx, dy,
                        wheel ? " wheel=" : "",
                        wheel ? std::to_string(wheelDelta).c_str() : "");

            // If you need per-device info, raw->header.hDevice is the handle.
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

int main()
{
    // Register a tiny window class
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kClassName;

    if (!RegisterClassA(&wc))
    {
        std::fprintf(stderr, "RegisterClassA failed (%lu)\n", GetLastError());
        return 1;
    }

    // Create a hidden window to receive messages
    HWND hwnd = CreateWindowExA(0, kClassName, "RawMouseSink",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                                nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hwnd)
    {
        std::fprintf(stderr, "CreateWindowExA failed (%lu)\n", GetLastError());
        return 1;
    }

    // Register for raw mouse input
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;                         // Generic desktop controls
    rid.usUsage = 0x02;                             // Mouse
    rid.dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY; // receive in background, suppress WM_MOUSE*
    rid.hwndTarget = hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
    {
        std::fprintf(stderr, "RegisterRawInputDevices failed (%lu)\n", GetLastError());
        return 1;
    }

    // Hide the window (we only need its message pump)
    ShowWindow(hwnd, SW_HIDE);

    std::puts("Capturing RAW mouse deltas (Ctrl+C to quit)...");
    // Standard message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
