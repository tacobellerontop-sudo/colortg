// assumption: Windows 10/11 x64, Desktop resolution auto-detected, 32bpp GDI screen capture
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <commctrl.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef CTL_CODE
#define CTL_CODE( DeviceType, Function, Method, Access ) ( \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
)
#endif
#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif
#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif
#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif

#define IOCTL_MOUSE_CLICK CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// UI IDs
#define IDC_COMBO_COLOR   101
#define IDC_EDIT_WIDTH    102
#define IDC_EDIT_HEIGHT   103
#define IDC_EDIT_OFFSETX  104
#define IDC_EDIT_OFFSETY  105
#define IDC_EDIT_COOLDOWN 106
#define IDC_COMBO_KEY     107
#define IDC_BTN_TOGGLE    108

enum EnemyColor {
    COLOR_PURPLE = 0,
    COLOR_YELLOW = 1,
    COLOR_RED = 2
};

struct Config {
    std::atomic<int> colorMode{ COLOR_PURPLE };
    std::atomic<int> fovWidth{ 8 };
    std::atomic<int> fovHeight{ 8 };
    std::atomic<int> offsetX{ 0 };
    std::atomic<int> offsetY{ 0 };
    std::atomic<int> cooldownMs{ 40 };
    std::atomic<int> hotkey{ VK_XBUTTON2 };
    std::atomic<bool> enabled{ true };
    std::atomic<bool> running{ true };
} g_Cfg;

class DriverBridge {
    HANDLE handle = INVALID_HANDLE_VALUE;
public:
    DriverBridge() {
        handle = CreateFileW(
            L"\\\\.\\MouClicker",
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }

    ~DriverBridge() {
        if (IsValid()) CloseHandle(handle);
    }

    bool IsValid() const { return handle != INVALID_HANDLE_VALUE; }

    void Click() const {
        if (!IsValid()) return;
        DWORD bytes = 0;
        DeviceIoControl(handle, IOCTL_MOUSE_CLICK, nullptr, 0, nullptr, 0, &bytes, nullptr);
    }
};

static inline bool MatchPixel(BYTE r, BYTE g, BYTE b, int mode) {
    switch (mode) {
    case COLOR_PURPLE: // Magenta / Purple outline
        return (r >= 140 && b >= 160 && g <= 120 && abs((int)r - (int)b) <= 75);
    case COLOR_YELLOW: // Yellow outline (Deuteranopia / Protanopia)
        return (r >= 180 && g >= 180 && b <= 110 && abs((int)r - (int)g) <= 60);
    case COLOR_RED:    // Red outline
        return (r >= 180 && g <= 90 && b <= 90 && (r - g) >= 80 && (r - b) >= 80);
    default:
        return false;
    }
}

void TriggerWorker() {
    DriverBridge driver;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int centerX = screenW / 2;
    int centerY = screenH / 2;

    HDC hScreen = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hScreen);

    while (g_Cfg.running) {
        if (!g_Cfg.enabled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        int key = g_Cfg.hotkey.load();
        bool active = (key == 0) || ((GetAsyncKeyState(key) & 0x8000) != 0);

        if (active) {
            int w = g_Cfg.fovWidth.load();
            int h = g_Cfg.fovHeight.load();
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (w > 64) w = 64;
            if (h > 64) h = 64;

            int targetX = centerX + g_Cfg.offsetX.load() - (w / 2);
            int targetY = centerY + g_Cfg.offsetY.load() - (h / 2);

            BITMAPINFO bmi = { 0 };
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h; // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            HBITMAP hBitmap = CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (hBitmap && bits) {
                HGDIOBJ hOld = SelectObject(hMemDC, hBitmap);
                BitBlt(hMemDC, 0, 0, w, h, hScreen, targetX, targetY, SRCCOPY);

                int mode = g_Cfg.colorMode.load();
                bool detected = false;
                RGBQUAD* pixels = (RGBQUAD*)bits;
                int totalPixels = w * h;

                for (int i = 0; i < totalPixels; ++i) {
                    if (MatchPixel(pixels[i].rgbRed, pixels[i].rgbGreen, pixels[i].rgbBlue, mode)) {
                        detected = true;
                        break;
                    }
                }

                SelectObject(hMemDC, hOld);
                DeleteObject(hBitmap);

                if (detected) {
                    driver.Click();
                    std::this_thread::sleep_for(std::chrono::milliseconds(g_Cfg.cooldownMs.load()));
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreen);
}

// Win32 GUI controls
HWND hComboColor, hEditW, hEditH, hEditOffX, hEditOffY, hEditCd, hComboKey, hBtnToggle, hStatusLabel;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        CreateWindowW(L"STATIC", L"Outline Color:", WS_VISIBLE | WS_CHILD, 20, 20, 100, 20, hWnd, NULL, NULL, NULL);
        hComboColor = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST, 130, 18, 140, 120, hWnd, (HMENU)IDC_COMBO_COLOR, NULL, NULL);
        SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"Purple / Magenta");
        SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"Yellow");
        SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"Red");
        SendMessageW(hComboColor, CB_SETCURSEL, 0, 0);

        CreateWindowW(L"STATIC", L"FOV Width (px):", WS_VISIBLE | WS_CHILD, 20, 60, 100, 20, hWnd, NULL, NULL, NULL);
        hEditW = CreateWindowW(L"EDIT", L"8", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 130, 58, 60, 22, hWnd, (HMENU)IDC_EDIT_WIDTH, NULL, NULL);

        CreateWindowW(L"STATIC", L"FOV Height (px):", WS_VISIBLE | WS_CHILD, 20, 95, 100, 20, hWnd, NULL, NULL, NULL);
        hEditH = CreateWindowW(L"EDIT", L"8", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 130, 93, 60, 22, hWnd, (HMENU)IDC_EDIT_HEIGHT, NULL, NULL);

        CreateWindowW(L"STATIC", L"X Offset (px):", WS_VISIBLE | WS_CHILD, 20, 130, 100, 20, hWnd, NULL, NULL, NULL);
        hEditOffX = CreateWindowW(L"EDIT", L"0", WS_VISIBLE | WS_CHILD | WS_BORDER, 130, 128, 60, 22, hWnd, (HMENU)IDC_EDIT_OFFSETX, NULL, NULL);

        CreateWindowW(L"STATIC", L"Y Offset (px):", WS_VISIBLE | WS_CHILD, 20, 165, 100, 20, hWnd, NULL, NULL, NULL);
        hEditOffY = CreateWindowW(L"EDIT", L"0", WS_VISIBLE | WS_CHILD | WS_BORDER, 130, 163, 60, 22, hWnd, (HMENU)IDC_EDIT_OFFSETY, NULL, NULL);

        CreateWindowW(L"STATIC", L"Shot Delay (ms):", WS_VISIBLE | WS_CHILD, 20, 200, 100, 20, hWnd, NULL, NULL, NULL);
        hEditCd = CreateWindowW(L"EDIT", L"40", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 130, 198, 60, 22, hWnd, (HMENU)IDC_EDIT_COOLDOWN, NULL, NULL);

        CreateWindowW(L"STATIC", L"Hold Key:", WS_VISIBLE | WS_CHILD, 20, 235, 100, 20, hWnd, NULL, NULL, NULL);
        hComboKey = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST, 130, 233, 140, 120, hWnd, (HMENU)IDC_COMBO_KEY, NULL, NULL);
        SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"Mouse 5 (XButton2)");
        SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"Mouse 4 (XButton1)");
        SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"Left Alt (VK_MENU)");
        SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"Left Shift");
        SendMessageW(hComboKey, CB_ADDSTRING, 0, (LPARAM)L"Always Active");
        SendMessageW(hComboKey, CB_SETCURSEL, 0, 0);

        hBtnToggle = CreateWindowW(L"BUTTON", L"Enabled (Toggle)", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 20, 275, 250, 32, hWnd, (HMENU)IDC_BTN_TOGGLE, NULL, NULL);

        DriverBridge testDriver;
        std::wstring status = testDriver.IsValid() ? L"Driver: Connected (\\\\.\\MouClicker)" : L"Driver: Offline (needs start)";
        hStatusLabel = CreateWindowW(L"STATIC", status.c_str(), WS_VISIBLE | WS_CHILD, 20, 318, 250, 20, hWnd, NULL, NULL, NULL);

        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_COMBO_COLOR && HIWORD(wParam) == CBN_SELCHANGE) {
            g_Cfg.colorMode = (int)SendMessageW(hComboColor, CB_GETCURSEL, 0, 0);
        } else if (id == IDC_COMBO_KEY && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(hComboKey, CB_GETCURSEL, 0, 0);
            int keys[] = { VK_XBUTTON2, VK_XBUTTON1, VK_MENU, VK_LSHIFT, 0 };
            g_Cfg.hotkey = keys[sel];
        } else if (id == IDC_BTN_TOGGLE) {
            g_Cfg.enabled = !g_Cfg.enabled;
            SetWindowTextW(hBtnToggle, g_Cfg.enabled ? L"Enabled (Click to Pause)" : L"Paused (Click to Enable)");
        } else if (HIWORD(wParam) == EN_CHANGE) {
            wchar_t buf[32];
            GetWindowTextW(hEditW, buf, 32);
            if (buf[0]) g_Cfg.fovWidth = _wtoi(buf);
            GetWindowTextW(hEditH, buf, 32);
            if (buf[0]) g_Cfg.fovHeight = _wtoi(buf);
            GetWindowTextW(hEditOffX, buf, 32);
            if (buf[0]) g_Cfg.offsetX = _wtoi(buf);
            GetWindowTextW(hEditOffY, buf, 32);
            if (buf[0]) g_Cfg.offsetY = _wtoi(buf);
            GetWindowTextW(hEditCd, buf, 32);
            if (buf[0]) g_Cfg.cooldownMs = _wtoi(buf);
        }
        break;
    }
    case WM_DESTROY:
        g_Cfg.running = false;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ColorTriggerMenu";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"ColorTriggerMenu",
        L"Color Triggerbot | Bench",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        310, 385,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    std::thread worker(TriggerWorker);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (worker.joinable()) worker.join();
    return (int)msg.wParam;
}
