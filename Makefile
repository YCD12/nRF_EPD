CC = gcc
CFLAGS = -Wall -IGUI -Itools/test/host/shim -Itools -Itools/test -IEPD -D__HEAP_SIZE=32768
LDFLAGS = -lgdi32 -mwindows

SRCS = GUI/Adafruit_GFX.c GUI/u8g2_font.c GUI/fonts.c GUI/GUI.c GUI/Lunar.c \
       GUI/gui_porting_hal.c \
       tools/res_hal_host.c tools/test/host_stubs.c emulator.c
OBJS = $(SRCS:.c=.o)
TARGET = emulator.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
