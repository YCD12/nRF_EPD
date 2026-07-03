// GUI emulator for Windows — 800×480 BWR，与真机同源 GUI + res_hal_host
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <windows.h>

#include "GUI.h"
#include "Lunar.h"
#include "res_hal_host.h"

#define EPD_WIDTH 800
#define EPD_HEIGHT 480
#define DISPLAY_SCALE 1
#define WINDOW_WIDTH (EPD_WIDTH + 50)
#define WINDOW_HEIGHT (EPD_HEIGHT + 90)

// Global variables
HINSTANCE g_hInstance;
HWND g_hwnd;
HDC g_paintHDC = NULL;
display_mode_t g_display_mode = MODE_CALENDAR;
BOOL g_bwr_mode = TRUE;
uint8_t g_week_start = 0;
time_t g_display_time;
struct tm g_tm_time;
wchar_t g_status_text[128] = L"";
static char g_res_bin_path[MAX_PATH] = "tools\\epd_resources.bin";

static void emulator_reload_ui(void) {
    uint8_t color = g_bwr_mode ? 2U : 1U;
    ret_code_t ret = GUI_ReloadUICache(EPD_WIDTH, EPD_HEIGHT, color);
    if (ret == NRF_SUCCESS) {
        g_status_text[0] = L'\0';
    } else {
        swprintf(g_status_text, 128, L"UI bundle error: 0x%08lX", (unsigned long)ret);
    }
}

static int emulator_init_resources(void) {
    if (res_hal_host_init(g_res_bin_path) != 0) {
        swprintf(g_status_text, 128, L"res_hal init failed: %hs", g_res_bin_path);
        return -1;
    }
    lunar_db_reset();
    emulator_reload_ui();
    return 0;
}

// Implementation of the buffer_callback function
void DrawBitmap(void* user_data, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    HDC hdc = g_paintHDC;
    int scale = DISPLAY_SCALE;
    RECT clientRect;
    int drawX;
    int drawY;

    (void)user_data;
    if (!hdc) return;

    GetClientRect(g_hwnd, &clientRect);
    drawX = (clientRect.right - EPD_WIDTH * scale) / 2;
    drawY = (clientRect.bottom - EPD_HEIGHT * scale) / 2;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(BITMAPINFO));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 4;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biClrUsed = 16;

    for (int i = 0; i < 16; i++) {
        bmi.bmiColors[i].rgbBlue = 255;
        bmi.bmiColors[i].rgbGreen = 255;
        bmi.bmiColors[i].rgbRed = 255;
        bmi.bmiColors[i].rgbReserved = 0;
    }

    bmi.bmiColors[1].rgbBlue = 0;
    bmi.bmiColors[1].rgbGreen = 0;
    bmi.bmiColors[1].rgbRed = 0;

    bmi.bmiColors[2].rgbBlue = 0;
    bmi.bmiColors[2].rgbGreen = 0;
    bmi.bmiColors[2].rgbRed = 255;

    int pixelsPerByte = 2;
    int bytesPerRow = ((w + pixelsPerByte - 1) / pixelsPerByte);
    bytesPerRow = ((bytesPerRow + 3) / 4) * 4;
    int totalSize = bytesPerRow * h;

    uint8_t* bitmap4bit = (uint8_t*)malloc(totalSize);
    if (!bitmap4bit) {
        return;
    }
    memset(bitmap4bit, 0, totalSize);

    int ePaperBytesPerRow = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int bytePos = row * ePaperBytesPerRow + col / 8;
            int bitPos = 7 - (col % 8);

            int blackBit = !((black[bytePos] >> bitPos) & 0x01);
            int colorBit = color ? !((color[bytePos] >> bitPos) & 0x01) : 0;
            uint8_t pixelValue = colorBit ? 2 : (blackBit ? 1 : 0);

            int bitmap4bitBytePos = row * bytesPerRow + col / pixelsPerByte;
            int isHighNibble = (col % pixelsPerByte) == 0;

            if (isHighNibble) {
                bitmap4bit[bitmap4bitBytePos] &= 0x0F;
                bitmap4bit[bitmap4bitBytePos] |= (pixelValue << 4);
            } else {
                bitmap4bit[bitmap4bitBytePos] &= 0xF0;
                bitmap4bit[bitmap4bitBytePos] |= pixelValue;
            }
        }
    }

    StretchDIBits(hdc, drawX + x * scale, drawY + y * scale, w * scale, h * scale, 0, 0, w, h, bitmap4bit, &bmi,
                  DIB_RGB_COLORS, SRCCOPY);

    free(bitmap4bit);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_display_time = time(NULL);
            SetTimer(hwnd, 1, 1000, NULL);
            return 0;

        case WM_TIMER:
            if (g_display_mode == MODE_CLOCK) {
                g_display_time = time(NULL);
                if (g_display_time % 60 == 0) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT clientRect;
            int scale = DISPLAY_SCALE;
            int drawX;
            int drawY;
            gui_data_t data;

            g_paintHDC = hdc;

            GetClientRect(hwnd, &clientRect);

            HBRUSH bgBrush = CreateSolidBrush(RGB(240, 240, 240));
            FillRect(hdc, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            drawX = (clientRect.right - EPD_WIDTH * scale) / 2;
            drawY = (clientRect.bottom - EPD_HEIGHT * scale) / 2;

            HPEN borderPen = CreatePen(PS_DOT, 1, RGB(0, 0, 255));
            HPEN oldPen = SelectObject(hdc, borderPen);
            HBRUSH oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            Rectangle(hdc, drawX - 1, drawY - 1, drawX + EPD_WIDTH * scale + 1, drawY + EPD_HEIGHT * scale + 1);

            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(borderPen);

            const wchar_t* modeText = (g_display_mode == MODE_CLOCK) ? L"时钟模式" : L"日历模式";
            int modeTextY = drawY - 20;
            SetTextColor(hdc, RGB(50, 50, 50));
            SetBkMode(hdc, TRANSPARENT);

            HFONT modeFont = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
            HFONT oldFont = SelectObject(hdc, modeFont);

            SIZE modeTextSize;
            GetTextExtentPoint32W(hdc, modeText, (int)wcslen(modeText), &modeTextSize);
            int modeCenteredX = drawX + (EPD_WIDTH - modeTextSize.cx) / 2;

            TextOutW(hdc, modeCenteredX, modeTextY, modeText, (int)wcslen(modeText));

            const wchar_t helpText[] = L"空格-模式 | R-颜色 | W-星期起点 | 方向键-调日期 | 资源: tools\\epd_resources.bin";
            int helpTextY = drawY + EPD_HEIGHT * scale + 5;
            SetTextColor(hdc, RGB(80, 80, 80));

            HFONT helpFont =
                CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
            SelectObject(hdc, modeFont);
            DeleteObject(modeFont);
            SelectObject(hdc, helpFont);

            SIZE textSize;
            GetTextExtentPoint32W(hdc, helpText, (int)wcslen(helpText), &textSize);
            int centeredX = drawX + (EPD_WIDTH - textSize.cx) / 2;

            TextOutW(hdc, centeredX, helpTextY, helpText, (int)wcslen(helpText));

            if (g_status_text[0] != L'\0') {
                SetTextColor(hdc, RGB(180, 0, 0));
                TextOutW(hdc, drawX, helpTextY + 18, g_status_text, (int)wcslen(g_status_text));
            }

            SelectObject(hdc, oldFont);
            DeleteObject(helpFont);

            data.mode = g_display_mode;
            data.color = g_bwr_mode ? 2 : 1;
            data.width = EPD_WIDTH;
            data.height = EPD_HEIGHT;
            data.timestamp = (uint32_t)g_display_time;
            data.week_start = g_week_start;
            data.temperature = 25;
            data.voltage_mv = 3300;
            strncpy(data.ssid, "NRF_EPD_EMU", sizeof(data.ssid) - 1);
            data.ssid[sizeof(data.ssid) - 1] = '\0';

            DrawGUI(&data, DrawBitmap, NULL);

            g_paintHDC = NULL;

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_SPACE) {
                g_display_mode = (g_display_mode == MODE_CLOCK) ? MODE_CALENDAR : MODE_CLOCK;
                InvalidateRect(hwnd, NULL, TRUE);
            } else if (wParam == 'R') {
                g_bwr_mode = !g_bwr_mode;
                emulator_reload_ui();
                InvalidateRect(hwnd, NULL, TRUE);
            } else if (wParam == 'W') {
                g_week_start++;
                if (g_week_start > 6) g_week_start = 0;
                InvalidateRect(hwnd, NULL, TRUE);
            } else if (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT) {
                g_tm_time = *localtime(&g_display_time);

                if (wParam == VK_UP) {
                    g_tm_time.tm_mon++;
                    if (g_tm_time.tm_mon > 11) {
                        g_tm_time.tm_mon = 0;
                        g_tm_time.tm_year++;
                    }
                } else if (wParam == VK_DOWN) {
                    g_tm_time.tm_mon--;
                    if (g_tm_time.tm_mon < 0) {
                        g_tm_time.tm_mon = 11;
                        g_tm_time.tm_year--;
                    }
                } else if (wParam == VK_RIGHT) {
                    g_tm_time.tm_mday++;
                } else if (wParam == VK_LEFT) {
                    g_tm_time.tm_mday--;
                }

                g_display_time = mktime(&g_tm_time);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            res_hal_host_shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    g_hInstance = hInstance;

    if (lpCmdLine != NULL && lpCmdLine[0] != '\0') {
        strncpy(g_res_bin_path, lpCmdLine, sizeof(g_res_bin_path) - 1);
        g_res_bin_path[sizeof(g_res_bin_path) - 1] = '\0';
    }

    if (emulator_init_resources() != 0) {
        MessageBoxW(NULL, g_status_text, L"资源加载失败", MB_ICONWARNING | MB_OK);
    }

    WNDCLASSW wc = {0};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"Emurator";

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Window Registration Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    g_hwnd = CreateWindowW(L"Emurator", L"EPD 模拟器 800x480", WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH, WINDOW_HEIGHT, NULL, NULL, hInstance, NULL);

    if (!g_hwnd) {
        MessageBoxW(NULL, L"Window Creation Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
