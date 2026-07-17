#include "gui_porting_hal.h"

// 外部硬件平台提供的喂狗函数前向声明
extern void app_feed_wdt(void);

void gui_porting_feed_wdt(void) {
    app_feed_wdt();
}
