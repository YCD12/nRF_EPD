CC = gcc
# Windows GUI 模拟器（host-only）。为了不依赖 MCU/外置 Flash，复用 tests/stubs
# 中的 host 资源桩：GUI 解码/待办/日历逻辑仍然是生产代码，只替换数据来源。
CFLAGS = -Wall -O2 -IGUI -IEPD/hal -IEPD/driver -IEPD/board -IEPD/storage -IEPD/app -IEPD/service \
         -Itests/stubs -Itools/test/host/shim -Itools -Itools/test \
         -D__HEAP_SIZE=32768 -DGUI_USE_EXTERNAL_FONTS=0 -DGUI_INCLUDE_INTERNAL_FONTS=1
LDFLAGS = -lgdi32 -mwindows

SRCS = GUI/Adafruit_GFX.c GUI/u8g2_font.c GUI/fonts.c GUI/font_geometry.c GUI/GUI.c \
       GUI/calendar_annotations.c GUI/Lunar.c GUI/todo_items.c \
       tests/stubs/todo_resources_stub.c tests/stubs/calendar_resources_stub.c \
       tests/stubs/ui_resources_stub.c EPD/app/epd_refresh_policy.c \
       tools/test/host_stubs.c emulator.c
OBJS = $(SRCS:.c=.o)
TARGET = emulator.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
