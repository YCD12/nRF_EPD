//  创建Linux code is a simple SDL2 application that emulates the display of an e-paper device.

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "GUI.h"

#define BITMAP_WIDTH 400
#define BITMAP_HEIGHT 300
#define WINDOW_WIDTH 450
#define WINDOW_HEIGHT 380

// Global variables
SDL_Window* g_window = NULL;
SDL_Renderer* g_renderer = NULL;
SDL_Texture* g_texture = NULL;
TTF_Font* g_font = NULL;
TTF_Font* g_help_font = NULL;

display_mode_t g_display_mode = MODE_CALENDAR;  // Default to calendar mode
int g_bwr_mode = 1;                             // Default to BWR mode
uint8_t g_week_start = 0;                       // Default week start (0=Sunday, 1=Monday, etc.)
time_t g_display_time;
struct tm g_tm_time;
uint32_t g_last_update = 0;

// Implementation of the buffer_callback function
void DrawBitmap(void* user_data, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!g_renderer || !g_texture) return;

    // Calculate position to center the entire bitmap in the window
    int drawX = (WINDOW_WIDTH - BITMAP_WIDTH) / 2;
    int drawY = (WINDOW_HEIGHT - BITMAP_HEIGHT) / 2;

    // Create pixel data for the texture
    static uint32_t* pixels = NULL;
    static int pixels_allocated = 0;
    
    int needed_pixels = w * h;
    if (pixels_allocated < needed_pixels) {
        pixels = realloc(pixels, needed_pixels * sizeof(uint32_t));
        pixels_allocated = needed_pixels;
    }
    
    if (!pixels) return;

    int ePaperBytesPerRow = (w + 7) / 8;
    
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int bytePos = row * ePaperBytesPerRow + col / 8;
            int bitPos = 7 - (col % 8);

            int blackBit = !((black[bytePos] >> bitPos) & 0x01);
            int colorBit = color ? !((color[bytePos] >> bitPos) & 0x01) : 0;

            // Determine pixel color: white, black, or red
            uint32_t pixelColor;
            if (colorBit) {
                pixelColor = 0xFF0000FF; // Red (ARGB format)
            } else if (blackBit) {
                pixelColor = 0xFF000000; // Black
            } else {
                pixelColor = 0xFFFFFFFF; // White
            }
            
            pixels[row * w + col] = pixelColor;
        }
    }

    // Update texture with pixel data
    SDL_Rect srcRect = {0, 0, w, h};
    SDL_Rect dstRect = {drawX + x, drawY + y, w, h};
    
    SDL_Texture* tempTexture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, 
                                                 SDL_TEXTUREACCESS_STREAMING, w, h);
    if (tempTexture) {
        void* texturePixels;
        int pitch;
        SDL_LockTexture(tempTexture, NULL, &texturePixels, &pitch);
        memcpy(texturePixels, pixels, w * h * sizeof(uint32_t));
        SDL_UnlockTexture(tempTexture);
        
        SDL_RenderCopy(g_renderer, tempTexture, &srcRect, &dstRect);
        SDL_DestroyTexture(tempTexture);
    }
}

void DrawText(const char* text, int x, int y, TTF_Font* font, SDL_Color color) {
    if (!font || !g_renderer) return;
    
    SDL_Surface* textSurface = TTF_RenderUTF8_Solid(font, text, color);
    if (textSurface) {
        SDL_Texture* textTexture = SDL_CreateTextureFromSurface(g_renderer, textSurface);
        if (textTexture) {
            SDL_Rect dstRect = {x, y, textSurface->w, textSurface->h};
            SDL_RenderCopy(g_renderer, textTexture, NULL, &dstRect);
            SDL_DestroyTexture(textTexture);
        }
        SDL_FreeSurface(textSurface);
    }
}

void RenderFrame() {
    // Clear screen with light gray background
    SDL_SetRenderDrawColor(g_renderer, 240, 240, 240, 255);
    SDL_RenderClear(g_renderer);

    // Calculate bitmap position
    int drawX = (WINDOW_WIDTH - BITMAP_WIDTH) / 2;
    int drawY = (WINDOW_HEIGHT - BITMAP_HEIGHT) / 2;

    // Draw border around bitmap area
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 255, 255);
    SDL_Rect borderRect = {drawX - 1, drawY - 1, BITMAP_WIDTH + 2, BITMAP_HEIGHT + 2};
    SDL_RenderDrawRect(g_renderer, &borderRect);

    // Display current mode text
    const char* modeText = (g_display_mode == MODE_CLOCK) ? "时钟模式" : "日历模式";
    SDL_Color darkGray = {50, 50, 50, 255};
    
    if (g_font) {
        int textW, textH;
        TTF_SizeUTF8(g_font, modeText, &textW, &textH);
        int modeTextX = drawX + (BITMAP_WIDTH - textW) / 2;
        int modeTextY = drawY - 25;
        DrawText(modeText, modeTextX, modeTextY, g_font, darkGray);
    }

    // Render GUI content
    gui_data_t data = {
        .mode = g_display_mode,
        .color = g_bwr_mode ? 2 : 1,
        .width = BITMAP_WIDTH,
        .height = BITMAP_HEIGHT,
        .timestamp = g_display_time,
        .week_start = g_week_start,
        .temperature = 25,
        .voltage = 3.2f,
        .ssid = "NRF_EPD_84AC",
    };

    // Call DrawGUI to render the interface
    DrawGUI(&data, DrawBitmap, NULL);

    // Draw help text below bitmap
    const char* helpText = "空格-切换模式 | R-切换颜色 | W-星期起点 | 方向键-调整日期/月份";
    SDL_Color gray = {80, 80, 80, 255};
    
    if (g_help_font) {
        int textW, textH;
        TTF_SizeUTF8(g_help_font, helpText, &textW, &textH);
        int helpTextX = drawX + (BITMAP_WIDTH - textW) / 2;
        int helpTextY = drawY + BITMAP_HEIGHT + 10;
        DrawText(helpText, helpTextX, helpTextY, g_help_font, gray);
    }

    SDL_RenderPresent(g_renderer);
}

int InitSDL() {
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        printf("SDL initialization failed: %s\n", SDL_GetError());
        return 0;
    }

    // Initialize SDL_ttf
    if (TTF_Init() == -1) {
        printf("TTF initialization failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 0;
    }

    // Create window
    g_window = SDL_CreateWindow("nRF52 GUI 模拟器",
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                WINDOW_WIDTH, WINDOW_HEIGHT,
                                SDL_WINDOW_SHOWN);
    if (!g_window) {
        printf("Window creation failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 0;
    }

    // Create renderer
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        printf("Renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        TTF_Quit();
        SDL_Quit();
        return 0;
    }

    // Try to load system fonts (fallback to built-in if not available)
    const char* font_paths[] = {
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",  // Chinese font
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/System/Library/Fonts/Arial.ttf",  // macOS
        NULL
    };

    for (int i = 0; font_paths[i] != NULL; i++) {
        g_font = TTF_OpenFont(font_paths[i], 16);
        if (g_font) {
            g_help_font = TTF_OpenFont(font_paths[i], 14);
            break;
        }
    }

    if (!g_font) {
        printf("Warning: Could not load any font. Text will not be displayed.\n");
        printf("Install fonts with: sudo apt-get install fonts-wqy-zenhei\n");
    }

    return 1;
}

void CleanupSDL() {
    if (g_help_font) TTF_CloseFont(g_help_font);
    if (g_font) TTF_CloseFont(g_font);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    TTF_Quit();
    SDL_Quit();
}

void HandleKeyPress(SDL_Keycode key) {
    switch (key) {
        case SDLK_SPACE:
            // Toggle display mode
            g_display_mode = (g_display_mode == MODE_CLOCK) ? MODE_CALENDAR : MODE_CLOCK;
            break;
            
        case SDLK_r:
            // Toggle BWR mode
            g_bwr_mode = !g_bwr_mode;
            break;
            
        case SDLK_w:
            // Increase week start
            g_week_start++;
            if (g_week_start > 6) g_week_start = 0;
            break;
            
        case SDLK_UP:
        case SDLK_DOWN:
        case SDLK_LEFT:
        case SDLK_RIGHT: {
            // Get current time structure
            g_tm_time = *localtime(&g_display_time);
            
            if (key == SDLK_UP) {
                g_tm_time.tm_mon++;
                if (g_tm_time.tm_mon > 11) {
                    g_tm_time.tm_mon = 0;
                    g_tm_time.tm_year++;
                }
            } else if (key == SDLK_DOWN) {
                g_tm_time.tm_mon--;
                if (g_tm_time.tm_mon < 0) {
                    g_tm_time.tm_mon = 11;
                    g_tm_time.tm_year--;
                }
            } else if (key == SDLK_RIGHT) {
                g_tm_time.tm_mday++;
            } else if (key == SDLK_LEFT) {
                g_tm_time.tm_mday--;
            }
            
            // Convert back to time_t
            g_display_time = mktime(&g_tm_time);
            break;
        }
    }
}

int main(int argc, char* argv[]) {
    printf("Starting nRF52 GUI Emulator (Linux version)\n");
    
    if (!InitSDL()) {
        return 1;
    }

    // Initialize display time
    g_display_time = time(NULL) + 8 * 3600;  // GMT+8
    g_last_update = SDL_GetTicks();

    int running = 1;
    SDL_Event event;

    printf("Emulator controls:\n");
    printf("  SPACE - Toggle mode (Clock/Calendar)\n");
    printf("  R     - Toggle color mode (BW/BWR)\n");
    printf("  W     - Change week start day\n");
    printf("  Arrow keys - Adjust date/month\n");
    printf("  ESC/Close - Exit\n\n");

    while (running) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                    
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        running = 0;
                    } else {
                        HandleKeyPress(event.key.keysym.sym);
                    }
                    break;
            }
        }

        // Update clock mode periodically
        uint32_t currentTime = SDL_GetTicks();
        if (g_display_mode == MODE_CLOCK && currentTime - g_last_update > 1000) {
            g_display_time = time(NULL) + 8 * 3600;
            g_last_update = currentTime;
        }

        // Render frame
        RenderFrame();

        // Small delay to prevent excessive CPU usage
        SDL_Delay(16); // ~60 FPS
    }

    CleanupSDL();
    printf("nRF52 GUI Emulator ended\n");
    return 0;
}
