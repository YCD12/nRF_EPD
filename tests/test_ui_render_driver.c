/* Reuse font I/O, page capture and partial-refresh regression helpers. */
#define main legacy_gui_regressions
#include "test_gui_render.c"
#undef main
#include <stdlib.h>
void ui_resources_stub_load(const uint8_t *bytes, size_t n);
static size_t read_file(const char *path, uint8_t *bytes, size_t capacity) {
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t n = fread(bytes, 1, capacity, file);
    assert(feof(file));
    fclose(file);
    return n;
}
int main(int argc, char **argv) {
    assert(argc == 12);
    uint8_t bytes[8192];
    size_t n = read_file(argv[1], bytes, sizeof(bytes));
    ui_resources_stub_load(bytes, n);
    assert(ui_resources_init());
    n = read_file(argv[2], bytes, sizeof(bytes));
    todo_resources_stub_load(bytes, n);
    todo_items_invalidate();
    assert(todo_items_init());
    todo_font_size = todo_items_font_size();
    todo_font_key = todo_items_font_key();
    const char *names[] = {"font0.bin", "font1.bin", "font2.bin", "font3.bin"};
    for (unsigned i = 0; i < 4; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", argv[4], names[i]);
        FILE *file = fopen(path, "rb");
        if (!file) {
            assert(i == 3);
            break;
        }
        n = fread(fonts[i], 1, sizeof(fonts[i]), file);
        assert(feof(file));
        fclose(file);
        sources[i] = (u8g2_font_source_t){NULL, n, read_font, (const void *)(size_t)i, 1};
        gui_font_metrics_t *m = i == 3 ? &extended_todo_metrics : &runtime_metrics[i];
        memcpy(m, fonts[i] + 9, 8);
        m->advance = m->max_char_width + 1;
        if (i == 3)
            extended_todo_fixture = true;
    }
    use_runtime_metrics = true;
    n = read_file(argv[3], bytes, sizeof(bytes));
    assert(calendar_annotations_load(bytes, n));
    gui_data_t data = {.mode = atoi(argv[6]),
                       .color = 2,
                       .width = 800,
                       .height = 480,
                       .timestamp = strtoul(argv[7], NULL, 10),
                       .week_start = atoi(argv[8]),
                       .temperature = atoi(argv[9]),
                       .voltage_mv = atoi(argv[11])};
    snprintf(data.ssid, sizeof(data.ssid), "%s", argv[10]);
    memset(frame, 255, sizeof(frame));
    DrawGUI(&data, capture, NULL);
    assert(GUI_TodoRenderValid());
    FILE *out = fopen(argv[5], "wb");
    assert(out);
    for (unsigned y = 0; y < 480; y++)
        for (unsigned x = 0; x < 800; x++) {
            unsigned bit = 0x80U >> (x & 7), at = y * 100 + x / 8;
            fputc(!(frame[1][at] & bit) ? 2 : !(frame[0][at] & bit) ? 0 : 1, out);
        }
    fclose(out);
    return 0;
}
